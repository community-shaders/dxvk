#include <mutex>
#include <unordered_map>

#include "dxvk_interop.h"

namespace dxvk {

  namespace {

    struct InteropState {
      std::mutex mutex;
      std::vector<DxvkInteropFeature> request;
      std::unordered_map<VkDevice, std::shared_ptr<DxvkInteropDeviceRecord>> devices;
      DxvkInteropTeardownCallback teardown = nullptr;
      void* teardownUser = nullptr;
    };

    InteropState& state() {
      static InteropState s_state;
      return s_state;
    }

  }


  void setInteropFeatureRequest(std::vector<DxvkInteropFeature> features) {
    std::lock_guard lock(state().mutex);
    state().request = std::move(features);
  }


  std::vector<DxvkInteropFeature> getInteropFeatureRequest() {
    std::lock_guard lock(state().mutex);
    return state().request;
  }


  void DxvkInteropDeviceRecord::chain() {
    features.pNext = &vk11;
    vk11.pNext = &vk12;
    vk12.pNext = &vk13;
    vk13.pNext = &descriptorHeap;
    descriptorHeap.pNext = &deviceGeneratedCommands;
    deviceGeneratedCommands.pNext = &robustness2;
    robustness2.pNext = nullptr;

    extensionPointers.clear();

    for (const auto& name : extensionNames)
      extensionPointers.push_back(name.c_str());

    instanceExtensionPointers.clear();

    for (const auto& name : instanceExtensionNames)
      instanceExtensionPointers.push_back(name.c_str());
  }


  void registerInteropDevice(std::shared_ptr<DxvkInteropDeviceRecord> record) {
    std::lock_guard lock(state().mutex);
    state().devices[record->device] = std::move(record);
  }


  std::shared_ptr<const DxvkInteropDeviceRecord> findInteropDevice(VkDevice device) {
    std::lock_guard lock(state().mutex);
    auto entry = state().devices.find(device);
    return entry != state().devices.end() ? entry->second : nullptr;
  }


  void forgetInteropDevice(VkDevice device) {
    std::lock_guard lock(state().mutex);
    state().devices.erase(device);
  }


  void setInteropTeardownCallback(DxvkInteropTeardownCallback callback, void* user) {
    std::lock_guard lock(state().mutex);
    state().teardown = callback;
    state().teardownUser = user;
  }


  void notifyInteropTeardown(VkDevice device) {
    DxvkInteropTeardownCallback callback = nullptr;
    void* user = nullptr;

    { std::lock_guard lock(state().mutex);
      callback = state().teardown;
      user = state().teardownUser;
    }

    // Called without the lock: the client waits for its own GPU work here.
    if (callback)
      callback(user, device);
  }

}
