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

/**
 * Called on DXVK's submission thread in stream order with the graphics queue locked
 * (dxvkEnqueueQueueCallback): the client may make queue-level calls on it, such as a GPU
 * profiler's session and pass boundaries, between DXVK's own submissions.
 */
typedef void (*PFN_dxvkOrgInteropQueueCallback)(void* user, VkQueue queue);

/**
 * Called on DXVK's worker thread in D3D11 stream order, outside any render pass, with the
 * command buffer DXVK is recording (dxvkEmitCommandBufferCallback): the client may record
 * commands into it, such as a GPU profiler's ranges around the work that follows.
 */
typedef void (*PFN_dxvkOrgInteropCommandBufferCallback)(void* user, VkCommandBuffer commandBuffer);

/*
 * dxvkSetCommandBufferBoundaryCallbacks: on DXVK's worker thread, outside any render pass,
 * onEnd records into each command buffer that dxvkEmitCommandBufferCallback records into
 * right before it ends, and onBegin into the next one right after it begins. A client keeps
 * what it opened in one command buffer balanced across DXVK's flushes: a GPU profiler closes
 * its open ranges at the end and reopens them at the beginning. Null callbacks clear them.
 */

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


// Stable registration is separate from describing a view. Each call owns one
// lease; backingToken is shared by all registrations of the same native handle.
// Release a lease only after all uses are retired. Submission-owned references
// must retain it independently of the client's registration lifetime.
// Imported Vulkan objects remain owned by the importing client; registration
// retains DXVK wrappers, not the external owner's allocation.
#define DXVK_ORG_RESOURCE_REGISTRATION_VERSION 1u
typedef struct DxvkOrgInteropRegistration {
  uint32_t version;
  uint32_t queueFamily;
  uint64_t leaseToken;
  uint64_t backingToken;
  VkPipelineStageFlags2 legalStages;
  VkAccessFlags2 legalAccess;
  DxvkOrgInteropResourceInfo resource;
} DxvkOrgInteropRegistration;

typedef HRESULT (__stdcall *PFN_dxvkRegisterInteropResource)(ID3D11Device* pDevice,
  IUnknown* pObject, DxvkOrgInteropRegistration* pRegistration);
typedef HRESULT (__stdcall *PFN_dxvkUnregisterInteropResource)(ID3D11Device* pDevice,
  uint64_t leaseToken);

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
 * Several client queue submissions enqueued as one (dxvkEnqueueInteropSubmissions): one flush of the
 * immediate context, one entry in DXVK's stream and one vkQueueSubmit2 carrying every VkSubmitInfo2 in
 * order. pNext chains and flags of the submit infos and of the arrays they point at are ignored.
 */
typedef struct DxvkOrgInteropSubmissionBatch {
  uint32_t version;
  uint32_t submitCount;
  const VkSubmitInfo2* submits;
  /** On DXVK's submission thread, after the vkQueueSubmit2. */
  PFN_dxvkOrgInteropSubmitted onSubmitted;
  void* user;
  /** Optional: a queue label around the submission under a capture tool. */
  const char* label;
} DxvkOrgInteropSubmissionBatch;

/** As dxvkEnqueueInteropSubmission, for a batch of submissions (same ordering and lifetime rules). */
typedef HRESULT (__stdcall *PFN_dxvkEnqueueInteropSubmissions)(ID3D11Device* pDevice,
  const DxvkOrgInteropSubmissionBatch* pBatch);

// Incremental resource interface. Capabilities are explicit: retained submission
// alone does not advertise automatic resource-scoped synchronization.
#define DXVK_ORG_RESOURCE_INTERFACE_VERSION 1u
#define DXVK_ORG_CAP_RESOURCE_REGISTRATION 0x1ull
#define DXVK_ORG_CAP_RETAINED_SUBMISSION 0x2ull
#define DXVK_ORG_CAP_SCOPED_SYNCHRONIZATION 0x4ull

typedef struct DxvkOrgInteropLeasedSubmission {
  uint32_t version;
  DxvkOrgInteropSubmissionBatch batch;
  uint32_t leaseCount;
  const uint64_t* leaseTokens;
  PFN_dxvkOrgInteropSubmitted onCompleted; // GPU retirement, or terminal failure
  void* completionUser;
} DxvkOrgInteropLeasedSubmission;


#define DXVK_ORG_BUFFER_HANDOFF_VERSION 1u
// Development buffer-only interface. The general resource
// interface must not advertise SCOPED_SYNCHRONIZATION until all consumers exist.
typedef struct DxvkOrgInteropBufferAccess {
  uint64_t leaseToken;
  VkDeviceSize offset; // relative to the registered D3D11 buffer range
  VkDeviceSize size;
  VkPipelineStageFlags2 stages;
  VkAccessFlags2 access;
} DxvkOrgInteropBufferAccess;
typedef struct DxvkOrgInteropBufferHandoff {
  uint32_t version;
  DxvkOrgInteropLeasedSubmission submission; // exactly one epoch/submit
  uint32_t accessCount;
  const DxvkOrgInteropBufferAccess* accesses;
} DxvkOrgInteropBufferHandoff;
typedef HRESULT (__stdcall *PFN_dxvkEnqueueBufferHandoff)(void* context, const DxvkOrgInteropBufferHandoff*);

