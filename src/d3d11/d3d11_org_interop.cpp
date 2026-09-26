#include "d3d11_buffer.h"
#include <atomic>
#include "d3d11_context_imm.h"
#include "d3d11_device.h"
#include "d3d11_interop.h"
#include "d3d11_org_interop.h"
#include "d3d11_texture.h"
#include "d3d11_view_srv.h"

#include "../dxvk/dxvk_adapter.h"
#include "../dxvk/dxvk_device.h"
#include "../dxvk/dxvk_instance.h"
#include "../dxvk/dxvk_interop.h"

namespace dxvk {

  namespace {

    D3D11VkInterop* GetInterop(ID3D11Device* pDevice, Com<IDXGIVkInteropDevice1>& ref) {
      if (!pDevice || FAILED(pDevice->QueryInterface(__uuidof(IDXGIVkInteropDevice1), reinterpret_cast<void**>(&ref))))
        return nullptr;

      // A DXVK device hands out its D3D11VkInterop member for this interface.
      return static_cast<D3D11VkInterop*>(ref.ptr());
    }

  }



  struct D3D11VkInterop::RegisteredBacking {
    uint64_t token;
    std::pair<uint32_t, uint64_t> identity;
    std::weak_ptr<RegistrationState> registry;
    ~RegisteredBacking() {
      // The last reference may retire on the finish worker after the client
      // registration (or the D3D11 interop object itself) has been destroyed.
      if (auto state = registry.lock()) {
        std::lock_guard lock(state->mutex);
        auto found = state->backings.find(identity);
        if (found != state->backings.end() && found->second.expired())
          state->backings.erase(found);
      }
    }
  };

  struct D3D11VkInterop::RegisteredResource {
    std::shared_ptr<RegisteredBacking> backing;
    Rc<DxvkBuffer> buffer;
    Rc<DxvkImage> image;
    DxvkOrgInteropRegistration description;
  };

  HRESULT D3D11VkInterop::RegisterResource(
          IUnknown* object, DxvkOrgInteropRegistration* registration) {
    if (!registration || registration->version != DXVK_ORG_RESOURCE_REGISTRATION_VERSION)
      return E_INVALIDARG;
    *registration = {};
    registration->version = DXVK_ORG_RESOURCE_REGISTRATION_VERSION;
    if (!object) return E_INVALIDARG;

    // Validate device ownership before any implementation-specific casts.
    Com<ID3D11DeviceChild> child;
    if (FAILED(object->QueryInterface(__uuidof(ID3D11DeviceChild), reinterpret_cast<void**>(&child))))
      return E_INVALIDARG;
    Com<ID3D11Device> owner;
    child->GetDevice(&owner);
    if (owner.ptr() != static_cast<ID3D11Device*>(m_device)) return E_INVALIDARG;

    try {
      auto entry = std::make_shared<RegisteredResource>();
      auto& description = entry->description;
      description.version = DXVK_ORG_RESOURCE_REGISTRATION_VERSION;
      description.resource.version = DXVK_ORG_INTEROP_VERSION;
      HRESULT result = GetResourceInfo(object, &description.resource);
      if (FAILED(result)) return result;

      Com<ID3D11Resource> resource;
      Com<ID3D11ShaderResourceView> view;
      if (SUCCEEDED(object->QueryInterface(__uuidof(ID3D11ShaderResourceView), reinterpret_cast<void**>(&view))))
        view->GetResource(&resource);
      else if (FAILED(object->QueryInterface(__uuidof(ID3D11Resource), reinterpret_cast<void**>(&resource))))
        return E_INVALIDARG;

      uint64_t nativeHandle = 0;
      if (description.resource.kind == DXVK_ORG_INTEROP_RESOURCE_BUFFER) {
        entry->buffer = GetCommonBuffer(resource.ptr())->GetBuffer();
        description.legalStages = entry->buffer->info().stages;
        description.legalAccess = entry->buffer->info().access;
        nativeHandle = uint64_t(description.resource.buffer.buffer);
      } else {
        entry->image = GetCommonTexture(resource.ptr())->GetImage();
        description.legalStages = entry->image->info().stages;
        description.legalAccess = entry->image->info().access;
        nativeHandle = uint64_t(description.resource.image.image);
      }
      description.queueFamily = GetDXVKDevice()->queues().graphics.queueFamily;
      const auto key = std::make_pair(description.resource.kind, nativeHandle);
      // Process-wide monotonically allocated tokens also reject tokens from a
      // different device or an earlier device lifetime. No COM owners are kept:
      // native resource references pin DXVK-owned allocations without a D3D11
      // cycle. Externally imported allocations still require their owner lease.
      static std::atomic<uint64_t> nextToken{1};
      std::lock_guard lock(m_registration->mutex);
      auto found = m_registration->backings.find(key);
      if (found != m_registration->backings.end()) entry->backing = found->second.lock();
      if (!entry->backing) {
        entry->backing = std::make_shared<RegisteredBacking>();
        entry->backing->token = nextToken.fetch_add(1);
        entry->backing->identity = key;
        entry->backing->registry = m_registration;
      }
      description.backingToken = entry->backing->token;
      description.leaseToken = nextToken.fetch_add(1);
      m_registration->resources.emplace(description.leaseToken, entry);
      try {
        m_registration->backings.insert_or_assign(key, entry->backing);
      } catch (...) {
        m_registration->resources.erase(description.leaseToken);
        throw;
      }
      *registration = description;
      return S_OK;
    } catch (const std::bad_alloc&) {
      return E_OUTOFMEMORY;
    } catch (const DxvkError& error) {
      Logger::err(error.message());
      return E_FAIL;
    }
  }

