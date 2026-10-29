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

#define DXVK_ORG_INTEROP_VERSION 4u

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
  /** Version 3: a queue of a compute-only family that DXVK never uses, or null. The client submits to it itself,
      under a lock of its own; DXVK's resources stay exclusive to its graphics and transfer families. */
  VkQueue computeQueue;
  uint32_t computeQueueFamily;
  uint32_t computeQueueIndex;
  /** Version 4: a transfer-capable queue for the client's uploads that neither DXVK nor computeQueue uses, or null. The
      client submits to it itself, under DXVK's submission lock (LockSubmissionQueue). */
  VkQueue uploadQueue;
  uint32_t uploadQueueFamily;
  uint32_t uploadQueueIndex;
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

/**
 * Descriptor binding models (DxvkOrgInteropBindingModelNegotiation). The values equal BasicRHI's
 * rhi::vulkan::BindingModelBits, so a BasicRHI client can return EvaluateBindingModels' mask as is.
 */
typedef enum DxvkOrgInteropBindingModelBits {
  /** VK_EXT_descriptor_heap: the client is granted descriptorHeap. */
  DXVK_ORG_BINDING_MODEL_DESCRIPTOR_HEAP   = 1u << 0,
  /** VK_EXT_descriptor_buffer: the client is granted descriptorBuffer and mutableDescriptorType. */
  DXVK_ORG_BINDING_MODEL_DESCRIPTOR_BUFFER = 1u << 1,
} DxvkOrgInteropBindingModelBits;

/**
 * Called during the next device creation (each attempt: DXVK retries in a safe mode), once DXVK
 * picked its physical device and its own descriptor model (dxvkPreferredModel: one
 * DxvkOrgInteropBindingModelBits value, or 0 for legacy descriptor sets), before the VkDevice
 * exists. Returns the mask of models the client can record with. Instance-level Vulkan queries on
 * physicalDevice only; must not call into D3D11 or DXVK, and must not throw.
 */
typedef uint32_t (*PFN_dxvkOrgInteropBindingModelCallback)(void* user, VkInstance instance,
  VkPhysicalDevice physicalDevice, PFN_vkGetInstanceProcAddr getInstanceProcAddr, uint32_t dxvkPreferredModel);

#define DXVK_ORG_BINDING_MODEL_NEGOTIATION_VERSION 1u

typedef struct DxvkOrgInteropBindingModelNegotiation {
  /** DXVK_ORG_BINDING_MODEL_NEGOTIATION_VERSION. */
  uint32_t version;
  /** Null clears the callback. */
  PFN_dxvkOrgInteropBindingModelCallback callback;
  void* user;
} DxvkOrgInteropBindingModelNegotiation;

/**
 * dxvkSetBindingModelCallback: a client that records into DXVK's command buffers
 * (dxvkEmitExternalCommands) must bind descriptors the way DXVK does. Call before the device is
 * created, alongside dxvkRequestDeviceFeatures. DXVK then runs on:
 *
 * - its own choice, if the callback accepts it;
 * - else, if DXVK chose heaps or descriptor buffers, the other one if the callback accepts it,
 *   the device supports it (descriptor buffers also need VK_EXT_mutable_descriptor_type) and
 *   dxvk.enableDescriptorHeap / dxvk.enableDescriptorBuffer do not forbid it. A model forced on
 *   with those options is never switched away from;
 * - else its own choice. Legacy descriptor sets (0) are never switched away from.
 *
 * Whatever DXVK runs on, a request for descriptorHeap is granted only on heaps, and descriptorBuffer
 * and mutableDescriptorType only on descriptor buffers (denied features are counted in
 * DxvkOrgInteropDeviceInfo::deniedFeatureCount), with or without a callback. On legacy descriptor
 * sets the client gets neither model. The outcome is in the device's enabledFeatures chain.
 */
typedef HRESULT (__stdcall *PFN_dxvkSetBindingModelCallback)(const DxvkOrgInteropBindingModelNegotiation* pNegotiation);