#define DXVK_ORG_RESOURCE_HANDOFF_VERSION 2u
// Image uses name absolute subresources of the registered backing. This version
// requires GENERAL throughout the external epoch; no implicit layout fallback.
typedef struct DxvkOrgInteropImageAccess {
  uint64_t leaseToken;
  VkImageSubresourceRange range;
  VkPipelineStageFlags2 stages;
  VkAccessFlags2 access;
  VkImageLayout layout;
} DxvkOrgInteropImageAccess;
typedef struct DxvkOrgInteropEpochAccesses {
  VkBool32 complete; // incomplete declarations are rejected, including empty lists
  uint32_t bufferCount;
  const DxvkOrgInteropBufferAccess* buffers;
  uint32_t imageCount;
  const DxvkOrgInteropImageAccess* images;
} DxvkOrgInteropEpochAccesses;
typedef struct DxvkOrgInteropResourceHandoff {
  uint32_t version;
  DxvkOrgInteropLeasedSubmission submission;
  uint32_t epochCount; // exactly one manifest per VkSubmitInfo2, in order
  const DxvkOrgInteropEpochAccesses* epochs;
} DxvkOrgInteropResourceHandoff;
typedef HRESULT (__stdcall *PFN_dxvkEnqueueResourceHandoff)(void* context, const DxvkOrgInteropResourceHandoff*);

typedef struct DxvkOrgInteropResourceInterface {
  uint32_t version;
  uint64_t capabilities;
  void* context; // borrowed; valid while the D3D11 device lives
  HRESULT (__stdcall *registerResource)(void*, IUnknown*, DxvkOrgInteropRegistration*);
  HRESULT (__stdcall *unregisterResource)(void*, uint64_t);
  // Copies all arguments. No COM/resource-description calls or worker/GPU waits.
  HRESULT (__stdcall *enqueue)(void*, const DxvkOrgInteropLeasedSubmission*);
} DxvkOrgInteropResourceInterface;

typedef HRESULT (__stdcall *PFN_dxvkGetResourceInteropInterface)(ID3D11Device*, DxvkOrgInteropResourceInterface*);


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

typedef enum DxvkOrgSubmissionKind {
  /** A DXVK command list. */
  DXVK_ORG_SUBMISSION_COMMAND_LIST = 0,
  /** An enqueued interop submission (dxvkEnqueueInteropSubmission). */
  DXVK_ORG_SUBMISSION_EXTERNAL = 1,
  /** A present (no timestamps). */
  DXVK_ORG_SUBMISSION_PRESENT = 2,
} DxvkOrgSubmissionKind;

/** One submission on DXVK's graphics queue. Times are QueryPerformanceCounter values. */
typedef struct DxvkOrgSubmissionTraceRecord {
  uint32_t kind;          /* DxvkOrgSubmissionKind */
  uint32_t flushType;     /* command lists: the GpuFlushType that closed it, ~0u if unknown */
  uint64_t submissionId;  /* command lists flushed by the immediate context: its submission counter value, else 0 */
  int64_t  appQpc;        /* command lists: the application thread issued the flush */
  int64_t  csQpc;         /* command lists: DXVK's CS thread closed the list */
  int64_t  queueQpc;      /* the submission thread handed it to the queue */
  uint64_t gpuBegin;      /* timestamp (device ticks) when the GPU reached it; 0 for presents */
  uint64_t gpuEnd;        /* timestamp once everything up to its end had completed; 0 for presents */
  char     label[64];     /* flush reason for command lists, the client's label for external submissions */
} DxvkOrgSubmissionTraceRecord;

/**
 * Turns the submission trace on or off. While on, DXVK brackets every work
 * submission on its graphics queue with two timestamp-only submissions and
 * keeps a record per submission (a bounded backlog) for
 * dxvkReadSubmissionTrace. Costs two extra queue submissions per submission;
 * for diagnostics only.
 */
typedef HRESULT (__stdcall *PFN_dxvkSetSubmissionTrace)(ID3D11Device* pDevice, BOOL enable);

/**
 * Takes completed trace records, oldest first, up to pCapacity. Stops at the
 * first record whose timestamps are not available yet, so records come out
 * in queue order and none is skipped.
 */
typedef HRESULT (__stdcall *PFN_dxvkReadSubmissionTrace)(ID3D11Device* pDevice,
  DxvkOrgSubmissionTraceRecord* pRecords, uint32_t capacity, uint32_t* pCount);

#ifdef __cplusplus
}
#endif
