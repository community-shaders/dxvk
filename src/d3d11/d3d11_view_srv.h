#pragma once

#include "../dxvk/dxvk_device.h"

#include "../d3d10/d3d10_view_srv.h"

#include "d3d11_device_child.h"
#include "d3d11_view.h"

namespace dxvk {

  class D3D11CommonTexture;
  
  class D3D11Device;
  
  /**
   * \brief Shader resource view
   */
  class D3D11ShaderResourceView : public D3D11DeviceChild<ID3D11ShaderResourceView1> {
    
  public:
    
    D3D11ShaderResourceView(
            D3D11Device*                      pDevice,
            ID3D11Resource*                   pResource,
      const D3D11_SHADER_RESOURCE_VIEW_DESC1* pDesc);
    
    ~D3D11ShaderResourceView();
    
    ULONG STDMETHODCALLTYPE AddRef();

    ULONG STDMETHODCALLTYPE Release();

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) final;
    
    void STDMETHODCALLTYPE GetResource(ID3D11Resource** ppResource) final;
    
    void STDMETHODCALLTYPE GetDesc(D3D11_SHADER_RESOURCE_VIEW_DESC* pDesc) final;

    void STDMETHODCALLTYPE GetDesc1(D3D11_SHADER_RESOURCE_VIEW_DESC1* pDesc) final;
    
    const D3D11_VK_VIEW_INFO& GetViewInfo() const {
      return m_info;
    }

    BOOL TestHazards() const {
      return m_info.BindFlags & (D3D11_BIND_RENDER_TARGET | D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_UNORDERED_ACCESS);
    }

    D3D11_RESOURCE_DIMENSION GetResourceType() const {
      D3D11_RESOURCE_DIMENSION type;
      m_resource->GetType(&type);
      return type;
    }

    D3D11_COMMON_RESOURCE_DESC GetResourceDesc() const {
      D3D11_COMMON_RESOURCE_DESC desc;
      GetCommonResourceDesc(m_resource, &desc);
      return desc;
    }
    
    Rc<DxvkBufferView> GetBufferView() const {
      return m_bufferView;
    }
    
    Rc<DxvkImageView> GetImageView() const {
      if (unlikely(m_deferredTexture.load(std::memory_order_relaxed))) {
        if (m_stream)
          return GetStreamedImageView();
        CommitDeferredTexture();
      }
      return m_imageView;
    }

    /**
     * \brief Makes this view a streamed texture's shell (dxvkStreamEnableView)
     *
     * Its texture keeps no storage. A bind reads the window set with SetStreamWindow. With no window,
     * pfnMissing runs on the binding thread and may set one; otherwise the bind reads zero. pfnGone runs
     * first in the view's destructor, so whoever names the view can stop before it is destroyed.
     * \returns \c false if the texture has storage already, or this view is a shell already
     */
    bool EnableStream(PFN_D3D11StreamMissing pfnMissing, PFN_D3D11StreamMissing pfnGone, uint64_t user);

    /** \brief A shell's window. Any thread. Null clears it. */
    void SetStreamWindow(D3D11ShaderResourceView* pWindow);

    /** \brief A shell's window with a reference, or null. Any thread. */
    D3D11ShaderResourceView* GetStreamWindow() const;

    /** \brief How many binds have read this shell. Any thread. */
    uint64_t GetStreamBinds() const;

    D3D10ShaderResourceView* GetD3D10Iface() {
      return &m_d3d10;
    }
    
    static HRESULT GetDescFromResource(
            ID3D11Resource*                   pResource,
            D3D11_SHADER_RESOURCE_VIEW_DESC1* pDesc);
    
    static D3D11_SHADER_RESOURCE_VIEW_DESC1 PromoteDesc(
      const D3D11_SHADER_RESOURCE_VIEW_DESC*  pDesc,
            UINT                              Plane);
    
    static HRESULT NormalizeDesc(
            ID3D11Resource*                   pResource,
            D3D11_SHADER_RESOURCE_VIEW_DESC1* pDesc);

    static UINT GetPlaneSlice(
      const D3D11_SHADER_RESOURCE_VIEW_DESC1* pDesc);

  private:
    
    ID3D11Resource*                   m_resource;
    // The texture, while its storage may still wait for its first use (D3D11CommonTexture::EnsureStorage).
    mutable std::atomic<D3D11CommonTexture*> m_deferredTexture = { nullptr };

    void CommitDeferredTexture() const;

    struct StreamState {
      PFN_D3D11StreamMissing        missing = nullptr;
      PFN_D3D11StreamMissing        gone = nullptr;
      uint64_t                      user = 0;
      mutable sync::Spinlock        lock;
      Com<D3D11ShaderResourceView>  window;
      std::atomic<uint64_t>         binds = { 0 };
    };

    std::unique_ptr<StreamState> m_stream;

    Rc<DxvkImageView> GetStreamedImageView() const;
    D3D11_SHADER_RESOURCE_VIEW_DESC1  m_desc;
    D3D11_VK_VIEW_INFO                m_info;
    Rc<DxvkBufferView>                m_bufferView;
    Rc<DxvkImageView>                 m_imageView;
    D3D10ShaderResourceView           m_d3d10;

    // The viewed texture's (DXVK_ALLOC_TRACE_REFS): this view's references are recorded there.
    D3D11AllocTraceHandle*            m_allocTrace = nullptr;

    D3DDestructionNotifier            m_destructionNotifier;

  };
  
}