  HRESULT D3D11VkInterop::UnregisterResource(uint64_t leaseToken) {
    std::shared_ptr<RegisteredResource> retired;
    {
      std::lock_guard lock(m_registration->mutex);
      auto found = m_registration->resources.find(leaseToken);
      if (found == m_registration->resources.end()) return E_INVALIDARG;
      retired = std::move(found->second);
      m_registration->resources.erase(found);
      // The resource entry itself may have submitted owners. Its backing's
      // shared_ptr use_count cannot count those owners: they share the entry.
      // RegisteredBacking removes the weak lookup on actual final retirement.
    }
    // Native-resource destruction may acquire allocator locks.
    retired.reset();
    return S_OK;
  }


  HRESULT D3D11VkInterop::CreateBufferFromVkBuffer(
    const D3D11_BUFFER_DESC*          pDesc,
          VkBuffer                    buffer,
          ID3D11Buffer**              ppBuffer) {
    InitReturnPtr(ppBuffer);

    if (!pDesc || buffer == VK_NULL_HANDLE)
      return E_INVALIDARG;

    D3D11_BUFFER_DESC desc = *pDesc;
    HRESULT hr = D3D11Buffer::NormalizeBufferProperties(&desc);

    if (FAILED(hr))
      return hr;

    // The client owns the memory: nothing may rename, relocate or map it.
    if (desc.Usage != D3D11_USAGE_DEFAULT || desc.CPUAccessFlags
     || (desc.MiscFlags & (D3D11_RESOURCE_MISC_TILED | D3D11_RESOURCE_MISC_TILE_POOL)))
      return E_INVALIDARG;

    if (!ppBuffer)
      return S_FALSE;

    // Same import path as D3D11On12: a buffer backed by an existing VkBuffer.
    D3D11_ON_12_RESOURCE_INFO info = { };
    info.VulkanHandle = uint64_t(buffer);
    info.VulkanOffset = 0u;

    try {
      Com<D3D11Buffer> result = new D3D11Buffer(m_device, &desc, &info);
      *ppBuffer = result.ref();
      return S_OK;
    } catch (const DxvkError& e) {
      Logger::err(e.message());
      return E_INVALIDARG;
    }
  }


