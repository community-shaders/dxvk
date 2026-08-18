#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <limits>
#include <mutex>

#include "dxvk_device.h"
#include "dxvk_presenter.h"

#include "../../include/cs_dxvk_api.h"

#include "../wsi/wsi_window.h"

namespace dxvk {

  extern "C" uint32_t dxvkGetCsApiVersion() {
    return CS_DXVK_API_VERSION;
  }

  static std::atomic<int32_t> g_dxvkTearingPreference = { -1 };

  // 0 = force tear-free, 1 = allow tearing, 2 = use DXVK configuration.
  extern "C" void dxvkSetTearingPreference(uint32_t preference) {
    g_dxvkTearingPreference.store(preference <= 1u ? int32_t(preference) : -1,
      std::memory_order_release);
  }

  struct PresenterSurfaceStateSnapshot {
    uint64_t serial = 0;
    uint32_t format = VK_FORMAT_UNDEFINED;
    uint32_t requestedColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    uint32_t effectiveColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
  };

  static std::mutex g_dxvkPresenterSurfaceStateMutex;
  static PresenterSurfaceStateSnapshot g_dxvkPresenterSurfaceState;

  static uint64_t getPresenterSurfaceState(
          uint32_t* format,
          uint32_t* requestedColorSpace,
          uint32_t* effectiveColorSpace) {
    std::lock_guard lock(g_dxvkPresenterSurfaceStateMutex);

    if (!g_dxvkPresenterSurfaceState.serial)
      return 0;

    if (format)
      *format = g_dxvkPresenterSurfaceState.format;
    if (requestedColorSpace)
      *requestedColorSpace = g_dxvkPresenterSurfaceState.requestedColorSpace;
    if (effectiveColorSpace)
      *effectiveColorSpace = g_dxvkPresenterSurfaceState.effectiveColorSpace;

    return g_dxvkPresenterSurfaceState.serial;
  }

  extern "C" uint64_t dxvkGetPresenterSurfaceState(
          uint32_t* format,
          uint32_t* requestedColorSpace,
          uint32_t* effectiveColorSpace) {
    return getPresenterSurfaceState(format, requestedColorSpace, effectiveColorSpace);
  }

  static void publishPresenterSurfaceState(
          VkFormat format,
          VkColorSpaceKHR requestedColorSpace,
          VkColorSpaceKHR effectiveColorSpace) {
    std::lock_guard lock(g_dxvkPresenterSurfaceStateMutex);

    g_dxvkPresenterSurfaceState.serial += 1;
    g_dxvkPresenterSurfaceState.format = uint32_t(format);
    g_dxvkPresenterSurfaceState.requestedColorSpace = uint32_t(requestedColorSpace);
    g_dxvkPresenterSurfaceState.effectiveColorSpace = uint32_t(effectiveColorSpace);
  }

  // External FFX frame-generation ownership predicate, set by the CS WSI hook. Returns true for the
  // external FFX frame-interpolation swapchain; when owned, the Presenter becomes a thin submit +
  // hand-off and lets FFX own the present loop. Null => never owned.
  // 0 = normal swapchain, 1 = FSR replacement swapchain, 2 = DLSS-G proxy.
  std::atomic<uint32_t (*)(VkSwapchainKHR)> g_dxvkFrameGenOwnsSwapchain = { nullptr };

  extern "C" void dxvkSetFrameGenOwnershipQuery(uint32_t (*query)(VkSwapchainKHR)) {
    g_dxvkFrameGenOwnsSwapchain.store(query, std::memory_order_release);
  }

  using DxvkPresentCallbackInfo = CsDxvkPresentCallbackInfo;

  // Presenter-scoped callbacks. The payload prevents global Streamline state
  // from consuming options, markers, or semaphore generations for an unrelated
  // swapchain when more than one DXVK presenter exists.
  std::atomic<void (*)(const DxvkPresentCallbackInfo*)> g_dxvkPresentCompletedCallback = { nullptr };

  extern "C" void dxvkSetPresentCompletedCallback(void (*callback)(const DxvkPresentCallbackInfo*)) {
    g_dxvkPresentCompletedCallback.store(callback, std::memory_order_release);
  }

  // Runs immediately before vkQueuePresentKHR on the same thread. Streamline
  // requires DLSS-G option changes to be ordered with the Present that consumes
  // them; issuing SetOptions from the D3D render thread races DXVK's async
  // presenter and can wedge the plugin pacer during a mode transition.
  std::atomic<void (*)(const DxvkPresentCallbackInfo*)> g_dxvkPresentBeginCallback = { nullptr };

  extern "C" void dxvkSetPresentBeginCallback(void (*callback)(const DxvkPresentCallbackInfo*)) {
    g_dxvkPresentBeginCallback.store(callback, std::memory_order_release);
  }

  // One-shot request to recreate the Vulkan swapchain on the next acquire. CS uses this on a DLSS-G ->
  // FSR frame-gen switch: sl.dlss_g's sticky present proxy bypasses the Vulkan present hooks, so FSR
  // never returns VK_SUBOPTIMAL to trigger a recreate. A DXVK-internal recreate preserves the D3D11
  // back buffers and re-runs vkCreate/DestroySwapchainKHR, releasing the proxy.
  std::atomic<bool> g_dxvkForceSwapchainRecreate = { false };

  extern "C" void dxvkRequestSwapchainRecreate() {
    g_dxvkForceSwapchainRecreate.store(true, std::memory_order_release);
  }

  // Callback invoked inside recreateSwapChain() while DXVK owns no swapchain (after destroy, before create).
  // Returning false keeps the presenter empty so an external wrapper that failed teardown cannot overlap
  // a replacement. The callback is invoked again on the next recreation attempt. Runs under m_surfaceMutex.
  std::atomic<bool (*)()> g_dxvkSwapchainTornDownCallback = { nullptr };

  extern "C" void dxvkSetSwapchainTornDownCallback(bool (*cb)()) {
    g_dxvkSwapchainTornDownCallback.store(cb, std::memory_order_release);
  }

  bool notifySwapchainTornDown() {
    if (auto cb = g_dxvkSwapchainTornDownCallback.load(std::memory_order_acquire); cb && !cb()) {
      Logger::err("Presenter: External swapchain teardown incomplete; deferring recreation.");
      return false;
    }
    return true;
  }

  // External frame-rate cap, in fps, driven by CS to pace presents when nothing else does (FSR frame
  // generation forces Reflex off, so it has no Reflex limiter). NaN => unset, 0 => unlimited, >0 => cap.
  // Reconciled into m_fpsLimiter every present via applyExternalFrameRateLimit().
  std::atomic<double> g_dxvkExternalFrameRate = { std::numeric_limits<double>::quiet_NaN() };

  extern "C" void dxvkSetTargetFrameRate(double fps) {
    g_dxvkExternalFrameRate.store(fps < 0.0 ? 0.0 : fps, std::memory_order_release);
  }

  enum class PresentWaitState : uint32_t {
    None = 0,
    Pending = 1,
    Queued = 2,
    Uncertain = 3,
    Released = 4,
  };

  struct PresentWaitSnapshot {
    VkSemaphore semaphore = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    uint64_t swapchainSerial = 0;
    uint64_t generation = 0;
    uint32_t imageIndex = 0;
    PresentWaitState state = PresentWaitState::None;
    bool attached = false;
    bool eligible = false;
  };

  static constexpr uint32_t MaxPresentWaitSnapshots = 64;
  static std::mutex g_dxvkPresentWaitMutex;
  static std::array<PresentWaitSnapshot, MaxPresentWaitSnapshots> g_dxvkPresentWaits;
  static uint64_t g_dxvkNextPresentWaitGeneration = 0;
  static uint64_t g_dxvkNextPresentWaitSwapchainSerial = 0;

  // Pending records form a generation-ordered FIFO. DXVK's D3D11 present path
  // is asynchronous, so the app may register several frames before the Vulkan
  // presenter consumes them. Queued records remain keyed until reacquire proof.
  static uint64_t createPresentWaitSemaphore(VkSemaphore sem) {
    if (sem == VK_NULL_HANDLE)
      return 0;

    std::lock_guard lock(g_dxvkPresentWaitMutex);
    PresentWaitSnapshot* freeSnapshot = nullptr;
    for (auto& snapshot : g_dxvkPresentWaits) {
      if (!freeSnapshot && snapshot.state == PresentWaitState::None)
        freeSnapshot = &snapshot;
    }
    if (!freeSnapshot)
      return 0;

    uint64_t generation = ++g_dxvkNextPresentWaitGeneration;
    if (!generation)
      generation = ++g_dxvkNextPresentWaitGeneration;
    *freeSnapshot = { sem, VK_NULL_HANDLE, 0, generation, 0, PresentWaitState::Pending, false, false };
    return generation;
  }

  uint64_t reservePresentWaitSemaphore(VkSemaphore sem) {
    return createPresentWaitSemaphore(sem);
  }

  void activatePresentWaitSemaphore(uint64_t generation) {
    std::lock_guard lock(g_dxvkPresentWaitMutex);
    for (auto& snapshot : g_dxvkPresentWaits) {
      if (snapshot.generation == generation && snapshot.state == PresentWaitState::Pending) {
        snapshot.eligible = true;
        return;
      }
    }
  }

  void failPresentWaitSemaphore(uint64_t generation) {
    std::lock_guard lock(g_dxvkPresentWaitMutex);
    for (auto& snapshot : g_dxvkPresentWaits) {
      if (snapshot.generation == generation && snapshot.state == PresentWaitState::Pending) {
        snapshot.state = PresentWaitState::Uncertain;
        return;
      }
    }
  }

