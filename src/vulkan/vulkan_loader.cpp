#include <algorithm>
#include <mutex>
#include <tuple>
#include <vector>

#include "vulkan_loader.h"

#include "../util/log/log.h"

#include "../util/util_string.h"
#include "../util/util_win32_compat.h"

namespace dxvk::vk {

  static std::pair<HMODULE, PFN_vkGetInstanceProcAddr> loadVulkanLibrary() {
    static const std::array<const char*, 3> dllNames = {{
#ifdef _WIN32
      // An external Vulkan-layer interposer / frame-generation host may be aliased in ahead of the real
      // loader to wrap DXVK's ENTIRE Vulkan surface: it hooks vkCreateInstance/Device/present/acquire and
      // owns the per-frame frame-generation contract itself. Such an interposer forwards every call to the
      // real driver, so it is a transparent passthrough when no frame-gen feature is active. It is loaded
      // first (the entry below); if absent (or it fails to resolve vkGetInstanceProcAddr) we fall through
      // to the real loader. The interposer itself loads the genuine "vulkan-1.dll" (from System32) to
      // forward to, so there is no name collision.
      "sl.interposer.dll",
      "winevulkan.dll",
      "vulkan-1.dll",
#else
      "libvulkan.so",
      "libvulkan.so.1",
      nullptr,
#endif
    }};

    for (auto dllName : dllNames) {
      if (!dllName)
        continue;

      HMODULE library = LoadLibraryA(dllName);

      if (!library)
        continue;

      auto proc = GetProcAddress(library, "vkGetInstanceProcAddr");

      if (!proc) {
        FreeLibrary(library);
        continue;
      }

      Logger::info(str::format("Vulkan: Found vkGetInstanceProcAddr in ", dllName, " @ 0x", std::hex, reinterpret_cast<uintptr_t>(proc)));
      return std::make_pair(library, reinterpret_cast<PFN_vkGetInstanceProcAddr>(proc));
    }

    Logger::err("Vulkan: vkGetInstanceProcAddr not found");
    return { };
  }

  LibraryLoader::LibraryLoader() {
    std::tie(m_library, m_getInstanceProcAddr) = loadVulkanLibrary();
  }

  LibraryLoader::LibraryLoader(PFN_vkGetInstanceProcAddr loaderProc) {
    m_getInstanceProcAddr = loaderProc;
  }

  LibraryLoader::~LibraryLoader() {
    if (m_library)
      FreeLibrary(m_library);
  }

  PFN_vkVoidFunction LibraryLoader::sym(VkInstance instance, const char* name) const {
    return m_getInstanceProcAddr(instance, name);
  }

  PFN_vkVoidFunction LibraryLoader::sym(const char* name) const {
    return sym(nullptr, name);
  }

  
  InstanceLoader::InstanceLoader(const Rc<LibraryLoader>& library, bool owned, VkInstance instance)
  : m_library(library), m_instance(instance), m_owned(owned) { }
  
  
  PFN_vkVoidFunction InstanceLoader::sym(const char* name) const {
    return m_library->sym(m_instance, name);
  }
  
  
  DeviceLoader::DeviceLoader(const Rc<InstanceLoader>& library, bool owned, VkDevice device)
  : m_library(library)
  , m_getDeviceProcAddr(reinterpret_cast<PFN_vkGetDeviceProcAddr>(
      m_library->sym("vkGetDeviceProcAddr"))),
    m_device(device), m_owned(owned) { }
  
  
  PFN_vkVoidFunction DeviceLoader::sym(const char* name) const {
    return m_getDeviceProcAddr(m_device, name);
  }
  
  
  LibraryFn::LibraryFn() { }
  LibraryFn::LibraryFn(PFN_vkGetInstanceProcAddr loaderProc)
  : LibraryLoader(loaderProc) { }
  LibraryFn::~LibraryFn() { }
  
  
  InstanceFn::InstanceFn(const Rc<LibraryLoader>& library, bool owned, VkInstance instance)
  : InstanceLoader(library, owned, instance) { }
  InstanceFn::~InstanceFn() {
    if (m_owned)
      this->vkDestroyInstance(m_instance, nullptr);
  }
  
  
  namespace {

    // A device's own functions, for its maintenance5 fallbacks (installMaintenance5Fallbacks),
    // which the device's functions are replaced by.
    struct LegacyDeviceFns {
      VkDevice                                device = VK_NULL_HANDLE;
      PFN_vkCreateGraphicsPipelines           createGraphicsPipelines = nullptr;
      PFN_vkCreateComputePipelines            createComputePipelines = nullptr;
      PFN_vkCreateBufferView                  createBufferView = nullptr;
      PFN_vkCreateShaderModule                createShaderModule = nullptr;
      PFN_vkDestroyShaderModule               destroyShaderModule = nullptr;
      PFN_vkCreateImage                       createImage = nullptr;
      PFN_vkDestroyImage                      destroyImage = nullptr;
      PFN_vkGetImageSubresourceLayout         getImageSubresourceLayout = nullptr;
    };

