# Community Shaders DXVK fork

A fork of [doitsujin/dxvk](https://github.com/doitsujin/dxvk) carrying changes that
Skyrim Special Edition and [Community Shaders](https://github.com/doodlum/skyrim-community-shaders)
need and that do not belong upstream: a private interop ABI for external frame generation,
HDR fixes specific to the NVIDIA Windows driver, and fullscreen behaviour changes.

Everything here is deliberate. If a change looks arbitrary, it is a bug in this document —
please fix the document rather than deleting the change silently.

**Branches**

| Branch | Contents |
| --- | --- |
| `master` | Pristine mirror of `upstream/master`. Never commit here. |
| `interop` | Single commit on top of `upstream/master` with CS interop, HDR and bug fixes. |
| `native` | Legacy branch with interop + CPU-side optimisations. Being replaced by `interop`. |

Merging upstream means rebasing `interop` onto the new `upstream/master`. See
[Merging upstream](#merging-upstream) for the checklist.

---

## 1. Build and distribution

### Renamed DLLs

The build emits `dxvk_d3d11.dll` and `dxvk_dxgi.dll` rather than `d3d11.dll` / `dxgi.dll`.

Community Shaders ships DXVK next to the game executable rather than installing it as a
system-wide override. Under the original names those files alias the System32 copies, which
is fragile: load order decides which one wins, and other tools that poke at `d3d11.dll` get
the wrong module.

The prefix is applied through meson's existing `dxvk_name_prefix`, and the `.def` files carry
matching `LIBRARY DXVK_D3D11.DLL` / `LIBRARY DXVK_DXGI.DLL` directives so the generated import
libraries have the new names. `src/d3d11/meson.build` links DXVK's own `dxgi_dep` instead of
the system `lib_dxgi`, so `dxvk_d3d11.dll` imports `dxvk_dxgi.dll` directly. No post-build
byte patching of import names is needed.

*Files: `meson.build`, `src/d3d11/meson.build`, `src/d3d11/d3d11.def`, `src/dxgi/dxgi.def`*

### Streamline interposer

`src/vulkan/vulkan_loader.cpp` tries `sl.interposer.dll` before `vulkan-1.dll`. Streamline's
interposer wraps DXVK's entire Vulkan surface — it hooks instance/device creation, present and
acquire, and owns the per-frame frame-generation contract. It forwards everything to the real
loader, so it is a transparent passthrough when no frame-gen feature is active, and it loads
the genuine `vulkan-1.dll` from System32 itself, so there is no name collision. If it is absent
or fails to resolve `vkGetInstanceProcAddr`, loading falls through to the normal loader.

---

## 2. HDR

Three separate things had to be fixed before HDR reached the display. Each was verified by
measurement, not by eye — see the measurement harness notes in the CS repo.

### The NVIDIA `"DXVK"` application profile

The NVIDIA Windows driver matches an application profile on the exact, case-sensitive Vulkan
engine name `"DXVK"`, and that profile forces swapchain composition into SDR: scRGB output
clamps at the SDR white level regardless of which colour space the swapchain was created with.

Measured on driver 610.88 against a native Vulkan reference app: a byte-identical swapchain
presents correct HDR as soon as the engine name differs by any character (`"dxvk"`, `"DXVK2"`
and `"vkd3d"` all work).

`DxvkInstance` therefore reports `"DXVK_HDR"` when HDR is configured, and `"DXVK"` otherwise —
so SDR titles keep whatever else that profile does for them. `DXVK_ENGINE_NAME` overrides the
choice outright, for testing or to opt a title back into the profile without a rebuild.

**Known gap.** `DxgiOptions` can force `enableHDR` back off for UE4 DX11 titles (see
`isHDRDisallowed`), and instance creation cannot see that — such a title would report
`"DXVK_HDR"` while HDR is actually disabled. The suppression keys off whether `d3d12.dll` is
loaded, which is not settled at instance-creation time, so replicating it here would be
unreliable rather than merely duplicated. Skyrim cannot hit this. Use
`DXVK_ENGINE_NAME=DXVK` if a UE4 title ever needs the profile back.

*File: `src/dxvk/dxvk_instance.cpp`*

### Missing colour-space instance extension

`VK_EXT_swapchain_colorspace` is now requested by the Win32 WSI driver. Without it,
`vkGetPhysicalDeviceSurfaceFormatsKHR` only ever returns sRGB formats, so `dxgi.enableHDR`
exposes the DXGI option while `SetColorSpace1` still fails — the Vulkan presenter never sees
an HDR10 or scRGB surface format to select.

*File: `src/wsi/win32/wsi_platform_win32.cpp`*

### Full-screen-exclusive present path

Chaining `VkSurfaceFullScreenExclusiveInfoEXT` into surface and swapchain queries — even with
`DISALLOWED`, which is what DXVK does by default — routes the NVIDIA ICD onto the compositor
GDI-copy present path. Not chaining it (the spec default) yields hardware flips.

Streamline's DLSS-G pacer requires hardware flips; its present wedges on the copy path. FSR-FG
runs fine either way. The chain is therefore only added when FSE is actually opted into.

Note the asymmetry if you ever force it: the driver flip-locks a window at its first flip
present, so the copy path is only reachable before any flip has occurred.

*File: `src/dxvk/dxvk_presenter.cpp` (`createSwapChain`)*

---

## 3. Fullscreen behaviour

### FSE off by default

`dxvk.allowFse` defaults to `false` here. The FFX frame-generation wrapper's inner swapchain
never gets FSE, so an FSE-acquired plain swapchain would present with different semantics, and
every frame-gen toggle would pay an FSE re-acquisition recreate. Disallowing it everywhere
keeps all swapchains on identical presentation.

*File: `src/dxvk/dxvk_options.cpp`*

### Fake fullscreen (`dxgi.fakeFullscreen`, default on)

On a fullscreen transition, do not change the real display mode: keep the desktop resolution
and refresh rate, cover the monitor with the window, and let the swapchain scale onto it.

A real exclusive mode-set is revoked by Windows on minimize or alt-tab. That recreates the
swapchain, which tears down the external frame-generation swapchain with it, and the result is
a deadlock — this was the root cause of the alt-tab/minimize freeze with FSR-FG.

Set `dxgi.fakeFullscreen = False` to restore upstream behaviour. Documented in `dxvk.conf`.

*Files: `src/dxgi/dxgi_options.{cpp,h}`, `src/dxgi/dxgi_swapchain.cpp`*

---

## 4. Frame generation and the interop ABI

This is the reason the fork exists. `include/cs_dxvk_api.h` is the shared private ABI; it is
kept in export-ordinal order so it can be diffed against `src/d3d11/d3d11.def` by eye.

**Typedefs are not linked by name.** A mismatch between the header and the `.def` is silent.
Keep them in sync whenever an export is added, renamed or removed. Ordinals 100, 104-108,
111-117, 119, 120 and 122-125 are holes left by removed exports — never reuse them, or a host
built against an older header will bind an old ordinal to a new, incompatible function.

The surface is seven exports, and every one is resolved and called by Community Shaders. An
export that nothing consumes is deleted rather than kept "just in case": the present-wait
semaphore system reached permanently-empty loops running on every present before it went.

### Frame generation is above DXVK

DXVK is never told which frame-generation layer, if any, owns the swapchain. It cannot know
whether the handle it holds is a real driver swapchain or one an interposer created, and the
reference integration (NVIDIA-RTX/Streamline_Sample, donut DeviceManager_VK) acquires and
presents unconditionally, letting the interposer pace transparently.

pNext chaining is therefore split by failure mode rather than by ownership. The
`swapchain_maintenance1` present *mode* struct is still chained -- an interposer that ignores it
only costs a mode change. The present *fence* and the present-ID are not: an interposer that
ignores those leaves the fence unsignalled, and `waitForSwapchainFence` then blocks the next
acquire forever. That is not theoretical -- a build that chained them unconditionally hung
Skyrim with frame generation on, no frames for ten minutes, the render loop simply stopped.

HDR metadata is likewise never submitted for a swapchain an interposer may own: FidelityFX
returns a `FrameInterpolationSwapChainVK*` cast to `VkSwapchainKHR`, and handing that to the real
`vkSetHdrMetadataEXT` corrupts the driver silently. See `canSubmitHdrMetadata`.

### Swapchain lifecycle

`dxvkRequestSwapchainRecreate` forces a full teardown and rebuild on the next acquire. Community
Shaders uses this when switching DLSS-G → FSR: `sl.dlss_g`'s sticky present proxy bypasses the
Vulkan present hooks, so FSR never returns `VK_SUBOPTIMAL` to trigger a recreate on its own. A
DXVK-internal recreate preserves the D3D11 back buffers while re-running
`vkCreate`/`vkDestroySwapchainKHR`, which evicts the proxy.

`dxvkSetSwapchainTornDownCallback` is invoked inside `recreateSwapChain` while DXVK owns no
swapchain — after destroy, before create. Returning false keeps the presenter empty so an
external wrapper that failed teardown cannot overlap a replacement; it is retried on the next
recreation attempt.

### Present pacing

`dxvkSetPresentQueueDepth` bounds outstanding intercepted present calls (0 completes each
present before returning, `UINT32_MAX` is unrestricted, otherwise capped at 7).
`dxvkSetSyncPresent` is the 0-or-unrestricted shorthand. CS uses zero for ownership and option
transitions and for FSR-G, and two for steady-state DLSS-G.

`dxvkSetTearingPreference` overrides `dxvk.tearFree` per frame-gen method.

There is deliberately no frame-rate cap export. DXVK applied one from `Presenter::signalFrame`,
on the submission thread after the present had already gone out, which is too late to pace
anything; Reflex sleeps in the render loop before simulation and owns the cap on every path.
Having a second limiter that could engage at all was a source of frame-time spikes, not a
safety net, so @103 is retired rather than left as a fallback.

### Other exports

`dxvkGetPresenterSurfaceState` reports the live surface format plus requested and effective
colour spaces, with a serial the host can poll for changes.

---

## 5. Bug fixes

### Callback fence use-after-free

`sync::CallbackFence::signal` invoked callbacks while iterating `m_callbacks` under the lock. A
frame-latency callback can tear down the swapchain, which releases the presenter, which releases
the last reference to the fence — so iteration resumed on a freed list. This reliably crashed the
Streamline path with a reused-memory list node. Ready callbacks are now detached and run outside
the lock; nothing after that point may touch `*this*`.

**This is an upstream bug, not a fork-specific one.** It is small, self-contained and not
CS-specific — it should be sent upstream as a standalone PR and dropped from the fork once
accepted.

*File: `src/util/sync/sync_signal.h`*

### Hardened present paths

- `acquireNextImage` clamps the dynamic-present-mode index. An externally wrapped swapchain can
  expose only one dynamic present mode, and the bare `.at(1)` for a non-zero sync interval threw
  `std::out_of_range` and killed the present thread.
- Stale image/frame indices are treated as a soft `OUT_OF_DATE` rather than throwing. An external
  layer can recreate the underlying `VkSwapchainKHR` out from under DXVK, leaving the cached
  image and semaphore arrays out of step with what `vkAcquireNextImageKHR` returns.
- VSync-on stays FIFO rather than preferring MAILBOX. MAILBOX was measured to sit outside
  IMMEDIATE's compatible-mode group on NVIDIA, so preferring it destroys dynamic present-mode
  switching — every vsync toggle then recreates the whole swapchain, with a visible flash.

*File: `src/dxvk/dxvk_presenter.cpp`*

### Descriptor heap disabled on NVIDIA

NGX 310.7 writes a combined D24S8 descriptor through the experimental descriptor-heap path,
violating `VUID-VkImageDescriptorInfoEXT-pView-11430`, and records no DLSS output. Streamline
interop stays on the established descriptor-set implementation.

This is a workaround for a driver/NGX bug and should be re-tested against new drivers.
`dxvk.enableDescriptorHeap` overrides it.

Descriptor *buffers* follow upstream behaviour unchanged.

*File: `src/dxvk/dxvk_device_info.cpp`*

---

## 6. Environment variables

Only two are fork-specific. Both are debugging aids, not shipping configuration.

| Variable | Effect |
| --- | --- |
| `DXVK_ENGINE_NAME` | Overrides the reported Vulkan engine name outright. Use `DXVK` to opt back into the NVIDIA application profile. |
| `DXVK_SYNC_PRESENT=1` | Forces synchronous present for the swapchain's whole lifetime. The `dxvkSetSyncPresent` export is the runtime equivalent; this exists to bisect without a CS build. |

---

## 7. Merging upstream

```
git checkout interop
git rebase upstream/master
```

After rebasing, check these by hand — each is a place where an upstream change could silently
undo or invalidate fork behaviour:

- [ ] **`include/cs_dxvk_api.h` vs `src/d3d11/d3d11.def`** still agree, in both directions.
- [ ] **`dxvk_name_prefix`** is still `'dxvk_'` and `d3d11_dxgi_deps` still uses `dxgi_dep`.
- [ ] **`allowFse`** still defaults to false and **`fakeFullscreen`** still defaults to true.
- [ ] **`enableDescriptorHeap`** is still forced off for NVIDIA, and descriptor *buffers* still
      match upstream.
- [ ] `sync_signal.h` — if upstream has taken the callback-fence fix, drop the fork's version.

---

## 8. Deliberately not done

- **A dedicated present queue.** Built while chasing a DLSS-G blocking-mode wedge; the actual
  fix turned out to be the FSE `pNext` change. The split also breaks `sl.fsr`'s game-queue
  capture — it identifies the game queue as "the queue the app presents on", so FFX composes
  against the present-only queue while real rendering runs elsewhere. Removed.
- **A D3D12/DXGI bridge presenter.** An earlier approach to HDR + DLSS-G routed present through
  a real D3D12 swapchain and shared the resources back into Vulkan. It required a full
  `vkDeviceWaitIdle` per present and was the tree's only D3D12 dependency. Removed.
- **`INPUT_ATTACHMENT` usage on every render target.** Added so exported handles stayed stable
  when Skyrim discovered input-attachment use late; it can inhibit framebuffer compression and
  applied to every target in the game, not just exported ones. Reverted.
