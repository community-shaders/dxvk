#include "d3d11_buffer.h"
#include "d3d11_context_imm.h"
#include "d3d11_device.h"
#include "d3d11_interop.h"
#include "d3d11_org_interop.h"
#include "d3d11_texture.h"
#include "d3d11_view_srv.h"

#include <algorithm>
#include <vector>

#include "../dxvk/dxvk_adapter.h"
#include "../dxvk/dxvk_device.h"
#include "../dxvk/dxvk_hang.h"
#include "../dxvk/dxvk_instance.h"
#include "../dxvk/dxvk_interop.h"

namespace dxvk {

  namespace {
    thread_local DxvkDrawCaller g_nextDrawCaller;
    thread_local std::string    g_nextShaderName;
  }

  std::string TakeNextShaderName() {
    return std::exchange(g_nextShaderName, std::string());
  }

  // Names the next shader this thread creates (CreateVertexShader, CreatePixelShader, ...) for
  // capture tools; null clears it. The name only reaches the driver under debug utils.
  extern "C" DLLEXPORT void __stdcall dxvkSetNextShaderName(const char* name) {
    g_nextShaderName = name ? name : "";
  }

  DxvkDrawCaller TakeDrawCaller() noexcept {
    auto caller = g_nextDrawCaller;
    g_nextDrawCaller = {};
    return caller;
  }

  extern "C" DLLEXPORT void __stdcall dxvkSetDrawCaller(
    const void* caller, const void* const* stack, uint32_t count) {
    g_nextDrawCaller = {};
    g_nextDrawCaller.caller = caller;
    g_nextDrawCaller.stackCount = std::min(count, 16u);
    for (uint32_t i = 0; i < g_nextDrawCaller.stackCount; i++)
      g_nextDrawCaller.stack[i] = stack[i];
  }

  namespace {

    // The view a texture (not a view of it) is described with: the whole image.
    DxvkImageViewKey WholeImageViewKey(const DxvkImage& image) {
      const auto& info = image.info();

      DxvkImageViewKey key = { };
      key.viewType = info.numLayers > 1u ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;

      if (info.type == VK_IMAGE_TYPE_1D)
        key.viewType = info.numLayers > 1u ? VK_IMAGE_VIEW_TYPE_1D_ARRAY : VK_IMAGE_VIEW_TYPE_1D;
      else if (info.type == VK_IMAGE_TYPE_3D)
        key.viewType = VK_IMAGE_VIEW_TYPE_3D;

      key.format = info.format;
      key.aspects = image.formatInfo()->aspectMask;
      key.mipIndex = 0u;
      key.mipCount = info.mipLevels;
      key.layerIndex = 0u;
      key.layerCount = info.numLayers;
      return key;
    }

    D3D11VkInterop* GetInterop(ID3D11Device* pDevice, Com<IDXGIVkInteropDevice1>& ref) {
      if (!pDevice || FAILED(pDevice->QueryInterface(__uuidof(IDXGIVkInteropDevice1), reinterpret_cast<void**>(&ref))))
        return nullptr;

      // A DXVK device hands out its D3D11VkInterop member for this interface.
      return static_cast<D3D11VkInterop*>(ref.ptr());
    }

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
      DxvkImageViewKey key = WholeImageViewKey(*image);
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


