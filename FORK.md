# Community Shaders DXVK fork

A fork of [doitsujin/dxvk](https://github.com/doitsujin/dxvk) carrying changes that
Skyrim Special Edition and [Community Shaders](https://github.com/doodlum/skyrim-community-shaders)
need and that do not belong upstream: a private interop ABI, HDR fixes specific to the
NVIDIA Windows driver, and CPU-side optimisations aimed at Skyrim's draw-call load.

Everything here is deliberate. If a change looks arbitrary, it is a bug in this document —
please fix the document rather than deleting the change silently.

**Branches**

| Branch | Contents |
| --- | --- |
| `master` | Pristine mirror of `upstream/master`. Never commit here. |
| `native` | All fork work. This is the branch you build and ship. |

Merging upstream means merging `master` into `native`. See
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
runs fine either way. The chain is therefore only added when FSE is actually opted into, and
the host can override per frame-generation method through `dxvkSetFsePNextChain`.

Note the asymmetry if you ever force it: the driver flip-locks a window at its first flip
present, so the copy path is only reachable before any flip has occurred.

*File: `src/dxvk/dxvk_presenter.cpp` (`updateFsePNextChainMode`, `createSwapChain`)*

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
Keep them in sync whenever an export is added, renamed or removed. Ordinals 104-106, 108, 115,
119 and 123-125 are holes left by removed exports — never reuse them, or a host built against
an older header will bind an old ordinal to a new, incompatible function.

### Frame-generation ownership

When an external layer (the FFX frame-interpolation swapchain, or Streamline's DLSS-G proxy)
replaces the `VkSwapchainKHR`, DXVK must stop acting as a present loop and become a thin
submit-and-hand-off. The host registers a predicate via `dxvkSetFrameGenOwnershipQuery`, which
`createSwapChain` calls to classify each swapchain: 0 = normal, 1 = FSR replacement,
2 = DLSS-G proxy.

When owned, the presenter:

- does not run its present-wait worker (the external layer does the display pacing);
- does not pre-acquire the next image after presenting — re-entering the wrapped swapchain
  from the submit thread while FFX's present thread is also driving it deadlocks;
- does not chain DXVK's `presentId` / present-fence / present-mode `pNext` structs, which FFX's
  replacement `vkQueuePresentKHR` does not handle (the present fence is never signalled, and
  the fourth acquire hangs in `waitForSwapchainFence`). DLSS-G is the exception: its pacer
  needs `presentId` for real-frame completion tracking, so that one is kept for owner type 2;
- releases the frame-latency signal directly on the submit thread from `signalFrame`.

### Present callbacks

`dxvkSetPresentBeginCallback` runs immediately before `vkQueuePresentKHR` on the present
thread; `dxvkSetPresentCompletedCallback` immediately after it returns. Both fire only for a
DLSS-G-owned swapchain, and both receive a `CsDxvkPresentCallbackInfo` payload identifying the
swapchain, presenter, queue and frame.

Ordering matters: Streamline requires DLSS-G option changes to be ordered with the present that
consumes them. Issuing `SetOptions` from the D3D render thread races DXVK's async presenter and
can wedge the plugin pacer during a mode transition, which is why the begin callback exists at
all rather than the host just calling before `Present`.

### Present-wait semaphores

`dxvkEnqueueInteropCommandBuffer` submits a host-owned command buffer through DXVK's submission
queue, optionally signalling a semaphore. That semaphore is registered in a generation-keyed
FIFO; the presenter attaches the oldest eligible one as an extra `pWaitSemaphores` entry on the
next DLSS-G present, so the host's evaluate/tag submissions are GPU-ordered ahead of the present
that reads them. Without it the generated frames flash. This is pure queue ordering — no CPU
stall is introduced.

The host tracks its own semaphores through `dxvkGetPresentWaitSemaphoreState`,
`dxvkClearPresentWaitSemaphore`, `dxvkCancelPresentWaitSemaphore` and
`dxvkReleaseQueuedPresentWaitSemaphoresAfterIdle`. A generation may only be cleared once
reacquire has proven the present consumed it; the release-after-idle path requires the caller
to have proven the device idle.

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

`dxvkSetTargetFrameRate` provides an external FPS cap, reconciled into the limiter on every
present. FSR frame generation forces Reflex off, so without this nothing paces the presents.

`dxvkSetTearingPreference` overrides `dxvk.tearFree` per frame-gen method.

### Other exports

`dxvkGetCsApiVersion` returns `CS_DXVK_API_VERSION`; check it before using anything else.
`dxvkGetPresenterSurfaceState` reports the live surface format plus requested and effective
colour spaces, with a serial the host can poll for changes.

### Preconditions

`dxvkEnqueueInteropCommandBuffer` routes to a single global active swapchain, last-constructor-
wins. This assumes one D3D11 swapchain per process, which is how CS drives DXVK. With several,
the others are unreachable for interop.

---

## 5. CPU-side optimisations

Skyrim is heavily CPU- and draw-call-bound; the render thread is the bottleneck. These changes
came out of live render-thread IP sampling. **All of them are optimisations, not features — if
one ever looks like it is causing a bug, reverting it is a legitimate first move.**

### Non-owning binding state

Constant-buffer, shader-resource, vertex-buffer and index-buffer bindings hold raw pointers
instead of `Com<T, false>`. The per-bind private-ref atomics on cold buffer cache lines were the
single hottest render-thread cost in the profile — Skyrim rebinds constant buffers on nearly
every draw and binds very large numbers of SRVs per frame (terrain layers, grass, LOD, objects).

Two mechanisms make this safe:

1. **Per-chunk keep-alive.** Buffers captured as raw pointers into CS closures take one private
   reference per buffer per flush window, deduplicated through a generation stamp on the buffer
   (`m_csRefSeq`), instead of one atomic per bind. References are released by a closure appended
   to the CS stream itself, so it replays after every command that captured them. Vertex and
   index buffers are not keep-alive tracked — Skyrim binds thousands of distinct ones once
   each per frame, so the stamp does not pay for itself there; they use non-owning bindings instead
     (see below).

2. **Sequence-barriered destruction.** `D3D11Buffer` and `D3D11ShaderResourceView` override
   `ComObject::deleteThis()` to park in `D3D11Device`'s retirement list instead of deleting
   immediately. Each batch carries a CS sequence barrier, and a batch is destroyed only once
   `DxvkCsThread::lastSequenceNumber()` has reached it — that is, once the CS thread has
   executed past every chunk that could still name those objects.

   The barrier is stamped by the immediate context, never by `RetireResource`: releases happen
   on arbitrary threads, where a stale read could produce a barrier *older* than the chunk
   holding the reference. `GetCurrentSequenceNumber()` on the recording thread is always >= any
   chunk emitted so far, so it can never be too early.

   Retirement is drained from `EndFrame` and from `ExecuteFlush`, so an app that churns
   resources without presenting does not accumulate them.

Deferred contexts keep fully owning captures throughout — their command lists can replay after
the buffer dies.

*Files: `src/d3d11/d3d11_context_state.h`, `src/d3d11/d3d11_context.{cpp,h}`,
`src/d3d11/d3d11_device.h`, `src/d3d11/d3d11_buffer.{cpp,h}`, `src/d3d11/d3d11_view_srv.{cpp,h}`,
`src/util/com/com_object.h`*

### Non-owning vertex and index bindings

`DxvkVertexInputState` stores `DxvkBufferSliceRef` — a raw `DxvkBuffer*` plus offset and length —
rather than an owning `DxvkBufferSlice`. Binding a vertex or index buffer was the hottest single
cost inside `dxvk_d3d11.dll` (3.25M + 1.14M samples, ~20% of the module), because the owning slice
paid an atomic when it was built on the CS thread and another when the previous binding was
overwritten, both on cold buffer cache lines, with no reuse across thousands of distinct
per-frame buffers.

The `Rc` inside `DxvkBufferSliceRef` is populated *only* by deferred contexts: a command list can
replay after the client released the buffer, and a bound vertex buffer — unlike a Map hazard — is
not recorded in `D3D11CommandList::m_resources`. The immediate context leaves it null and pays no
atomics, resting on the same contract as the non-owning D3D11 state bindings above: the client
keeps a bound buffer alive for the frame, a release while bound is deferred behind the CS sequence
barrier, and `DxvkCommandList::track()` still takes the GPU-side reference at draw time. The rare
barrier paths call `DxvkBufferSliceRef::slice()` to materialise an owning slice.

Measured (Whiterun exterior, no upscaling or frame generation): 167.50 -> 174.20 fps (+4.0%);
`dxvk_d3d11.dll` 21.03% -> 16.48% of process CPU; DXVK plus the Vulkan UMD 31.65% -> 23.60%.
`bindVertexBuffer` and `bindIndexBuffer` no longer appear in the profile.

*Files: `src/dxvk/dxvk_buffer.h`, `src/dxvk/dxvk_context_state.h`, `src/dxvk/dxvk_context.{h,cpp}`,
`src/d3d11/d3d11_buffer.h`, `src/d3d11/d3d11_context.cpp`*

### Allocation cache representation

The local and shared allocation caches hand allocations around in dense slot arrays rather than
intrusive linked lists. A list pop dereferences a next-pointer inside the cold allocation object
— one serial cache-line miss per allocation on the per-draw discard path. Slot-array pops touch
a single dense line and prefetch cold objects by index.

Each slot also carries the mapped pointer, captured at free time while the object's line was
still warm on the freeing thread, so the hot `Map(WRITE_DISCARD)` path never loads from the cold
allocation object at all. Freed allocations have their refcount pre-set to 1 for the next owner,
so consumers adopt via `Rc::unsafeCreate` with no atomic RMW.

`DxvkBuffer` precomputes its allocation descriptors and cache eligibility once, in
`initStorageInfo()`, called from both constructors. This is eager rather than lazy on purpose:
`allocateStorage` runs on both the render thread (`D3D11Buffer::DiscardSlice`) and the CS thread
(`DxvkContext::invalidateBuffer`), and a lazily-set flag with no release/acquire pairing could
expose a half-built struct.

`BatchCapacity` is 64 — the conservative value. It was raised to 256 at one point to deepen
recycling for the smallest size class under Skyrim's ~21k dynamic-CB discards per frame, but
that was a fit to one workload on one driver and has been reverted. The slot-array
representation is the structural win and is unaffected. If the locked global-alloc path shows
up in a profile again, this is the knob — but re-measure.

*Files: `src/dxvk/dxvk_memory.{cpp,h}`, `src/dxvk/dxvk_buffer.{cpp,h}`*

### Application profile

`SkyrimSE.exe` gets `d3d11.cachedDynamicResources = "a"` (its many dynamic `Map`/`DISCARD`
constant and vertex buffers live in cached system memory, avoiding GPU stalls on that traffic)
plus `d3d11.relaxedGraphicsBarriers` and `d3d11.relaxedBarriers` (it does not rely on DXVK's
implicit WAR/WAW barriers between draws, so relaxing them trims per-draw barrier work).

*File: `src/util/config/config.cpp`*

---

## 6. Bug fixes

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

## 7. Environment variables

Only two are fork-specific. Both are debugging aids, not shipping configuration.

| Variable | Effect |
| --- | --- |
| `DXVK_ENGINE_NAME` | Overrides the reported Vulkan engine name outright. Use `DXVK` to opt back into the NVIDIA application profile. |
| `DXVK_SYNC_PRESENT=1` | Forces synchronous present for the swapchain's whole lifetime. The `dxvkSetSyncPresent` export is the runtime equivalent; this exists to bisect without a CS build. |

---

## 8. Merging upstream

```
git checkout native
git merge master -Xignore-space-change
```

`-Xignore-space-change` matters: the fork has incidental trailing-whitespace differences that
would otherwise widen conflict hunks for no reason.

After merging, check these by hand — each is a place where an upstream change could silently
undo or invalidate fork behaviour:

- [ ] **`DxvkBuffer::initStorageInfo()`** mirrors the cache-eligibility predicate in
      `DxvkMemoryAllocator::createBufferResource`. If upstream changes that predicate, this copy
      must change with it — divergence is silent and produces wrong-pool allocations, not a
      compile error.
- [ ] **`include/cs_dxvk_api.h` vs `src/d3d11/d3d11.def`** still agree, in both directions.
- [ ] **`dxvk_name_prefix`** is still `'dxvk_'` and `d3d11_dxgi_deps` still uses `dxgi_dep`.
- [ ] **`allowFse`** still defaults to false and **`fakeFullscreen`** still defaults to true.
- [ ] **`enableDescriptorHeap`** is still forced off for NVIDIA, and descriptor *buffers* still
      match upstream.
- [ ] **`ComObject::deleteThis()`** is still virtual and still called from `ReleasePrivate`.
- [ ] The **binding state structs** in `d3d11_context_state.h` are still raw pointers — an
      upstream refactor that reintroduces `Com<>` there would silently undo the optimisation.
- [ ] `sync_signal.h` — if upstream has taken the callback-fence fix, drop the fork's version.

Then re-measure. The optimisations in section 5 were tuned against one Skyrim scene on one
driver; upstream's own CS and allocator paths move.

---

## 9. Deliberately not done

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
