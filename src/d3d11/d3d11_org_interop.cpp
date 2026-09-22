#include "d3d11_buffer.h"
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


  HRESULT D3D11VkInterop::EnqueueExternalSubmission(
    const DxvkOrgInteropSubmission*   pSubmission) {
    if (!pSubmission || pSubmission->version != DXVK_ORG_INTEROP_VERSION
     || (pSubmission->waitCount && !pSubmission->waits)
     || (pSubmission->commandBufferCount && !pSubmission->commandBuffers)
     || (pSubmission->signalCount && !pSubmission->signals))
      return E_INVALIDARG;

    DxvkExternalSubmitInfo submitInfo;
    submitInfo.waits.assign(pSubmission->waits, pSubmission->waits + pSubmission->waitCount);
    submitInfo.commandBuffers.assign(pSubmission->commandBuffers, pSubmission->commandBuffers + pSubmission->commandBufferCount);
    submitInfo.signals.assign(pSubmission->signals, pSubmission->signals + pSubmission->signalCount);

    if (pSubmission->label)
      submitInfo.label = pSubmission->label;

    for (auto& info : submitInfo.waits)
      info.pNext = nullptr;
    for (auto& info : submitInfo.commandBuffers)
      info.pNext = nullptr;
    for (auto& info : submitInfo.signals)
      info.pNext = nullptr;

    if (pSubmission->onSubmitted) {
      submitInfo.onSubmitted = [cb = pSubmission->onSubmitted, user = pSubmission->user] (VkResult result) {
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


  DLLEXPORT HRESULT __stdcall dxvkGetInteropResourceInfo(ID3D11Device* pDevice,
    IUnknown* pObject, DxvkOrgInteropResourceInfo* pInfo) {
    Com<IDXGIVkInteropDevice1> ref;
    auto interop = GetInterop(pDevice, ref);

    if (!interop)
      return E_NOINTERFACE;

    return interop->GetResourceInfo(pObject, pInfo);
  }


  DLLEXPORT HRESULT __stdcall dxvkSetDeviceTeardownCallback(PFN_dxvkOrgInteropTeardown pCallback, void* pUser) {
    setInteropTeardownCallback(pCallback, pUser);
    return S_OK;
  }

}