  HRESULT D3D11VkInterop::GetResourceInfo(
          IUnknown*                   pObject,
          DxvkOrgInteropResourceInfo* pInfo) {
    if (!pObject || !pInfo || pInfo->version != DXVK_ORG_INTEROP_VERSION)
      return E_INVALIDARG;

    uint32_t version = pInfo->version;
    *pInfo = DxvkOrgInteropResourceInfo();
    pInfo->version = version;

    // A shader resource view selects the image view; otherwise the resource itself.
    Com<ID3D11ShaderResourceView> srv;
    Com<ID3D11Resource> resource;
    Rc<DxvkImageView> view;

    if (SUCCEEDED(pObject->QueryInterface(__uuidof(ID3D11ShaderResourceView), reinterpret_cast<void**>(&srv)))) {
      auto srvImpl = static_cast<D3D11ShaderResourceView*>(srv.ptr());
      view = srvImpl->GetImageView();

      if (view == nullptr)
        return E_INVALIDARG;  // buffer view
    } else if (FAILED(pObject->QueryInterface(__uuidof(ID3D11Resource), reinterpret_cast<void**>(&resource)))) {
      return E_INVALIDARG;
    }

    if (resource != nullptr) {
      D3D11_COMMON_RESOURCE_DESC desc = { };

      if (FAILED(GetCommonResourceDesc(resource.ptr(), &desc)))
        return E_INVALIDARG;

      if (desc.Dim == D3D11_RESOURCE_DIMENSION_BUFFER) {
        auto buffer = GetCommonBuffer(resource.ptr());

        // Discard maps rename the buffer; nothing about it can be stable.
        if (buffer->Desc()->Usage == D3D11_USAGE_DYNAMIC || buffer->Desc()->CPUAccessFlags)
          return E_INVALIDARG;

        Rc<DxvkBuffer> dxvkBuffer = buffer->GetBuffer();

        if (dxvkBuffer->canRelocate()) {
          auto chunk = m_device->AllocCsChunk(DxvkCsChunkFlag::SingleUse);

          chunk->push([cBuffer = dxvkBuffer] (DxvkContext* ctx) {
            ctx->ensureBufferAddress(cBuffer);
          });

          m_device->GetContext()->InjectCsChunk(DxvkCsQueue::HighPriority, std::move(chunk), true);
        }

        auto slice = dxvkBuffer->getSliceInfo();
        pInfo->kind = DXVK_ORG_INTEROP_RESOURCE_BUFFER;
        pInfo->buffer.buffer = slice.buffer;
        pInfo->buffer.offset = slice.offset;
        pInfo->buffer.size = buffer->Desc()->ByteWidth;
        pInfo->buffer.address = slice.gpuAddress;
        pInfo->buffer.usage = dxvkBuffer->info().usage;
        return S_OK;
      }

      auto texture = GetCommonTexture(resource.ptr());

      if (!texture)
        return E_INVALIDARG;

      Rc<DxvkImage> image = texture->GetImage();
      const auto& info = image->info();

      DxvkImageViewKey key = { };
      key.viewType = info.numLayers > 1u ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;

      if (info.type == VK_IMAGE_TYPE_1D)
        key.viewType = info.numLayers > 1u ? VK_IMAGE_VIEW_TYPE_1D_ARRAY : VK_IMAGE_VIEW_TYPE_1D;
      else if (info.type == VK_IMAGE_TYPE_3D)
        key.viewType = VK_IMAGE_VIEW_TYPE_3D;

      key.format = info.format;
      key.aspects = image->formatInfo()->aspectMask;
      key.mipIndex = 0u;
      key.mipCount = info.mipLevels;
      key.layerIndex = 0u;
      key.layerCount = info.numLayers;
      view = nullptr;

      if (!LockImage(image))
        return E_FAIL;

      FillImageInfo(image, key, pInfo->image);
      pInfo->kind = DXVK_ORG_INTEROP_RESOURCE_IMAGE;
      return S_OK;
    }

    Rc<DxvkImage> image = view->image();

    if (!LockImage(image))
      return E_FAIL;

    FillImageInfo(image, view->info(), pInfo->image);
    pInfo->kind = DXVK_ORG_INTEROP_RESOURCE_IMAGE;
    return S_OK;
  }


  bool D3D11VkInterop::LockImage(const Rc<DxvkImage>& image) {
    // Same condition as D3D11DeviceExt::LockImage: nothing to do for an image that cannot move.
    if (!image->canRelocate() && (image->info().usage & VK_IMAGE_USAGE_SAMPLED_BIT))
      return true;

    return m_device->LockImage(image, VK_IMAGE_USAGE_SAMPLED_BIT);
  }


  void D3D11VkInterop::FillImageInfo(const Rc<DxvkImage>& image, const DxvkImageViewKey& view, DxvkOrgInteropImageInfo& out) {
    const auto& info = image->info();
    out.image = image->handle();
    out.type = info.type;
    out.format = info.format;
    out.flags = info.flags;
    out.extent = info.extent;
    out.mipLevels = info.mipLevels;
    out.arrayLayers = info.numLayers;
    out.samples = info.sampleCount;
    out.usage = info.usage;
    out.layout = info.layout;
    out.viewType = view.viewType;
    out.viewFormat = view.format;
    out.components = view.unpackSwizzle();
    out.subresourceRange.aspectMask = view.aspects;
    out.subresourceRange.baseMipLevel = view.mipIndex;
    out.subresourceRange.levelCount = view.mipCount;
    out.subresourceRange.baseArrayLayer = view.layerIndex;
    out.subresourceRange.layerCount = view.layerCount;
  }


  const uint64_t* D3D11VkInterop::GetSubmissionCounter() const {
    return m_device->GetContext()->GetSubmissionCounter();
  }