  HRESULT D3D11VkInterop::GetResourceInfos(
          UINT                        Count,
          IUnknown* const*            ppObjects,
          DxvkOrgInteropResourceInfo* pInfos,
          HRESULT*                    pResults) {
    if (Count && (!ppObjects || !pInfos || !pResults))
      return E_INVALIDARG;

    // Buffers are pinned together: one chunk, one synchronization. Everything
    // else goes through GetResourceInfo.
    struct Pending {
      UINT index;
      D3D11Buffer* buffer;
      Rc<DxvkBuffer> dxvkBuffer;
    };

    std::vector<Pending> pending;
    pending.reserve(Count);

    for (UINT i = 0; i < Count; i++) {
      pResults[i] = E_INVALIDARG;

      if (!ppObjects[i] || pInfos[i].version != DXVK_ORG_INTEROP_VERSION)
        continue;

      Com<ID3D11Resource> resource;
      D3D11_COMMON_RESOURCE_DESC desc = { };

      if (SUCCEEDED(ppObjects[i]->QueryInterface(__uuidof(ID3D11Resource), reinterpret_cast<void**>(&resource)))
       && SUCCEEDED(GetCommonResourceDesc(resource.ptr(), &desc))
       && desc.Dim == D3D11_RESOURCE_DIMENSION_BUFFER) {
        auto buffer = GetCommonBuffer(resource.ptr());
        uint32_t version = pInfos[i].version;
        pInfos[i] = DxvkOrgInteropResourceInfo();
        pInfos[i].version = version;

        // Discard maps rename the buffer; nothing about it can be stable.
        if (buffer->Desc()->Usage == D3D11_USAGE_DYNAMIC || buffer->Desc()->CPUAccessFlags)
          continue;

        pending.push_back({ i, buffer, buffer->GetBuffer() });
      } else {
        pResults[i] = GetResourceInfo(ppObjects[i], &pInfos[i]);
      }
    }

    // A full chunk goes ahead unsynchronized; the queue runs them in order, so
    // the last one's synchronization covers every pin.
    DxvkCsChunkRef chunk;

    for (const auto& entry : pending) {
      if (!entry.dxvkBuffer->canRelocate())
        continue;

      auto command = [cBuffer = entry.dxvkBuffer] (DxvkContext* ctx) {
        ctx->ensureBufferAddress(cBuffer);
      };

      if (!chunk || !chunk->push(command)) {
        if (chunk)
          m_device->GetContext()->InjectCsChunk(DxvkCsQueue::HighPriority, std::move(chunk), false);

        chunk = m_device->AllocCsChunk(DxvkCsChunkFlag::SingleUse);
        chunk->push(command);
      }
    }

    if (chunk)
      m_device->GetContext()->InjectCsChunk(DxvkCsQueue::HighPriority, std::move(chunk), true);

    for (const auto& entry : pending) {
      auto slice = entry.dxvkBuffer->getSliceInfo();
      auto& info = pInfos[entry.index];
      info.kind = DXVK_ORG_INTEROP_RESOURCE_BUFFER;
      info.buffer.buffer = slice.buffer;
      info.buffer.offset = slice.offset;
      info.buffer.size = entry.buffer->Desc()->ByteWidth;
      info.buffer.address = slice.gpuAddress;
      info.buffer.usage = entry.dxvkBuffer->info().usage;
      pResults[entry.index] = S_OK;
    }

    return S_OK;
  }


  bool D3D11VkInterop::LockImage(const Rc<DxvkImage>& image) {
    // The client imports an image in the layout it rests in between commands (FillImageInfo), and expects GENERAL: without
    // VK_KHR_unified_image_layouts (AMD's legacy Windows driver) an image rests in its usage's optimal layout, so it is moved to
    // GENERAL, which becomes its resting layout (ensureImageCompatibility: a barrier, no new storage).
    const VkImageLayout layout = image->hasUnifiedLayout() || image->info().layout == VK_IMAGE_LAYOUT_GENERAL
      ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_GENERAL;

    // Same condition as D3D11DeviceExt::LockImage: nothing to do for an image that cannot move.
    if (!image->canRelocate() && (image->info().usage & VK_IMAGE_USAGE_SAMPLED_BIT) && layout == VK_IMAGE_LAYOUT_UNDEFINED) {
      image->markOrgInteropImage();
      return true;
    }

    const bool locked = m_device->LockImage(image, VK_IMAGE_USAGE_SAMPLED_BIT, layout);
    if (locked)
      image->markOrgInteropImage();
    return locked;
  }