  // Cancellation is valid only while the exact registered handle is still pending.
  extern "C" uint32_t dxvkCancelPresentWaitSemaphore(VkSemaphore sem) {
    if (sem == VK_NULL_HANDLE)
      return 0;

    std::lock_guard lock(g_dxvkPresentWaitMutex);
    for (auto& snapshot : g_dxvkPresentWaits) {
      if (snapshot.state == PresentWaitState::Pending && !snapshot.attached &&
          snapshot.semaphore == sem) {
        snapshot = { };
        return 1;
      }
    }
    return 0;
  }

  extern "C" uint32_t dxvkGetPresentWaitSemaphoreState(uint64_t generation) {
    std::lock_guard lock(g_dxvkPresentWaitMutex);
    if (generation) {
      for (const auto& snapshot : g_dxvkPresentWaits) {
        if (snapshot.generation == generation)
          return uint32_t(snapshot.state);
      }
    }
    return uint32_t(PresentWaitState::None);
  }

  // Only a reacquire-proven generation may be removed from the ownership table.
  extern "C" uint32_t dxvkClearPresentWaitSemaphore(uint64_t generation) {
    std::lock_guard lock(g_dxvkPresentWaitMutex);
    if (generation) {
      for (auto& snapshot : g_dxvkPresentWaits) {
        if (snapshot.generation == generation && snapshot.state == PresentWaitState::Released) {
          snapshot = { };
          return 1;
        }
      }
    }
    return 0;
  }

  // The caller must prove the Vulkan device idle before releasing registered
  // waits. Pending entries were never attached to a present; queued entries may
  // have been consumed. Device idle makes both safe to retire.
  extern "C" uint32_t dxvkReleaseQueuedPresentWaitSemaphoresAfterIdle() {
    std::lock_guard lock(g_dxvkPresentWaitMutex);
    uint32_t released = 0;
    for (auto& snapshot : g_dxvkPresentWaits) {
      if (snapshot.state == PresentWaitState::Pending ||
          snapshot.state == PresentWaitState::Queued) {
        snapshot.state = PresentWaitState::Released;
        released += 1;
      }
    }
    return released;
  }

  void releasePresentWaitsForImage(
      VkSwapchainKHR swapchain, uint64_t swapchainSerial, uint32_t imageIndex) {
    std::lock_guard lock(g_dxvkPresentWaitMutex);
    for (auto& snapshot : g_dxvkPresentWaits) {
      if (snapshot.state == PresentWaitState::Queued && snapshot.swapchain == swapchain &&
          snapshot.swapchainSerial == swapchainSerial && snapshot.imageIndex == imageIndex)
        snapshot.state = PresentWaitState::Released;
    }
  }

  void releasePresentWaitsForSwapchain(
      VkSwapchainKHR swapchain, uint64_t swapchainSerial) {
    std::lock_guard lock(g_dxvkPresentWaitMutex);
    for (auto& snapshot : g_dxvkPresentWaits) {
      if (snapshot.state == PresentWaitState::Queued && snapshot.swapchain == swapchain &&
          snapshot.swapchainSerial == swapchainSerial)
        snapshot.state = PresentWaitState::Released;
    }
  }

  // Present-path override. Chaining VkSurfaceFullScreenExclusiveInfoEXT with DISALLOWED makes
  // the NVIDIA ICD route presents through the compositor GDI-copy path; not chaining it (the
  // spec default, and the dxvk default here via allowFse=false) yields hardware flips, which
  // Streamline DLSS-G's pacer requires (its hardware present wedges on the copy path) and
  // FSR-FG runs fine on. 1 = force chain (copy), 0 = force no-chain (flips), -1 = default.
  // Note the asymmetry if ever forcing: the driver flip-locks a window at its first flip
  // present, so copy is only reachable before any flip has occurred.
  std::atomic<int32_t> g_dxvkFsePNextChainOverride = { -1 };

  extern "C" void dxvkSetFsePNextChain(int32_t mode) {
    g_dxvkFsePNextChainOverride.store(mode, std::memory_order_release);
  }

  const std::array<std::pair<VkColorSpaceKHR, VkColorSpaceKHR>, 2> Presenter::s_colorSpaceFallbacks = {{
    { VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT, VK_COLOR_SPACE_HDR10_ST2084_EXT },

    { VK_COLOR_SPACE_HDR10_ST2084_EXT, VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT },
  }};


  Presenter::Presenter(
    const Rc<DxvkDevice>&   device,
    const Rc<sync::Signal>& signal,
    const PresenterDesc&    desc,
          PresenterSurfaceProc&& proc)
  : m_device(device), m_signal(signal),
    m_vki(device->instance()->vki()),
    m_vkd(device->vkd()),
    m_surfaceProc(std::move(proc)) {
    // Only enable FSE if the user explicitly opts in. On Windows, FSE
    // is required to support VRR or HDR, but blocks alt-tabbing or
    // overlapping windows, which breaks a number of games.
    m_fullscreenMode = m_device->config().allowFse
      ? VK_FULL_SCREEN_EXCLUSIVE_ALLOWED_EXT
      : VK_FULL_SCREEN_EXCLUSIVE_DISALLOWED_EXT;

    updateFsePNextChainMode();

    // Create Vulkan surface immediately if possible, but ignore
    // certain errors since the app window may still be in use in
    // some way at this point, e.g. by a different device.
    if (!desc.deferSurfaceCreation) {
      VkResult vr = createSurface();

      if (vr != VK_SUCCESS && vr != VK_ERROR_NATIVE_WINDOW_IN_USE_KHR)
        throw DxvkError(str::format("Failed to create Vulkan surface, ", vr));
    }

    // KHR and EXT variants of this extension are completely identical
    m_hasSwapchainMaintenance1 = m_device->features().khrSwapchainMaintenance1.swapchainMaintenance1
                              || m_device->features().extSwapchainMaintenance1.swapchainMaintenance1;

    // Gamescope WSI is currently broken and doesn't properly signal
    // the present fence if presentation is queued but fails.
    // TODO Remove this hack when this gets fixed in stable SteamOS.
    m_hasGamescopeFenceSignalBug = env::getEnvVar("ENABLE_GAMESCOPE_WSI") == "1";
  }

  
  Presenter::~Presenter() {
    destroySwapchain();
    destroySurface();
    destroyLatencySemaphore();

    if (m_frameThread.joinable()) {
      { std::lock_guard lock(m_frameMutex);

        m_frameQueue.push(PresenterFrame());
        m_frameCond.notify_one();
      }

      m_frameThread.join();
    }
  }


  VkResult Presenter::checkSwapChainStatus() {
    std::lock_guard lock(m_surfaceMutex);

    if (!m_swapchain)
      return recreateSwapChain();

    return VK_SUCCESS;
  }


  VkResult Presenter::acquireNextImage(PresenterSync& sync, Rc<DxvkImage>& image) {
    std::unique_lock lock(m_surfaceMutex);

    // Don't acquire more than one image at a time
    VkResult status = VK_SUCCESS;

    m_surfaceCond.wait(lock, [this, &status] {
      status = m_device->getDeviceStatus();
      return !m_presentPending || status < 0;
    });

    if (status < 0)
      return status;

    // Ensure that the swap chain gets recreated if it is dirty
    bool hasSwapchain = m_swapchain != VK_NULL_HANDLE;

    if (!updateSwapChain())
      return VK_NOT_READY;

    // Don't acquire if we already did so after present
    if (m_acquireStatus == VK_NOT_READY && m_swapchain) {
      PresenterSync sync = m_semaphores.at(m_frameIndex);

      waitForSwapchainFence(sync);

      m_acquireStatus = m_vkd->vkAcquireNextImageKHR(m_vkd->device(),
        m_swapchain, std::numeric_limits<uint64_t>::max(),
        sync.acquire, VK_NULL_HANDLE, &m_imageIndex);
    }

    // This is a normal occurence, but may be useful for
    // debugging purposes in case WSI goes wrong somehow.
    if (m_acquireStatus != VK_SUCCESS && m_swapchain)
      Logger::info(str::format("Presenter: Got ", m_acquireStatus, ", recreating swapchain"));

    // If the swap chain is out of date, recreate it and retry. It
    // is possible that we do not get a new swap chain here, e.g.
    // because the window is minimized.
    if (m_acquireStatus != VK_SUCCESS || !m_swapchain) {
      VkResult vr = recreateSwapChain();

      if (vr == VK_NOT_READY && hasSwapchain)
        Logger::info("Presenter: Surface does not allow swapchain creation.");

      if (vr != VK_SUCCESS)
        return softError(vr);

      PresenterSync sync = m_semaphores.at(m_frameIndex);

      m_acquireStatus = m_vkd->vkAcquireNextImageKHR(m_vkd->device(),
        m_swapchain, std::numeric_limits<uint64_t>::max(),
        sync.acquire, VK_NULL_HANDLE, &m_imageIndex);

      if (m_acquireStatus < 0) {
        Logger::info(str::format("Presenter: Got ", m_acquireStatus, " from fresh swapchain"));
        return softError(m_acquireStatus);
      }
    }

    // Update HDR metadata after a successful acquire. We know
    // that there won't be a present in flight at this point.
    if (m_hdrMetadataDirty && m_hdrMetadata) {
      m_hdrMetadataDirty = false;

      if (m_device->features().extHdrMetadata) {
        m_vkd->vkSetHdrMetadataEXT(m_vkd->device(),
          1, &m_swapchain, &(*m_hdrMetadata));
      }
    }

    // Apply latency sleep mode if the swapchain supports it
    if (m_latencySleepModeDirty && m_latencySleepMode) {
      m_latencySleepModeDirty = false;

      if (m_latencySleepSupported) {
        m_vkd->vkSetLatencySleepModeNV(m_vkd->device(),
          m_swapchain, &(*m_latencySleepMode));
      }
    }

    // Set dynamic present mode for the next frame if possible. Clamp the index to the actual count:
    // an externally-wrapped swapchain (FFX frame-generation) can expose only ONE dynamic present mode,
    // and the bare .at(1) for a non-zero sync interval then throws std::out_of_range ("invalid vector
    // subscript") and crashes the present thread. Falling back to mode 0 just drops the vsync-interval
    // distinction for that frame, which is harmless.
    if (!m_dynamicModes.empty()) {
      size_t modeIndex = (m_preferredSyncInterval ? 1u : 0u);
      if (modeIndex >= m_dynamicModes.size())
        modeIndex = m_dynamicModes.size() - 1u;
      m_presentMode = m_dynamicModes.at(modeIndex);
    }

    // Return relevant Vulkan objects for the acquired image.
    // An externally-wrapped swapchain (the FFX FrameInterpolationSwapChain installed for FSR frame
    // generation) can recreate the underlying VkSwapchainKHR out from under us, leaving m_imageIndex /
    // m_frameIndex stale relative to our cached m_images / m_semaphores arrays — vkAcquireNextImageKHR
    // then hands back an index that no longer fits. Guard the .at() accesses: a stale index is treated as
    // a soft out-of-date so this present is skipped and the swapchain recreated next frame, instead of
    // throwing std::out_of_range ("invalid vector subscript") and killing the present thread.
    if (m_imageIndex >= m_images.size() || m_frameIndex >= m_semaphores.size())
      return softError(VK_ERROR_OUT_OF_DATE_KHR);

    sync = m_semaphores.at(m_frameIndex);
    image = m_images.at(m_imageIndex);

    m_presentPending = true;
    return m_acquireStatus;
  }