    std::mutex                    g_legacyMutex;
    std::vector<LegacyDeviceFns>  g_legacyDevices;

    LegacyDeviceFns legacyFns(VkDevice device) {
      std::lock_guard lock(g_legacyMutex);

      for (const auto& e : g_legacyDevices) {
        if (e.device == device)
          return e;
      }

      return LegacyDeviceFns();
    }


    // Unlinks the first structure of type from head's pNext chain and returns it. head is the
    // caller's copy, whose pNext changes freely; a deeper link is cut in the chain itself and
    // restored by ~ChainCuts (the call is synchronous).
    class ChainCuts {

    public:

      ~ChainCuts() {
        for (auto i = m_cuts.rbegin(); i != m_cuts.rend(); i++)
          i->first->pNext = i->second;
      }

      template<typename T>
      const VkBaseInStructure* cut(T& head, VkStructureType type) {
        const VkBaseInStructure* prev = nullptr;

        for (auto node = reinterpret_cast<const VkBaseInStructure*>(head.pNext); node; node = node->pNext) {
          if (node->sType == type) {
            if (!prev) {
              head.pNext = node->pNext;
            } else {
              auto link = const_cast<VkBaseInStructure*>(prev);
              m_cuts.push_back({ link, link->pNext });
              link->pNext = node->pNext;
            }

            return node;
          }

          prev = node;
        }

        return nullptr;
      }

    private:

      std::vector<std::pair<VkBaseInStructure*, const VkBaseInStructure*>> m_cuts;

    };


    // A pipeline create info's VkPipelineCreateFlags2CreateInfo, as legacy flags (the same bits below 32).
    template<typename T>
    bool lowerPipelineFlags(ChainCuts& cuts, T& info) {
      auto node = cuts.cut(info, VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO);

      if (!node)
        return true;

      auto flags = reinterpret_cast<const VkPipelineCreateFlags2CreateInfo*>(node)->flags;

      if (flags >> 32u) {
        Logger::err(str::format("Vulkan: pipeline create flags 0x", std::hex, uint64_t(flags), " need VK_KHR_maintenance5"));
        return false;
      }

      info.flags |= VkPipelineCreateFlags(flags);
      return true;
    }


    // Pipeline create infos without maintenance5: legacy flags, and a shader module for each stage
    // given inline (destroyed with this, once the pipelines exist).
    class LoweredStages {

    public:

      LoweredStages(const LegacyDeviceFns& fns, VkDevice device)
      : m_fns(fns), m_device(device) { }

      ~LoweredStages() {
        for (auto module : m_modules)
          m_fns.destroyShaderModule(m_device, module, nullptr);
      }

      bool lower(ChainCuts& cuts, VkPipelineShaderStageCreateInfo& stage) {
        // Stage names given inline name the inline module alone
        cuts.cut(stage, VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT);

        if (stage.module)
          return true;

        auto node = cuts.cut(stage, VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO);

        if (!node)
          return true;

        // Its chain (heap mappings, names) is for inline modules
        VkShaderModuleCreateInfo info = *reinterpret_cast<const VkShaderModuleCreateInfo*>(node);
        info.pNext = nullptr;

        if (m_fns.createShaderModule(m_device, &info, nullptr, &stage.module) != VK_SUCCESS)
          return false;

        m_modules.push_back(stage.module);
        return true;
      }

    private:

      const LegacyDeviceFns&      m_fns;
      VkDevice                    m_device;
      std::vector<VkShaderModule> m_modules;

    };


    VKAPI_ATTR VkResult VKAPI_CALL legacyCreateGraphicsPipelines(
            VkDevice                        device,
            VkPipelineCache                 pipelineCache,
            uint32_t                        createInfoCount,
      const VkGraphicsPipelineCreateInfo*   pCreateInfos,
      const VkAllocationCallbacks*          pAllocator,
            VkPipeline*                     pPipelines) {
      auto fns = legacyFns(device);

      ChainCuts cuts;
      LoweredStages modules(fns, device);

      std::vector<VkGraphicsPipelineCreateInfo> infos(pCreateInfos, pCreateInfos + createInfoCount);
      std::vector<std::vector<VkPipelineShaderStageCreateInfo>> stages(createInfoCount);

      for (uint32_t i = 0u; i < createInfoCount; i++) {
        if (!lowerPipelineFlags(cuts, infos[i]))
          return VK_ERROR_FEATURE_NOT_PRESENT;

        stages[i].assign(infos[i].pStages, infos[i].pStages + infos[i].stageCount);

        for (auto& stage : stages[i]) {
          if (!modules.lower(cuts, stage))
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        }

        infos[i].pStages = stages[i].data();
      }

      return fns.createGraphicsPipelines(device, pipelineCache,
        createInfoCount, infos.data(), pAllocator, pPipelines);
    }