  Rc<DxvkPagedResource> D3D11VkInterop::ResolveResource(IUnknown* pObject, DxvkImageViewKey* pKey, D3D11Buffer** ppBuffer) {
    if (ppBuffer)
      *ppBuffer = nullptr;

    if (!pObject)
      return nullptr;

    // A shader resource view selects the image view; otherwise the resource itself.
    Com<ID3D11ShaderResourceView> srv;

    if (SUCCEEDED(pObject->QueryInterface(__uuidof(ID3D11ShaderResourceView), reinterpret_cast<void**>(&srv)))) {
      Rc<DxvkImageView> view = static_cast<D3D11ShaderResourceView*>(srv.ptr())->GetImageView();

      if (view == nullptr)
        return nullptr;  // buffer view

      if (pKey)
        *pKey = view->info();

      return view->image();
    }

    Com<ID3D11Resource> resource;
    D3D11_COMMON_RESOURCE_DESC desc = { };

    if (FAILED(pObject->QueryInterface(__uuidof(ID3D11Resource), reinterpret_cast<void**>(&resource)))
     || FAILED(GetCommonResourceDesc(resource.ptr(), &desc)))
      return nullptr;

    if (desc.Dim == D3D11_RESOURCE_DIMENSION_BUFFER) {
      auto buffer = GetCommonBuffer(resource.ptr());

      if (ppBuffer)
        *ppBuffer = buffer;

      return buffer->GetBuffer();
    }

    auto texture = GetCommonTexture(resource.ptr());

    if (!texture)
      return nullptr;

    Rc<DxvkImage> image = texture->GetImage();

    if (pKey)
      *pKey = WholeImageViewKey(*image);

    return image;
  }


  HRESULT D3D11VkInterop::TrackResources(
          UINT                            Count,
          IUnknown* const*                ppObjects,
          DxvkOrgInteropTrackedResource*  pResources,
          HRESULT*                        pResults) {
    if (Count && (!ppObjects || !pResources || !pResults))
      return E_INVALIDARG;

    struct Item {
      UINT index = 0u;
      Rc<DxvkBuffer> buffer;
      VkDeviceSize byteWidth = 0u;
      Rc<DxvkImage> image;
      DxvkImageViewKey key = { };
    };

    std::vector<Item> items;
    items.reserve(Count);

    for (UINT i = 0; i < Count; i++) {
      pResults[i] = E_INVALIDARG;

      auto& out = pResources[i];

      if (!ppObjects[i] || out.version != DXVK_ORG_TRACKED_RESOURCE_VERSION || out.info.version != DXVK_ORG_INTEROP_VERSION)
        continue;

      out = DxvkOrgInteropTrackedResource();
      out.version = DXVK_ORG_TRACKED_RESOURCE_VERSION;
      out.info.version = DXVK_ORG_INTEROP_VERSION;

      Item item;
      item.index = i;

      D3D11Buffer* buffer = nullptr;
      Rc<DxvkPagedResource> resource = ResolveResource(ppObjects[i], &item.key, &buffer);

      if (resource == nullptr)
        continue;

      if (buffer) {
        // Discard maps rename the buffer: it cannot be imported at all.
        if (buffer->Desc()->Usage == D3D11_USAGE_DYNAMIC || buffer->Desc()->CPUAccessFlags)
          continue;

        item.buffer = buffer->GetBuffer();
        item.byteWidth = buffer->Desc()->ByteWidth;
      } else {
        item.image = static_cast<DxvkImage*>(resource.ptr());
      }

      items.push_back(std::move(item));
    }

    if (items.empty())
      return S_OK;

    // On the CS thread, in order with every relocation: the description and the import's count
    // name the same storage, and a later move of it is reported.
    auto chunk = m_device->AllocCsChunk(DxvkCsChunkFlag::SingleUse);
    auto allocator = &m_device->GetDXVKDevice()->getMemoryAllocator();

    chunk->push([cItems = std::move(items), pResources, pResults, allocator] (DxvkContext* ctx) {
      for (const auto& item : cItems) {
        auto& out = pResources[item.index];

        if (item.buffer != nullptr) {
          // One the GPU writes stays pinned: after a move, the client would read the old storage's stale contents.
          constexpr VkBufferUsageFlags writable = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT
            | VK_BUFFER_USAGE_TRANSFORM_FEEDBACK_BUFFER_BIT_EXT | VK_BUFFER_USAGE_TRANSFORM_FEEDBACK_COUNTER_BUFFER_BIT_EXT;

          if (item.buffer->info().usage & writable) {
            ctx->ensureBufferAddress(item.buffer);
            out.flags |= DXVK_ORG_TRACKED_PINNED;
          }

          item.buffer->markOrgInterop();
          item.buffer->addOrgTrack();

          auto slice = item.buffer->getSliceInfo();
          out.info.kind = DXVK_ORG_INTEROP_RESOURCE_BUFFER;
          out.info.buffer.buffer = slice.buffer;
          out.info.buffer.offset = slice.offset;
          out.info.buffer.size = item.byteWidth;
          out.info.buffer.address = slice.gpuAddress;
          out.info.buffer.usage = item.buffer->info().usage;
          out.cookie = item.buffer->cookie();
          out.storageSerial = item.buffer->orgStorageSerial();
        } else {
          // As LockImage: the client samples the image in GENERAL, which becomes its resting
          // layout without VK_KHR_unified_image_layouts. No stable address: it may move.
          DxvkImageUsageInfo usageInfo;
          usageInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
          usageInfo.layout = item.image->hasUnifiedLayout() || item.image->info().layout == VK_IMAGE_LAYOUT_GENERAL
            ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_GENERAL;

          // One the GPU writes stays pinned, as for buffers.
          constexpr VkImageUsageFlags writable = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
            | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_STORAGE_BIT;

          if (item.image->info().usage & writable) {
            usageInfo.stableGpuAddress = VK_TRUE;
            out.flags |= DXVK_ORG_TRACKED_PINNED;
          }

          if (!ctx->ensureImageCompatibility(item.image, usageInfo)) {
            pResults[item.index] = E_FAIL;
            continue;
          }

          item.image->markOrgInterop();
          item.image->addOrgTrack();

          FillImageInfo(item.image, item.key, out.info.image);
          out.info.kind = DXVK_ORG_INTEROP_RESOURCE_IMAGE;
          out.cookie = item.image->cookie();
          out.storageSerial = item.image->orgStorageSerial();
        }

        allocator->noteOrgTrack(1);
        pResults[item.index] = S_OK;
      }
    });

    m_device->GetContext()->InjectCsChunk(DxvkCsQueue::HighPriority, std::move(chunk), true);
    return S_OK;
  }