/**
 * Describes the Vulkan device behind a DXVK D3D11 device. enabledFeatures chains
 * VkPhysicalDeviceVulkan11/12/13Features and the descriptor heap, descriptor buffer, mutable
 * descriptor type, device-generated commands and robustness2 feature structs; walk it by sType
 * (structs may be added without a version change).
 */
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
 * dxvkGetInteropResourceInfo for many objects at once. Every buffer that
 * still needs a stable address is pinned by one chunk on DXVK's worker
 * thread, so the call synchronizes once rather than once per buffer.
 * pInfos[i].version must be DXVK_ORG_INTEROP_VERSION; pResults[i] receives
 * object i's result, as dxvkGetInteropResourceInfo would return it.
 */
typedef HRESULT (__stdcall *PFN_dxvkGetInteropResourceInfos)(ID3D11Device* pDevice,
  UINT count, IUnknown* const* pObjects, DxvkOrgInteropResourceInfo* pInfos, HRESULT* pResults);

#define DXVK_ORG_TRACKED_RESOURCE_VERSION 1u

/**
 * A tracked import: the resource's current storage, and what names it in
 * relocation events. info.version must be DXVK_ORG_INTEROP_VERSION.
 */
typedef enum DxvkOrgInteropTrackedFlagBits {
  /**
   * The resource is pinned as by dxvkGetInteropResourceInfo (it never moves, no
   * relocation is reported): one the GPU writes (a render target, depth, or
   * storage image or buffer), whose old storage would read stale after a move.
   */
  DXVK_ORG_TRACKED_PINNED = 1u << 0,
} DxvkOrgInteropTrackedFlagBits;

typedef struct DxvkOrgInteropTrackedResource {
  /** DXVK_ORG_TRACKED_RESOURCE_VERSION. */
  uint32_t version;
  /** DxvkOrgInteropTrackedFlagBits. */
  uint32_t flags;
  /** Names the resource (not the view) in relocation events; never reused. */
  uint64_t cookie;
  /** The storage info describes: a relocation with a greater serial replaced it. */
  uint64_t storageSerial;
  DxvkOrgInteropResourceInfo info;
} DxvkOrgInteropTrackedResource;

/**
 * Describes resources like dxvkGetInteropResourceInfos, without pinning them.
 *
 * Each call counts one import of each resource (the image behind a texture or
 * view, or the buffer). While a resource has any, DXVK may still move it
 * (defragmentation, eviction, a usage change), but only with moves that copy
 * its contents; renames that discard them are suppressed, the writes happen in
 * place. Each move is reported as a relocation (dxvkPollInteropRelocations)
 * holding the old storage, which stays valid, and in the layout described,
 * until the client releases it. Tracked resources are never evicted.
 *
 * Images are moved to VK_IMAGE_LAYOUT_GENERAL as with dxvkGetInteropResourceInfo.
 * Buffers the application can map are rejected with E_INVALIDARG. The client
 * must keep a reference on each object until it ends the import
 * (dxvkUntrackInteropResources). Synchronizes once with DXVK's worker thread.
 */
typedef HRESULT (__stdcall *PFN_dxvkTrackInteropResources)(ID3D11Device* pDevice,
  UINT count, IUnknown* const* pObjects, DxvkOrgInteropTrackedResource* pResources, HRESULT* pResults);

/**
 * Ends one import of each object's resource (the objects passed to
 * dxvkTrackInteropResources), once the client's work no longer uses it. Any
 * thread; does not synchronize. Relocations reported before still arrive.
 */
typedef HRESULT (__stdcall *PFN_dxvkUntrackInteropResources)(ID3D11Device* pDevice,
  UINT count, IUnknown* const* pObjects);

