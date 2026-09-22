#pragma once

/*
 * C ABI for in-process clients that record their own Vulkan work on DXVK's
 * device (e.g. an OpenRenderGraph runtime adopting the VkDevice). Exported
 * from dxvk_d3d11.dll; resolve with GetProcAddress.
 *
 * All structs are versioned: set `version` to DXVK_ORG_INTEROP_VERSION.
 */

#include <stdint.h>

#include "../vulkan/vulkan_loader.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DXVK_ORG_INTEROP_VERSION 2u

struct ID3D11Device;
struct ID3D11Buffer;
struct D3D11_BUFFER_DESC;
struct IUnknown;

typedef struct DxvkOrgInteropFeature {
  /** Extension exposing the feature, or null for core features. */
  const char* extension;
  /** Feature struct member name, e.g. "descriptorHeap". */
  const char* feature;
} DxvkOrgInteropFeature;

typedef struct DxvkOrgInteropFeatureRequest {
  uint32_t version;
  uint32_t featureCount;
  const DxvkOrgInteropFeature* features;
} DxvkOrgInteropFeatureRequest;

typedef struct DxvkOrgInteropDeviceInfo {
  uint32_t version;
  /** The loader entry point DXVK itself uses (it may be an interposer's). */
  PFN_vkGetInstanceProcAddr getInstanceProcAddr;
  VkInstance instance;
  uint32_t instanceApiVersion;
  VkPhysicalDevice physicalDevice;
  VkDevice device;
  VkQueue graphicsQueue;
  uint32_t graphicsQueueFamily;
  uint32_t graphicsQueueIndex;
  /** Everything the device was created with. Valid while the device lives. */
  uint32_t enabledExtensionCount;
  const char* const* enabledExtensions;
  const VkPhysicalDeviceFeatures2* enabledFeatures;
  /** Outcome of dxvkRequestDeviceFeatures for this device. */
  uint32_t grantedFeatureCount;
  uint32_t deniedFeatureCount;
  /** The instance's enabled extensions (version 2), e.g. whether VK_EXT_debug_utils may be called. */
  uint32_t enabledInstanceExtensionCount;
  const char* const* enabledInstanceExtensions;
} DxvkOrgInteropDeviceInfo;

typedef enum DxvkOrgInteropResourceKind {
  DXVK_ORG_INTEROP_RESOURCE_BUFFER = 1,
  DXVK_ORG_INTEROP_RESOURCE_IMAGE  = 2,
} DxvkOrgInteropResourceKind;

typedef struct DxvkOrgInteropBufferInfo {
  /** A D3D11 buffer is a range of a (possibly shared) VkBuffer. */
  VkBuffer buffer;
  VkDeviceSize offset;
  VkDeviceSize size;
  /** Device address of the range's first byte. */
  VkDeviceAddress address;
  VkBufferUsageFlags usage;
} DxvkOrgInteropBufferInfo;

typedef struct DxvkOrgInteropImageInfo {
  VkImage image;
  VkImageType type;
  VkFormat format;
  VkImageCreateFlags flags;
  VkExtent3D extent;
  uint32_t mipLevels;
  uint32_t arrayLayers;
  VkSampleCountFlagBits samples;
  VkImageUsageFlags usage;
  /** The layout DXVK keeps the image in between its own commands. */
  VkImageLayout layout;
  /** For a shader resource view: the view DXVK created for it. For a texture: the whole image. */
  VkImageViewType viewType;
  VkFormat viewFormat;
  VkComponentMapping components;
  VkImageSubresourceRange subresourceRange;
} DxvkOrgInteropImageInfo;

typedef struct DxvkOrgInteropResourceInfo {
  uint32_t version;
  /** DxvkOrgInteropResourceKind; selects buffer or image below. */
  uint32_t kind;
  DxvkOrgInteropBufferInfo buffer;
  DxvkOrgInteropImageInfo image;
} DxvkOrgInteropResourceInfo;

typedef void (*PFN_dxvkOrgInteropTeardown)(void* user, VkDevice device);

typedef void (*PFN_dxvkOrgInteropSubmitted)(void* user, VkResult result);