  VkResult Presenter::presentImage(uint64_t frameId, const Rc<DxvkLatencyTracker>& tracker) {
    PresenterSync& currSync = m_semaphores.at(m_frameIndex);

    releasePresentWaitsForImage(m_swapchain, m_presentWaitSwapchainSerial, m_imageIndex);

    VkPresentIdKHR presentId = { VK_STRUCTURE_TYPE_PRESENT_ID_KHR };
    presentId.swapchainCount = 1;
    presentId.pPresentIds   = &frameId;

    VkPresentId2KHR presentId2 = { VK_STRUCTURE_TYPE_PRESENT_ID_2_KHR };
    presentId2.swapchainCount = 1;
    presentId2.pPresentIds  = &frameId;

    VkSwapchainPresentFenceInfoKHR fenceInfo = { VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_FENCE_INFO_KHR };
    fenceInfo.swapchainCount = 1;
    fenceInfo.pFences       = &currSync.fence;

    VkSwapchainPresentModeInfoKHR modeInfo = { VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_MODE_INFO_KHR };
    modeInfo.swapchainCount = 1;
    modeInfo.pPresentModes  = &m_presentMode;

    // App-provided extra present-wait semaphore: queue-orders
    // the present after external work the app submitted outside DXVK's own timeline — used by
    // Streamline DLSS-G so its evaluate/tag submissions are GPU-ordered ahead of the present
    // that reads them (pure queue sync, no CPU stall; the generated frames flash without it).
    std::array<VkSemaphore, 2> waitSemaphores = { currSync.present, VK_NULL_HANDLE };
    uint32_t waitSemaphoreCount = 1;

    uint64_t extraWaitGeneration = 0;
    uint32_t pendingPresentWaitCount = 0;
    // Present-tag waits belong exclusively to the DLSS-G proxy swapchain.
    // Never let an auxiliary presenter consume the process-global FIFO.
    bool dlssgOwned = m_dlssgOwned.load(std::memory_order_acquire);
    if (dlssgOwned) {
      std::lock_guard lock(g_dxvkPresentWaitMutex);
      PresentWaitSnapshot* oldestPending = nullptr;
      for (auto& snapshot : g_dxvkPresentWaits) {
        if (snapshot.state == PresentWaitState::Pending && snapshot.eligible && !snapshot.attached &&
            snapshot.semaphore != VK_NULL_HANDLE) {
          pendingPresentWaitCount += 1;
          if (!oldestPending || snapshot.generation < oldestPending->generation)
            oldestPending = &snapshot;
        }
      }
      if (oldestPending) {
        waitSemaphores[waitSemaphoreCount++] = oldestPending->semaphore;
        extraWaitGeneration = oldestPending->generation;
        oldestPending->swapchain = m_swapchain;
        oldestPending->swapchainSerial = m_presentWaitSwapchainSerial;
        oldestPending->imageIndex = m_imageIndex;
        oldestPending->attached = true;
      }
    }

    VkPresentInfoKHR info = { VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
    info.waitSemaphoreCount = waitSemaphoreCount;
    info.pWaitSemaphores    = waitSemaphores.data();
    info.swapchainCount     = 1;
    info.pSwapchains        = &m_swapchain;
    info.pImageIndices      = &m_imageIndex;

    // FFX's replacement vkQueuePresentKHR does not handle DXVK's extension pNext structs
    // (present-ID, present-fence, present-mode from swapchain_maintenance1). Passing them
    // causes the present fence to never be signaled, hanging the 4th acquire when DXVK's
    // waitForSwapchainFence blocks on the unsignaled fence. Pass a clean VkPresentInfoKHR.
    bool fgOwned = m_frameGenOwned.load();

    // FSR's replacement swapchain rejects these pNext structures, but DLSS-G's
    // pacer requires the present ID for real-frame completion tracking.
    if ((!fgOwned || dlssgOwned) && frameId && m_hasPresentId) {
      if (m_device->features().khrPresentId2.presentId2)
        presentId2.pNext = const_cast<void*>(std::exchange(info.pNext, &presentId2));
      else
        presentId.pNext = const_cast<void*>(std::exchange(info.pNext, &presentId));
    }

    if (!fgOwned && m_hasSwapchainMaintenance1) {
      modeInfo.pNext = const_cast<void*>(std::exchange(info.pNext, &modeInfo));
      fenceInfo.pNext = const_cast<void*>(std::exchange(info.pNext, &fenceInfo));
    }

    // DLSS-G's input tags and intercepted present must share one queue order.
    // DXVK's submission thread externally synchronizes this call with graphics
    // submissions. Other presenters retain the dedicated present queue.
    VkQueue presentQueue = dlssgOwned
      ? m_device->queues().graphics.queueHandle
      : m_device->queues().present.queueHandle;
    DxvkPresentCallbackInfo callbackInfo = {
      sizeof(DxvkPresentCallbackInfo), 1u, dlssgOwned ? 2u : (fgOwned ? 1u : 0u), m_imageIndex,
      frameId, uint64_t(reinterpret_cast<uintptr_t>(m_swapchain)), m_presentWaitSwapchainSerial,
      uint64_t(reinterpret_cast<uintptr_t>(this)),
      uint64_t(reinterpret_cast<uintptr_t>(presentQueue)), extraWaitGeneration,
      pendingPresentWaitCount, int32_t(VK_NOT_READY)
    };
    if (dlssgOwned) {
      if (auto callback = g_dxvkPresentBeginCallback.load(std::memory_order_acquire))
        callback(&callbackInfo);
    }

    VkResult status = m_vkd->vkQueuePresentKHR(presentQueue, &info);

    // Keep option delivery, markers, and state queries on the real present thread.
    // Community Shaders associates the returned input-completion timeline with
    // the corresponding resource slot and polls it before reuse. No GPU queue
    // wait is injected here, avoiding a wait-before-signal cycle.
    if (dlssgOwned) {
      callbackInfo.presentResult = int32_t(status);
      if (auto callback = g_dxvkPresentCompletedCallback.load(std::memory_order_acquire))
        callback(&callbackInfo);
    }

    if (extraWaitGeneration) {
      PresentWaitState waitState = PresentWaitState::Uncertain;
      switch (status) {
        case VK_SUCCESS:
        case VK_SUBOPTIMAL_KHR:
        case VK_ERROR_OUT_OF_DATE_KHR:
        case VK_ERROR_SURFACE_LOST_KHR:
        case VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT:
        case VK_ERROR_PRESENT_TIMING_QUEUE_FULL_EXT:
          waitState = PresentWaitState::Queued;
          break;
        default:
          break;
      }

      std::lock_guard lock(g_dxvkPresentWaitMutex);
      for (auto& snapshot : g_dxvkPresentWaits) {
        if (snapshot.generation == extraWaitGeneration &&
            snapshot.state == PresentWaitState::Pending && snapshot.attached) {
          snapshot.state = waitState;
          break;
        }
      }
    }

    // Maintain valid state if presentation succeeded, even if we want to
    // recreate the swapchain. Spec says that 'queue' operations, i.e. the
    // semaphore and fence signals, still happen if present fails with
    // normal swapchain errors, such as OUT_OF_DATE or SURFACE_LOST.
    if (!fgOwned && m_hasSwapchainMaintenance1) {
      currSync.fenceSignaled = status != VK_ERROR_OUT_OF_DEVICE_MEMORY
                            && status != VK_ERROR_OUT_OF_HOST_MEMORY
                            && status != VK_ERROR_DEVICE_LOST;

      if (m_hasGamescopeFenceSignalBug)
        currSync.fenceSignaled = status >= 0;
    }

    if (status >= 0) {
      m_acquireStatus = VK_NOT_READY;

      m_frameIndex += 1;
      m_frameIndex %= m_semaphores.size();
    }

    // Add frame to waiter queue with current properties. Skipped for an FFX frame-generation-owned
    // swapchain: its present-wait worker is disabled (FFX does the display pacing) and signalFrame
    // releases the frame-latency signal directly on this thread instead.
    if (m_hasPresentWait && !m_frameGenOwned.load()) {
      std::lock_guard lock(m_frameMutex);

      auto& frame = m_frameQueue.emplace();
      frame.frameId = frameId;
      frame.tracker = tracker;
      frame.mode = m_presentMode;
      frame.result = status;

      m_frameCond.notify_one();
    }

    // On a successful present, try to acquire next image already, in
    // order to hide potential delays from the application thread.
    //
    // For an FFX frame-generation-owned swapchain this MUST be skipped: FFX's own present thread
    // drives acquire on the same wrapped swapchain object, so re-entering it here from the submit
    // thread (concurrently with FFX's present thread) deadlocks — the present hands off to FFX and
    // never returns. With this skipped, the per-frame acquire always happens in acquireNextImage()
    // (one acquire per frame), matching the official "app acquires once, FFX owns the rest" model.
    if (status == VK_SUCCESS && !m_frameGenOwned.load()) {
      PresenterSync& nextSync = m_semaphores.at(m_frameIndex);
      waitForSwapchainFence(nextSync);

      m_acquireStatus = m_vkd->vkAcquireNextImageKHR(m_vkd->device(),
        m_swapchain, std::numeric_limits<uint64_t>::max(),
        nextSync.acquire, VK_NULL_HANDLE, &m_imageIndex);
    }

    // Recreate the swapchain on the next acquire, even if we get suboptimal.
    // There is no guarantee that suboptimal state is returned by both functions.
    std::lock_guard lock(m_surfaceMutex);

    if (status != VK_SUCCESS) {
      Logger::info(str::format("Presenter: Got ", status, ", recreating swapchain"));

      m_dirtySwapchain = true;
    }

    m_presentPending = false;
    m_surfaceCond.notify_one();
    return status;
  }


  void Presenter::signalFrame(
          uint64_t                frameId,
    const Rc<DxvkLatencyTracker>& tracker) {
    if (m_signal == nullptr || !frameId)
      return;

    // FFX frame-generation-owned swapchain: FFX owns presentation + display pacing and DXVK's
    // present-wait worker is disabled, so nothing else will ever complete this frame. Release the
    // frame-latency signal immediately on the submit thread (no DXVK fps limiter — FFX paces).
    if (m_frameGenOwned.load()) {
      // Under full interposition every swapchain reports frame-gen-owned, so this is the ONLY present
      // path that runs and the two delay() sites below are dead. Apply the external frame-rate cap here
      // so CS can limit present rate when nothing else paces it — notably FSR frame generation, which
      // forces Reflex off and so has no Reflex limiter. No-op unless CS set a cap via dxvkSetTargetFrameRate.
      applyExternalFrameRateLimit();
      m_fpsLimiter.delay();

      // FFX paces display; treat the frame as completed immediately so any path that checks the
      // present-wait bookkeeping (m_lastCompleted) is satisfied too, then release the latency signal.
      { std::lock_guard lock(m_frameMutex);
        m_lastSignaled = frameId;
        m_lastCompleted = frameId;
      }

      m_signal->signal(frameId);

      if (tracker)
        tracker->notifyGpuPresentEnd(frameId);

      return;
    }

    if (m_hasPresentWait) {
      bool canSignal = false;

      { std::unique_lock lock(m_frameMutex);

        m_lastSignaled = frameId;
        canSignal = m_lastCompleted >= frameId;
      }

      if (canSignal)
        m_signal->signal(frameId);
    } else {
      applyExternalFrameRateLimit();
      m_fpsLimiter.delay();
      m_signal->signal(frameId);

      if (tracker)
        tracker->notifyGpuPresentEnd(frameId);
    }
  }


  bool Presenter::supportsColorSpace(VkColorSpaceKHR colorspace) {
    std::lock_guard lock(m_surfaceMutex);

    if (!m_surface) {
      VkResult vr = createSurface();

      if (vr != VK_SUCCESS)
        return false;
    }

    std::vector<VkSurfaceFormatKHR> surfaceFormats;
    getSupportedFormats(surfaceFormats);

    for (const auto& surfaceFormat : surfaceFormats) {
      if (surfaceFormat.colorSpace == colorspace)
        return true;

      for (const auto& fallback : s_colorSpaceFallbacks) {
        if (fallback.first == colorspace && fallback.second == surfaceFormat.colorSpace)
          return true;
      }
    }

    if (colorspace == VK_COLOR_SPACE_HDR10_ST2084_EXT) {
      Logger::warn(str::format("Presenter: HDR10 unsupported; query returned ",
        surfaceFormats.size(), " surface format(s); FSE pNext=", m_chainFseInfo ? "yes" : "no"));
      for (const auto& surfaceFormat : surfaceFormats) {
        Logger::warn(str::format("Presenter:   format=", surfaceFormat.format,
          ", colorSpace=", surfaceFormat.colorSpace));
      }
    }

    return false;
  }


  void Presenter::invalidateSurface() {
    std::lock_guard lock(m_surfaceMutex);

    m_dirtySurface = true;
  }


  void Presenter::destroyResources() {
    std::unique_lock lock(m_surfaceMutex);

    m_surfaceCond.wait(lock, [this] {
      VkResult status = m_device->getDeviceStatus();
      return !m_presentPending || status < 0;
    });

    destroySwapchain();
    destroySurface();
  }


  void Presenter::setLatencySleepModeNv(
    const VkLatencySleepModeInfoNV& sleepMode) {
    std::unique_lock lock(m_surfaceMutex);

    if (sleepMode.sType != VK_STRUCTURE_TYPE_LATENCY_SLEEP_MODE_INFO_NV)
      return;

    if (sleepMode.pNext)
      Logger::warn("Presenter: Extended sleep mode info not supported");

    // Avoid creating a swapchain with low-latency features
    // enabled if the functionality isn't required
    bool isDefault = !sleepMode.lowLatencyMode
                  && !sleepMode.lowLatencyBoost
                  && !sleepMode.minimumIntervalUs;

    if (!m_latencySleepMode && isDefault)
      return;

    m_dirtySwapchain |= !m_latencySleepMode;

    if (m_latencySleepMode) {
      m_latencySleepModeDirty |=
        m_latencySleepMode->lowLatencyMode != sleepMode.lowLatencyMode ||
        m_latencySleepMode->lowLatencyBoost != sleepMode.lowLatencyBoost ||
        m_latencySleepMode->minimumIntervalUs != sleepMode.minimumIntervalUs;
    }

    m_latencySleepMode = sleepMode;
    m_latencySleepMode->pNext = nullptr;
  }


  dxvk::high_resolution_clock::time_point Presenter::setLatencyMarkerNv(
          uint64_t                frameId,
          VkLatencyMarkerNV       marker) {
    std::unique_lock lock(m_surfaceMutex);

    if (!m_latencySleepMode) {
      // Applications may use latency markers without enabling
      // low-latency mode, make sure we have a compatible swapchain
      m_latencySleepMode = { VK_STRUCTURE_TYPE_LATENCY_SLEEP_MODE_INFO_NV };
      m_dirtySwapchain = true;

      return dxvk::high_resolution_clock::now();
    }

    // Return a CPU timestamp to correlate timestamps from
    // latency frame reports with actual CPU timestamps
    auto t0 = dxvk::high_resolution_clock::now();

    if (m_latencySleepSupported) {
      VkSetLatencyMarkerInfoNV info = { VK_STRUCTURE_TYPE_SET_LATENCY_MARKER_INFO_NV };
      info.presentID = frameId;
      info.marker = marker;

      m_vkd->vkSetLatencyMarkerNV(m_vkd->device(), m_swapchain, &info);
    }

    auto t1 = dxvk::high_resolution_clock::now();
    return t0 + (t1 - t0) / 2u;
  }


  dxvk::high_resolution_clock::duration Presenter::latencySleepNv() {
    std::unique_lock lock(m_surfaceMutex);

    if (!m_latencySleepSupported)
      return dxvk::high_resolution_clock::duration(0u);

    if (!m_latencySemaphore) {
      if (createLatencySemaphore() != VK_SUCCESS)
        return dxvk::high_resolution_clock::duration(0u);
    }

    VkLatencySleepInfoNV info = { VK_STRUCTURE_TYPE_LATENCY_SLEEP_INFO_NV };
    info.signalSemaphore = m_latencySemaphore;
    info.value = ++m_latencySleepCounter;

    m_vkd->vkLatencySleepNV(m_vkd->device(), m_swapchain, &info);

    lock.unlock();

    auto t0 = dxvk::high_resolution_clock::now();

    VkSemaphoreWaitInfo waitInfo = { VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO };
    waitInfo.semaphoreCount = 1;
    waitInfo.pSemaphores = &info.signalSemaphore;
    waitInfo.pValues = &info.value;

    m_vkd->vkWaitSemaphores(m_vkd->device(), &waitInfo, ~0ull);

    auto t1 = dxvk::high_resolution_clock::now();
    return t1 - t0;
  }


  uint32_t Presenter::getLatencyTimingsNv(
          uint32_t                timingCount,
          VkLatencyTimingsFrameReportNV* timings) {
    std::unique_lock lock(m_surfaceMutex);

    if (!m_latencySleepSupported)
      return 0u;

    VkGetLatencyMarkerInfoNV info = { VK_STRUCTURE_TYPE_GET_LATENCY_MARKER_INFO_NV };
    info.timingCount = timingCount;
    info.pTimings = timings;

    m_vkd->vkGetLatencyTimingsNV(m_vkd->device(), m_swapchain, &info);
    return info.timingCount;
  }


  void Presenter::setSyncInterval(uint32_t syncInterval) {
    std::lock_guard lock(m_surfaceMutex);

    // Normalize sync interval for present modes. We currently
    // cannot support anything other than 1 natively anyway.
    syncInterval = std::min(syncInterval, 1u);

    if (m_preferredSyncInterval != syncInterval) {
      m_preferredSyncInterval = syncInterval;

      if (m_dynamicModes.empty())
        m_dirtySwapchain = true;
    }
  }


  void Presenter::setFrameRateLimit(double frameRate, uint32_t maxLatency) {
    m_frameRateLimitLatency = maxLatency;

    // An active external cap (CS) takes precedence over the swapchain's DXGI target; don't let a swapchain
    // re-push clobber it. applyExternalFrameRateLimit() re-asserts the override each present regardless.
    if (std::isnan(g_dxvkExternalFrameRate.load(std::memory_order_acquire)))
      m_fpsLimiter.setTargetFrameRate(frameRate, maxLatency);
  }


  void Presenter::applyExternalFrameRateLimit() {
    double external = g_dxvkExternalFrameRate.load(std::memory_order_acquire);
    if (!std::isnan(external))
      m_fpsLimiter.setTargetFrameRate(external, m_frameRateLimitLatency);
  }


  void Presenter::setSurfaceFormat(VkSurfaceFormatKHR format) {
    std::lock_guard lock(m_surfaceMutex);

    if (m_preferredFormat.format != format.format || m_preferredFormat.colorSpace != format.colorSpace) {
      m_preferredFormat = format;
      m_dirtySwapchain = true;
    }
  }


  void Presenter::setSurfaceExtent(VkExtent2D extent) {
    std::lock_guard lock(m_surfaceMutex);

    if (m_preferredExtent != extent) {
      m_preferredExtent = extent;
      m_dirtySwapchain = true;
    }
  }


  void Presenter::setHdrMetadata(VkHdrMetadataEXT hdrMetadata) {
    std::lock_guard lock(m_surfaceMutex);

    if (hdrMetadata.sType != VK_STRUCTURE_TYPE_HDR_METADATA_EXT) {
      m_hdrMetadata = std::nullopt;
      return;
    }

    if (hdrMetadata.pNext)
      Logger::warn("Presenter: HDR metadata extensions not currently supported.");

    m_hdrMetadata = hdrMetadata;
    m_hdrMetadata->pNext = nullptr;

    m_hdrMetadataDirty = true;
  }


  VkResult Presenter::recreateSwapChain() {
    VkResult vr;

    if (m_swapchain)
      destroySwapchain();

    // Confirm external wrappers are gone before publishing another swapchain. This still runs when
    // m_swapchain is already null, allowing a retryable teardown failure to complete on a later attempt.
    if (!notifySwapchainTornDown())
      return VK_NOT_READY;

    if (m_surface) {
      vr = createSwapChain();

      if (vr == VK_ERROR_SURFACE_LOST_KHR)
        destroySurface();
    }

    if (!m_surface) {
      vr = createSurface();

      if (vr == VK_SUCCESS)
        vr = createSwapChain();
    }

    return vr;
  }


  bool Presenter::updateSwapChain() {
    // Honour a one-shot external recreate request (CS frame-gen method switch — see
    // g_dxvkForceSwapchainRecreate above). Treated as a dirty surface so the swapchain is fully torn
    // down and rebuilt, evicting any sticky external present proxy (sl.dlss_g) on the way.
    if (g_dxvkForceSwapchainRecreate.exchange(false, std::memory_order_acq_rel))
      m_dirtySurface = true;

    if (m_dirtySurface || m_dirtySwapchain) {
      destroySwapchain();
      m_dirtySwapchain = false;
      if (!notifySwapchainTornDown())
        return false;
    }

    if (m_dirtySurface) {
      destroySurface();
      m_dirtySurface = false;
    }

    return true;
  }


  void Presenter::updateFsePNextChainMode() {
    // Default: chain only when FSE is actually opted into — chaining an explicit DISALLOWED
    // costs the hardware-flip present path (see g_dxvkFsePNextChainOverride).
    bool chain = m_device->config().allowFse;

    int32_t forced = g_dxvkFsePNextChainOverride.load(std::memory_order_acquire);
    if (forced >= 0)
      chain = forced != 0;

    // A/B diagnostic override.
    if (const char* v = std::getenv("DXVK_CHAIN_FSE"); v && v[0] == '1')
      chain = true;

    if (chain != m_chainFseInfo)
      Logger::info(str::format("Presenter: FSE pNext ", chain ? "chained (copy-path presents)"
                                                              : "not chained (flip-model presents)"));
    m_chainFseInfo = chain;
  }


  VkResult Presenter::createSwapChain() {
    // The frame-generation method may have changed since the last (re)create.
    updateFsePNextChainMode();

    VkSurfaceFullScreenExclusiveInfoEXT fullScreenExclusiveInfo = { VK_STRUCTURE_TYPE_SURFACE_FULL_SCREEN_EXCLUSIVE_INFO_EXT };
    fullScreenExclusiveInfo.fullScreenExclusive = m_fullscreenMode;

    VkPhysicalDeviceSurfaceInfo2KHR surfaceInfo = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SURFACE_INFO_2_KHR };
    surfaceInfo.surface = m_surface;

    if (m_device->features().extFullScreenExclusive && m_chainFseInfo)
      surfaceInfo.pNext = &fullScreenExclusiveInfo;

    // Query surface capabilities. Some properties might have changed,
    // including the size limits and supported present modes, so we'll
    // just query everything again.
    VkSurfaceCapabilitiesPresentWait2KHR presentWait2Caps = { VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_PRESENT_WAIT_2_KHR };
    VkSurfaceCapabilitiesPresentId2KHR presentId2Caps = { VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_PRESENT_ID_2_KHR };

    VkSurfaceCapabilities2KHR caps = { VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_2_KHR };

    if (m_device->features().khrPresentId2.presentId2) {
      presentId2Caps.pNext = std::exchange(caps.pNext, &presentId2Caps);

      if (m_device->features().khrPresentWait2.presentWait2)
        presentWait2Caps.pNext = std::exchange(caps.pNext, &presentWait2Caps);
    }

    std::vector<VkSurfaceFormatKHR> formats;
    std::vector<VkPresentModeKHR> modes;

    VkResult status;

    if (m_device->instance()->extensions().khrGetSurfaceCapabilities2.specVersion) {
      status = m_vki->vkGetPhysicalDeviceSurfaceCapabilities2KHR(
        m_device->adapter()->handle(), &surfaceInfo, &caps);
    } else {
      status = m_vki->vkGetPhysicalDeviceSurfaceCapabilitiesKHR(
        m_device->adapter()->handle(), m_surface, &caps.surfaceCapabilities);
    }

    if (status) {
      Logger::err(str::format("Presenter: Failed to get surface capabilities: ", status));
      return status;
    }

    // Select image extent based on current surface capabilities, and return
    // immediately if we cannot create an actual swap chain.
    VkExtent2D imageExtent = pickImageExtent(caps.surfaceCapabilities, m_preferredExtent);

    if (!imageExtent.width || !imageExtent.height)
      return VK_NOT_READY;

    // Select format based on swap chain properties
    if ((status = getSupportedFormats(formats)))
      return status;

    VkSurfaceFormatKHR surfaceFormat = pickSurfaceFormat(formats.size(), formats.data(), m_preferredFormat);

    // Set up image format list for mutable swap chain if necessary
    small_vector<VkFormat, 2> viewFormats = { };

    auto formatPair = vk::getSrgbFormatPair(surfaceFormat.format);

    if (formatPair.second) {
      viewFormats.push_back(formatPair.first);
      viewFormats.push_back(formatPair.second);
    }

    // Select a present mode for the current sync interval
    if ((status = getSupportedPresentModes(modes)))
      return status;

    m_presentMode = pickPresentMode(modes.size(), modes.data(), m_preferredSyncInterval);

    // Check whether we can change present modes dynamically. This may
    // influence the image count as well as further swap chain creation.
    std::vector<VkPresentModeKHR> dynamicModes = {{
      pickPresentMode(modes.size(), modes.data(), 0),
      pickPresentMode(modes.size(), modes.data(), 1),
    }};

    std::vector<VkPresentModeKHR> compatibleModes;

    // As for the minimum image count, start with the most generic value
    // that works with all present modes.
    uint32_t minImageCount = caps.surfaceCapabilities.minImageCount;
    uint32_t maxImageCount = caps.surfaceCapabilities.maxImageCount;

    if (m_hasSwapchainMaintenance1) {
      VkSurfacePresentModeCompatibilityKHR compatibleModeInfo = { VK_STRUCTURE_TYPE_SURFACE_PRESENT_MODE_COMPATIBILITY_KHR };

      VkSurfacePresentModeKHR presentModeInfo = { VK_STRUCTURE_TYPE_SURFACE_PRESENT_MODE_KHR };
      presentModeInfo.pNext = const_cast<void*>(std::exchange(surfaceInfo.pNext, &presentModeInfo));
      presentModeInfo.presentMode = m_presentMode;

      caps.pNext = &compatibleModeInfo;

      if ((status = m_vki->vkGetPhysicalDeviceSurfaceCapabilities2KHR(
          m_device->adapter()->handle(), &surfaceInfo, &caps))) {
        Logger::err(str::format("Presenter: Failed to get surface capabilities: ", status));
        return status;
      }

      compatibleModes.resize(compatibleModeInfo.presentModeCount);
      compatibleModeInfo.pPresentModes = compatibleModes.data();

      if ((status = m_vki->vkGetPhysicalDeviceSurfaceCapabilities2KHR(
          m_device->adapter()->handle(), &surfaceInfo, &caps))) {
        Logger::err(str::format("Presenter: Failed to get surface capabilities: ", status));
        return status;
      }

      // Remove modes we don't need for the purpose of finding the minimum
      // image count, as well as for swap chain creation later.
      compatibleModes.erase(std::remove_if(compatibleModes.begin(), compatibleModes.end(),
        [&dynamicModes] (VkPresentModeKHR mode) {
          return std::find(dynamicModes.begin(), dynamicModes.end(), mode) == dynamicModes.end();
        }), compatibleModes.end());

      minImageCount = 0;
      caps.pNext = nullptr;

      for (auto mode : compatibleModes) {
        presentModeInfo.presentMode = mode;

        if ((status = m_vki->vkGetPhysicalDeviceSurfaceCapabilities2KHR(
            m_device->adapter()->handle(), &surfaceInfo, &caps))) {
          Logger::err(str::format("Presenter: Failed to get surface capabilities: ", status));
          return status;
        }

        minImageCount = std::max(minImageCount, caps.surfaceCapabilities.minImageCount);

        if (caps.surfaceCapabilities.maxImageCount) {
          maxImageCount = maxImageCount
            ? std::min(maxImageCount, caps.surfaceCapabilities.maxImageCount)
            : caps.surfaceCapabilities.maxImageCount;
        }
      }

      // If any required mode is not supported for dynamic present
      // mode switching, clear the dynamic mode array.
      for (auto mode : dynamicModes) {
        if (std::find(compatibleModes.begin(), compatibleModes.end(), mode) == compatibleModes.end()) {
          dynamicModes.clear();
          break;
        }
      }
    } else if (dynamicModes[0] != dynamicModes[1]) {
      // If we can't switch modes dynamically, clear the
      // array so that setSyncInterval errors out properly.
      dynamicModes.clear();
    }

    // Compute swap chain image count based on available info
    VkSurfaceFullScreenExclusiveInfoEXT fullScreenInfo = { VK_STRUCTURE_TYPE_SURFACE_FULL_SCREEN_EXCLUSIVE_INFO_EXT };
    fullScreenInfo.fullScreenExclusive = m_fullscreenMode;

    VkSwapchainPresentModesCreateInfoKHR modeInfo = { VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_MODES_CREATE_INFO_KHR };
    modeInfo.presentModeCount       = compatibleModes.size();
    modeInfo.pPresentModes          = compatibleModes.data();

    VkSwapchainLatencyCreateInfoNV latencyInfo = { VK_STRUCTURE_TYPE_SWAPCHAIN_LATENCY_CREATE_INFO_NV };
    latencyInfo.latencyModeEnable   = m_latencySleepMode.has_value();

    VkImageFormatListCreateInfo formatList = { VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO };
    formatList.viewFormatCount      = viewFormats.size();
    formatList.pViewFormats         = viewFormats.data();

    VkSwapchainCreateInfoKHR swapInfo = { VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
    swapInfo.surface                = m_surface;
    swapInfo.minImageCount          = pickImageCount(minImageCount, maxImageCount);
    swapInfo.imageFormat            = surfaceFormat.format;
    swapInfo.imageColorSpace        = surfaceFormat.colorSpace;
    swapInfo.imageExtent            = imageExtent;
    swapInfo.imageArrayLayers       = 1;
    swapInfo.imageUsage             = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
                                    | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    swapInfo.imageSharingMode       = VK_SHARING_MODE_EXCLUSIVE;
    swapInfo.preTransform           = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    swapInfo.compositeAlpha         = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    swapInfo.presentMode            = m_presentMode;
    swapInfo.clipped                = VK_TRUE;

    if (m_device->features().khrSwapchainMutableFormat && formatList.viewFormatCount) {
      swapInfo.flags |= VK_SWAPCHAIN_CREATE_MUTABLE_FORMAT_BIT_KHR;
      formatList.pNext = std::exchange(swapInfo.pNext, &formatList);
    }

    if (presentId2Caps.presentId2Supported)
      swapInfo.flags |= VK_SWAPCHAIN_CREATE_PRESENT_ID_2_BIT_KHR;

    if (presentWait2Caps.presentWait2Supported)
      swapInfo.flags |= VK_SWAPCHAIN_CREATE_PRESENT_WAIT_2_BIT_KHR;

    if (m_device->features().extFullScreenExclusive && m_chainFseInfo)
      fullScreenInfo.pNext = const_cast<void*>(std::exchange(swapInfo.pNext, &fullScreenInfo));

    if (m_hasSwapchainMaintenance1)
      modeInfo.pNext = std::exchange(swapInfo.pNext, &modeInfo);

    if (m_device->features().nvLowLatency2)
      latencyInfo.pNext = std::exchange(swapInfo.pNext, &latencyInfo);

    Logger::info(str::format(
      "Presenter: Actual swapchain properties:"
      "\n  Format:       ", swapInfo.imageFormat,
      "\n  Color space:  ", swapInfo.imageColorSpace,
      "\n  Present mode: ", swapInfo.presentMode, " (dynamic: ", (dynamicModes.empty() ? "no)" : "yes)"),
      "\n  Buffer size:  ", swapInfo.imageExtent.width, "x", swapInfo.imageExtent.height,
      "\n  Image count:  ", swapInfo.minImageCount));
    
    if ((status = m_vkd->vkCreateSwapchainKHR(m_vkd->device(), &swapInfo, nullptr, &m_swapchain))) {
      Logger::err(str::format("Presenter: Failed to create Vulkan swapchain: ", status));
      return status;
    }

    // An external FFX frame-generation layer may have replaced this swapchain with its own wrapped
    // handle (vkCreateSwapchainKHR is intercepted). When it owns the swapchain, DXVK must act as a
    // thin submit + hand-off (no second present loop) — query the registered predicate now.
    {
      auto query = g_dxvkFrameGenOwnsSwapchain.load(std::memory_order_acquire);
      setFrameGenOwner(query && m_swapchain ? query(m_swapchain) : 0u);
    }

    // Import actual swap chain images
    std::vector<VkImage> images;

    if ((status = getSwapImages(images)))
      return status;
    
    for (uint32_t i = 0; i < images.size(); i++) {
      std::string debugName = str::format("Vulkan swap image ", i);

      DxvkImageCreateInfo imageInfo = { };
      imageInfo.type        = VK_IMAGE_TYPE_2D;
      imageInfo.format      = swapInfo.imageFormat;
      imageInfo.sampleCount = VK_SAMPLE_COUNT_1_BIT;
      imageInfo.extent      = { swapInfo.imageExtent.width, swapInfo.imageExtent.height, 1u };
      imageInfo.numLayers   = swapInfo.imageArrayLayers;
      imageInfo.mipLevels   = 1u;
      imageInfo.usage       = swapInfo.imageUsage;
      imageInfo.tiling      = VK_IMAGE_TILING_OPTIMAL;
      imageInfo.layout      = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
      imageInfo.colorSpace  = swapInfo.imageColorSpace;
      imageInfo.shared      = VK_TRUE;
      imageInfo.debugName   = debugName.c_str();

      // If possible, expose the image with an sRGB format internally so
      // that it will be used as the default format for composition.
      if (swapInfo.flags & VK_SWAPCHAIN_CREATE_MUTABLE_FORMAT_BIT_KHR) {
        imageInfo.flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT;
        imageInfo.viewFormatCount = formatList.viewFormatCount;
        imageInfo.viewFormats = formatList.pViewFormats;

        if (formatPair.second)
          imageInfo.format = formatPair.second;
      }

      m_images.push_back(m_device->importImage(imageInfo, images[i],
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT));
    }

    // Create one set of semaphores per swap image, as well as a fence
    // that we use to ensure that semaphores are safe to access.
    uint32_t semaphoreCount = images.size();

    if (!m_hasSwapchainMaintenance1) {
      // Without support for present fences, just give up and allocate extra
      // semaphores. We have no real guarantees when they are safe to access.
      semaphoreCount *= 2u;
    }

    m_semaphores.resize(semaphoreCount);

    for (uint32_t i = 0; i < semaphoreCount; i++) {
      VkSemaphoreCreateInfo semInfo = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };

      if ((status = m_vkd->vkCreateSemaphore(m_vkd->device(),
          &semInfo, nullptr, &m_semaphores[i].acquire))) {
        Logger::err(str::format("Presenter: Failed to create semaphore: ", status));
        return status;
      }

      if ((status = m_vkd->vkCreateSemaphore(m_vkd->device(),
          &semInfo, nullptr, &m_semaphores[i].present))) {
        Logger::err(str::format("Presenter: Failed to create semaphore: ", status));
        return status;
      }

      VkFenceCreateInfo fenceInfo = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };

      if ((status = m_vkd->vkCreateFence(m_vkd->device(),
          &fenceInfo, nullptr, &m_semaphores[i].fence))) {
        Logger::err(str::format("Presenter: Failed to create fence: ", status));
        return status;
      }
    }
    