/** One storage replacement of a tracked resource. */
typedef struct DxvkOrgInteropRelocation {
  /** The resource's cookie (DxvkOrgInteropTrackedResource::cookie). */
  uint64_t cookie;
  /** The new storage's serial: imports describing an older one use the old storage. */
  uint64_t storageSerial;
  /** The old storage, alive until dxvkReleaseInteropStorage. */
  void* storage;
  /** DxvkOrgInteropResourceKind. */
  uint32_t kind;
  uint32_t reserved;
} DxvkOrgInteropRelocation;

/**
 * Takes up to capacity relocations, oldest first. Any thread. Every one taken
 * must be released with dxvkReleaseInteropStorage, including those of
 * resources the client no longer imports.
 */
typedef HRESULT (__stdcall *PFN_dxvkPollInteropRelocations)(ID3D11Device* pDevice,
  DxvkOrgInteropRelocation* pRelocations, uint32_t capacity, uint32_t* pCount);

/**
 * Releases relocations' old storage, once the client's work no longer uses
 * it. Any thread.
 */
typedef HRESULT (__stdcall *PFN_dxvkReleaseInteropStorage)(ID3D11Device* pDevice,
  UINT count, void* const* pStorages);

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

/** DxvkOrgInteropExternalCommands::flags */
typedef enum DxvkOrgInteropExternalCommandsFlagBits {
  /**
   * The callback binds state of its own: pipelines, pipeline layouts, descriptor heaps or
   * descriptor buffers, push constants or push data, vertex and index buffers, dynamic state.
   * Afterwards DXVK forgets every binding it tracks and sets up everything again before its
   * next draw or dispatch, its descriptor heaps or buffers included.
   */
  DXVK_ORG_EXTERNAL_COMMANDS_BINDS_STATE   = 1u << 0,
  /**
   * DXVK records a full memory barrier after the callback (all commands, memory writes to all
   * commands, memory reads and writes). Without it the client must release its own accesses,
   * see dxvkEmitExternalCommands.
   */
  DXVK_ORG_EXTERNAL_COMMANDS_FULL_BARRIER  = 1u << 1,
} DxvkOrgInteropExternalCommandsFlagBits;

#define DXVK_ORG_EXTERNAL_COMMANDS_VERSION 1u

typedef struct DxvkOrgInteropExternalCommands {
  /** DXVK_ORG_EXTERNAL_COMMANDS_VERSION. */
  uint32_t version;
  /** DxvkOrgInteropExternalCommandsFlagBits. */
  uint32_t flags;
  PFN_dxvkOrgInteropCommandBufferCallback callback;
  void* user;
  /**
   * Timeline semaphore waits joined to the graphics queue submission that executes
   * the command buffer the commands land in, so they block DXVK's commands in that submission
   * too, at the given stages (0 means all commands). Copied at the call; pNext and deviceIndex
   * are ignored.
   */
  uint32_t waitCount;
  const VkSemaphoreSubmitInfo* waits;
  /**
   * Timeline semaphore signals joined to that same submission: each signals once everything up
   * to the end of the submission completed, the client's commands included, which is when the
   * client may release what they use. Values must grow in stream order per semaphore; signals of
   * one semaphore landing in one submission fold into the highest. Stages 0 means all commands.
   * Copied at the call; pNext and deviceIndex are ignored.
   */
  uint32_t signalCount;
  const VkSemaphoreSubmitInfo* signals;
  /** Optional: a debug label region around the commands under a capture tool. Copied at the call. */
  const char* label;
} DxvkOrgInteropExternalCommands;

