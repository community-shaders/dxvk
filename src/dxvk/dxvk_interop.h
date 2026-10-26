#pragma once

#include <memory>
#include <string>
#include <vector>

#include "../vulkan/vulkan_loader.h"

namespace dxvk {

  /**
   * \brief Device feature requested by an in-process interop client
   *
   * An embedding application that records its own Vulkan work on DXVK's device
   * (e.g. a render graph adopting the VkDevice) may need features DXVK does not
   * use itself. \c extension names the extension that exposes the feature, or is
   * empty for core features; \c feature is the member name of the feature struct
   * (e.g. "descriptorHeap"). Requested features are enabled on the VkDevice only:
   * DXVK's own code paths keep seeing the feature set they would have chosen.
   */
  struct DxvkInteropFeature {
    std::string extension;
    std::string feature;
    /// Why a request was denied (empty: the device does not support it), for the log.
    std::string reason = { };
  };

  /**
   * \brief Sets the features to request at the next device creation
   *
   * Must be called before the D3D11 device is created. Unsupported features
   * are skipped, not errors; the result is reported per device.
   */
  void setInteropFeatureRequest(std::vector<DxvkInteropFeature> features);

  std::vector<DxvkInteropFeature> getInteropFeatureRequest();

  /**
   * \brief Descriptor binding models, as a mask
   *
   * A client recording into DXVK's command buffers must bind descriptors the
   * way DXVK does, so the two negotiate one model at device creation. The
   * values equal BasicRHI's rhi::vulkan::BindingModelBits; 0 is DXVK's legacy
   * descriptor sets, which no client accepts.
   */
  enum DxvkInteropBindingModelBits : uint32_t {
    DxvkInteropBindingModelHeap   = 1u << 0, ///< VK_EXT_descriptor_heap
    DxvkInteropBindingModelBuffer = 1u << 1, ///< VK_EXT_descriptor_buffer (+ VK_EXT_mutable_descriptor_type for the client)
  };

  /**
   * \brief Binding model negotiation callback
   *
   * Called during device creation, once the physical device is known and
   * DXVK made its own choice (\c preferredModel, one bit or 0), before the
   * VkDevice exists. Returns the mask of models the client accepts. Must not
   * call back into DXVK or D3D11.
   */
  using DxvkInteropBindingModelCallback = uint32_t (*)(void* user, VkInstance instance,
    VkPhysicalDevice physicalDevice, PFN_vkGetInstanceProcAddr getInstanceProcAddr, uint32_t preferredModel);

  struct DxvkInteropBindingModelNegotiation {
    DxvkInteropBindingModelCallback callback = nullptr;
    void* user = nullptr;
  };

  /**
   * \brief Sets the negotiation callback for the next device creation
   *
   * Like the feature request, must be set before the D3D11 device is created.
   * A null callback clears it, and DXVK keeps its own choice.
   */
  void setInteropBindingModelNegotiation(DxvkInteropBindingModelNegotiation negotiation);

  DxvkInteropBindingModelNegotiation getInteropBindingModelNegotiation();

  /**
   * \brief What a VkDevice was actually created with
   *
   * Kept for the device's lifetime so that an adopting client can reconstruct
   * the enabled extension and feature set. The feature chain is owned here.
   */
  struct DxvkInteropDeviceRecord {
    VkDevice device = VK_NULL_HANDLE;
    std::vector<std::string> extensionNames;
    std::vector<const char*> extensionPointers;
    /// The instance's, e.g. VK_EXT_debug_utils, which adopters must not call into unless enabled.
    std::vector<std::string> instanceExtensionNames;
    std::vector<const char*> instanceExtensionPointers;
    VkPhysicalDeviceFeatures2 features = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
    VkPhysicalDeviceVulkan11Features vk11 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES };
    VkPhysicalDeviceVulkan12Features vk12 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
    VkPhysicalDeviceVulkan13Features vk13 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
    VkPhysicalDeviceDescriptorHeapFeaturesEXT descriptorHeap = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_HEAP_FEATURES_EXT };
    VkPhysicalDeviceDescriptorBufferFeaturesEXT descriptorBuffer = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_BUFFER_FEATURES_EXT };
    VkPhysicalDeviceMutableDescriptorTypeFeaturesEXT mutableDescriptorType = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MUTABLE_DESCRIPTOR_TYPE_FEATURES_EXT };
    VkPhysicalDeviceDeviceGeneratedCommandsFeaturesEXT deviceGeneratedCommands = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEVICE_GENERATED_COMMANDS_FEATURES_EXT };
    VkPhysicalDeviceRobustness2FeaturesEXT robustness2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT };
    std::vector<DxvkInteropFeature> grantedFeatures;
    std::vector<DxvkInteropFeature> deniedFeatures;

    /// Links features -> vk11 -> vk12 -> vk13 -> descriptorHeap -> descriptorBuffer -> mutableDescriptorType
    /// -> deviceGeneratedCommands -> robustness2. Adopters find structs by sType, so appending to the chain
    /// keeps older ones working.
    void chain();
  };

  void registerInteropDevice(std::shared_ptr<DxvkInteropDeviceRecord> record);

  std::shared_ptr<const DxvkInteropDeviceRecord> findInteropDevice(VkDevice device);

  void forgetInteropDevice(VkDevice device);

  /**
   * \brief Device teardown callback
   *
   * Invoked from the DxvkDevice destructor before any Vulkan object is destroyed,
   * so that a client recording on the device can retire its work first. Not
   * invoked during DLL process detachment, when no synchronization is possible.
   */
  using DxvkInteropTeardownCallback = void (*)(void* user, VkDevice device);

  void setInteropTeardownCallback(DxvkInteropTeardownCallback callback, void* user);

  void notifyInteropTeardown(VkDevice device);

}
