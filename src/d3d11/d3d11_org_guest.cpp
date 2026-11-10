#include "d3d11_include.h"
#include "d3d11_org_guest.h"
#include "d3d11_org_interop.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "../dxvk/dxvk_adapter.h"
#include "../dxvk/dxvk_interop.h"

namespace dxvk {

  namespace {

    // The host's instance and device as DXVK sees them, from the requirement queries to the D3D11 device that takes them.
    struct GuestSession {
      dxvk::mutex                         mutex;

      std::vector<std::string>            instanceExtensions;
      std::vector<const char*>            instanceExtensionPointers;
      std::string                         engineName;

      Rc<DxvkInstance>                    instance;
      Rc<DxvkAdapter>                     adapter;
      uint32_t                            bindingModel = 0u;
      std::unique_ptr<DxvkDeviceCapabilities> caps;
      std::vector<DxvkOrgGuestFeatureStruct> features;
      std::vector<VkExtensionProperties>  extensions;
      std::vector<const char*>            extensionPointers;
      std::vector<VkDeviceQueueCreateInfo> queues;

      Rc<DxvkDevice>                      pending;
    };

    GuestSession& session() {
      static GuestSession s_session;
      return s_session;
    }

    // DXVK binds descriptors with the host's model: the only one this accepts.
    uint32_t negotiateHostModel(void* user, VkInstance, VkPhysicalDevice, PFN_vkGetInstanceProcAddr, uint32_t) {
      return *static_cast<const uint32_t*>(user);
    }

    DxvkDeviceQueueIndex queueIndex(const DxvkOrgGuestQueue& queue) {
      DxvkDeviceQueueIndex result = { };

      if (queue.queue) {
        result.family = queue.family;
        result.index = queue.index;
      }

      return result;
    }

    uint32_t enabledBindingModel(const DxvkDeviceCapabilities& caps) {
      const auto& features = caps.getFeatures();

      if (features.extDescriptorHeap.descriptorHeap)
        return DXVK_ORG_BINDING_MODEL_DESCRIPTOR_HEAP;

      if (features.extDescriptorBuffer.descriptorBuffer)
        return DXVK_ORG_BINDING_MODEL_DESCRIPTOR_BUFFER;

      return 0u;
    }

  }


  bool takeGuestDevice(
          Rc<DxvkInstance>&   instance,
          Rc<DxvkAdapter>&    adapter,
          Rc<DxvkDevice>&     device) {
    auto& s = session();
    std::lock_guard lock(s.mutex);

    if (s.pending == nullptr)
      return false;

    instance = s.instance;
    adapter = s.adapter;
    device = std::move(s.pending);
    // A flag on the device rather than a list of addresses: a later device at a reused address is not a guest.
    device->markGuest();

    Logger::info("D3D11: creating the device on the host's Vulkan device (guest device)");
    return true;
  }


  bool isGuestDevice(
    const DxvkDevice*         device) {
    return device && device->isGuest();
  }

}