  HRESULT D3D11VkInterop::EnqueueExternalSubmission(
    const DxvkOrgInteropSubmission*   pSubmission) {
    if (!pSubmission || pSubmission->version != DXVK_ORG_INTEROP_VERSION
     || (pSubmission->waitCount && !pSubmission->waits)
     || (pSubmission->commandBufferCount && !pSubmission->commandBuffers)
     || (pSubmission->signalCount && !pSubmission->signals))
      return E_INVALIDARG;

    DxvkExternalSubmitInfo submitInfo;
    auto& submit = submitInfo.submits.emplace_back();
    submit.waits.assign(pSubmission->waits, pSubmission->waits + pSubmission->waitCount);
    submit.commandBuffers.assign(pSubmission->commandBuffers, pSubmission->commandBuffers + pSubmission->commandBufferCount);
    submit.signals.assign(pSubmission->signals, pSubmission->signals + pSubmission->signalCount);

    if (pSubmission->label)
      submitInfo.label = pSubmission->label;

    for (auto& info : submit.waits)
      info.pNext = nullptr;
    for (auto& info : submit.commandBuffers)
      info.pNext = nullptr;
    for (auto& info : submit.signals)
      info.pNext = nullptr;

    if (pSubmission->onSubmitted) {
      submitInfo.onSubmitted = [cb = pSubmission->onSubmitted, user = pSubmission->user] (VkResult result) {
        cb(user, result);
      };
    }

    m_device->GetContext()->EnqueueExternalSubmission(std::move(submitInfo));
    return S_OK;
  }


  HRESULT D3D11VkInterop::EnqueueQueueCallback(
          void                          (*pCallback)(void*, VkQueue),
          void*                           pUser) {
    if (!pCallback)
      return E_INVALIDARG;

    DxvkExternalSubmitInfo submitInfo;
    submitInfo.queueCallback = [pCallback, pUser] (VkQueue queue) {
      pCallback(pUser, queue);
    };

    m_device->GetContext()->EnqueueExternalSubmission(std::move(submitInfo));
    return S_OK;
  }


  HRESULT D3D11VkInterop::EmitCommandBufferCallback(
          void                          (*pCallback)(void*, VkCommandBuffer),
          void*                                   pUser) {
    if (!pCallback)
      return E_INVALIDARG;

    m_device->GetContext()->EmitExternalCommands([pCallback, pUser] (VkCommandBuffer commandBuffer) {
      pCallback(pUser, commandBuffer);
    });
    return S_OK;
  }


  HRESULT D3D11VkInterop::SetCommandBufferBoundaryCallbacks(
          void                          (*pOnEnd)(void*, VkCommandBuffer),
          void                          (*pOnBegin)(void*, VkCommandBuffer),
          void*                                   pUser) {
    std::function<void (VkCommandBuffer)> onEnd, onBegin;

    if (pOnEnd)
      onEnd = [pOnEnd, pUser] (VkCommandBuffer commandBuffer) { pOnEnd(pUser, commandBuffer); };
    if (pOnBegin)
      onBegin = [pOnBegin, pUser] (VkCommandBuffer commandBuffer) { pOnBegin(pUser, commandBuffer); };

    m_device->GetContext()->SetExternalCommandBufferHooks(std::move(onEnd), std::move(onBegin));
    return S_OK;
  }


  static HRESULT CopyExternalSubmissions(const DxvkOrgInteropSubmissionBatch* pBatch,
      DxvkExternalSubmitInfo& submitInfo) {
    if (!pBatch || pBatch->version != DXVK_ORG_INTEROP_VERSION
     || !pBatch->submitCount || !pBatch->submits)
      return E_INVALIDARG;

    submitInfo.submits.resize(pBatch->submitCount);

    for (uint32_t i = 0; i < pBatch->submitCount; i++) {
      const auto& source = pBatch->submits[i];

      if ((source.waitSemaphoreInfoCount && !source.pWaitSemaphoreInfos)
       || (source.commandBufferInfoCount && !source.pCommandBufferInfos)
       || (source.signalSemaphoreInfoCount && !source.pSignalSemaphoreInfos))
        return E_INVALIDARG;

      auto& submit = submitInfo.submits[i];
      if (source.waitSemaphoreInfoCount)
        submit.waits.assign(source.pWaitSemaphoreInfos, source.pWaitSemaphoreInfos + source.waitSemaphoreInfoCount);
      if (source.commandBufferInfoCount)
        submit.commandBuffers.assign(source.pCommandBufferInfos, source.pCommandBufferInfos + source.commandBufferInfoCount);
      if (source.signalSemaphoreInfoCount)
        submit.signals.assign(source.pSignalSemaphoreInfos, source.pSignalSemaphoreInfos + source.signalSemaphoreInfoCount);

      for (auto& info : submit.waits)
        info.pNext = nullptr;
      for (auto& info : submit.commandBuffers)
        info.pNext = nullptr;
      for (auto& info : submit.signals)
        info.pNext = nullptr;
    }

    if (pBatch->label)
      submitInfo.label = pBatch->label;

    if (pBatch->onSubmitted) {
      submitInfo.onSubmitted = [cb = pBatch->onSubmitted, user = pBatch->user] (VkResult result) {
        cb(user, result);
      };
    }

    return S_OK;
  }