    // Invalidate indices
    m_latencySleepSupported = m_device->features().nvLowLatency2 && latencyInfo.latencyModeEnable;

    m_imageIndex = 0;
    m_frameIndex = 0;

    m_dynamicModes = std::move(dynamicModes);

    // Set up feature support for present wait / id, and launch sync thread as necessary
    m_hasPresentId = presentId2Caps.presentId2Supported || m_device->features().khrPresentId.presentId;
    m_hasPresentWait = presentWait2Caps.presentWait2Supported || m_device->features().khrPresentWait.presentWait;

    // Don't launch the present-wait worker for an FFX frame-generation-owned swapchain: it would call
    // vkWaitForPresentKHR on FFX's wrapped swapchain (which FFX, not DXVK, paces), racing FFX's own
    // present thread. signalFrame releases the frame-latency signal directly instead.
    if (m_signal && m_hasPresentWait && !m_frameGenOwned.load() && !m_frameThread.joinable())
      m_frameThread = dxvk::thread([this] { runFrameThread(); });

    {
      std::lock_guard lock(g_dxvkPresentWaitMutex);
      m_presentWaitSwapchainSerial = ++g_dxvkNextPresentWaitSwapchainSerial;
      if (!m_presentWaitSwapchainSerial)
        m_presentWaitSwapchainSerial = ++g_dxvkNextPresentWaitSwapchainSerial;
    }