    VKAPI_ATTR VkResult VKAPI_CALL legacyCreateComputePipelines(
            VkDevice                        device,
            VkPipelineCache                 pipelineCache,
            uint32_t                        createInfoCount,
      const VkComputePipelineCreateInfo*    pCreateInfos,
      const VkAllocationCallbacks*          pAllocator,
            VkPipeline*                     pPipelines) {
      auto fns = legacyFns(device);

      ChainCuts cuts;
      LoweredStages modules(fns, device);

      std::vector<VkComputePipelineCreateInfo> infos(pCreateInfos, pCreateInfos + createInfoCount);

      for (auto& info : infos) {
        if (!lowerPipelineFlags(cuts, info))
          return VK_ERROR_FEATURE_NOT_PRESENT;

        if (!modules.lower(cuts, info.stage))
          return VK_ERROR_OUT_OF_HOST_MEMORY;
      }

      return fns.createComputePipelines(device, pipelineCache,
        createInfoCount, infos.data(), pAllocator, pPipelines);
    }


    VKAPI_ATTR VkResult VKAPI_CALL legacyCreateBufferView(
            VkDevice                        device,
      const VkBufferViewCreateInfo*         pCreateInfo,
      const VkAllocationCallbacks*          pAllocator,
            VkBufferView*                   pView) {
      auto fns = legacyFns(device);

      // The view's usage is its buffer's
      ChainCuts cuts;
      VkBufferViewCreateInfo info = *pCreateInfo;
      cuts.cut(info, VK_STRUCTURE_TYPE_BUFFER_USAGE_FLAGS_2_CREATE_INFO);

      return fns.createBufferView(device, &info, pAllocator, pView);
    }


    VKAPI_ATTR void VKAPI_CALL legacyGetDeviceImageSubresourceLayout(
            VkDevice                        device,
      const VkDeviceImageSubresourceInfo*   pInfo,
            VkSubresourceLayout2*           pLayout) {
      auto fns = legacyFns(device);

      // The layout of an image created from the same info (no memory bound)
      pLayout->subresourceLayout = { };

      VkImage image = VK_NULL_HANDLE;

      if (fns.createImage(device, pInfo->pCreateInfo, nullptr, &image) != VK_SUCCESS)
        return;

      fns.getImageSubresourceLayout(device, image,
        &pInfo->pSubresource->imageSubresource, &pLayout->subresourceLayout);
      fns.destroyImage(device, image, nullptr);
    }

  }


  DeviceFn::DeviceFn(const Rc<InstanceLoader>& library, bool owned, VkDevice device)
  : DeviceLoader(library, owned, device) { }
  DeviceFn::~DeviceFn() {
    if (m_owned)
      this->vkDestroyDevice(m_device, nullptr);

    if (m_maintenance5Fallbacks) {
      std::lock_guard lock(g_legacyMutex);

      g_legacyDevices.erase(std::remove_if(g_legacyDevices.begin(), g_legacyDevices.end(),
        [this] (const LegacyDeviceFns& e) { return e.device == m_device; }), g_legacyDevices.end());
    }
  }


  void DeviceFn::installMaintenance5Fallbacks() {
    if (m_maintenance5Fallbacks)
      return;

    LegacyDeviceFns fns;
    fns.device = m_device;
    fns.createGraphicsPipelines = vkCreateGraphicsPipelines;
    fns.createComputePipelines = vkCreateComputePipelines;
    fns.createBufferView = vkCreateBufferView;
    fns.createShaderModule = vkCreateShaderModule;
    fns.destroyShaderModule = vkDestroyShaderModule;
    fns.createImage = vkCreateImage;
    fns.destroyImage = vkDestroyImage;
    fns.getImageSubresourceLayout = vkGetImageSubresourceLayout;

    { std::lock_guard lock(g_legacyMutex);
      g_legacyDevices.push_back(fns);
    }

    vkCreateGraphicsPipelines = &legacyCreateGraphicsPipelines;
    vkCreateComputePipelines = &legacyCreateComputePipelines;
    vkCreateBufferView = &legacyCreateBufferView;
    vkGetDeviceImageSubresourceLayoutKHR = &legacyGetDeviceImageSubresourceLayout;

    m_maintenance5Fallbacks = true;

    Logger::info("Vulkan: no VK_KHR_maintenance5; pipeline, buffer view and image layout functions take their legacy forms");
  }
  
}
