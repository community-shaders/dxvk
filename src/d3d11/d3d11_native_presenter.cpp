#include "d3d11_native_presenter.h"

#include "d3d11_device.h"
#include "d3d11_context_imm.h"
#include "d3d11_texture.h"
#include "../../include/cs_dxvk_api.h"

#include <array>
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

    std::mutex g_dlssgApiMutex;
    CsDxvkDlssgPresenterWorkaroundApi g_dlssgApi = { };

    struct NativeFrameGenerationSources {
      Com<ID3D11Resource> depth;
      Com<ID3D11Resource> motionVectors;
      Com<ID3D11Resource> hudlessColor;
    };

    std::mutex g_nativeSourcesMutex;
    NativeFrameGenerationSources g_nativeSources;
    std::mutex g_nativePresenterMutex;
    D3D11NativePresenter* g_nativePresenter = nullptr;

    CsDxvkDlssgPresenterWorkaroundApi GetDlssgApi() {
      std::lock_guard<std::mutex> lock(g_dlssgApiMutex);
      return g_dlssgApi;
    }

    bool IsDlssgApiConfigured(const CsDxvkDlssgPresenterWorkaroundApi& api) {
      return api.size >= sizeof(api) && api.version == CS_DXVK_API_VERSION &&
        api.upgradeObject && api.presentBegin && api.presentEnd;
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

    DXGI_COLOR_SPACE_TYPE GetDxgiColorSpace(VkColorSpaceKHR colorSpace) {
      return colorSpace == VK_COLOR_SPACE_HDR10_ST2084_EXT
        ? DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020
        : DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    }

    bool InvokeDlssgPresentBegin(PFN_csDxvkDlssgPresentBegin callback,
                                 const CsDxvkDlssgPresentInfo* info) noexcept {
      __try {
        callback(info);
        return true;
      } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
      }
    }

    bool InvokeDlssgPresentEnd(PFN_csDxvkDlssgPresentEnd callback,
                               int32_t result) noexcept {
      __try {
        callback(result);
        return true;
      } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
      }
    }

    bool InvokeDlssEvaluation(PFN_csDxvkEvaluateDlss callback,
                              const CsDxvkDlssEvaluationInfo* info) noexcept {
      __try {
        return callback && callback(info);
      } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
      }
    }

  }


  extern "C" void dxvkConfigureDlssgPresenterWorkaround(
          const CsDxvkDlssgPresenterWorkaroundApi* api) {
    std::lock_guard<std::mutex> lock(g_dlssgApiMutex);
    g_dlssgApi = { };
    if (api && IsDlssgApiConfigured(*api))
      g_dlssgApi = *api;
  }


  extern "C" void dxvkSetDlssgPresenterResources(
          void*     depth,
          void*     motionVectors,
          void*     hudlessColor) {
    std::lock_guard<std::mutex> lock(g_nativeSourcesMutex);
    g_nativeSources.depth = static_cast<ID3D11Resource*>(depth);
    g_nativeSources.motionVectors = static_cast<ID3D11Resource*>(motionVectors);
    g_nativeSources.hudlessColor = static_cast<ID3D11Resource*>(hudlessColor);
  }


  extern "C" bool dxvkEvaluateDlssWorkaround(
          const CsDxvkDlssUpscaleRequest* request) {
    if (!request || request->size < sizeof(*request) ||
        request->version != CS_DXVK_API_VERSION)
      return false;
    std::lock_guard<std::mutex> lock(g_nativePresenterMutex);
    return g_nativePresenter && g_nativePresenter->evaluateDlss(*request);
  }


  struct D3D11NativePresenter::Impl {
    struct FrameGenerationBridgeResource {
      Com<ID3D12Resource> resource;
      Rc<DxvkImage> image;
      VkFormat format = VK_FORMAT_UNDEFINED;
      VkExtent3D extent = { };
      D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE;
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
    VkColorSpaceKHR colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
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
    uint64_t fgFrameId = 0u;
    bool loggedUnsupportedFgFormat = false;
    FrameGenerationBridgeResource dlssColorIn;
    FrameGenerationBridgeResource dlssColorOut;
    FrameGenerationBridgeResource dlssDepth;
    FrameGenerationBridgeResource dlssMotionVectors;

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

    bool ensureBridge(DxvkDevice* device, const Rc<DxvkImage>& source,
                      FrameGenerationBridgeResource& bridge, const char* debugName,
                      D3D12_RESOURCE_FLAGS requiredFlags = D3D12_RESOURCE_FLAG_NONE) {
      if (!source) {
        bridge = { };
        return false;
      }

      const auto& sourceInfo = source->info();
      if (bridge.resource && bridge.format == sourceInfo.format &&
          bridge.extent == sourceInfo.extent && bridge.flags == requiredFlags)
        return true;

      bridge = { };
      const DXGI_FORMAT format = GetDxgiFormat(sourceInfo.format);
      if (format == DXGI_FORMAT_UNKNOWN) {
        Logger::err(str::format("D3D11NativePresenter: Unsupported bridge VkFormat ",
          uint32_t(sourceInfo.format), " for ", debugName));
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
      desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS | requiredFlags;

      HRESULT hr = this->device->CreateCommittedResource(&heapProperties, D3D12_HEAP_FLAG_SHARED,
        &desc, D3D12_RESOURCE_STATE_COMMON, nullptr, __uuidof(ID3D12Resource),
        reinterpret_cast<void**>(&bridge.resource));
      if (FAILED(hr)) {
        Logger::err(str::format("D3D11NativePresenter: Bridge creation failed for ", debugName, ": ", hr));
        return false;
      }

      HANDLE sharedHandle = nullptr;
      hr = this->device->CreateSharedHandle(bridge.resource.ptr(), nullptr,
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
        imageInfo.debugName = debugName;
        bridge.image = device->createImage(imageInfo, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
      } catch (const DxvkError& e) {
        Logger::err(str::format("D3D11NativePresenter: Vulkan bridge import failed for ",
          debugName, ": ", e.message()));
      }
      CloseHandle(sharedHandle);

      if (!bridge.image) {
        bridge = { };
        return false;
      }
      bridge.format = sourceInfo.format;
      bridge.extent = sourceInfo.extent;
      bridge.flags = requiredFlags;
      return true;
    }

  };


  D3D11NativePresenter::D3D11NativePresenter(const Rc<DxvkDevice>& device, D3D11Device* d3d11Device)
  : m_device(device), m_d3d11Device(d3d11Device), m_impl(std::make_unique<Impl>()) {

  }


  D3D11NativePresenter::~D3D11NativePresenter() {
    reset();
  }


  bool D3D11NativePresenter::workaroundConfigured() {
    return IsDlssgApiConfigured(GetDlssgApi());
  }


  bool D3D11NativePresenter::initialize(
          HWND      window,
          uint32_t  width,
          uint32_t  height,
          uint32_t  bufferCount,
          VkColorSpaceKHR colorSpace) {
    if (!window || !width || !height)
      return false;
    if (colorSpace != VK_COLOR_SPACE_HDR10_ST2084_EXT &&
        colorSpace != VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
      return false;

    const CsDxvkDlssgPresenterWorkaroundApi dlssgApi = GetDlssgApi();
    if (!IsDlssgApiConfigured(dlssgApi))
      return false;

    reset();

    m_impl->dxgiModule = LoadLibraryExW(L"dxgi.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    m_impl->d3d12Module = LoadLibraryExW(L"d3d12.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!m_impl->dxgiModule || !m_impl->d3d12Module) {
      Logger::err("D3D11NativePresenter: Failed to load system DXGI/D3D12");
      reset();
      return false;
    }

    const auto createFactory = reinterpret_cast<PFN_CreateDXGIFactory2>(
      GetProcAddress(m_impl->dxgiModule, "CreateDXGIFactory2"));
    const auto createDevice = reinterpret_cast<PFN_D3D12CreateDevice>(
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
    dlssgApi.upgradeObject(CS_DXVK_DLSSG_OBJECT_DEVICE,
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
    dlssgApi.upgradeObject(CS_DXVK_DLSSG_OBJECT_FACTORY,
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

    UINT colorSpaceSupport = 0u;
    const DXGI_COLOR_SPACE_TYPE dxgiColorSpace = GetDxgiColorSpace(colorSpace);
    hr = m_impl->swapchain->CheckColorSpaceSupport(dxgiColorSpace, &colorSpaceSupport);
    if (FAILED(hr) || !(colorSpaceSupport & DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT) ||
        FAILED(m_impl->swapchain->SetColorSpace1(dxgiColorSpace))) {
      Logger::err("D3D11NativePresenter: Requested native DXGI color space is unavailable");
      reset();
      return false;
    }
    m_impl->colorSpace = colorSpace;

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
        imageInfo.colorSpace = colorSpace;
        imageInfo.shared = VK_TRUE;
        imageInfo.sharing.mode = DxvkSharedHandleMode::Import;
        imageInfo.sharing.type = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT;
        imageInfo.sharing.handle = sharedHandle;
        imageInfo.debugName = "Native DXGI presentation bridge image";

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
    m_impl->imageIndex = 0u;
    m_impl->loggedFirstPresent = false;
	{
	  std::lock_guard<std::mutex> lock(g_nativePresenterMutex);
	  g_nativePresenter = this;
	}

    Logger::info(str::format("D3D11NativePresenter: DLSS-G workaround ready (",
      width, "x", height, ", buffers=", swapchainDesc.BufferCount,
      ", colorSpace=", uint32_t(colorSpace), ")"));
    return true;
  }


  bool D3D11NativePresenter::resize(
          uint32_t width,
          uint32_t height,
          uint32_t bufferCount) {
    if (!m_impl->window)
      return false;
    const HWND window = m_impl->window;
    return initialize(window, width, height, bufferCount, m_impl->colorSpace);
  }


  bool D3D11NativePresenter::setColorSpace(VkColorSpaceKHR colorSpace) {
    if (!ready())
      return false;
    if (colorSpace != VK_COLOR_SPACE_HDR10_ST2084_EXT &&
        colorSpace != VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
      return false;

    const DXGI_COLOR_SPACE_TYPE dxgiColorSpace = GetDxgiColorSpace(colorSpace);
    UINT support = 0u;
    if (FAILED(m_impl->swapchain->CheckColorSpaceSupport(dxgiColorSpace, &support)) ||
        !(support & DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT) ||
        FAILED(m_impl->swapchain->SetColorSpace1(dxgiColorSpace)))
      return false;

    m_impl->colorSpace = colorSpace;
    Logger::info(str::format("D3D11NativePresenter: Changed presentation color space to ",
      uint32_t(colorSpace)));
    return true;
  }


  void D3D11NativePresenter::reset() {
    if (!m_impl)
      return;
	{
	  std::lock_guard<std::mutex> lock(g_nativePresenterMutex);
	  if (g_nativePresenter == this)
	    g_nativePresenter = nullptr;
	}

    m_impl->waitForGpu();
    m_device->waitForIdle();

    m_impl->sharedImages.clear();
    m_impl->sharedResources.clear();
    m_impl->fgDepth = { };
    m_impl->fgMotionVectors = { };
    m_impl->fgHudlessColor = { };
	m_impl->dlssColorIn = { };
	m_impl->dlssColorOut = { };
	m_impl->dlssDepth = { };
	m_impl->dlssMotionVectors = { };
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


  bool D3D11NativePresenter::evaluateDlss(const CsDxvkDlssUpscaleRequest& request) {
    const CsDxvkDlssgPresenterWorkaroundApi api = GetDlssgApi();
    if (!ready() || !api.evaluateDlss || !m_d3d11Device ||
        !request.colorIn || !request.colorOut || !request.depth || !request.motionVectors)
      return false;

    const auto getImage = [] (void* resource) -> Rc<DxvkImage> {
      return resource ? GetCommonTexture(static_cast<ID3D11Resource*>(resource))->GetImage() : nullptr;
    };
    const Rc<DxvkImage> colorIn = getImage(request.colorIn);
    const Rc<DxvkImage> colorOut = getImage(request.colorOut);
    const Rc<DxvkImage> depth = getImage(request.depth);
    const Rc<DxvkImage> motionVectors = getImage(request.motionVectors);
    if (!m_impl->ensureBridge(m_device.ptr(), colorIn, m_impl->dlssColorIn, "Native DLSS color input") ||
        !m_impl->ensureBridge(m_device.ptr(), colorOut, m_impl->dlssColorOut, "Native DLSS color output",
          D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) ||
        !m_impl->ensureBridge(m_device.ptr(), depth, m_impl->dlssDepth, "Native DLSS depth") ||
        !m_impl->ensureBridge(m_device.ptr(), motionVectors, m_impl->dlssMotionVectors, "Native DLSS motion vectors"))
      return false;

    auto* immediateContext = m_d3d11Device->GetContext();
    if (!immediateContext)
      return false;

    {
      auto immediateContextLock = immediateContext->LockContext();
      immediateContext->EmitCs([
        cColorIn = colorIn,
        cDepth = depth,
        cMotionVectors = motionVectors,
        cColorBridge = m_impl->dlssColorIn.image,
        cDepthBridge = m_impl->dlssDepth.image,
        cMotionBridge = m_impl->dlssMotionVectors.image
      ] (DxvkContext* context) {
        const auto copy = [&] (const Rc<DxvkImage>& destination, const Rc<DxvkImage>& source) {
          VkImageSubresourceLayers layers = { };
          layers.aspectMask = source->formatInfo()->aspectMask;
          layers.layerCount = 1u;
          context->copyImage(destination, layers, { }, source, layers, { }, source->info().extent);
        };
        copy(cColorBridge, cColorIn);
        copy(cDepthBridge, cDepth);
        copy(cMotionBridge, cMotionVectors);
        context->flushCommandList(nullptr, nullptr);
      });
      immediateContext->FlushCsChunk();
      immediateContext->SynchronizeCsThread(DxvkCsThread::SynchronizeAll);
    }
    if (m_device->waitForIdle() != VK_SUCCESS)
      return false;

    if (FAILED(m_impl->allocator->Reset()) ||
        FAILED(m_impl->commandList->Reset(m_impl->allocator.ptr(), nullptr)))
      return false;

    CsDxvkDlssEvaluationInfo info = { };
    info.size = sizeof(info);
    info.version = CS_DXVK_API_VERSION;
    info.commandList = m_impl->commandList.ptr();
    info.colorIn = m_impl->dlssColorIn.resource.ptr();
    info.colorOut = m_impl->dlssColorOut.resource.ptr();
    info.depth = m_impl->dlssDepth.resource.ptr();
    info.motionVectors = m_impl->dlssMotionVectors.resource.ptr();
    info.renderWidth = request.renderWidth;
    info.renderHeight = request.renderHeight;
    info.outputWidth = request.outputWidth;
    info.outputHeight = request.outputHeight;
    info.qualityMode = request.qualityMode;
    info.jitterX = request.jitterX;
    info.jitterY = request.jitterY;
    info.frameId = request.frameId;
    if (!InvokeDlssEvaluation(api.evaluateDlss, &info) || FAILED(m_impl->commandList->Close()))
      return false;

    ID3D12CommandList* lists[] = { m_impl->commandList.ptr() };
    m_impl->queue->ExecuteCommandLists(1u, lists);
    if (!m_impl->waitForGpu())
      return false;

    {
      auto immediateContextLock = immediateContext->LockContext();
      immediateContext->EmitCs([
        cOutput = colorOut,
        cOutputBridge = m_impl->dlssColorOut.image
      ] (DxvkContext* context) {
        VkImageSubresourceLayers layers = { };
        layers.aspectMask = cOutput->formatInfo()->aspectMask;
        layers.layerCount = 1u;
        context->copyImage(cOutput, layers, { }, cOutputBridge, layers, { }, cOutput->info().extent);
        context->flushCommandList(nullptr, nullptr);
      });
      immediateContext->FlushCsChunk();
      immediateContext->SynchronizeCsThread(DxvkCsThread::SynchronizeAll);
    }
    return m_device->waitForIdle() == VK_SUCCESS;
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

    const CsDxvkDlssgPresenterWorkaroundApi dlssgApi = GetDlssgApi();
    if (!IsDlssgApiConfigured(dlssgApi))
      return E_FAIL;
    {
      CsDxvkDlssgPresentInfo info = { };
      info.size = sizeof(info);
      info.version = CS_DXVK_API_VERSION;
      info.commandList = m_impl->commandList.ptr();
      info.depth = m_impl->fgDepth.resource.ptr();
      info.motionVectors = m_impl->fgMotionVectors.resource.ptr();
      info.hudlessColor = m_impl->fgHudlessColor.resource.ptr();
      info.frameId = ++m_impl->fgFrameId;
      if (!InvokeDlssgPresentBegin(dlssgApi.presentBegin, &info))
        Logger::err("D3D11NativePresenter: Streamline native present-begin callback faulted");
    }

    if (FAILED(m_impl->commandList->Close()))
      return E_FAIL;

    ID3D12CommandList* lists[] = { m_impl->commandList.ptr() };
    m_impl->queue->ExecuteCommandLists(1u, lists);

    hr = m_impl->swapchain->Present(syncInterval, 0u);
    if (!InvokeDlssgPresentEnd(dlssgApi.presentEnd, hr))
      Logger::err("D3D11NativePresenter: Streamline native present-end callback faulted");

    if (FAILED(hr))
      return hr;

    if (!m_impl->waitForGpu())
      return E_FAIL;

    if (!m_impl->loggedFirstPresent) {
      Logger::info("D3D11NativePresenter: First Vulkan-to-D3D12 frame presented successfully");
      m_impl->loggedFirstPresent = true;
    }

    m_impl->imageIndex = (m_impl->imageIndex + 1u) % m_impl->sharedImages.size();
    return S_OK;
  }

}