/**
 * dxvkEmitExternalCommands: like dxvkEmitCommandBufferCallback, for a client that records real
 * work into DXVK's command buffer. The callback runs on DXVK's worker thread in D3D11 stream
 * order, once, with the execution command buffer DXVK is recording:
 *
 * - Outside any render pass: DXVK ended its pass (not suspended), so every image is in the
 *   layout DXVK keeps it in (DxvkOrgInteropImageInfo::layout) and deferred clears are done.
 * - After DXVK flushed its barriers: every access DXVK made before is visible to the stages
 *   the resource can be used in.
 * - The client must leave every image it touched in that layout, end every render pass and
 *   query it began, and cover its own accesses with barriers: DXVK does not see them, and its
 *   barrier tracker is empty after the callback, so its next access to a resource the client
 *   wrote emits no barrier. Either release them (an exit barrier from the client's stages and
 *   accesses to all commands, memory reads and writes) or set
 *   DXVK_ORG_EXTERNAL_COMMANDS_FULL_BARRIER.
 * - Without DXVK_ORG_EXTERNAL_COMMANDS_BINDS_STATE the callback must bind nothing.
 */
typedef HRESULT (__stdcall *PFN_dxvkEmitExternalCommands)(ID3D11Device* pDevice,
  const DxvkOrgInteropExternalCommands* pCommands);

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

/* ---------------------------------------------------------------------------
 * VRAM attribution
 *
 * Every DXVK buffer and image carries a label: the owner scope active on the
 * thread that created it (dxvkPushResourceOwner), DXVK's internal kind for
 * its own resources, the API object it backs and the debug name the
 * application gave that object. Labels are kept independent of DXVK_DEBUG.
 * ------------------------------------------------------------------------- */

#define DXVK_MEMORY_REPORT_VERSION 1u

/** Allocation flags of a resource's current storage (DxvkAllocationFlag bits). */
#define DXVK_MEMORY_RESOURCE_OWNS_MEMORY   (1u << 0)  /* dedicated VkDeviceMemory */
#define DXVK_MEMORY_RESOURCE_CAN_MOVE      (1u << 4)
#define DXVK_MEMORY_RESOURCE_IMPORTED      (1u << 5)  /* memory DXVK did not allocate, e.g. dxvkCreateBufferFromVkBuffer */
#define DXVK_MEMORY_RESOURCE_ORG_RETIRED   (1u << 8)

/** One live DXVK buffer or image and its current storage. */
typedef struct DxvkMemoryResourceRow {
  uint64_t cookie;
  /** Cookie of the resource this one is part of (a texture's mapped subresource buffers), or 0. */
  uint64_t parentCookie;
  /** Size of the current storage in bytes. */
  uint64_t size;
  uint64_t memory;        /* VkDeviceMemory, 0 for imported or sparse storage */
  uint64_t offset;
  uint32_t flags;         /* DXVK_MEMORY_RESOURCE_* */
  uint32_t isImage;
  uint32_t isSparse;
  uint32_t memoryType;    /* ~0u for imported or sparse storage */
  uint32_t heapIndex;     /* ~0u for imported or sparse storage */
  uint32_t format;        /* VkFormat, images only */
  uint32_t width;         /* buffers: size in bytes, clamped */
  uint32_t height;
  uint32_t depth;
  uint32_t mipLevels;
  uint32_t layers;
  uint32_t usage;         /* VkImageUsageFlags or VkBufferUsageFlags */
  /** Interned strings, valid while the device lives; null when absent. */
  const char* owner;      /* the creating thread's owner scope */
  const char* kind;       /* DXVK's internal kind, e.g. "Staging buffer" */
  const char* origin;     /* e.g. "D3D11 buffer", "D3D11 texture", "D3D11 mapped subresource" */
  char name[96];          /* debug name the application gave the API object, truncated */
} DxvkMemoryResourceRow;

/** One memory heap: Vulkan's view of it, and DXVK's share. */
typedef struct DxvkMemoryHeapReport {
  uint32_t version;       /* DXVK_MEMORY_REPORT_VERSION */
  uint32_t heapIndex;
  uint32_t heapFlags;     /* VkMemoryHeapFlags */
  uint32_t hasVkBudget;   /* VK_EXT_memory_budget is supported: vkUsage and vkBudget are valid */
  uint64_t heapSize;
  uint64_t vkUsage;       /* process-wide usage of the heap (VK_EXT_memory_budget) */
  uint64_t vkBudget;
  uint64_t dxvkBudget;    /* DXVK's own budget for the heap */
  uint64_t dxvkAllocated; /* VkDeviceMemory DXVK allocated: chunks and dedicated allocations */
  uint64_t dxvkUsed;      /* suballocated and dedicated memory in use, caches included */
  uint64_t cacheBytes;    /* free allocations held by DXVK's shared allocation caches */
  uint64_t orgRetiredBytes; /* old storages a client holds after relocations (dxvkReleaseInteropStorage) */
} DxvkMemoryHeapReport;

