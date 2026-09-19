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
   * \brief What a VkDevice was actually created with
   *
   * Kept for the device's lifetime so that an adopting client can reconstruct
   * the enabled extension and feature set. The feature chain is owned here.
   */
  struct DxvkInteropDeviceRecord {
    VkDevice device = VK_NULL_HANDLE;
    std::vector<std::string> extensionNames;
    std::vector<const char*> extensionPointers;
    VkPhysicalDeviceFeatures2 features = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
    VkPhysicalDeviceVulkan11Features vk11 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES };
    VkPhysicalDeviceVulkan12Features vk12 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
    VkPhysicalDeviceVulkan13Features vk13 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
    VkPhysicalDeviceDescriptorHeapFeaturesEXT descriptorHeap = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_HEAP_FEATURES_EXT };
    std::vector<DxvkInteropFeature> grantedFeatures;
    std::vector<DxvkInteropFeature> deniedFeatures;

    /// Links features -> vk11 -> vk12 -> vk13 -> descriptorHeap.
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