extern "C" {
  using namespace dxvk;

  DLLEXPORT HRESULT __stdcall dxvkGetGuestInstanceRequirements(PFN_vkGetInstanceProcAddr getInstanceProcAddr,
    DxvkOrgGuestInstanceRequirements* pRequirements) {
    if (!pRequirements || pRequirements->version != DXVK_ORG_GUEST_VERSION)
      return E_INVALIDARG;

    auto& s = session();
    std::lock_guard lock(s.mutex);

    try {
      if (!DxvkInstance::queryGuestRequirements(getInstanceProcAddr, s.instanceExtensions, s.engineName))
        return E_FAIL;
    } catch (const DxvkError& e) {
      Logger::err(str::format("dxvkGetGuestInstanceRequirements: ", e.message()));
      return E_FAIL;
    }

    s.instanceExtensionPointers.clear();

    for (const auto& name : s.instanceExtensions)
      s.instanceExtensionPointers.push_back(name.c_str());

    pRequirements->apiVersion = DxvkVulkanApiVersion;
    pRequirements->extensionCount = uint32_t(s.instanceExtensionPointers.size());
    pRequirements->extensions = s.instanceExtensionPointers.data();
    pRequirements->engineName = s.engineName.c_str();
    return S_OK;
  }


  DLLEXPORT HRESULT __stdcall dxvkGetGuestDeviceRequirements(const DxvkOrgGuestDeviceQuery* pQuery,
    DxvkOrgGuestDeviceRequirements* pRequirements) {
    if (!pQuery || pQuery->version != DXVK_ORG_GUEST_VERSION || !pQuery->instance || !pQuery->physicalDevice
     || !pRequirements || pRequirements->version != DXVK_ORG_GUEST_VERSION)
      return E_INVALIDARG;

    auto& s = session();
    std::lock_guard lock(s.mutex);

    s.caps = nullptr;
    s.adapter = nullptr;
    s.instance = nullptr;

    try {
      // The host's instance, filtered by what it enabled. DXVK never destroys it.
      std::vector<const char*> instanceExtensions(pQuery->instanceExtensions, pQuery->instanceExtensions + pQuery->instanceExtensionCount);

      DxvkInstanceImportInfo instanceInfo = { };
      instanceInfo.loaderProc = pQuery->getInstanceProcAddr;
      instanceInfo.instance = pQuery->instance;
      instanceInfo.extensionCount = uint32_t(instanceExtensions.size());
      instanceInfo.extensionNames = instanceExtensions.data();

      s.instance = new DxvkInstance(instanceInfo, 0);

      for (uint32_t i = 0; i < s.instance->adapterCount(); i++) {
        Rc<DxvkAdapter> adapter = s.instance->enumAdapters(i);

        if (adapter->handle() == pQuery->physicalDevice)
          s.adapter = std::move(adapter);
      }

      if (s.adapter == nullptr) {
        Logger::err("dxvkGetGuestDeviceRequirements: the host's physical device is not one DXVK can use");
        s.instance = nullptr;
        return E_INVALIDARG;
      }

      // What DXVK would create its own device with, on the host's binding model.
      s.bindingModel = pQuery->bindingModel;
      DxvkInteropBindingModelNegotiation negotiation = { &negotiateHostModel, &s.bindingModel };

      s.caps = std::make_unique<DxvkDeviceCapabilities>(*s.instance, s.adapter->handle(), nullptr, false, &negotiation);
    } catch (const DxvkError& e) {
      Logger::err(str::format("dxvkGetGuestDeviceRequirements: ", e.message()));
      s.caps = nullptr;
      s.adapter = nullptr;
      s.instance = nullptr;
      return E_FAIL;
    }

    std::array<char, 1024u> error = { };

    if (!s.caps->isSuitable(error.size(), error.data())) {
      Logger::err(str::format("dxvkGetGuestDeviceRequirements: ", error.data()));
      s.caps = nullptr;
      return E_FAIL;
    }

    if (enabledBindingModel(*s.caps) != s.bindingModel) {
      Logger::err(str::format("dxvkGetGuestDeviceRequirements: DXVK cannot bind descriptors with the host's model ", s.bindingModel,
        " (it would use ", enabledBindingModel(*s.caps), ")"));
      s.caps = nullptr;
      return E_FAIL;
    }

    s.features.clear();

    for (const auto& f : s.caps->getCreateFeatureStructs())
      s.features.push_back({ f.feature, f.size });

    uint32_t extensionCount = 0u;
    s.caps->queryDeviceExtensions(&extensionCount, nullptr);
    s.extensions.resize(extensionCount);
    s.caps->queryDeviceExtensions(&extensionCount, s.extensions.data());

    s.extensionPointers.clear();

    for (const auto& ext : s.extensions)
      s.extensionPointers.push_back(ext.extensionName);

    uint32_t queueCount = 0u;
    s.caps->queryDeviceQueues(&queueCount, nullptr);
    s.queues.assign(queueCount, { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO });
    s.caps->queryDeviceQueues(&queueCount, s.queues.data());

    for (auto& q : s.queues)
      q.pQueuePriorities = nullptr;

    Logger::info(str::format("dxvkGetGuestDeviceRequirements: ", s.extensions.size(), " extensions, ", s.features.size(),
      " feature structs, ", s.queues.size(), " queue families, binding model ", s.bindingModel));

    pRequirements->coreFeatures = &s.caps->getCreateFeatures().core.features;
    pRequirements->featureCount = uint32_t(s.features.size());
    pRequirements->features = s.features.data();
    pRequirements->extensionCount = uint32_t(s.extensionPointers.size());
    pRequirements->extensions = s.extensionPointers.data();
    pRequirements->queueCount = uint32_t(s.queues.size());
    pRequirements->queues = s.queues.data();
    return S_OK;
  }


  DLLEXPORT HRESULT __stdcall dxvkSetGuestDevice(const DxvkOrgGuestDeviceDesc* pDesc) {
    auto& s = session();
    std::lock_guard lock(s.mutex);

    if (!pDesc) {
      s.pending = nullptr;
      return S_OK;
    }

    if (pDesc->version != DXVK_ORG_GUEST_VERSION || !pDesc->device || !pDesc->graphics.queue
     || (pDesc->enabledExtensionCount && !pDesc->enabledExtensions))
      return E_INVALIDARG;

    // The import maps graphics, transfer and sparse binding to the first queue of the family it is given.
    if (pDesc->graphics.index != 0u) {
      Logger::err("dxvkSetGuestDevice: the graphics queue must be its family's first");
      return E_INVALIDARG;
    }

    if (!s.caps || s.adapter == nullptr) {
      Logger::err("dxvkSetGuestDevice: no device requirements were queried");
      return E_FAIL;
    }

    try {
      std::vector<const char*> extensions(pDesc->enabledExtensions, pDesc->enabledExtensions + pDesc->enabledExtensionCount);
      DxvkInteropBindingModelNegotiation negotiation = { &negotiateHostModel, &s.bindingModel };

      // DXVK's own requirement chain stands for the features: the host enabled at least those.
      DxvkDeviceImportInfo args = { };
      args.device = pDesc->device;
      args.queue = pDesc->graphics.queue;
      args.queueFamily = pDesc->graphics.family;
      args.extensionCount = uint32_t(extensions.size());
      args.extensionNames = extensions.data();
      args.features = &s.caps->getCreateFeatures().core;
      args.negotiation = &negotiation;
      args.interopCompute = queueIndex(pDesc->compute);
      args.interopUpload = queueIndex(pDesc->upload);
      args.interopCopy = queueIndex(pDesc->copy);
      args.registerInterop = true;
      args.hostFeatures = pDesc->enabledFeatures;

      s.pending = s.adapter->importDevice(args);
    } catch (const DxvkError& e) {
      Logger::err(str::format("dxvkSetGuestDevice: ", e.message()));
      s.pending = nullptr;
      return E_FAIL;
    }

    return S_OK;
  }

}