  HRESULT D3D11VkInterop::EnqueueExternalSubmissions(const DxvkOrgInteropSubmissionBatch* pBatch) {
    DxvkExternalSubmitInfo submitInfo;
    HRESULT result = CopyExternalSubmissions(pBatch, submitInfo);
    if (FAILED(result)) return result;
    m_device->GetContext()->EnqueueExternalSubmission(std::move(submitInfo));
    return S_OK;
  }

  HRESULT D3D11VkInterop::PrepareLeasedSubmission(const DxvkOrgInteropLeasedSubmission* submission, DxvkExternalSubmitInfo& info) {
    if (!submission || submission->version != DXVK_ORG_RESOURCE_INTERFACE_VERSION
        || (submission->leaseCount && !submission->leaseTokens))
      return E_INVALIDARG;
    try {
      HRESULT result = CopyExternalSubmissions(&submission->batch, info);
      if (FAILED(result)) return result;
      // The managed interface cannot silently discard submission extensions or
      // protected/device-group execution requirements it does not implement.
      for (uint32_t i = 0; i < submission->batch.submitCount; ++i) {
        const auto& source = submission->batch.submits[i];
        if (source.sType != VK_STRUCTURE_TYPE_SUBMIT_INFO_2 || source.pNext || source.flags)
          return E_INVALIDARG;
        for (uint32_t j = 0; j < source.commandBufferInfoCount; ++j) {
          const auto& command = source.pCommandBufferInfos[j];
          if (command.sType != VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO || command.pNext || !command.commandBuffer
              || command.deviceMask > 1u)
            return E_INVALIDARG;
        }
        auto validSemaphore = [](const VkSemaphoreSubmitInfo& semaphore) {
          return semaphore.sType == VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO && !semaphore.pNext
            && semaphore.semaphore && !semaphore.deviceIndex;
        };
        for (uint32_t j = 0; j < source.waitSemaphoreInfoCount; ++j)
          if (!validSemaphore(source.pWaitSemaphoreInfos[j])) return E_INVALIDARG;
        for (uint32_t j = 0; j < source.signalSemaphoreInfoCount; ++j)
          if (!validSemaphore(source.pSignalSemaphoreInfos[j])) return E_INVALIDARG;
      }
      info.trackCompletion = true;
      info.leases.reserve(submission->leaseCount);
      {
        std::lock_guard lock(m_registration->mutex);
        for (uint32_t i = 0; i < submission->leaseCount; ++i) {
          auto found = m_registration->resources.find(submission->leaseTokens[i]);
          if (found == m_registration->resources.end()) return E_INVALIDARG;
          info.leases.push_back(found->second);
        }
      }
      if (submission->onCompleted)
        info.onCompleted = [callback = submission->onCompleted, user = submission->completionUser](VkResult result) {
          callback(user, result);
        };
      return S_OK;
    } catch (const std::bad_alloc&) {
      return E_OUTOFMEMORY;
    } catch (const DxvkError& error) {
      Logger::err(error.message());
      return E_FAIL;
    }
  }

  HRESULT D3D11VkInterop::EnqueueLeasedSubmission(const DxvkOrgInteropLeasedSubmission* submission) {
    DxvkExternalSubmitInfo info;
    HRESULT result = PrepareLeasedSubmission(submission, info);
    if (FAILED(result)) return result;
    m_device->GetContext()->EnqueueExternalSubmission(std::move(info));
    return S_OK;
  }

  HRESULT D3D11VkInterop::EnqueueBufferHandoff(const DxvkOrgInteropBufferHandoff* handoff) {
    if (!handoff || handoff->version != DXVK_ORG_BUFFER_HANDOFF_VERSION
        || !handoff->accessCount || !handoff->accesses || handoff->submission.batch.submitCount != 1)
      return E_INVALIDARG;
    DxvkOrgInteropResourceHandoff resources = {};
    resources.version = DXVK_ORG_RESOURCE_HANDOFF_VERSION;
    resources.submission = handoff->submission;
    DxvkOrgInteropEpochAccesses epoch = {};
    epoch.complete = VK_TRUE;
    epoch.bufferCount = handoff->accessCount;
    epoch.buffers = handoff->accesses;
    resources.epochCount = 1;
    resources.epochs = &epoch;
    return EnqueueResourceHandoff(&resources);
  }