typedef struct DxvkOrgInteropSubmission {
  uint32_t version;
  /** Copied at the call; pNext of each element must be null. */
  uint32_t waitCount;
  const VkSemaphoreSubmitInfo* waits;
  uint32_t commandBufferCount;
  const VkCommandBufferSubmitInfo* commandBuffers;
  uint32_t signalCount;
  const VkSemaphoreSubmitInfo* signals;
  /** Optional. Called on DXVK's submission thread once vkQueueSubmit2 returned. */
  PFN_dxvkOrgInteropSubmitted onSubmitted;
  void* user;
  /** Optional (version 2). Copied; a queue label around the submission while a capture tool is attached. */
  const char* label;
} DxvkOrgInteropSubmission;

/**
 * Requests device features for the next D3D11 device. Call before the device
 * is created. Features are enabled on the VkDevice only; DXVK's own code paths
 * keep the feature set they would have chosen. Unsupported features are
 * skipped and reported as denied in the device info.
 */
typedef HRESULT (__stdcall *PFN_dxvkRequestDeviceFeatures)(const DxvkOrgInteropFeatureRequest* pRequest);

/** Describes the Vulkan device behind a DXVK D3D11 device. */
typedef HRESULT (__stdcall *PFN_dxvkGetInteropDeviceInfo)(ID3D11Device* pDevice, DxvkOrgInteropDeviceInfo* pInfo);

/**
 * Wraps a client-owned VkBuffer as a D3D11 buffer. The client keeps ownership
 * of the buffer and its memory, which must outlive the D3D11 object. Only
 * D3D11_USAGE_DEFAULT buffers without CPU access are allowed: DXVK never
 * renames, relocates or maps an external buffer. The VkBuffer must carry every
 * usage the D3D11 bind flags imply (texel/storage for SRV/UAV, transfer, and
 * shader device address).
 */
typedef HRESULT (__stdcall *PFN_dxvkCreateBufferFromVkBuffer)(ID3D11Device* pDevice,
  const D3D11_BUFFER_DESC* pDesc, VkBuffer buffer, ID3D11Buffer** ppBuffer);

/**
 * Registers a callback invoked when DXVK destroys its VkDevice, before any
 * Vulkan object is destroyed, so the client can retire its work first. Not
 * invoked during process detachment. Pass null to unregister.
 */
typedef HRESULT (__stdcall *PFN_dxvkSetDeviceTeardownCallback)(PFN_dxvkOrgInteropTeardown pCallback, void* pUser);

/**
 * Describes the Vulkan resource behind a D3D11 buffer, texture or shader
 * resource view, and marks it stable: DXVK will not relocate or rename it
 * from now on (the same lock its NVX interop paths use), so the handles and
 * addresses stay valid for the resource's lifetime. The client must keep a
 * reference on the D3D11 object while it uses them.
 *
 * Buffers that the application can map (D3D11_USAGE_DYNAMIC or CPU access)
 * are renamed by every discard map and cannot be made stable; they are
 * rejected with E_INVALIDARG. Buffer views are not supported.
 *
 * Synchronizes with DXVK's worker thread; call it outside hot paths.
 */
typedef HRESULT (__stdcall *PFN_dxvkGetInteropResourceInfo)(ID3D11Device* pDevice,
  IUnknown* pObject, DxvkOrgInteropResourceInfo* pInfo);

/**
 * Submits client command buffers to DXVK's graphics queue in D3D11 stream
 * order: they execute after every command issued on the immediate context
 * before this call and before every command issued after it. Nothing is
 * waited for; the submission happens later on DXVK's submission thread.
 * Must be called from the thread that uses the immediate context. The
 * command buffers and semaphores must stay valid until onSubmitted runs (and
 * the command buffers until the client's signals complete).
 */
typedef HRESULT (__stdcall *PFN_dxvkEnqueueInteropSubmission)(ID3D11Device* pDevice,
  const DxvkOrgInteropSubmission* pSubmission);

/**
 * Returns the address of the immediate context's submission counter, which
 * grows by one each time DXVK closes a command list and hands it to the
 * queue: implicit flushes, explicit Flush() and the flush ahead of an
 * enqueued interop submission. Two D3D11 commands issued while the counter
 * holds the same value are in the same Vulkan submission, so a timestamp
 * query pair around them measures only the GPU work between them; a pair
 * across a change also counts any time the queue sat idle between the two
 * submissions. Valid for the device's lifetime. Written only by the thread
 * using the immediate context, which is also the one that should read it.
 */
typedef HRESULT (__stdcall *PFN_dxvkGetSubmissionCounter)(ID3D11Device* pDevice,
  const volatile uint64_t** ppCounter);

#ifdef __cplusplus
}
#endif
