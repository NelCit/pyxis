# RFC 0008: Direct GPU color AOV to the Kit viewport (no readback)

- Status: Accepted (GL external-memory direct AOV — implemented + verified; the
  shared-device design is superseded, see "Revision" and "Why rejected")
- Author(s): Pyxis team
- Created: 2026-05-27
- Last updated: 2026-05-27
- Implementation PRs: (this branch)
- Amends: RFC 0004 (§32 NVRHI device sharing / own-device + external-memory interop)

## Summary

When the Pyxis Hydra delegate is hosted in Omniverse Kit's `omni.hydra.pxr`
viewport, render on **Kit's existing Vulkan device** instead of creating a separate
own-device, and write the color AOV into a **GPU texture the host samples directly**
(`HdRenderBuffer::GetResource()` → `HgiTexture`), eliminating the per-frame
GPU→CPU→GPU readback. The standalone / usdview / headless paths keep the own-device
(`VkDeviceManagerHeadless`) — they have no Kit device — so this is an *additional*
Kit-only code path, gated at runtime.

## Motivation

Today the delegate's color AOV (`StubRenderBuffer`) is CPU-only (`Map()` → a
`std::vector`), and `PyxisEngine` renders on its own headless Vulkan device. Kit's
viewport reads the AOV via `Map()`, so every viewport frame does a full
GPU→CPU readback (`PyxisEngine::ReadbackColorHdr`) + CPU reconvert + the host's
CPU→GPU upload. The branch audit measured this as the dominant per-frame cost on the
Omniverse path (the per-frame *allocations* and the full `waitForIdle` were already
removed; the remaining cost is the readback round-trip itself). The readback exists
purely as a **device bridge**: our own device ≠ Kit's device, and Hgi has no public
API to adopt our externally-exported `VkImage` as an `HgiTexture`. Rendering on
Kit's device removes the bridge entirely.

## Detailed design

1. **Capture Kit's Hgi.** `HdRenderDelegate::SetDrivers(HdDriverVector const&)` is
   called by the host with the `HgiTokens->renderDriver` driver carrying `Hgi*`.
   Capture it. `HgiVulkan::GetPrimaryDevice()` exposes the `VkInstance` /
   `VkPhysicalDevice` / `VkDevice` / graphics queue + family.

