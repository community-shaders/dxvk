#pragma once

/* Shared private ABI between Community Shaders and its bundled DXVK fork. */

#include <stdint.h>
#ifndef __cplusplus
#include <stdbool.h>
#endif
#include <vulkan/vulkan.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Entry points exported by dxvk_d3d11.dll, in export-ordinal order so this can
 * be diffed against src/d3d11/d3d11.def by eye.
 *
 * Typedefs are not linked by name: a mismatch between this header and the .def
 * is silent. Keep them in sync whenever an export is added or removed.
 *
 * Retired ordinals — never reuse one, or a host built against an older header
 * binds it to a new, incompatible function:
 *   100  frame-generation ownership query   (DXVK no longer branches on ownership)
 *   103  external frame-rate cap            (Reflex owns the cap; see FORK.md)
 *   104-108, 115, 119, 123-125              (earlier removals)
 *   111-114  present-wait semaphore state   (the system they served is gone)
 *   116, 117 present begin/completed callbacks
 *   120  enqueue interop command buffer     (the host submits via IDXGIVkInteropDevice)
 *   121  tearing preference                 (DXVK picks the present mode from the sync interval)
 *   122  API version                        (had no caller)
 */

/* @101 */ typedef void (*PFN_dxvkRequestSwapchainRecreate)(void);
/* @102 */ typedef bool (*PFN_csDxvkSwapchainTornDownCallback)(void);
/* @102 */ typedef void (*PFN_dxvkSetSwapchainTornDownCallback)(PFN_csDxvkSwapchainTornDownCallback callback);
/* @109 */ typedef uint64_t (*PFN_dxvkGetPresenterSurfaceState)(uint32_t* format,
                                                               uint32_t* requestedColorSpace,
                                                               uint32_t* effectiveColorSpace);
/* @110 */ typedef void (*PFN_dxvkSetSyncPresent)(uint32_t on);
/* @118 */ typedef void (*PFN_dxvkSetPresentQueueDepth)(uint32_t depth);

#ifdef __cplusplus
}
#endif
