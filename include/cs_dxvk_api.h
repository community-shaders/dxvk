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

#define CS_DXVK_API_VERSION 1u

typedef enum CsDxvkFrameGenOwner {
  CS_DXVK_FRAME_GEN_NONE = 0,
  CS_DXVK_FRAME_GEN_FSR = 1,
  CS_DXVK_FRAME_GEN_DLSS_G = 2,
} CsDxvkFrameGenOwner;

typedef struct CsDxvkPresentCallbackInfo {
  uint32_t size;
  uint32_t version;
  uint32_t frameGenOwner;
  uint32_t imageIndex;
  uint64_t frameId;
  uint64_t swapchain;
  uint64_t swapchainSerial;
  uint64_t presenter;
  uint64_t queue;
  uint64_t presentWaitGeneration;
  uint32_t pendingPresentWaitCount;
  int32_t presentResult;
} CsDxvkPresentCallbackInfo;


/* Entry points exported by dxvk_d3d11.dll.
 *
 * Each typedef below is named after the symbol it describes and is listed in
 * export-ordinal order, so this block and src/d3d11/d3d11.def can be diffed by
 * eye. Typedefs are not linked by name, so a mismatch here is silent -- keep
 * the two in sync whenever an export is added, renamed, or removed.
 *
 * Ordinals 104-106, 108, 115, 119 and 123-125 are holes left by exports that
 * were removed (123-125 were the D3D12/DXGI bridge presenter). Do not reuse them:
 * a host built against an older header would bind the old ordinal to a new,
 * incompatible function. Always take the next free ordinal instead. */

/* @100 */ typedef uint32_t (*PFN_csDxvkFrameGenOwnershipQuery)(VkSwapchainKHR swapchain);
/* @100 */ typedef void (*PFN_dxvkSetFrameGenOwnershipQuery)(PFN_csDxvkFrameGenOwnershipQuery query);
/* @101 */ typedef void (*PFN_dxvkRequestSwapchainRecreate)(void);
/* @102 */ typedef bool (*PFN_csDxvkSwapchainTornDownCallback)(void);
/* @102 */ typedef void (*PFN_dxvkSetSwapchainTornDownCallback)(PFN_csDxvkSwapchainTornDownCallback callback);
/* @103 */ typedef void (*PFN_dxvkSetTargetFrameRate)(double fps);

/* @107 Selects whether VkSurfaceFullScreenExclusiveInfoEXT is chained into
 * surface and swapchain queries. 1 = force chain (compositor copy-path
 * presents), 0 = force no-chain (hardware flips), -1 = follow dxvk.allowFse. */
/* @107 */ typedef void (*PFN_dxvkSetFsePNextChain)(int32_t mode);

/* @109 */ typedef uint64_t (*PFN_dxvkGetPresenterSurfaceState)(uint32_t* format,
  uint32_t* requestedColorSpace, uint32_t* effectiveColorSpace);
/* @110 */ typedef void (*PFN_dxvkSetSyncPresent)(uint32_t on);
/* @111 */ typedef uint32_t (*PFN_dxvkGetPresentWaitSemaphoreState)(uint64_t generation);
/* @112 */ typedef uint32_t (*PFN_dxvkClearPresentWaitSemaphore)(uint64_t generation);
/* @113 */ typedef uint32_t (*PFN_dxvkCancelPresentWaitSemaphore)(VkSemaphore semaphore);
/* @114 */ typedef uint32_t (*PFN_dxvkReleaseQueuedPresentWaitSemaphoresAfterIdle)(void);

/* Present callbacks share one signature but are DISTINCT exports with distinct
 * ordering guarantees: PresentBegin runs immediately before vkQueuePresentKHR on
 * the present thread, PresentCompleted immediately after it returns. Both fire
 * only for a DLSS-G-owned swapchain. */
typedef void (*PFN_csDxvkPresentCallback)(const CsDxvkPresentCallbackInfo* info);
/* @116 */ typedef void (*PFN_dxvkSetPresentCompletedCallback)(PFN_csDxvkPresentCallback callback);
/* @117 */ typedef void (*PFN_dxvkSetPresentBeginCallback)(PFN_csDxvkPresentCallback callback);

/* @118 */ typedef void (*PFN_dxvkSetPresentQueueDepth)(uint32_t depth);

/* @120 */ typedef uint64_t (*PFN_dxvkEnqueueInteropCommandBuffer)(VkCommandBuffer commandBuffer,
  VkSemaphore signalSemaphore, VkFence fence);
/* @121 */ typedef void (*PFN_dxvkSetTearingPreference)(uint32_t preference);
/* @122 */ typedef uint32_t (*PFN_dxvkGetCsApiVersion)(void);

#ifdef __cplusplus
}
#endif