  HRESULT D3D11VkInterop::UntrackResources(
          UINT                        Count,
          IUnknown* const*            ppObjects) {
    if (Count && !ppObjects)
      return E_INVALIDARG;

    auto& allocator = m_device->GetDXVKDevice()->getMemoryAllocator();

    for (UINT i = 0; i < Count; i++) {
      Rc<DxvkPagedResource> resource = ResolveResource(ppObjects[i], nullptr, nullptr);

      if (resource != nullptr && resource->removeOrgTrack())
        allocator.noteOrgTrack(-1);
    }

    return S_OK;
  }


  HRESULT D3D11VkInterop::PollRelocations(
          DxvkOrgInteropRelocation*   pRelocations,
          uint32_t                    Capacity,
          uint32_t*                   pCount) {
    if (!pCount || (Capacity && !pRelocations))
      return E_INVALIDARG;

    *pCount = 0u;

    if (!Capacity)
      return S_OK;

    std::vector<DxvkOrgRelocation> entries(Capacity);
    size_t count = m_device->GetDXVKDevice()->getMemoryAllocator().pollOrgRelocations(entries.data(), Capacity);

    for (size_t i = 0u; i < count; i++) {
      auto& dst = pRelocations[i];
      dst.cookie = entries[i].cookie;
      dst.storageSerial = entries[i].serial;
      dst.kind = entries[i].image ? DXVK_ORG_INTEROP_RESOURCE_IMAGE : DXVK_ORG_INTEROP_RESOURCE_BUFFER;
      dst.reserved = 0u;
      // The client's reference, given back by ReleaseStorage.
      dst.storage = entries[i].storage.unsafeExtract();
    }

    *pCount = uint32_t(count);
    return S_OK;
  }


