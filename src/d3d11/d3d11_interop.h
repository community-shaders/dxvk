#pragma once

#include <map>
#include <memory>
#include <mutex>
#include <unordered_map>

#include "../dxgi/dxgi_interfaces.h"

#include "d3d11_include.h"

struct DxvkOrgInteropSubmission;
struct DxvkOrgInteropSubmissionBatch;
struct DxvkOrgInteropResourceInfo;
struct DxvkOrgInteropRegistration;
struct DxvkOrgInteropLeasedSubmission;
struct DxvkOrgInteropBufferHandoff;
struct DxvkOrgInteropResourceHandoff;
struct DxvkOrgInteropImageInfo;

namespace dxvk {

  class D3D11Device;
  struct DxvkExternalSubmitInfo;
  
  class D3D11VkInterop : public ComObject<IDXGIVkInteropDevice1> {
    
  public:
    
    D3D11VkInterop(
            IDXGIObject*          pContainer,
            D3D11Device*          pDevice);

    ~D3D11VkInterop();
    
    ULONG STDMETHODCALLTYPE AddRef();
    
    ULONG STDMETHODCALLTYPE Release();
    
    HRESULT STDMETHODCALLTYPE QueryInterface(
            REFIID                riid,
            void**                ppvObject);
    
    void STDMETHODCALLTYPE GetVulkanHandles(
            VkInstance*           pInstance,
            VkPhysicalDevice*     pPhysDev,
            VkDevice*             pDevice);
    
    void STDMETHODCALLTYPE GetSubmissionQueue(
            VkQueue*              pQueue,
            uint32_t*             pQueueFamilyIndex);
    
    void STDMETHODCALLTYPE TransitionSurfaceLayout(
            IDXGIVkInteropSurface*    pSurface,
      const VkImageSubresourceRange*  pSubresources,
            VkImageLayout             OldLayout,
            VkImageLayout             NewLayout);
    
    void STDMETHODCALLTYPE FlushRenderingCommands();
    
    void STDMETHODCALLTYPE LockSubmissionQueue();
    
    void STDMETHODCALLTYPE ReleaseSubmissionQueue();
    
    void STDMETHODCALLTYPE GetSubmissionQueue1(
            VkQueue*              pQueue,
            uint32_t*             pQueueIndex,
            uint32_t*             pQueueFamilyIndex);
    
    HRESULT STDMETHODCALLTYPE CreateTexture2DFromVkImage(
            const D3D11_TEXTURE2D_DESC1* pDesc,
            VkImage                      vkImage,
            ID3D11Texture2D**            ppTexture2D);

    /**
     * \brief Wraps a client-owned VkBuffer (see d3d11_org_interop.h)
     */
    HRESULT CreateBufferFromVkBuffer(
      const D3D11_BUFFER_DESC*          pDesc,
            VkBuffer                    buffer,
            ID3D11Buffer**              ppBuffer);

    /**
     * \brief Submits client command buffers in D3D11 stream order (see d3d11_org_interop.h)
     */
    HRESULT EnqueueExternalSubmission(
      const DxvkOrgInteropSubmission* pSubmission);

    HRESULT EnqueueExternalSubmissions(
      const DxvkOrgInteropSubmissionBatch* pBatch);

    /**
     * \brief Runs a client callback on the submission thread in stream order (see d3d11_org_interop.h)
     */
    HRESULT EnqueueQueueCallback(
            void                          (*pCallback)(void*, VkQueue),
            void*                           pUser);

    /**
     * \brief Runs a client callback on the worker thread in stream order (see d3d11_org_interop.h)
     */
    HRESULT EmitCommandBufferCallback(
            void                          (*pCallback)(void*, VkCommandBuffer),
            void*                                   pUser);

    /**
     * rief Client callbacks at DXVK's command buffer boundaries (see d3d11_org_interop.h)
     */
    HRESULT SetCommandBufferBoundaryCallbacks(
            void                          (*pOnEnd)(void*, VkCommandBuffer),
            void                          (*pOnBegin)(void*, VkCommandBuffer),
            void*                                   pUser);

    /**
     * \brief Describes and locks a buffer, texture or SRV (see d3d11_org_interop.h)
     */
    HRESULT GetResourceInfo(
            IUnknown*                   pObject,
            DxvkOrgInteropResourceInfo* pInfo);

    HRESULT RegisterResource(IUnknown* object, DxvkOrgInteropRegistration* registration);
    HRESULT UnregisterResource(uint64_t leaseToken);
    HRESULT EnqueueLeasedSubmission(const DxvkOrgInteropLeasedSubmission* submission);
    HRESULT EnqueueBufferHandoff(const DxvkOrgInteropBufferHandoff* handoff);
    HRESULT EnqueueResourceHandoff(const DxvkOrgInteropResourceHandoff* handoff);

    Rc<DxvkDevice> GetDXVKDevice() const;

    /**
     * \brief The immediate context's submission counter (see d3d11_org_interop.h)
     */
    const uint64_t* GetSubmissionCounter() const;
    
  private:

    struct RegisteredBacking;
    struct RegisteredResource;
    struct RegistrationState {
      std::mutex mutex;
      std::map<std::pair<uint32_t, uint64_t>, std::weak_ptr<RegisteredBacking>> backings;
      std::unordered_map<uint64_t, std::shared_ptr<RegisteredResource>> resources;
    };
    std::shared_ptr<RegistrationState> m_registration = std::make_shared<RegistrationState>();

    HRESULT PrepareLeasedSubmission(const DxvkOrgInteropLeasedSubmission* submission, DxvkExternalSubmitInfo& info);

    bool LockImage(const Rc<DxvkImage>& image);

    static void FillImageInfo(const Rc<DxvkImage>& image, const DxvkImageViewKey& view, DxvkOrgInteropImageInfo& out);
    
    IDXGIObject* m_container;
    D3D11Device* m_device;
    
  };
  
}