2. **Feasibility gate (FIRST, before any of the below).** Pyxis requires the
   ray-tracing extensions (`VK_KHR_ray_tracing_pipeline`,
   `VK_KHR_acceleration_structure`, `buffer_device_address`, …) to be enabled **at
   device-creation time** — they cannot be added to an already-created device. So
   shared-device rendering is only possible if **Kit's device already enabled them**
   (Kit's RTX path does; the shared `omni.gpu_foundation` device is the candidate).
   A startup probe logs Kit's device's enabled extensions; if the RT set is missing,
   shared-device rendering is **infeasible** and the delegate falls back to the
   own-device + readback path (today's behaviour). This gate is implemented and
   verified before the rest of the design is built.

3. **NVRHI on Kit's device.** When the gate passes, create the NVRHI Vulkan device
   from Kit's existing handles (`nvrhi::vulkan::createDevice` with the instance /
   physical / device / queue + the enabled extension list) instead of
   `CreateHeadlessDeviceManager`. `PyxisEngine` gains a "hosted" construction path
   that adopts an external device + queue rather than owning a `VkDeviceManager`.

4. **GPU color AOV.** Replace the CPU `StubRenderBuffer` (for the Kit path) with a
   GPU-backed render buffer whose `GetResource()` returns an `HgiTextureHandle`. The
   AOV's `VkImage` is created via Hgi (so the host accepts it) and adopted into NVRHI
   (`createHandleForNativeTexture`) so the path tracer renders into it directly.
   `WritePyxisColorToAov` + `ReadbackColorHdr` are skipped on this path.

5. **Synchronisation.** Same-device now, so a normal NVRHI barrier / the existing
   timeline suffices; no cross-device external-memory semaphore.

6. **Fallback unchanged.** Standalone/headless and any Kit session whose device
   fails the gate keep `VkDeviceManagerHeadless` + the readback. The color math
   (delegate writes linear, host applies sRGB) is identical on both paths, so the
   §25.O.3 / display-parity invariant (`viewer == headless == Kit`) must still hold.

## Alternatives considered

- **Cross-device external-memory import into Kit's Hgi (keep own-device).** Import
  our exported `VkImage` into Kit's `VkDevice` and wrap as an `HgiTexture`. Rejected:
  Hgi exposes no public API to adopt an external `VkImage`; would require Hgi
  internals.
- **Keep the readback, double-buffer it (read frame N-1).** Hides the stall without
  any architecture change. Rejected as the primary fix (still a full round-trip +
  CPU reconvert; 1-frame-stale), but is the safe fallback if shared-device proves
  infeasible.
- **Status quo (readback).** Already optimised (no per-frame allocs, targeted
  EventQuery wait). Rejected: the round-trip itself remains.

## Drawbacks / risks

- **Feasibility risk (gating).** If Kit's shared device did not enable the RT
  extensions, shared-device rendering is impossible — Pyxis can't ray-trace on it.
  Mitigated by the gate + fallback.
- **Hgi/NVRHI interop risk.** Adopting an Hgi-created `VkImage` into NVRHI (or vice
  versa) and getting the closed pxr engine to consume a GPU-resource AOV via
  `GetResource()` is unverified against Kit's engine; may not work as documented.
- **Device-lifetime risk.** Pyxis no longer owns the device; teardown ordering and
  not destroying Kit-owned resources must be exact.
- **Parity risk.** Re-verify `viewer == headless == Kit` (display-parity ctest)
  after the change — the color result must be byte-identical to today.
- **§32 reversal.** Contradicts RFC 0004's "Pyxis owns its own device" for the Kit
  case; this RFC amends §32 to "own-device by default; share the host device when
  hosted and the host device is RT-capable."

## Migration & impact

- No public API change (the delegate + engine are `Private/`); `RenderTargets` /
  §18 surface untouched. Affects M8a/M8b (Omniverse path perf) — a latency win, no
  milestone exit-criteria change.
- The own-device path stays the default and the only path for standalone/headless,
  so non-Kit behaviour is unchanged.

## Open questions

- Does Kit's shared `omni.gpu_foundation` device enable the full RT extension set
  Pyxis needs? (Moot — see "Why rejected".)
- Does `omni.hydra.pxr`'s `HdxColorCorrectionTask` consume a delegate-provided
  `GetResource()` GPU AOV, or does it always go through `Map()`? (Moot.)

## Revision — the GL external-memory path is viable (supersedes "Why rejected")

The original draft + "Why rejected" below assumed (a) Kit's pxr viewport runs an Hgi
**Vulkan** backend and (b) the closed engine never calls `HdRenderBuffer::GetResource()`.
A runtime probe (`HdPyxisRenderDelegate::SetDrivers` logs `Hgi::GetAPIName()`;
`StubRenderBuffer` logs `Map()`/`GetResource()` calls) proved **both wrong**:

```
SetDrivers: host Hgi API = 'OpenGL'
RenderBuffer::Map() called by host        (the CPU path we use today)
RenderBuffer::GetResource() called by host (the GPU path IS exercised)
```

So Kit's pxr engine runs **HgiGL**, and it **does** query `GetResource()`. That makes
the direct GPU AOV reachable WITHOUT sharing the device — via **Vulkan→GL
external-memory interop** (we already ship hgiGL + the exportable image):

1. `PyxisEngine` keeps its own Vulkan device and renders into the exportable
   `exportedColor` image (Win32 handle + `allocationSize`) — unchanged.
2. In Kit's GL context (current on the host render thread where `_Execute` /
   `GetResource` run), import that Win32 handle as a GL memory object +
   `glTextureStorageMem2DEXT` texture (`GL_EXT_memory_object_win32`) — once per size.
