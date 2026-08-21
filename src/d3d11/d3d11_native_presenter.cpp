#include "d3d11_native_presenter.h"

#include "d3d11_texture.h"
#include "../../include/cs_dxvk_api.h"

#include <array>
#include <limits>
#include <mutex>

#include <d3d12.h>
#include <dxgi1_6.h>

#include "../util/com/com_pointer.h"
#include "../util/log/log.h"
#include "../util/util_string.h"

namespace dxvk {

  namespace {

    using PFN_CreateDXGIFactory2 = HRESULT (WINAPI*) (UINT, REFIID, void**);
    using PFN_D3D12CreateDevice = HRESULT (WINAPI*) (IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);

    template<typename T>
    HRESULT QueryInterface(IUnknown* object, Com<T>& result) {
      return object->QueryInterface(__uuidof(T), reinterpret_cast<void**>(&result));
    }

    std::mutex g_nativeApiMutex;
    CsDxvkNativePresenterApi g_nativeApi = { };

    struct NativeFrameGenerationSources {
      Com<ID3D11Resource> depth;
      Com<ID3D11Resource> motionVectors;
      Com<ID3D11Resource> hudlessColor;
      uint32_t renderWidth = 0u;
      uint32_t renderHeight = 0u;
      uint32_t displayWidth = 0u;
      uint32_t displayHeight = 0u;
    };

    std::mutex g_nativeSourcesMutex;
    NativeFrameGenerationSources g_nativeSources;

    CsDxvkNativePresenterApi GetNativeApi() {
      std::lock_guard<std::mutex> lock(g_nativeApiMutex);
      return g_nativeApi;
    }

    DXGI_FORMAT GetDxgiFormat(VkFormat format) {
      switch (format) {
        case VK_FORMAT_R32_SFLOAT:                 return DXGI_FORMAT_R32_FLOAT;
        case VK_FORMAT_R16G16_SFLOAT:              return DXGI_FORMAT_R16G16_FLOAT;
        case VK_FORMAT_R16G16B16A16_SFLOAT:        return DXGI_FORMAT_R16G16B16A16_FLOAT;
        case VK_FORMAT_R8G8B8A8_UNORM:             return DXGI_FORMAT_R8G8B8A8_UNORM;
        case VK_FORMAT_B8G8R8A8_UNORM:             return DXGI_FORMAT_B8G8R8A8_UNORM;
        case VK_FORMAT_A2B10G10R10_UNORM_PACK32:   return DXGI_FORMAT_R10G10B10A2_UNORM;
        case VK_FORMAT_D32_SFLOAT:                 return DXGI_FORMAT_D32_FLOAT;
        case VK_FORMAT_D32_SFLOAT_S8_UINT:         return DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
        case VK_FORMAT_D24_UNORM_S8_UINT:           return DXGI_FORMAT_D24_UNORM_S8_UINT;
        default:                                   return DXGI_FORMAT_UNKNOWN;
      }
    }

    bool InvokeNativePresentBegin(PFN_csDxvkNativePresentBegin callback,
                                  const CsDxvkNativePresentInfo* info) noexcept {
      __try {
        callback(info);
        return true;
      } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
      }
    }