  HRESULT D3D11VkInterop::ReleaseStorage(
          UINT                        Count,
          void* const*                ppStorages) {
    if (Count && !ppStorages)
      return E_INVALIDARG;

    auto& allocator = m_device->GetDXVKDevice()->getMemoryAllocator();

    for (UINT i = 0; i < Count; i++)
      allocator.releaseOrgStorage(static_cast<DxvkResourceAllocation*>(ppStorages[i]));

    return S_OK;
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


  // The last completed resource snapshot (RequestMemorySnapshot); the CS thread replaces it.
  struct D3D11MemorySnapshot {
    dxvk::mutex                           mutex;
    std::vector<DxvkMemoryResourceReport> rows;
    uint64_t                              serial = 0u;
    std::atomic<bool>                     pending = { false };
  };


  std::shared_ptr<D3D11MemorySnapshot> D3D11VkInterop::GetMemorySnapshot() {
    std::lock_guard lock(m_memorySnapshotMutex);

    if (!m_memorySnapshot)
      m_memorySnapshot = std::make_shared<D3D11MemorySnapshot>();

    return m_memorySnapshot;
  }


  HRESULT D3D11VkInterop::RequestMemorySnapshot() {
    auto snapshot = GetMemorySnapshot();

    // One in flight at a time: a request while one is pending is already served by it.
    if (snapshot->pending.exchange(true, std::memory_order_acq_rel))
      return S_FALSE;

    // On the CS thread, the only one that replaces a published resource's storage.
    m_device->GetContext()->InjectCs(DxvkCsQueue::HighPriority, [
      cDevice   = m_device->GetDXVKDevice(),
      cSnapshot = std::move(snapshot)
    ] (DxvkContext* ctx) {
      std::vector<DxvkMemoryResourceReport> rows;
      cDevice->getMemoryAllocator().enumerateResources(rows);

      { std::lock_guard lock(cSnapshot->mutex);
        cSnapshot->rows = std::move(rows);
        cSnapshot->serial += 1u;
      }

      cSnapshot->pending.store(false, std::memory_order_release);
    });

    return S_OK;
  }


  HRESULT D3D11VkInterop::ReadMemorySnapshot(
          DxvkMemoryResourceRow*      pRows,
          uint32_t                    capacity,
          uint32_t*                   pCount,
          uint64_t*                   pSerial) {
    if (!pCount || (capacity && !pRows))
      return E_INVALIDARG;

    auto snapshot = GetMemorySnapshot();
    std::lock_guard lock(snapshot->mutex);

    uint32_t total = uint32_t(snapshot->rows.size());
    uint32_t count = std::min(total, capacity);

    for (uint32_t i = 0u; i < count; i++) {
      const auto& src = snapshot->rows[i];
      auto& dst = pRows[i];
      dst = { };
      dst.cookie = src.cookie;
      dst.parentCookie = src.label.parentCookie;
      dst.size = src.size;
      dst.memory = uint64_t(src.memory);
      dst.offset = src.offset;
      dst.flags = src.flags;
      dst.isImage = src.image;
      dst.isSparse = src.sparse;
      dst.memoryType = src.memoryType;
      dst.heapIndex = src.heap;
      dst.format = uint32_t(src.format);
      dst.width = src.extent.width;
      dst.height = src.extent.height;
      dst.depth = src.extent.depth;
      dst.mipLevels = src.mipLevels;
      dst.layers = src.layers;
      dst.usage = src.usage;
      dst.owner = src.label.owner;
      dst.kind = src.label.kind;
      dst.origin = src.label.origin;
      std::memcpy(dst.name, src.label.name.data(), std::min(src.label.name.size(), sizeof(dst.name) - 1u));
    }

    *pCount = total;

    if (pSerial)
      *pSerial = snapshot->serial;

    return count < total ? S_FALSE : S_OK;
  }


  HRESULT D3D11VkInterop::SetResourceOwner(
          IUnknown*                   pObject,
    const char*                       pOwner,
          BOOL                        overwrite) {
    Rc<DxvkPagedResource> resource = ResolveResource(pObject, nullptr, nullptr);

    if (resource == nullptr)
      return E_INVALIDARG;

    m_device->GetDXVKDevice()->getMemoryAllocator().setResourceOwner(resource->cookie(), pOwner, overwrite);
    return S_OK;
  }


  HRESULT D3D11VkInterop::GetMemoryHeapReports(
          DxvkMemoryHeapReport*       pReports,
          uint32_t                    capacity,
          uint32_t*                   pCount) {
    if (!pCount || (capacity && !pReports))
      return E_INVALIDARG;

    for (uint32_t i = 0u; i < capacity; i++) {
      if (pReports[i].version != DXVK_MEMORY_REPORT_VERSION)
        return E_INVALIDARG;
    }

    Rc<DxvkDevice> device = m_device->GetDXVKDevice();
    auto& allocator = device->getMemoryAllocator();

    VkPhysicalDeviceMemoryProperties properties = { };
    VkPhysicalDeviceMemoryBudgetPropertiesEXT budget = { };
    bool hasBudget = device->adapter()->getRawMemoryBudget(properties, budget);

    uint32_t count = std::min(properties.memoryHeapCount, capacity);

    for (uint32_t i = 0u; i < count; i++) {
      DxvkMemoryHeapTotals heap = allocator.getHeapTotals(i);

      auto& dst = pReports[i];
      dst.heapIndex = i;
      dst.heapFlags = properties.memoryHeaps[i].flags;
      dst.hasVkBudget = hasBudget;
      dst.heapSize = properties.memoryHeaps[i].size;
      dst.vkUsage = hasBudget ? budget.heapUsage[i] : 0u;
      dst.vkBudget = hasBudget ? budget.heapBudget[i] : 0u;
      dst.dxvkBudget = heap.budget;
      dst.dxvkAllocated = heap.allocated;
      dst.dxvkUsed = heap.used;
      dst.cacheBytes = heap.cacheBytes;
      dst.orgRetiredBytes = heap.orgRetiredBytes;
    }

    *pCount = properties.memoryHeapCount;
    return count < properties.memoryHeapCount ? S_FALSE : S_OK;
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


  HRESULT D3D11VkInterop::EmitExternalCommands(
    const DxvkOrgInteropExternalCommands* pCommands) {
    if (!pCommands || pCommands->version != DXVK_ORG_EXTERNAL_COMMANDS_VERSION
     || !pCommands->callback
     || (pCommands->waitCount && !pCommands->waits)
     || (pCommands->signalCount && !pCommands->signals))
      return E_INVALIDARG;

    std::vector<VkSemaphoreSubmitInfo> waits(pCommands->waits, pCommands->waits + pCommands->waitCount);
    std::vector<VkSemaphoreSubmitInfo> signals(pCommands->signals, pCommands->signals + pCommands->signalCount);

    for (auto* list : { &waits, &signals }) {
      for (auto& info : *list) {
        if (!info.stageMask)
          info.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
      }
    }

    m_device->GetContext()->EmitExternalCommands(
      [cb = pCommands->callback, user = pCommands->user] (VkCommandBuffer commandBuffer) {
        cb(user, commandBuffer);
      }, std::move(waits), std::move(signals),
      (pCommands->flags & DXVK_ORG_EXTERNAL_COMMANDS_BINDS_STATE) != 0u,
      (pCommands->flags & DXVK_ORG_EXTERNAL_COMMANDS_FULL_BARRIER) != 0u,
      pCommands->label ? std::string(pCommands->label) : std::string());
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


  HRESULT D3D11VkInterop::EnqueueExternalSubmissions(
    const DxvkOrgInteropSubmissionBatch*  pBatch) {
    if (!pBatch || pBatch->version != DXVK_ORG_INTEROP_VERSION
     || !pBatch->submitCount || !pBatch->submits)
      return E_INVALIDARG;

    DxvkExternalSubmitInfo submitInfo;
    submitInfo.submits.resize(pBatch->submitCount);

    for (uint32_t i = 0; i < pBatch->submitCount; i++) {
      const auto& source = pBatch->submits[i];

      if ((source.waitSemaphoreInfoCount && !source.pWaitSemaphoreInfos)
       || (source.commandBufferInfoCount && !source.pCommandBufferInfos)
       || (source.signalSemaphoreInfoCount && !source.pSignalSemaphoreInfos))
        return E_INVALIDARG;

      auto& submit = submitInfo.submits[i];
      submit.waits.assign(source.pWaitSemaphoreInfos, source.pWaitSemaphoreInfos + source.waitSemaphoreInfoCount);
      submit.commandBuffers.assign(source.pCommandBufferInfos, source.pCommandBufferInfos + source.commandBufferInfoCount);
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

    m_device->GetContext()->EnqueueExternalSubmission(std::move(submitInfo));
    return S_OK;
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


  DLLEXPORT HRESULT __stdcall dxvkSetBindingModelCallback(const DxvkOrgInteropBindingModelNegotiation* pNegotiation) {
    if (!pNegotiation || pNegotiation->version != DXVK_ORG_BINDING_MODEL_NEGOTIATION_VERSION)
      return E_INVALIDARG;

    DxvkInteropBindingModelNegotiation negotiation;
    negotiation.callback = pNegotiation->callback;
    negotiation.user = pNegotiation->callback ? pNegotiation->user : nullptr;

    setInteropBindingModelNegotiation(negotiation);
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
    const auto& compute = device->queues().compute;
    pInfo->computeQueue = compute.queueHandle;
    pInfo->computeQueueFamily = compute.queueFamily;
    pInfo->computeQueueIndex = compute.queueIndex;
    const auto& upload = device->queues().upload;
    pInfo->uploadQueue = upload.queueHandle;
    pInfo->uploadQueueFamily = upload.queueFamily;
    pInfo->uploadQueueIndex = upload.queueIndex;
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


  DLLEXPORT HRESULT __stdcall dxvkEmitExternalCommands(ID3D11Device* pDevice,
    const DxvkOrgInteropExternalCommands* pCommands) {
    Com<IDXGIVkInteropDevice1> ref;
    auto interop = GetInterop(pDevice, ref);

    if (!interop)
      return E_NOINTERFACE;

    return interop->EmitExternalCommands(pCommands);
  }


  DLLEXPORT HRESULT __stdcall dxvkSetCommandBufferBoundaryCallbacks(ID3D11Device* pDevice,
    PFN_dxvkOrgInteropCommandBufferCallback pOnEnd, PFN_dxvkOrgInteropCommandBufferCallback pOnBegin, void* pUser) {
    Com<IDXGIVkInteropDevice1> ref;
    auto interop = GetInterop(pDevice, ref);

    if (!interop)
      return E_NOINTERFACE;

    return interop->SetCommandBufferBoundaryCallbacks(pOnEnd, pOnBegin, pUser);
  }


  DLLEXPORT HRESULT __stdcall dxvkGetInteropResourceInfo(ID3D11Device* pDevice,
    IUnknown* pObject, DxvkOrgInteropResourceInfo* pInfo) {
    Com<IDXGIVkInteropDevice1> ref;
    auto interop = GetInterop(pDevice, ref);

    if (!interop)
      return E_NOINTERFACE;

    return interop->GetResourceInfo(pObject, pInfo);
  }


  DLLEXPORT HRESULT __stdcall dxvkGetInteropResourceInfos(ID3D11Device* pDevice,
    UINT count, IUnknown* const* pObjects, DxvkOrgInteropResourceInfo* pInfos, HRESULT* pResults) {
    Com<IDXGIVkInteropDevice1> ref;
    auto interop = GetInterop(pDevice, ref);

    if (!interop)
      return E_NOINTERFACE;

    return interop->GetResourceInfos(count, pObjects, pInfos, pResults);
  }


  DLLEXPORT HRESULT __stdcall dxvkTrackInteropResources(ID3D11Device* pDevice,
    UINT count, IUnknown* const* pObjects, DxvkOrgInteropTrackedResource* pResources, HRESULT* pResults) {
    Com<IDXGIVkInteropDevice1> ref;
    auto interop = GetInterop(pDevice, ref);

    if (!interop)
      return E_NOINTERFACE;

    return interop->TrackResources(count, pObjects, pResources, pResults);
  }


  DLLEXPORT HRESULT __stdcall dxvkUntrackInteropResources(ID3D11Device* pDevice,
    UINT count, IUnknown* const* pObjects) {
    Com<IDXGIVkInteropDevice1> ref;
    auto interop = GetInterop(pDevice, ref);

    if (!interop)
      return E_NOINTERFACE;

    return interop->UntrackResources(count, pObjects);
  }


  DLLEXPORT HRESULT __stdcall dxvkPollInteropRelocations(ID3D11Device* pDevice,
    DxvkOrgInteropRelocation* pRelocations, uint32_t capacity, uint32_t* pCount) {
    Com<IDXGIVkInteropDevice1> ref;
    auto interop = GetInterop(pDevice, ref);

    if (!interop)
      return E_NOINTERFACE;

    return interop->PollRelocations(pRelocations, capacity, pCount);
  }


  DLLEXPORT HRESULT __stdcall dxvkReleaseInteropStorage(ID3D11Device* pDevice,
    UINT count, void* const* pStorages) {
    Com<IDXGIVkInteropDevice1> ref;
    auto interop = GetInterop(pDevice, ref);

    if (!interop)
      return E_NOINTERFACE;

    return interop->ReleaseStorage(count, pStorages);
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


  DLLEXPORT BOOL __stdcall dxvkResolveCheckpointMarker(ID3D11Device* pDevice,
    const void* marker, char* text, UINT capacity) {
    Com<IDXGIVkInteropDevice1> ref;
    auto interop = GetInterop(pDevice, ref);
    if (!interop)
      return FALSE;
    auto* checkpoints = interop->GetDXVKDevice()->getCheckpointBuffer();
    return checkpoints && checkpoints->copyCheckpointText(reinterpret_cast<uintptr_t>(marker), text, capacity) ? TRUE : FALSE;
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


  DLLEXPORT void __stdcall dxvkPushResourceOwner(const char* owner) {
    pushResourceOwner(owner);
  }


  DLLEXPORT void __stdcall dxvkPopResourceOwner() {
    popResourceOwner();
  }


  DLLEXPORT HRESULT __stdcall dxvkRequestMemorySnapshot(ID3D11Device* pDevice) {
    Com<IDXGIVkInteropDevice1> ref;
    auto interop = GetInterop(pDevice, ref);

    if (!interop)
      return E_NOINTERFACE;

    return interop->RequestMemorySnapshot();
  }


  DLLEXPORT HRESULT __stdcall dxvkReadMemorySnapshot(ID3D11Device* pDevice,
    DxvkMemoryResourceRow* pRows, uint32_t capacity, uint32_t* pCount, uint64_t* pSerial) {
    Com<IDXGIVkInteropDevice1> ref;
    auto interop = GetInterop(pDevice, ref);

    if (!interop)
      return E_NOINTERFACE;

    return interop->ReadMemorySnapshot(pRows, capacity, pCount, pSerial);
  }


  DLLEXPORT HRESULT __stdcall dxvkSetResourceOwner(ID3D11Device* pDevice,
    IUnknown* pObject, const char* pOwner, BOOL overwrite) {
    Com<IDXGIVkInteropDevice1> ref;
    auto interop = GetInterop(pDevice, ref);

    if (!interop)
      return E_NOINTERFACE;

    return interop->SetResourceOwner(pObject, pOwner, overwrite);
  }


  DLLEXPORT void __stdcall dxvkConfigureMemoryDebug(uint32_t flags) {
    setMemoryDebugFlags(flags);
  }


  DLLEXPORT HRESULT __stdcall dxvkGetResourceCreationStack(ID3D11Device* pDevice,
    uint64_t cookie, void** pFrames, uint32_t capacity, uint32_t* pCount) {
    if (!pCount || (capacity && !pFrames))
      return E_INVALIDARG;
    *pCount = 0u;

    Com<IDXGIVkInteropDevice1> ref;
    auto interop = GetInterop(pDevice, ref);

    if (!interop)
      return E_NOINTERFACE;

    auto stack = interop->GetDXVKDevice()->getMemoryAllocator().getResourceStack(cookie);

    if (!stack)
      return S_FALSE;

    uint32_t count = std::min(capacity, uint32_t(stack->size()));

    for (uint32_t i = 0u; i < count; i++)
      pFrames[i] = const_cast<void*>((*stack)[i]);

    *pCount = uint32_t(stack->size());
    return S_OK;
  }


  DLLEXPORT HRESULT __stdcall dxvkGetMemoryHeapReports(ID3D11Device* pDevice,
    DxvkMemoryHeapReport* pReports, uint32_t capacity, uint32_t* pCount) {
    Com<IDXGIVkInteropDevice1> ref;
    auto interop = GetInterop(pDevice, ref);

    if (!interop)
      return E_NOINTERFACE;

    return interop->GetMemoryHeapReports(pReports, capacity, pCount);
  }

}
