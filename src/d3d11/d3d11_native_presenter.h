#pragma once

#include <memory>

#include "../dxvk/dxvk_device.h"
#include "../../include/cs_dxvk_api.h"

struct ID3D11Resource;
namespace dxvk {

  class D3D11Device;

  /**
   * Native D3D12/DXGI bridge for the Community Shaders DLSS-G workaround.
   *
   * The visible swapchain is owned by system DXGI. Its input images are
   * D3D12 committed resources imported into Vulkan, so DXVK can render into
   * them without a CPU copy. It is unavailable unless Community Shaders has
   * explicitly registered the DLSS-G-only callback API.
   */
  class D3D11NativePresenter {

  public:

    D3D11NativePresenter(const Rc<DxvkDevice>& device, D3D11Device* d3d11Device);
    ~D3D11NativePresenter();

    D3D11NativePresenter(const D3D11NativePresenter&) = delete;
    D3D11NativePresenter& operator=(const D3D11NativePresenter&) = delete;

    static bool workaroundConfigured();

    bool initialize(HWND window, uint32_t width, uint32_t height, uint32_t bufferCount,
      VkColorSpaceKHR colorSpace);
    bool resize(uint32_t width, uint32_t height, uint32_t bufferCount);
    bool setColorSpace(VkColorSpaceKHR colorSpace);
    void reset();

    bool ready() const;

    Rc<DxvkImage> acquireImage();
    HRESULT present(uint32_t syncInterval);

    void updateFrameGenerationResources();
    void recordFrameGenerationCopies(DxvkContext* context);
    bool evaluateDlss(const CsDxvkDlssUpscaleRequest& request);

  private:

    struct Impl;

    Rc<DxvkDevice>        m_device;
    D3D11Device*          m_d3d11Device;
    std::unique_ptr<Impl> m_impl;

  };

}