    publishPresenterSurfaceState(
      surfaceFormat.format,
      m_preferredFormat.colorSpace,
      surfaceFormat.colorSpace);

    return VK_SUCCESS;
  }


  VkResult Presenter::getSupportedFormats(std::vector<VkSurfaceFormatKHR>& formats) const {
    uint32_t numFormats = 0;

    VkSurfaceFullScreenExclusiveInfoEXT fullScreenInfo = { VK_STRUCTURE_TYPE_SURFACE_FULL_SCREEN_EXCLUSIVE_INFO_EXT };
    fullScreenInfo.fullScreenExclusive = m_fullscreenMode;

    VkPhysicalDeviceSurfaceInfo2KHR surfaceInfo = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SURFACE_INFO_2_KHR };
    if (m_chainFseInfo)
      surfaceInfo.pNext = &fullScreenInfo;
    surfaceInfo.surface = m_surface;

    VkResult status;
    
    if (m_device->features().extFullScreenExclusive) {
      status = m_vki->vkGetPhysicalDeviceSurfaceFormats2KHR(
        m_device->adapter()->handle(), &surfaceInfo, &numFormats, nullptr);
    } else {
      status = m_vki->vkGetPhysicalDeviceSurfaceFormatsKHR(
        m_device->adapter()->handle(), m_surface, &numFormats, nullptr);
    }

    if (status != VK_SUCCESS) {
      Logger::err(str::format("Presenter: Failed to query surface formats: ", status));
      return status;
    }
    
    formats.resize(numFormats);

    if (m_device->features().extFullScreenExclusive) {
      std::vector<VkSurfaceFormat2KHR> tmpFormats(numFormats, 
        { VK_STRUCTURE_TYPE_SURFACE_FORMAT_2_KHR, nullptr, VkSurfaceFormatKHR() });

      status = m_vki->vkGetPhysicalDeviceSurfaceFormats2KHR(
        m_device->adapter()->handle(), &surfaceInfo, &numFormats, tmpFormats.data());

      for (uint32_t i = 0; i < numFormats; i++)
        formats[i] = tmpFormats[i].surfaceFormat;
    } else {
      status = m_vki->vkGetPhysicalDeviceSurfaceFormatsKHR(
        m_device->adapter()->handle(), m_surface, &numFormats, formats.data());
    }

    if (status != VK_SUCCESS)
      Logger::err(str::format("Presenter: Failed to query surface formats: ", status));

    return status;
  }

  
  VkResult Presenter::getSupportedPresentModes(std::vector<VkPresentModeKHR>& modes) const {
    uint32_t numModes = 0;

    VkSurfaceFullScreenExclusiveInfoEXT fullScreenInfo = { VK_STRUCTURE_TYPE_SURFACE_FULL_SCREEN_EXCLUSIVE_INFO_EXT };
    fullScreenInfo.fullScreenExclusive = m_fullscreenMode;

    VkPhysicalDeviceSurfaceInfo2KHR surfaceInfo = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SURFACE_INFO_2_KHR };
    if (m_chainFseInfo)
      surfaceInfo.pNext = &fullScreenInfo;
    surfaceInfo.surface = m_surface;

    VkResult status;

    if (m_device->features().extFullScreenExclusive) {
      status = m_vki->vkGetPhysicalDeviceSurfacePresentModes2EXT(
        m_device->adapter()->handle(), &surfaceInfo, &numModes, nullptr);
    } else {
      status = m_vki->vkGetPhysicalDeviceSurfacePresentModesKHR(
        m_device->adapter()->handle(), m_surface, &numModes, nullptr);
    }

    if (status != VK_SUCCESS) {
      Logger::err(str::format("Presenter: Failed to query present modes: ", status));
      return status;
    }
    
    modes.resize(numModes);

    if (m_device->features().extFullScreenExclusive) {
      status = m_vki->vkGetPhysicalDeviceSurfacePresentModes2EXT(
        m_device->adapter()->handle(), &surfaceInfo, &numModes, modes.data());
    } else {
      status = m_vki->vkGetPhysicalDeviceSurfacePresentModesKHR(
        m_device->adapter()->handle(), m_surface, &numModes, modes.data());
    }

    if (status != VK_SUCCESS)
      Logger::err(str::format("Presenter: Failed to query present modes: ", status));

    return status;
  }


  VkResult Presenter::getSwapImages(std::vector<VkImage>& images) {
    uint32_t imageCount = 0;

    VkResult status = m_vkd->vkGetSwapchainImagesKHR(
      m_vkd->device(), m_swapchain, &imageCount, nullptr);
    
    if (status != VK_SUCCESS) {
      Logger::err(str::format("Presenter: Failed to query swapchain images: ", status));
      return status;
    }
    
    images.resize(imageCount);

    status = m_vkd->vkGetSwapchainImagesKHR(
      m_vkd->device(), m_swapchain, &imageCount, images.data());

    if (status != VK_SUCCESS)
      Logger::err(str::format("Presenter: Failed to query swapchain images: ", status));

    return status;
  }


  VkSurfaceFormatKHR Presenter::pickSurfaceFormat(
          uint32_t                  numSupported,
    const VkSurfaceFormatKHR*       pSupported,
    const VkSurfaceFormatKHR&       desired) {
    VkSurfaceFormatKHR result = { };
    result.colorSpace = pickColorSpace(numSupported, pSupported, desired.colorSpace);
    result.format = pickFormat(numSupported, pSupported, result.colorSpace,
      result.colorSpace == desired.colorSpace ? desired.format : VK_FORMAT_UNDEFINED);
    return result;
  }


  VkColorSpaceKHR Presenter::pickColorSpace(
          uint32_t                  numSupported,
    const VkSurfaceFormatKHR*       pSupported,
          VkColorSpaceKHR           desired) {
    VkColorSpaceKHR fallback = pSupported[0].colorSpace;

    for (uint32_t i = 0; i < numSupported; i++) {
      if (pSupported[i].colorSpace == desired)
        return desired;

      if (pSupported[i].colorSpace == VK_COLORSPACE_SRGB_NONLINEAR_KHR)
        fallback = pSupported[i].colorSpace;
    }

    for (const auto& f : s_colorSpaceFallbacks) {
      if (f.first != desired)
        continue;

      for (uint32_t i = 0; i < numSupported; i++) {
        if (pSupported[i].colorSpace == f.second)
          return f.second;
      }
    }

    Logger::warn(str::format("No fallback color space found for ", desired, ", using ", fallback));
    return fallback;
  }


  VkFormat Presenter::pickFormat(
          uint32_t                  numSupported,
    const VkSurfaceFormatKHR*       pSupported,
          VkColorSpaceKHR           colorSpace,
          VkFormat                  format) {
    static const std::array<VkFormat, 13> srgbFormatList = {
      VK_FORMAT_B5G5R5A1_UNORM_PACK16,
      VK_FORMAT_R5G5B5A1_UNORM_PACK16,
      VK_FORMAT_A1B5G5R5_UNORM_PACK16_KHR,
      VK_FORMAT_R5G6B5_UNORM_PACK16,
      VK_FORMAT_B5G6R5_UNORM_PACK16,
      VK_FORMAT_R8G8B8A8_UNORM,
      VK_FORMAT_B8G8R8A8_UNORM,
      VK_FORMAT_A8B8G8R8_UNORM_PACK32,
      VK_FORMAT_A2R10G10B10_UNORM_PACK32,
      VK_FORMAT_A2B10G10R10_UNORM_PACK32,
      VK_FORMAT_E5B9G9R9_UFLOAT_PACK32,
      VK_FORMAT_R16G16B16A16_UNORM,
      VK_FORMAT_R16G16B16A16_SFLOAT,
    };

    static const std::array<VkFormat, 5> hdr10FormatList = {
      VK_FORMAT_A2R10G10B10_UNORM_PACK32,
      VK_FORMAT_A2B10G10R10_UNORM_PACK32,
      VK_FORMAT_E5B9G9R9_UFLOAT_PACK32,
      VK_FORMAT_R16G16B16A16_UNORM,
      VK_FORMAT_R16G16B16A16_SFLOAT,
    };

    static const std::array<VkFormat, 1> scRGBFormatList = {
      VK_FORMAT_R16G16B16A16_SFLOAT,
    };

    static const std::array<PresenterFormatList, 3> compatLists = {{
      { VK_COLOR_SPACE_SRGB_NONLINEAR_KHR,
        srgbFormatList.size(), srgbFormatList.data() },
      { VK_COLOR_SPACE_HDR10_ST2084_EXT,
        hdr10FormatList.size(), hdr10FormatList.data() },
      { VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT,
        scRGBFormatList.size(), scRGBFormatList.data() },
    }};

    // Some third-party overlays don't handle sRGB image formats
    // correctly, so use the corresponding UNORM format instead.
    auto formatPair = vk::getSrgbFormatPair(format);

    if (formatPair.first)
      format = formatPair.first;

    // If the desired format is supported natively, use it
    VkFormat fallback = VK_FORMAT_UNDEFINED;

    for (uint32_t i = 0; i < numSupported; i++) {
      if (pSupported[i].colorSpace == colorSpace) {
        if (pSupported[i].format == format)
          return pSupported[i].format;

        if (!fallback)
          fallback = pSupported[i].format;
      }
    }

    // Otherwise, find a supported format for the color space
    const PresenterFormatList* compatList = nullptr;

    for (const auto& l : compatLists) {
      if (l.colorSpace == colorSpace)
        compatList = &l;
    }

    if (!compatList)
      return fallback;

    bool desiredFound = false;

    for (uint32_t i = 0; i < compatList->formatCount; i++) {
      bool isSupported = false;

      if (compatList->formats[i] == format)
        desiredFound = true;

      for (uint32_t j = 0; j < numSupported && !isSupported; j++)
        isSupported = pSupported[j].colorSpace == colorSpace && pSupported[j].format == compatList->formats[i];

      if (isSupported) {
        fallback = compatList->formats[i];

        if (desiredFound)
          break;
      }
    }

    if (!desiredFound && format)
      Logger::warn(str::format("Desired format ", format, " not in compatibility list for ", colorSpace, ", using ", fallback));

    return fallback;
  }


  VkPresentModeKHR Presenter::pickPresentMode(
          uint32_t                  numSupported,
    const VkPresentModeKHR*         pSupported,
          uint32_t                  syncInterval) {
    std::array<VkPresentModeKHR, 2> desired = { };
    uint32_t numDesired = 0;

    Tristate tearFree = m_device->config().tearFree;
    const int32_t tearingPreference = g_dxvkTearingPreference.load(std::memory_order_acquire);
    if (tearingPreference >= 0)
      tearFree = tearingPreference ? Tristate::False : Tristate::True;

    if (!syncInterval) {
      if (tearFree != Tristate::True)
        desired[numDesired++] = VK_PRESENT_MODE_IMMEDIATE_KHR;
      desired[numDesired++] = VK_PRESENT_MODE_MAILBOX_KHR;
    } else {
      // VSync-on stays FIFO (the fallback below): MAILBOX was measured to sit
      // outside IMMEDIATE's compatible-mode group on NVIDIA, so preferring it
      // here destroys dynamic present-mode switching and every vsync toggle
      // then recreates the whole swapchain (visible flash + the recreate path
      // is this fork's most delicate code). FIFO never drops frames either.
      if (tearFree == Tristate::False)
        desired[numDesired++] = VK_PRESENT_MODE_FIFO_RELAXED_KHR;
    }

    // Just pick the first desired and supported mode
    for (uint32_t i = 0; i < numDesired; i++) {
      for (uint32_t j = 0; j < numSupported; j++) {
        if (pSupported[j] == desired[i])
          return pSupported[j];
      }
    }
    
    // Guaranteed to be available
    return VK_PRESENT_MODE_FIFO_KHR;
  }


  VkExtent2D Presenter::pickImageExtent(
    const VkSurfaceCapabilitiesKHR& caps,
          VkExtent2D                desired) {
    if (caps.currentExtent.width != std::numeric_limits<uint32_t>::max())
      return caps.currentExtent;

    VkExtent2D actual;
    actual.width  = clamp(desired.width,  caps.minImageExtent.width,  caps.maxImageExtent.width);
    actual.height = clamp(desired.height, caps.minImageExtent.height, caps.maxImageExtent.height);
    return actual;
  }


  uint32_t Presenter::pickImageCount(
          uint32_t                  minImageCount,
          uint32_t                  maxImageCount) {
    uint32_t count = minImageCount + 1;

    if (count > maxImageCount && maxImageCount != 0)
      count = maxImageCount;

    return count;
  }


  VkResult Presenter::createSurface() {
    VkResult vr = m_surfaceProc(&m_surface);

    if (vr != VK_SUCCESS)
      Logger::err(str::format("Presenter: Failed to create Vulkan surface: ", vr));

    return vr;
  }


  VkResult Presenter::createLatencySemaphore() {
    VkSemaphoreTypeCreateInfo typeInfo = { VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO };
    typeInfo.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;

    VkSemaphoreCreateInfo info = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &typeInfo };
    VkResult vr = m_vkd->vkCreateSemaphore(m_vkd->device(), &info, nullptr, &m_latencySemaphore);

    if (vr != VK_SUCCESS)
      Logger::err(str::format("Presenter: Failed to create latency semaphore: ", vr));

    return vr;
  }


  void Presenter::destroySwapchain() {
    // Without usable present fences, waiting for the queue or device to go idle
    // is the only way to properly synchronize swapchain teardown. Frame-generation
    // owned presents do not chain DXVK's maintenance1 fence.
    const bool usePresentFences = m_hasSwapchainMaintenance1 &&
      !m_hasGamescopeFenceSignalBug && !m_frameGenOwned.load();
    bool presentCompletionProven = usePresentFences || m_device->waitForIdle() == VK_SUCCESS;

    // Wait for the presentWait worker to finish using
    // the swapchain before destroying it.
    std::unique_lock lock(m_frameMutex);

    m_frameDrain.wait(lock, [this] {
      return m_frameQueue.empty();
    });

    if (usePresentFences) {
      for (auto& sem : m_semaphores)
        presentCompletionProven &= waitForSwapchainFence(sem);
    }

    if (presentCompletionProven)
      releasePresentWaitsForSwapchain(m_swapchain, m_presentWaitSwapchainSerial);

    for (const auto& sem : m_semaphores) {
      m_vkd->vkDestroySemaphore(m_vkd->device(), sem.acquire, nullptr);
      m_vkd->vkDestroySemaphore(m_vkd->device(), sem.present, nullptr);
      m_vkd->vkDestroyFence(m_vkd->device(), sem.fence, nullptr);
    }

    // The conditional is here because some third party layers don't properly handle null swapchains
    if (m_swapchain)
      m_vkd->vkDestroySwapchainKHR(m_vkd->device(), m_swapchain, nullptr);

    m_images.clear();
    m_semaphores.clear();
    m_dynamicModes.clear();

    m_swapchain = VK_NULL_HANDLE;
    m_presentWaitSwapchainSerial = 0;
    m_acquireStatus = VK_NOT_READY;

    m_presentPending = false;

    m_hdrMetadataDirty = true;

    m_latencySleepModeDirty = true;
    m_latencySleepSupported = false;

    m_hasPresentId = false;
    m_hasPresentWait = false;
  }


  void Presenter::destroySurface() {
    m_vki->vkDestroySurfaceKHR(m_vki->instance(), m_surface, nullptr);

    m_surface = VK_NULL_HANDLE;
  }


  void Presenter::destroyLatencySemaphore() {
    m_vkd->vkDestroySemaphore(m_vkd->device(), m_latencySemaphore, nullptr);

    m_latencySemaphore = VK_NULL_HANDLE;
  }


  bool Presenter::waitForSwapchainFence(
          PresenterSync&            sync) {
    if (!sync.fenceSignaled)
      return true;

    VkResult vr = m_vkd->vkWaitForFences(m_vkd->device(),
      1, &sync.fence, VK_TRUE, ~0ull);

    if (vr) {
      Logger::err(str::format("Presenter: Failed to wait for WSI fence: ", vr));
      return false;
    }

    if ((vr = m_vkd->vkResetFences(m_vkd->device(), 1, &sync.fence)))
      Logger::err(str::format("Presenter: Failed to reset WSI fence: ", vr));

    sync.fenceSignaled = VK_FALSE;
    return true;
  }


  void Presenter::runFrameThread() {
    env::setThreadName("dxvk-frame");

    // Defense-in-depth: if the swapchain became FFX frame-generation-owned after this thread started
    // (e.g. FG toggled on), bail out — FFX owns present-wait/pacing and presentImage stops queuing
    // frames here. A recreate (which CS forces on toggle) re-evaluates ownership at line ~902.
    if (m_frameGenOwned.load())
      return;

    while (true) {
      PresenterFrame frame = { };

      // Wait for all GPU work for this frame to complete in order to maintain
      // ordering guarantees of the frame signal w.r.t. objects being released
      { std::unique_lock lock(m_frameMutex);

        m_frameCond.wait(lock, [this] {
          return !m_frameQueue.empty();
        });

        // Use a frame ID of 0 as an exit condition
        frame = m_frameQueue.front();

        if (!frame.frameId) {
          m_frameQueue.pop();
          return;
        }
      }

      // If the present operation has succeeded, actually wait for it to complete.
      // Don't bother with it on MAILBOX / IMMEDIATE modes since doing so would
      // restrict us to the display refresh rate on some platforms (XWayland).
      if (frame.result >= 0 && (frame.mode == VK_PRESENT_MODE_FIFO_KHR || frame.mode == VK_PRESENT_MODE_FIFO_RELAXED_KHR)) {
        VkResult vr;

        if (m_device->features().khrPresentWait2.presentWait2) {
          VkPresentWait2InfoKHR waitInfo = { VK_STRUCTURE_TYPE_PRESENT_WAIT_2_INFO_KHR };
          waitInfo.presentId = frame.frameId;
          waitInfo.timeout = std::numeric_limits<uint64_t>::max();

          vr = m_vkd->vkWaitForPresent2KHR(m_vkd->device(), m_swapchain, &waitInfo);
        } else {
          vr = m_vkd->vkWaitForPresentKHR(m_vkd->device(),
            m_swapchain, frame.frameId, std::numeric_limits<uint64_t>::max());
        }

        if (vr < 0 && vr != VK_ERROR_OUT_OF_DATE_KHR && vr != VK_ERROR_SURFACE_LOST_KHR)
          Logger::err(str::format("Presenter: vkWaitForPresentKHR failed: ", vr));
      }

      // Signal latency tracker right away to get more accurate
      // measurements if the frame rate limiter is enabled.
      if (frame.tracker) {
        frame.tracker->notifyGpuPresentEnd(frame.frameId);
        frame.tracker = nullptr;
      }

      // Apply FPS limiter here to align it as closely with scanout as we can,
      // and delay signaling the frame latency event to emulate behaviour of a
      // low refresh rate display as closely as we can.
      applyExternalFrameRateLimit();
      m_fpsLimiter.delay();

      // Wake up any thread that may be waiting for the queue to become empty
      bool canSignal = false;

      { std::unique_lock lock(m_frameMutex);

        m_frameQueue.pop();
        m_frameDrain.notify_one();

        m_lastCompleted = frame.frameId;
        canSignal = m_lastSignaled >= frame.frameId;
      }

      // Always signal even on error, since failures here
      // are transparent to the front-end.
      if (canSignal)
        m_signal->signal(frame.frameId);
    }
  }


  VkResult Presenter::softError(
          VkResult                  vr) {
    // Don't return these as an error state to the caller. The app can't
    // do much anyway, so just pretend that we don't have a valid swap
    // chain and move on. An alternative would be to handle errors in a
    // loop, however this may also not be desireable since it could stall
    // the app indefinitely in case the surface is in a weird state.
    if (vr == VK_ERROR_SURFACE_LOST_KHR || vr == VK_ERROR_OUT_OF_DATE_KHR)
      return VK_NOT_READY;

    return vr;
  }

}
