#pragma once

#include <memory>

#include "../dxvk/dxvk_device.h"

struct ID3D11Resource;
struct CsDxvkNativePresenterApi;

namespace dxvk {

  /**
   * Native D3D12/DXGI presentation bridge used by the HDR probe.
   *
   * The visible swapchain is owned by system DXGI. Its input images are
   * D3D12 committed resources imported into Vulkan, so DXVK can render into
   * them without a CPU copy. The initial implementation deliberately waits
   * at the API boundary in order to prove color and ownership before adding
   * cross-API fence pipelining.
   */
  class D3D11NativePresenter {

  public:

    explicit D3D11NativePresenter(const Rc<DxvkDevice>& device);
    ~D3D11NativePresenter();

    D3D11NativePresenter(const D3D11NativePresenter&) = delete;
    D3D11NativePresenter& operator=(const D3D11NativePresenter&) = delete;

    bool initialize(HWND window, uint32_t width, uint32_t height, uint32_t bufferCount);
    bool resize(uint32_t width, uint32_t height, uint32_t bufferCount);
    void reset();

    bool ready() const;

    Rc<DxvkImage> acquireImage();
    HRESULT present(uint32_t syncInterval);

    void updateFrameGenerationResources();
    void recordFrameGenerationCopies(DxvkContext* context);

  private:

    struct Impl;

    Rc<DxvkDevice>        m_device;
    std::unique_ptr<Impl> m_impl;

  };

}