3. The color AOV is a normal HgiGL texture (`Hgi::CreateTexture`); each frame copy
   the imported texture into it (GPU→GPU, no CPU) with a **vertically-flipping
   `glBlitNamedFramebuffer`** (source rows 0..h → destination rows h..0), after a
   targeted wait on our device's render-complete (the existing EventQuery) so GL
   reads finished pixels. `StubRenderBuffer::GetResource()` returns that HgiGL handle.
   The flip is mandatory: Pyxis renders top-row-first (Vulkan) but the pxr engine
   presents the color AOV bottom-row-first (GL convention) — it mirrors the existing
   `WritePyxisColorToAov` readback flip. (`glCopyImageSubData`, the obvious copy, was
   the initial implementation and produced a **Y-inverted** viewport; it cannot flip,
   hence the framebuffer blit.)
4. `ReadbackColorHdr` / `WritePyxisColorToAov` are skipped on this path.

No device sharing (RT stays on our own device — no extension gate), no public API
change. Fallback to the CPU readback if any GL-interop step is unavailable (e.g. a
non-GL host, or `GL_EXT_memory_object_win32` missing). Implemented on this branch.

### Verification (World Lobby, 949×577, `DisplayParity.WorldLobby`)

- **Orientation.** Upright vs the standalone headless render: mean abs diff 2.76;
  the same capture compared *flipped* scores 72.9 — the blit flip is correct.
- **Color.** A no-render linear-ramp injected into the AOV and captured through Kit
  recovers Kit's transfer function as the **standard sRGB OETF**, matching Pyxis's
  `LinearToSrgb` to MAE **0.54** (vs 1.30 for pure gamma-2.2). Kit applies sRGB once;
  the delegate writes linear once — the `viewer == headless == Kit` invariant holds.
- **Pixel parity.** Per-channel abs-diff vs headless: **median 1**, mean 2.8, p95 5.
  The thin ~1% tail (50–250 LSB) is the bright windows — two *independent* renders of
  high-variance highlights, not a color/orientation bug (the residual is frame-index-
  independent and pixel-aligned: a shift search bottoms out at 0,0).
- **Gate.** `display_parity_check.py` uses robust median + mean: headless-vs-viewer
  strict (median 0, mean ≤ 1); Kit median ≤ 2 and mean ≤ 6 — which the correct capture
  passes and a flip (≈70), a double/zero sRGB encode (tens of LSB), or a 1-px
  misalignment (mean ≈17) all fail. The prior mean+correlation gate was flip-blind.

## Why rejected (SUPERSEDED — kept for history; the assumptions were wrong)

Both this RFC's shared-device design AND the lighter alternative proposed during
review (keep the own-device, import the already-exported `VkImage` into Kit's
device, and present it as the AOV's GPU resource) require the **same last mile**:
handing the closed `omni.hydra.pxr` viewport a GPU texture via
`HdRenderBuffer::GetResource()` → an `HgiTexture`. Constructing a Vulkan-backed
`HgiTexture` requires Pixar's **HgiVulkan** backend, which is present in **neither**
the Pyxis nv-usd build **nor** Kit:

- `build/omniverse/usd-deps/usd/lib` ships `usd_hgi`, `usd_hgiGL`, `usd_hgiInterop`
  only — no `usd_hgiVulkan` (and no `pxr/imaging/hgiVulkan` headers).
- Kit's `omni.usd.libs` ships the same three — no `usd_hgiVulkan.dll` and no
  Hgi-Vulkan plugin. Kit's GPU layer is omni/carb (`omni.gpu_foundation`), not
  Pixar Hgi-Vulkan.

The Hgi base class is backend-agnostic and exposes no `VkDevice`/`VkImage`. So a
generic Hydra delegate in this stack has no way to create or hand the pxr viewport
a GPU texture — the only portable AOV channel to the closed engine is the CPU
`HdRenderBuffer::Map()` path. The per-frame GPU→CPU readback is therefore the
required bridge, not an oversight. (Re-opening this would require Pixar HgiVulkan in
nv-usd + Kit consuming a GPU-resource AOV — both outside Pyxis's control.)

**Achievable instead (no new API):** the readback's per-frame allocations are
already gone (cached staging + scratch) and it uses a targeted EventQuery wait;
the remaining render-thread stall is hidden by **double-buffering** the readback
(map frame N-1's already-finished copy while frame N's copy is in flight). That is
implemented in lieu of this RFC.
