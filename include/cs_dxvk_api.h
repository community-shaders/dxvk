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

typedef uint32_t (*PFN_csDxvkGetApiVersion)(void);
typedef void (*PFN_csDxvkSetTearingPreference)(uint32_t preference);
typedef uint64_t (*PFN_csDxvkGetPresenterSurfaceState)(uint32_t* format,
  uint32_t* requestedColorSpace, uint32_t* effectiveColorSpace);
typedef uint32_t (*PFN_csDxvkFrameGenOwnershipQuery)(VkSwapchainKHR swapchain);
typedef void (*PFN_csDxvkSetFrameGenOwnershipQuery)(PFN_csDxvkFrameGenOwnershipQuery query);
typedef void (*PFN_csDxvkPresentCallback)(const CsDxvkPresentCallbackInfo* info);
typedef void (*PFN_csDxvkSetPresentCallback)(PFN_csDxvkPresentCallback callback);
typedef void (*PFN_csDxvkRequestSwapchainRecreate)(void);
typedef bool (*PFN_csDxvkSwapchainTornDownCallback)(void);
typedef void (*PFN_csDxvkSetSwapchainTornDownCallback)(PFN_csDxvkSwapchainTornDownCallback callback);
typedef void (*PFN_csDxvkSetTargetFrameRate)(double fps);
typedef void (*PFN_csDxvkSetSyncPresent)(uint32_t on);
typedef void (*PFN_csDxvkSetPresentQueueDepth)(uint32_t depth);
typedef uint64_t (*PFN_csDxvkEnqueueInteropCommandBuffer)(VkCommandBuffer commandBuffer,
  VkSemaphore signalSemaphore, VkFence fence);
typedef uint32_t (*PFN_csDxvkGetPresentWaitSemaphoreState)(uint64_t generation);
typedef uint32_t (*PFN_csDxvkClearPresentWaitSemaphore)(uint64_t generation);
typedef uint32_t (*PFN_csDxvkCancelPresentWaitSemaphore)(VkSemaphore semaphore);
typedef uint32_t (*PFN_csDxvkReleaseQueuedPresentWaitSemaphoresAfterIdle)(void);

#ifdef __cplusplus
}
#endif