/**
 * Pushes an owner scope on the calling thread: every buffer and image DXVK
 * creates on this thread until the matching pop is labeled with it,
 * including those created by D3D11 calls (CreateBuffer, CreateTexture2D, a
 * staging texture's first Map). The string is copied when a resource is
 * created, so it must stay valid until the pop. Scopes nest.
 */
typedef void (__stdcall *PFN_dxvkPushResourceOwner)(const char* owner);
typedef void (__stdcall *PFN_dxvkPopResourceOwner)(void);

/**
 * Sets the owner of a resource that already exists: a buffer, a texture, or
 * the texture behind a shader resource view. Without overwrite, a resource
 * that was created in an owner scope keeps that owner. The string is copied.
 */
typedef HRESULT (__stdcall *PFN_dxvkSetResourceOwner)(ID3D11Device* pDevice,
  IUnknown* pObject, const char* pOwner, BOOL overwrite);

/**
 * Asks for a snapshot of every live buffer and image. It is taken on DXVK's
 * CS thread, in stream order, and replaces the previous one once complete;
 * the call does not wait for it.
 */
typedef HRESULT (__stdcall *PFN_dxvkRequestMemorySnapshot)(ID3D11Device* pDevice);

/**
 * Copies the last completed snapshot, up to capacity rows. *pCount receives
 * the snapshot's row count, and *pSerial (optional) the snapshot's serial, 0
 * before the first one completes. Returns S_FALSE if capacity was too small.
 */
typedef HRESULT (__stdcall *PFN_dxvkReadMemorySnapshot)(ID3D11Device* pDevice,
  DxvkMemoryResourceRow* pRows, uint32_t capacity, uint32_t* pCount, uint64_t* pSerial);

/**
 * Fills one report per memory heap, up to capacity (VK_MAX_MEMORY_HEAPS is
 * enough). *pCount receives the heap count. Set each report's version.
 */
typedef HRESULT (__stdcall *PFN_dxvkGetMemoryHeapReports)(ID3D11Device* pDevice,
  DxvkMemoryHeapReport* pReports, uint32_t capacity, uint32_t* pCount);

/** Debug features of VRAM attribution (dxvkConfigureMemoryDebug). */
#define DXVK_MEMORY_DEBUG_CREATION_STACKS (1u << 0)  /* record each buffer's and image's creation stack */

/**
 * Sets the process-wide VRAM attribution debug flags. Resources created
 * afterwards are affected, so call it before creating the device. Creation
 * stacks cost a stack walk per resource.
 */
typedef void (__stdcall *PFN_dxvkConfigureMemoryDebug)(uint32_t flags);

/**
 * Copies a live resource's creation stack (return addresses, innermost
 * first; the first frames are DXVK's own) up to capacity. *pCount receives
 * the stack's depth. S_FALSE if none was recorded or the resource is gone.
 */
typedef HRESULT (__stdcall *PFN_dxvkGetResourceCreationStack)(ID3D11Device* pDevice,
  uint64_t cookie, void** pFrames, uint32_t capacity, uint32_t* pCount);

#ifdef __cplusplus
}

namespace dxvk {
  struct DxvkDrawCaller {
    const void* caller = nullptr;
    uint32_t stackCount = 0;
    const void* stack[16]{};
  };
  DxvkDrawCaller TakeDrawCaller() noexcept;

  std::string TakeNextShaderName();
}
#endif