    bool InvokeNativePresentEnd(PFN_csDxvkNativePresentEnd callback,
                                int32_t result) noexcept {
      __try {
        callback(result);
        return true;
      } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
      }
    }

  }


  extern "C" void dxvkSetNativePresenterApi(const CsDxvkNativePresenterApi* api) {
    std::lock_guard<std::mutex> lock(g_nativeApiMutex);
    g_nativeApi = { };
    if (api && api->size >= sizeof(CsDxvkNativePresenterApi) &&
        api->version == CS_DXVK_API_VERSION)
      g_nativeApi = *api;
  }


  extern "C" void dxvkSetNativeFrameGenerationResources(
          void*     depth,
          void*     motionVectors,
          void*     hudlessColor,
          uint32_t  renderWidth,
          uint32_t  renderHeight,
          uint32_t  displayWidth,
          uint32_t  displayHeight) {
    std::lock_guard<std::mutex> lock(g_nativeSourcesMutex);
    g_nativeSources.depth = static_cast<ID3D11Resource*>(depth);
    g_nativeSources.motionVectors = static_cast<ID3D11Resource*>(motionVectors);
    g_nativeSources.hudlessColor = static_cast<ID3D11Resource*>(hudlessColor);
    g_nativeSources.renderWidth = renderWidth;
    g_nativeSources.renderHeight = renderHeight;
    g_nativeSources.displayWidth = displayWidth;
    g_nativeSources.displayHeight = displayHeight;
  }


  struct D3D11NativePresenter::Impl {
    struct FrameGenerationBridgeResource {
      Com<ID3D12Resource> resource;
      Rc<DxvkImage> image;
      VkFormat format = VK_FORMAT_UNDEFINED;
      VkExtent3D extent = { };
    };

    HMODULE dxgiModule = nullptr;
    HMODULE d3d12Module = nullptr;

    Com<IDXGIFactory4> factory;
    Com<IDXGIAdapter1> adapter;
    Com<ID3D12Device> device;
    Com<ID3D12CommandQueue> queue;
    Com<IDXGISwapChain4> swapchain;
    Com<ID3D12CommandAllocator> allocator;
    Com<ID3D12GraphicsCommandList> commandList;
    Com<ID3D12Fence> fence;

    HANDLE fenceEvent = nullptr;
    uint64_t fenceValue = 0u;

    HWND window = nullptr;
    uint32_t width = 0u;
    uint32_t height = 0u;
    uint32_t bufferCount = 0u;
    uint32_t imageIndex = 0u;
    bool loggedFirstPresent = false;

    std::vector<Com<ID3D12Resource>> sharedResources;
    std::vector<Rc<DxvkImage>> sharedImages;
    FrameGenerationBridgeResource fgDepth;
    FrameGenerationBridgeResource fgMotionVectors;
    FrameGenerationBridgeResource fgHudlessColor;
    Rc<DxvkImage> fgDepthSource;
    Rc<DxvkImage> fgMotionVectorsSource;
    Rc<DxvkImage> fgHudlessColorSource;
    uint32_t fgRenderWidth = 0u;
    uint32_t fgRenderHeight = 0u;
    uint32_t fgDisplayWidth = 0u;
    uint32_t fgDisplayHeight = 0u;
    uint64_t fgFrameId = 0u;
    bool loggedUnsupportedFgFormat = false;

    bool waitForGpu() {
      if (!queue || !fence || !fenceEvent)
        return true;

      const uint64_t value = ++fenceValue;
      if (FAILED(queue->Signal(fence.ptr(), value)))
        return false;

      if (fence->GetCompletedValue() < value) {
        if (FAILED(fence->SetEventOnCompletion(value, fenceEvent)))
          return false;
        if (WaitForSingleObject(fenceEvent, 10000u) != WAIT_OBJECT_0)
          return false;
      }

      return true;
    }

  };


  D3D11NativePresenter::D3D11NativePresenter(const Rc<DxvkDevice>& device)
  : m_device(device), m_impl(std::make_unique<Impl>()) {

  }


  D3D11NativePresenter::~D3D11NativePresenter() {
    reset();
  }


  bool D3D11NativePresenter::initialize(
          HWND      window,
          uint32_t  width,
          uint32_t  height,
          uint32_t  bufferCount) {
    reset();

    if (!window || !width || !height)
      return false;

    m_impl->dxgiModule = LoadLibraryExW(L"dxgi.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    m_impl->d3d12Module = LoadLibraryExW(L"d3d12.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!m_impl->dxgiModule || !m_impl->d3d12Module) {
      Logger::err("D3D11NativePresenter: Failed to load system DXGI/D3D12");
      reset();
      return false;
    }

    const CsDxvkNativePresenterApi nativeApi = GetNativeApi();
    const auto createFactory = reinterpret_cast<PFN_CreateDXGIFactory2>(
      nativeApi.createDXGIFactory2 ? nativeApi.createDXGIFactory2 :
      GetProcAddress(m_impl->dxgiModule, "CreateDXGIFactory2"));
    const auto createDevice = reinterpret_cast<PFN_D3D12CreateDevice>(
      nativeApi.d3d12CreateDevice ? nativeApi.d3d12CreateDevice :
      GetProcAddress(m_impl->d3d12Module, "D3D12CreateDevice"));
    if (!createFactory || !createDevice) {
      Logger::err("D3D11NativePresenter: Missing system DXGI/D3D12 exports");
      reset();
      return false;
    }

    HRESULT hr = createFactory(0u, __uuidof(IDXGIFactory4),
      reinterpret_cast<void**>(&m_impl->factory));
    if (FAILED(hr)) {
      Logger::err(str::format("D3D11NativePresenter: CreateDXGIFactory2 failed: ", hr));
      reset();
      return false;
    }
    const DxvkAdapterInfo adapterInfo = m_device->adapter()->info();
    if (!adapterInfo.luidIsValid) {
      Logger::err("D3D11NativePresenter: Vulkan adapter has no Windows LUID");
      reset();
      return false;
    }

    LUID luid = { };
    std::memcpy(&luid, adapterInfo.deviceLuid, sizeof(luid));
    hr = m_impl->factory->EnumAdapterByLuid(luid, __uuidof(IDXGIAdapter1),
      reinterpret_cast<void**>(&m_impl->adapter));
    if (FAILED(hr)) {
      Logger::err(str::format("D3D11NativePresenter: EnumAdapterByLuid failed: ", hr));
      reset();
      return false;
    }

    hr = createDevice(m_impl->adapter.ptr(), D3D_FEATURE_LEVEL_11_0,
      __uuidof(ID3D12Device), reinterpret_cast<void**>(&m_impl->device));
    if (FAILED(hr)) {
      Logger::err(str::format("D3D11NativePresenter: D3D12CreateDevice failed: ", hr));
      reset();
      return false;
    }
    if (nativeApi.upgradeObject)
      nativeApi.upgradeObject(CS_DXVK_NATIVE_OBJECT_DEVICE,
        reinterpret_cast<void**>(m_impl->device.operator&()));

    D3D12_COMMAND_QUEUE_DESC queueDesc = { };
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    hr = m_impl->device->CreateCommandQueue(&queueDesc, __uuidof(ID3D12CommandQueue),
      reinterpret_cast<void**>(&m_impl->queue));
    if (FAILED(hr)) {
      Logger::err(str::format("D3D11NativePresenter: CreateCommandQueue failed: ", hr));
      reset();
      return false;
    }

    DXGI_SWAP_CHAIN_DESC1 swapchainDesc = { };
    swapchainDesc.Width = width;
    swapchainDesc.Height = height;
    swapchainDesc.Format = DXGI_FORMAT_R10G10B10A2_UNORM;
    swapchainDesc.SampleDesc.Count = 1u;
    swapchainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapchainDesc.BufferCount = std::clamp(bufferCount, 2u, 4u);
    swapchainDesc.Scaling = DXGI_SCALING_STRETCH;
    swapchainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    swapchainDesc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    // Streamline's D3D12 present path adds DXGI_PRESENT_ALLOW_TEARING when
    // presenting with a zero sync interval. DXGI requires the corresponding
    // creation flag or rejects Present with DXGI_ERROR_INVALID_CALL.
    swapchainDesc.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;

    // Adapter selection must use the native factory. Calling any method on an
    // upgraded factory before slSetD3DDevice causes Streamline to initialize
    // its plugins without the intended device. Upgrade only at the first API
    // that actually requires interception: swapchain creation.
    if (nativeApi.upgradeObject)
      nativeApi.upgradeObject(CS_DXVK_NATIVE_OBJECT_FACTORY,
        reinterpret_cast<void**>(m_impl->factory.operator&()));

    Com<IDXGISwapChain1> baseSwapchain;
    hr = m_impl->factory->CreateSwapChainForHwnd(m_impl->queue.ptr(), window,
      &swapchainDesc, nullptr, nullptr, &baseSwapchain);
    if (FAILED(hr) || FAILED(QueryInterface(baseSwapchain.ptr(), m_impl->swapchain))) {
      Logger::err(str::format("D3D11NativePresenter: CreateSwapChainForHwnd failed: ", hr));
      reset();
      return false;
    }

    m_impl->factory->MakeWindowAssociation(window, DXGI_MWA_NO_ALT_ENTER);

    RECT windowRect = { };
    GetWindowRect(window, &windowRect);
    Logger::info(str::format("D3D11NativePresenter: Window bounds=",
      windowRect.left, ",", windowRect.top, "-", windowRect.right, ",", windowRect.bottom,
      ", factoryCurrent=", m_impl->factory->IsCurrent() ? "true" : "false"));

    Com<IDXGIOutput> bestOutput;
    int64_t bestIntersection = -1;
    for (uint32_t i = 0u; ; i++) {
      Com<IDXGIOutput> output;
      if (m_impl->adapter->EnumOutputs(i, &output) == DXGI_ERROR_NOT_FOUND)
        break;

      DXGI_OUTPUT_DESC outputDesc = { };
      if (FAILED(output->GetDesc(&outputDesc)))
        continue;

      const RECT& r = outputDesc.DesktopCoordinates;
      const LONG intersectWidth = std::max<LONG>(0,
        std::min(windowRect.right, r.right) - std::max(windowRect.left, r.left));
      const LONG intersectHeight = std::max<LONG>(0,
        std::min(windowRect.bottom, r.bottom) - std::max(windowRect.top, r.top));
      const int64_t intersection = int64_t(intersectWidth) * int64_t(intersectHeight);
      Logger::info(str::format("D3D11NativePresenter: Output ", i, " '",
        str::fromws(outputDesc.DeviceName), "' bounds=", r.left, ",", r.top, "-",
        r.right, ",", r.bottom, ", intersection=", intersection));
      if (intersection > bestIntersection) {
        bestIntersection = intersection;
        bestOutput = output;
      }
    }

    if (bestOutput) {
      Com<IDXGIOutput6> output6;
      DXGI_OUTPUT_DESC1 outputDesc = { };
      if (SUCCEEDED(QueryInterface(bestOutput.ptr(), output6)) &&
          SUCCEEDED(output6->GetDesc1(&outputDesc))) {
        Logger::info(str::format("D3D11NativePresenter: Active output '",
          str::fromws(outputDesc.DeviceName), "' colorSpace=", uint32_t(outputDesc.ColorSpace),
          ", bitsPerColor=", outputDesc.BitsPerColor, ", luminance=", outputDesc.MinLuminance,
          "/", outputDesc.MaxLuminance, "/", outputDesc.MaxFullFrameLuminance));
      }
    }

    UINT colorSpaceSupport = 0u;
    const DXGI_COLOR_SPACE_TYPE hdr10 = DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
    hr = m_impl->swapchain->CheckColorSpaceSupport(hdr10, &colorSpaceSupport);
    if (FAILED(hr) || !(colorSpaceSupport & DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT) ||
        FAILED(m_impl->swapchain->SetColorSpace1(hdr10))) {
      Logger::err("D3D11NativePresenter: Native DXGI HDR10 color space is unavailable");
      reset();
      return false;
    }

    hr = m_impl->device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
      __uuidof(ID3D12CommandAllocator), reinterpret_cast<void**>(&m_impl->allocator));
    if (FAILED(hr)) {
      Logger::err(str::format("D3D11NativePresenter: CreateCommandAllocator failed: ", hr));
      reset();
      return false;
    }

    hr = m_impl->device->CreateCommandList(0u, D3D12_COMMAND_LIST_TYPE_DIRECT,
      m_impl->allocator.ptr(), nullptr, __uuidof(ID3D12GraphicsCommandList),
      reinterpret_cast<void**>(&m_impl->commandList));
    if (FAILED(hr) || FAILED(m_impl->commandList->Close())) {
      Logger::err(str::format("D3D11NativePresenter: CreateCommandList failed: ", hr));
      reset();
      return false;
    }

    hr = m_impl->device->CreateFence(0u, D3D12_FENCE_FLAG_NONE,
      __uuidof(ID3D12Fence), reinterpret_cast<void**>(&m_impl->fence));
    m_impl->fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (FAILED(hr) || !m_impl->fenceEvent) {
      Logger::err(str::format("D3D11NativePresenter: CreateFence failed: ", hr));
      reset();
      return false;
    }

    D3D12_HEAP_PROPERTIES heapProperties = { };
    heapProperties.Type = D3D12_HEAP_TYPE_DEFAULT;
    heapProperties.CreationNodeMask = 1u;
    heapProperties.VisibleNodeMask = 1u;

    D3D12_RESOURCE_DESC resourceDesc = { };
    resourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    resourceDesc.Width = width;
    resourceDesc.Height = height;
    resourceDesc.DepthOrArraySize = 1u;
    resourceDesc.MipLevels = 1u;
    resourceDesc.Format = DXGI_FORMAT_R10G10B10A2_UNORM;
    resourceDesc.SampleDesc.Count = 1u;
    resourceDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    resourceDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;

    m_impl->sharedResources.resize(swapchainDesc.BufferCount);
    m_impl->sharedImages.reserve(swapchainDesc.BufferCount);

    for (uint32_t i = 0u; i < swapchainDesc.BufferCount; i++) {
      hr = m_impl->device->CreateCommittedResource(&heapProperties, D3D12_HEAP_FLAG_SHARED,
        &resourceDesc, D3D12_RESOURCE_STATE_COMMON, nullptr, __uuidof(ID3D12Resource),
        reinterpret_cast<void**>(&m_impl->sharedResources[i]));
      if (FAILED(hr)) {
        Logger::err(str::format("D3D11NativePresenter: CreateCommittedResource failed: ", hr));
        reset();
        return false;
      }

      HANDLE sharedHandle = nullptr;
      hr = m_impl->device->CreateSharedHandle(m_impl->sharedResources[i].ptr(), nullptr,
        GENERIC_ALL, nullptr, &sharedHandle);
      if (FAILED(hr) || !sharedHandle) {
        Logger::err(str::format("D3D11NativePresenter: CreateSharedHandle failed: ", hr));
        reset();
        return false;
      }

      try {
        DxvkImageCreateInfo imageInfo = { };
        imageInfo.type = VK_IMAGE_TYPE_2D;
        imageInfo.format = VK_FORMAT_A2B10G10R10_UNORM_PACK32;
        imageInfo.sampleCount = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.extent = { width, height, 1u };
        imageInfo.numLayers = 1u;
        imageInfo.mipLevels = 1u;
        imageInfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
          VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        imageInfo.stages = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
          VK_PIPELINE_STAGE_TRANSFER_BIT;
        imageInfo.access = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
          VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT |
          VK_ACCESS_TRANSFER_WRITE_BIT;
        imageInfo.layout = VK_IMAGE_LAYOUT_GENERAL;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_GENERAL;
        imageInfo.colorSpace = VK_COLOR_SPACE_HDR10_ST2084_EXT;
        imageInfo.shared = VK_TRUE;
        imageInfo.sharing.mode = DxvkSharedHandleMode::Import;
        imageInfo.sharing.type = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT;
        imageInfo.sharing.handle = sharedHandle;
        imageInfo.debugName = "Native DXGI HDR bridge image";

        m_impl->sharedImages.push_back(m_device->createImage(
          imageInfo, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT));
      } catch (const DxvkError& e) {
        CloseHandle(sharedHandle);
        Logger::err(str::format("D3D11NativePresenter: Vulkan import failed: ", e.message()));
        reset();
        return false;
      }

      CloseHandle(sharedHandle);
    }

    m_impl->window = window;
    m_impl->width = width;
    m_impl->height = height;
    m_impl->bufferCount = swapchainDesc.BufferCount;
    m_impl->imageIndex = 0u;
    m_impl->loggedFirstPresent = false;

    Logger::info(str::format("D3D11NativePresenter: Native HDR10 probe ready (",
      width, "x", height, ", buffers=", swapchainDesc.BufferCount, ")"));
    return true;
  }


  bool D3D11NativePresenter::resize(
          uint32_t width,
          uint32_t height,
          uint32_t bufferCount) {
    if (!m_impl->window)
      return false;
    const HWND window = m_impl->window;
    return initialize(window, width, height, bufferCount);
  }


  void D3D11NativePresenter::reset() {
    if (!m_impl)
      return;

    m_impl->waitForGpu();
    m_device->waitForIdle();

    m_impl->sharedImages.clear();
    m_impl->sharedResources.clear();
    m_impl->fgDepth = { };
    m_impl->fgMotionVectors = { };
    m_impl->fgHudlessColor = { };
    m_impl->fgDepthSource = nullptr;
    m_impl->fgMotionVectorsSource = nullptr;
    m_impl->fgHudlessColorSource = nullptr;
    m_impl->commandList = nullptr;
    m_impl->allocator = nullptr;
    m_impl->swapchain = nullptr;
    m_impl->queue = nullptr;
    m_impl->fence = nullptr;
    m_impl->device = nullptr;
    m_impl->adapter = nullptr;
    m_impl->factory = nullptr;

    if (m_impl->fenceEvent) {
      CloseHandle(m_impl->fenceEvent);
      m_impl->fenceEvent = nullptr;
    }
    if (m_impl->d3d12Module) {
      FreeLibrary(m_impl->d3d12Module);
      m_impl->d3d12Module = nullptr;
    }
    if (m_impl->dxgiModule) {
      FreeLibrary(m_impl->dxgiModule);
      m_impl->dxgiModule = nullptr;
    }

    m_impl->window = nullptr;
    m_impl->width = 0u;
    m_impl->height = 0u;
    m_impl->bufferCount = 0u;
    m_impl->imageIndex = 0u;
    m_impl->loggedFirstPresent = false;
  }


  bool D3D11NativePresenter::ready() const {
    return m_impl->swapchain && !m_impl->sharedImages.empty();
  }


  Rc<DxvkImage> D3D11NativePresenter::acquireImage() {
    if (!ready())
      return nullptr;
    return m_impl->sharedImages[m_impl->imageIndex % m_impl->sharedImages.size()];
  }


  void D3D11NativePresenter::updateFrameGenerationResources() {
    if (!ready())
      return;

    NativeFrameGenerationSources frame;
    {
      std::lock_guard<std::mutex> lock(g_nativeSourcesMutex);
      frame = g_nativeSources;
    }

    const auto getImage = [] (ID3D11Resource* resource) -> Rc<DxvkImage> {
      if (!resource)
        return nullptr;
      return GetCommonTexture(resource)->GetImage();
    };

    const Rc<DxvkImage> sources[] = {
      getImage(frame.depth.ptr()), getImage(frame.motionVectors.ptr()), getImage(frame.hudlessColor.ptr())
    };
    Impl::FrameGenerationBridgeResource* bridges[] = {
      &m_impl->fgDepth, &m_impl->fgMotionVectors, &m_impl->fgHudlessColor
    };

    const auto ensureBridge = [&] (const Rc<DxvkImage>& source,
                                   Impl::FrameGenerationBridgeResource& bridge) {
      if (!source) {
        bridge = { };
        return true;
      }

      const auto& sourceInfo = source->info();
      if (bridge.resource && bridge.format == sourceInfo.format &&
          bridge.extent == sourceInfo.extent)
        return true;

      bridge = { };
      const DXGI_FORMAT format = GetDxgiFormat(sourceInfo.format);
      if (format == DXGI_FORMAT_UNKNOWN) {
        if (!m_impl->loggedUnsupportedFgFormat) {
          m_impl->loggedUnsupportedFgFormat = true;
          Logger::err(str::format("D3D11NativePresenter: Unsupported frame-generation VkFormat ",
            uint32_t(sourceInfo.format)));
        }
        return false;
      }

      D3D12_HEAP_PROPERTIES heapProperties = { };
      heapProperties.Type = D3D12_HEAP_TYPE_DEFAULT;
      heapProperties.CreationNodeMask = 1u;
      heapProperties.VisibleNodeMask = 1u;

      D3D12_RESOURCE_DESC desc = { };
      desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
      desc.Width = sourceInfo.extent.width;
      desc.Height = sourceInfo.extent.height;
      desc.DepthOrArraySize = 1u;
      desc.MipLevels = 1u;
      desc.Format = format;
      desc.SampleDesc.Count = 1u;
      desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
      desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;

      HRESULT hr = m_impl->device->CreateCommittedResource(&heapProperties, D3D12_HEAP_FLAG_SHARED,
        &desc, D3D12_RESOURCE_STATE_COMMON, nullptr, __uuidof(ID3D12Resource),
        reinterpret_cast<void**>(&bridge.resource));
      if (FAILED(hr)) {
        Logger::err(str::format("D3D11NativePresenter: Frame-generation bridge creation failed: ", hr));
        return false;
      }

      HANDLE sharedHandle = nullptr;
      hr = m_impl->device->CreateSharedHandle(bridge.resource.ptr(), nullptr,
        GENERIC_ALL, nullptr, &sharedHandle);
      if (FAILED(hr) || !sharedHandle) {
        bridge = { };
        return false;
      }

      try {
        DxvkImageCreateInfo imageInfo = sourceInfo;
        imageInfo.usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        imageInfo.stages |= VK_PIPELINE_STAGE_TRANSFER_BIT;
        imageInfo.access |= VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
        imageInfo.layout = VK_IMAGE_LAYOUT_GENERAL;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_GENERAL;
        imageInfo.shared = VK_TRUE;
        imageInfo.sharing.mode = DxvkSharedHandleMode::Import;
        imageInfo.sharing.type = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT;
        imageInfo.sharing.handle = sharedHandle;
        imageInfo.debugName = "Native DXGI frame-generation bridge image";
        bridge.image = m_device->createImage(imageInfo, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
      } catch (const DxvkError& e) {
        Logger::err(str::format("D3D11NativePresenter: Frame-generation Vulkan import failed: ", e.message()));
      }
      CloseHandle(sharedHandle);

      if (!bridge.image) {
        bridge = { };
        return false;
      }
      bridge.format = sourceInfo.format;
      bridge.extent = sourceInfo.extent;
      return true;
    };

    bool valid = true;
    for (uint32_t i = 0u; i < 3u; i++)
      valid &= ensureBridge(sources[i], *bridges[i]);

    m_impl->fgDepthSource = valid ? sources[0] : nullptr;
    m_impl->fgMotionVectorsSource = valid ? sources[1] : nullptr;
    m_impl->fgHudlessColorSource = valid ? sources[2] : nullptr;
    m_impl->fgRenderWidth = frame.renderWidth;
    m_impl->fgRenderHeight = frame.renderHeight;
    m_impl->fgDisplayWidth = frame.displayWidth;
    m_impl->fgDisplayHeight = frame.displayHeight;
  }


  void D3D11NativePresenter::recordFrameGenerationCopies(DxvkContext* context) {
    if (!context)
      return;

    const auto copy = [&] (const Rc<DxvkImage>& source,
                           const Impl::FrameGenerationBridgeResource& bridge) {
      if (!source || !bridge.image)
        return;
      VkImageSubresourceLayers layers = { };
      layers.aspectMask = source->formatInfo()->aspectMask;
      layers.layerCount = 1u;
      context->copyImage(bridge.image, layers, { }, source, layers, { }, source->info().extent);
    };
    copy(m_impl->fgDepthSource, m_impl->fgDepth);
    copy(m_impl->fgMotionVectorsSource, m_impl->fgMotionVectors);
    copy(m_impl->fgHudlessColorSource, m_impl->fgHudlessColor);
  }


  HRESULT D3D11NativePresenter::present(uint32_t syncInterval) {
    if (!ready())
      return E_FAIL;

    if (FAILED(m_impl->allocator->Reset()) ||
        FAILED(m_impl->commandList->Reset(m_impl->allocator.ptr(), nullptr)))
      return E_FAIL;

    const uint32_t bridgeIndex = m_impl->imageIndex % m_impl->sharedResources.size();
    Com<ID3D12Resource> backbuffer;
    HRESULT hr = m_impl->swapchain->GetBuffer(m_impl->swapchain->GetCurrentBackBufferIndex(),
      __uuidof(ID3D12Resource), reinterpret_cast<void**>(&backbuffer));
    if (FAILED(hr))
      return hr;

    std::array<D3D12_RESOURCE_BARRIER, 4> barriers = { };
    barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barriers[0].Transition.pResource = m_impl->sharedResources[bridgeIndex].ptr();
    barriers[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barriers[1].Transition.pResource = backbuffer.ptr();
    barriers[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    barriers[2] = barriers[0];
    barriers[2].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    barriers[2].Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
    barriers[3] = barriers[1];
    barriers[3].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barriers[3].Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;

    m_impl->commandList->ResourceBarrier(2u, barriers.data());
    m_impl->commandList->CopyResource(backbuffer.ptr(), m_impl->sharedResources[bridgeIndex].ptr());

    m_impl->commandList->ResourceBarrier(2u, barriers.data() + 2u);

    const CsDxvkNativePresenterApi nativeApi = GetNativeApi();
    if (nativeApi.presentBegin) {
      CsDxvkNativePresentInfo info = { };
      info.size = sizeof(info);
      info.version = CS_DXVK_API_VERSION;
      info.commandList = m_impl->commandList.ptr();
      info.depth = m_impl->fgDepth.resource.ptr();
      info.motionVectors = m_impl->fgMotionVectors.resource.ptr();
      info.hudlessColor = m_impl->fgHudlessColor.resource.ptr();
      info.renderWidth = m_impl->fgRenderWidth;
      info.renderHeight = m_impl->fgRenderHeight;
      info.displayWidth = m_impl->fgDisplayWidth;
      info.displayHeight = m_impl->fgDisplayHeight;
      info.frameId = ++m_impl->fgFrameId;
      if (!InvokeNativePresentBegin(nativeApi.presentBegin, &info))
        Logger::err("D3D11NativePresenter: Streamline native present-begin callback faulted");
    }

    if (FAILED(m_impl->commandList->Close()))
      return E_FAIL;

    ID3D12CommandList* lists[] = { m_impl->commandList.ptr() };
    m_impl->queue->ExecuteCommandLists(1u, lists);

    hr = m_impl->swapchain->Present(syncInterval, 0u);
    if (nativeApi.presentEnd) {
      if (!InvokeNativePresentEnd(nativeApi.presentEnd, hr))
        Logger::err("D3D11NativePresenter: Streamline native present-end callback faulted");
    }

    if (FAILED(hr))
      return hr;

    if (!m_impl->waitForGpu())
      return E_FAIL;

    if (!m_impl->loggedFirstPresent) {
      Logger::info("D3D11NativePresenter: First Vulkan-to-D3D12 HDR10 frame presented successfully");
      m_impl->loggedFirstPresent = true;
    }

    m_impl->imageIndex = (m_impl->imageIndex + 1u) % m_impl->sharedImages.size();
    return S_OK;
  }

}
