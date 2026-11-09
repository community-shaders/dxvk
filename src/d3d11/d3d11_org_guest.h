#pragma once

#include "../dxvk/dxvk_device.h"
#include "../dxvk/dxvk_instance.h"

namespace dxvk {

  /**
   * \brief Takes the device a host handed to dxvkSetGuestDevice
   *
   * Called by D3D11 device creation. On success the D3D11 device is created
   * on the host's VkDevice, and the host's device is no longer pending.
   * \param [out] instance The guest instance, on the host's VkInstance
   * \param [out] adapter The host's physical device
   * \param [out] device The DXVK device on the host's VkDevice
   * \returns \c false when no guest device is pending
   */
  bool takeGuestDevice(
          Rc<DxvkInstance>&   instance,
          Rc<DxvkAdapter>&    adapter,
          Rc<DxvkDevice>&     device);

  /**
   * \brief Whether a DXVK device runs on a host's VkDevice
   *
   * Its DXGI swap chains are headless: the host presents.
   */
  bool isGuestDevice(
    const DxvkDevice*         device);

}