  HRESULT D3D11VkInterop::EnqueueResourceHandoff(const DxvkOrgInteropResourceHandoff* handoff) {
    if (!handoff || handoff->version != DXVK_ORG_RESOURCE_HANDOFF_VERSION
        || !handoff->epochs || !handoff->epochCount
        || handoff->submission.batch.submitCount != handoff->epochCount)
      return E_INVALIDARG;
    try {
      DxvkExternalSubmitInfo info;
      HRESULT result = PrepareLeasedSubmission(&handoff->submission, info);
      if (FAILED(result)) return result;
      {
        std::lock_guard lock(m_registration->mutex);
        for (uint32_t epochIndex = 0; epochIndex < handoff->epochCount; ++epochIndex) {
          const auto& epoch = handoff->epochs[epochIndex];
          if (epoch.complete != VK_TRUE || (epoch.bufferCount && !epoch.buffers) || (epoch.imageCount && !epoch.images))
            return E_INVALIDARG;
          auto& target = info.submits[epochIndex];
          target.bufferManifest.reserve(epoch.bufferCount);
          target.imageManifest.reserve(epoch.imageCount);
          for (uint32_t i = 0; i < epoch.bufferCount; ++i) {
            const auto& access = epoch.buffers[i];
            auto found = m_registration->resources.find(access.leaseToken);
            if (found == m_registration->resources.end() || !found->second->buffer
                || !access.stages || !access.access || !access.size)
              return E_INVALIDARG;
            const auto& entry = *found->second;
            if (access.offset >= entry.description.resource.buffer.size
                || access.size > entry.description.resource.buffer.size - access.offset)
              return E_INVALIDARG;
            target.bufferManifest.push_back({entry.buffer, access.offset, access.size, access.stages, access.access});
            info.leases.push_back(found->second);
          }
          for (uint32_t i = 0; i < epoch.imageCount; ++i) {
            const auto& access = epoch.images[i];
            auto found = m_registration->resources.find(access.leaseToken);
            if (found == m_registration->resources.end() || !found->second->image
                || !access.stages || !access.access || access.layout != VK_IMAGE_LAYOUT_GENERAL)
              return E_INVALIDARG;
            const auto& entry = *found->second;
            const auto& allowed = entry.description.resource.image.subresourceRange;
            const auto& range = access.range;
            if (entry.description.resource.image.layout != VK_IMAGE_LAYOUT_GENERAL
                || !range.aspectMask || (range.aspectMask & ~allowed.aspectMask)
                || !range.levelCount || !range.layerCount
                || range.baseMipLevel < allowed.baseMipLevel || range.baseArrayLayer < allowed.baseArrayLayer
                || range.baseMipLevel - allowed.baseMipLevel >= allowed.levelCount
                || range.levelCount > allowed.levelCount - (range.baseMipLevel - allowed.baseMipLevel)
                || range.baseArrayLayer - allowed.baseArrayLayer >= allowed.layerCount
                || range.layerCount > allowed.layerCount - (range.baseArrayLayer - allowed.baseArrayLayer))
              return E_INVALIDARG;
            target.imageManifest.push_back({entry.image, range, access.stages, access.access});
            info.leases.push_back(found->second);
          }
        }
      }
      m_device->GetContext()->EnqueueExternalSubmission(std::move(info));
      return S_OK;
    } catch (const std::bad_alloc&) {
      return E_OUTOFMEMORY;
    }
  }

}


extern "C" {

  using namespace dxvk;

  DLLEXPORT HRESULT __stdcall dxvkRequestDeviceFeatures(const DxvkOrgInteropFeatureRequest* pRequest) {
    if (!pRequest || pRequest->version != DXVK_ORG_INTEROP_VERSION || (pRequest->featureCount && !pRequest->features))
      return E_INVALIDARG;

    std::vector<DxvkInteropFeature> features;

    for (uint32_t i = 0u; i < pRequest->featureCount; i++) {
      const auto& f = pRequest->features[i];

      if (!f.feature)
        return E_INVALIDARG;

      features.push_back({ f.extension ? f.extension : "", f.feature });
    }

    setInteropFeatureRequest(std::move(features));
    return S_OK;
  }


  DLLEXPORT HRESULT __stdcall dxvkGetInteropDeviceInfo(ID3D11Device* pDevice, DxvkOrgInteropDeviceInfo* pInfo) {
    if (!pInfo || pInfo->version != DXVK_ORG_INTEROP_VERSION)
      return E_INVALIDARG;

    Com<IDXGIVkInteropDevice1> ref;
    auto interop = GetInterop(pDevice, ref);

    if (!interop)
      return E_NOINTERFACE;

    Rc<DxvkDevice> device = interop->GetDXVKDevice();
    auto record = findInteropDevice(device->handle());

    if (!record)
      return E_FAIL;

    const auto& queue = device->queues().graphics;

    pInfo->getInstanceProcAddr = device->instance()->vki()->getLoaderProc();
    pInfo->instance = device->instance()->handle();
    pInfo->instanceApiVersion = DxvkVulkanApiVersion;
    pInfo->physicalDevice = device->adapter()->handle();
    pInfo->device = device->handle();
    pInfo->graphicsQueue = queue.queueHandle;
    pInfo->graphicsQueueFamily = queue.queueFamily;
    pInfo->graphicsQueueIndex = queue.queueIndex;
    pInfo->enabledExtensionCount = uint32_t(record->extensionPointers.size());
    pInfo->enabledExtensions = record->extensionPointers.data();
    pInfo->enabledFeatures = &record->features;
    pInfo->grantedFeatureCount = uint32_t(record->grantedFeatures.size());
    pInfo->deniedFeatureCount = uint32_t(record->deniedFeatures.size());
    pInfo->enabledInstanceExtensionCount = uint32_t(record->instanceExtensionPointers.size());
    pInfo->enabledInstanceExtensions = record->instanceExtensionPointers.data();
    return S_OK;
  }


  DLLEXPORT HRESULT __stdcall dxvkCreateBufferFromVkBuffer(ID3D11Device* pDevice,
    const D3D11_BUFFER_DESC* pDesc, VkBuffer buffer, ID3D11Buffer** ppBuffer) {
    Com<IDXGIVkInteropDevice1> ref;
    auto interop = GetInterop(pDevice, ref);

    if (!interop)
      return E_NOINTERFACE;

    return interop->CreateBufferFromVkBuffer(pDesc, buffer, ppBuffer);
  }


  DLLEXPORT HRESULT __stdcall dxvkEnqueueInteropSubmission(ID3D11Device* pDevice,
    const DxvkOrgInteropSubmission* pSubmission) {
    Com<IDXGIVkInteropDevice1> ref;
    auto interop = GetInterop(pDevice, ref);

    if (!interop)
      return E_NOINTERFACE;

    return interop->EnqueueExternalSubmission(pSubmission);
  }


  DLLEXPORT HRESULT __stdcall dxvkEnqueueInteropSubmissions(ID3D11Device* pDevice,
    const DxvkOrgInteropSubmissionBatch* pBatch) {
    Com<IDXGIVkInteropDevice1> ref;
    auto interop = GetInterop(pDevice, ref);

    if (!interop)
      return E_NOINTERFACE;

    return interop->EnqueueExternalSubmissions(pBatch);
  }


  DLLEXPORT HRESULT __stdcall dxvkEnqueueQueueCallback(ID3D11Device* pDevice,
    PFN_dxvkOrgInteropQueueCallback pCallback, void* pUser) {
    Com<IDXGIVkInteropDevice1> ref;
    auto interop = GetInterop(pDevice, ref);

    if (!interop)
      return E_NOINTERFACE;

    return interop->EnqueueQueueCallback(pCallback, pUser);
  }


  DLLEXPORT HRESULT __stdcall dxvkEmitCommandBufferCallback(ID3D11Device* pDevice,
    PFN_dxvkOrgInteropCommandBufferCallback pCallback, void* pUser) {
    Com<IDXGIVkInteropDevice1> ref;
    auto interop = GetInterop(pDevice, ref);

    if (!interop)
      return E_NOINTERFACE;

    return interop->EmitCommandBufferCallback(pCallback, pUser);
  }


  DLLEXPORT HRESULT __stdcall dxvkSetCommandBufferBoundaryCallbacks(ID3D11Device* pDevice,
    PFN_dxvkOrgInteropCommandBufferCallback pOnEnd, PFN_dxvkOrgInteropCommandBufferCallback pOnBegin, void* pUser) {
    Com<IDXGIVkInteropDevice1> ref;
    auto interop = GetInterop(pDevice, ref);

    if (!interop)
      return E_NOINTERFACE;

    return interop->SetCommandBufferBoundaryCallbacks(pOnEnd, pOnBegin, pUser);
  }


  DLLEXPORT HRESULT __stdcall dxvkEnqueueBufferHandoff(void* context, const DxvkOrgInteropBufferHandoff* handoff) {
    if (!context) return E_INVALIDARG;
    return static_cast<D3D11VkInterop*>(context)->EnqueueBufferHandoff(handoff);
  }

  DLLEXPORT HRESULT __stdcall dxvkEnqueueResourceHandoff(void* context, const DxvkOrgInteropResourceHandoff* handoff) {
    if (!context) return E_INVALIDARG;
    return static_cast<D3D11VkInterop*>(context)->EnqueueResourceHandoff(handoff);
  }

  DLLEXPORT HRESULT __stdcall dxvkGetResourceInteropInterface(ID3D11Device* device,
      DxvkOrgInteropResourceInterface* output) {
    if (!output || output->version != DXVK_ORG_RESOURCE_INTERFACE_VERSION) return E_INVALIDARG;
    *output = {};
    output->version = DXVK_ORG_RESOURCE_INTERFACE_VERSION;
    Com<IDXGIVkInteropDevice1> ref;
    auto interop = GetInterop(device, ref);
    if (!interop) return E_NOINTERFACE;
    output->context = interop;
    output->capabilities = DXVK_ORG_CAP_RESOURCE_REGISTRATION | DXVK_ORG_CAP_RETAINED_SUBMISSION;
    output->registerResource = [](void* context, IUnknown* object, DxvkOrgInteropRegistration* registration) -> HRESULT {
      return static_cast<D3D11VkInterop*>(context)->RegisterResource(object, registration);
    };
    output->unregisterResource = [](void* context, uint64_t token) -> HRESULT {
      return static_cast<D3D11VkInterop*>(context)->UnregisterResource(token);
    };
    output->enqueue = [](void* context, const DxvkOrgInteropLeasedSubmission* submission) -> HRESULT {
      return static_cast<D3D11VkInterop*>(context)->EnqueueLeasedSubmission(submission);
    };
    return S_OK;
  }

  DLLEXPORT HRESULT __stdcall dxvkRegisterInteropResource(ID3D11Device* device,
    IUnknown* object, DxvkOrgInteropRegistration* registration) {
    Com<IDXGIVkInteropDevice1> ref;
    auto interop = GetInterop(device, ref);
    return interop ? interop->RegisterResource(object, registration) : E_NOINTERFACE;
  }

  DLLEXPORT HRESULT __stdcall dxvkUnregisterInteropResource(ID3D11Device* device, uint64_t leaseToken) {
    Com<IDXGIVkInteropDevice1> ref;
    auto interop = GetInterop(device, ref);
    return interop ? interop->UnregisterResource(leaseToken) : E_NOINTERFACE;
  }

  DLLEXPORT HRESULT __stdcall dxvkGetInteropResourceInfo(ID3D11Device* pDevice,
    IUnknown* pObject, DxvkOrgInteropResourceInfo* pInfo) {
    Com<IDXGIVkInteropDevice1> ref;
    auto interop = GetInterop(pDevice, ref);

    if (!interop)
      return E_NOINTERFACE;

    return interop->GetResourceInfo(pObject, pInfo);
  }


  DLLEXPORT HRESULT __stdcall dxvkGetSubmissionCounter(ID3D11Device* pDevice,
    const volatile uint64_t** ppCounter) {
    if (!ppCounter)
      return E_INVALIDARG;
    *ppCounter = nullptr;

    Com<IDXGIVkInteropDevice1> ref;
    auto interop = GetInterop(pDevice, ref);

    if (!interop)
      return E_NOINTERFACE;

    *ppCounter = interop->GetSubmissionCounter();
    return S_OK;
  }


  DLLEXPORT HRESULT __stdcall dxvkSetSubmissionTrace(ID3D11Device* pDevice, BOOL enable) {
    Com<IDXGIVkInteropDevice1> ref;
    auto interop = GetInterop(pDevice, ref);

    if (!interop)
      return E_NOINTERFACE;

    interop->GetDXVKDevice()->setSubmissionTrace(enable != FALSE);
    return S_OK;
  }


  DLLEXPORT HRESULT __stdcall dxvkReadSubmissionTrace(ID3D11Device* pDevice,
    DxvkOrgSubmissionTraceRecord* pRecords, uint32_t capacity, uint32_t* pCount) {
    if (!pCount || (capacity && !pRecords))
      return E_INVALIDARG;
    *pCount = 0u;

    Com<IDXGIVkInteropDevice1> ref;
    auto interop = GetInterop(pDevice, ref);

    if (!interop)
      return E_NOINTERFACE;

    std::vector<DxvkSubmissionTraceRecord> records(capacity);
    uint32_t count = interop->GetDXVKDevice()->readSubmissionTrace(records.data(), capacity);

    for (uint32_t i = 0u; i < count; i++) {
      const auto& src = records[i];
      auto& dst = pRecords[i];
      dst.kind = uint32_t(src.kind);
      dst.flushType = src.flushType;
      dst.submissionId = src.submissionId;
      dst.appQpc = src.appQpc;
      dst.csQpc = src.csQpc;
      dst.queueQpc = src.queueQpc;
      dst.gpuBegin = src.gpuBegin;
      dst.gpuEnd = src.gpuEnd;
      std::memset(dst.label, 0, sizeof(dst.label));
      std::memcpy(dst.label, src.label.data(), std::min(src.label.size(), sizeof(dst.label) - 1u));
    }

    *pCount = count;
    return S_OK;
  }


  DLLEXPORT HRESULT __stdcall dxvkSetDeviceTeardownCallback(PFN_dxvkOrgInteropTeardown pCallback, void* pUser) {
    setInteropTeardownCallback(pCallback, pUser);
    return S_OK;
  }

}
