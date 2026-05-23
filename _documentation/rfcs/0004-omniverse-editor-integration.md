# RFC 0004: Omniverse editor integration (Pyxis as authoring + viewport renderer)

- Status: Draft
- Author(s): Vincent Legrand
- Created: 2026-05-22
- Last updated: 2026-05-23
- Implementation PRs: Stage 2 (Pyxis-side GPU interop) landed on this branch —
  see "Implementation status" below.

## Implementation status (2026-05-23)

**Stage 2 — Pyxis-side GPU external-memory interop: DONE & VERIFIED on hardware.**

- `VK_KHR_external_memory_win32` + `external_semaphore_win32` enabled (when the
  adapter advertises them) in both `VkDeviceManager` and `VkDeviceManagerHeadless`
  via the shared `Private/Device/ExternalInterop.h`. Logged at device bringup.
- New public class `pyxis::GpuInteropExporter`
  (`Public/Pyxis/Platform/Interop/GpuInteropExporter.h`,
  `Private/Interop/GpuInteropExporter.cpp`): creates exportable VkImages (backed
  by `VkExportMemoryAllocateInfo` OPAQUE_WIN32, dedicated allocation), adopts
  them into NVRHI via `createHandleForNativeTexture` so the renderer can target
  them, and exports an OPAQUE_WIN32 timeline semaphore + the device UUID.
- Unit tests (`tests/unit/GpuInteropRoundTrip.cpp`), all **PASSING on RTX 5000
  Ada (driver 581.80)**, skip cleanly on CPU-only CI:
  - `GpuInteropRoundTrip.ExportImportTimelineBlit` — exports an image + timeline
    semaphore from the Pyxis device (A), imports both into a *second* logical
    Vulkan device (B) standing in for Kit, performs the `VK_QUEUE_FAMILY_EXTERNAL`
    ownership release/acquire + cross-device timeline wait, and reads back the
    exact pixels A wrote (**zero host copy** of image content).
  - `GpuInteropExport.SupportsAovFormats` — every §25.I.1 format (RGBA16F color,
    R32F depth, R32_UINT id, RGBA8) exports a valid, NVRHI-adoptable image.
  - `GpuInteropExport.UnsupportedFormatFailsCleanly` — an off-table VkFormat
    fails with an invalid result, never a silent substitution.
  - `GpuInteropExport.DeviceUuidStableAndValid` — the device UUID used for the
    Kit-side device match is stable, valid, non-zero.
  - `GpuInteropExport.MultipleResourcesGetDistinctHandles` — multi-AOV frames can
    share several surfaces + semaphores at once.

This de-risks the hardest part of the RFC (§4 viewport handoff) independently of
the Kit SDK: both halves of the cross-device protocol are proven on real
hardware.

**Stage 3 "C3" (2026-05-23): Kit-side importer is now a shipping class too.**
`pyxis::GpuInteropImporter` (`Public/Pyxis/Platform/Interop/GpuInteropImporter.h`)
recreates a VkImage on the importing device backed by the exporter's shared
Win32 memory + imports the timeline semaphore. `GpuInteropRoundTrip` was
refactored so device B (the Kit stand-in) imports via this class — both
`GpuInteropExporter` and `GpuInteropImporter` are now exercised end-to-end and
**PASS on RTX 5000**. The Kit render pass (C4) consumes these two classes
directly; the interop is no longer a risk, only wiring remains.

**Stage 3 — `pyxis_hydra_omni` (Kit extension): gated on the nv-usd 25.11 pull.**
Kit 110.1.1 bootstraps via Packman and the empty template builds, but the full
kit-kernel (which ships nv-usd 25.11 + Hd) only pulls when a Kit *app* is added.
In Kit 110 `repo template new` is an **interactive wizard** (only `--help` /
`--generate-playback`); the non-interactive route is `repo template new
--generate-playback <file>` once (interactively) then `repo template replay
<file>` in CI. So the one manual step to unblock Stage 3 is, in
`D:\pyxis_external\kit-app-template`:

```
repo.bat template new            # pick "kit_base_editor", name pyxis.viewer
repo.bat build --config release  # pulls kit-kernel incl. nv-usd 25.11 + Hd
```

**UPDATE (2026-05-23): nv-usd 25.11 dev acquired — Stage 3 unblocked.** The
`basic_cpp_extension` template only pulls Carbonite, not USD, so the dev package
was pulled directly: the Kit kernel's `dev/all-deps.packman.xml` pins
`usd-release → usd.py312.windows-x86_64.stock.release 0.25.11.kit.2-gl.19811`.
A single-dependency `packman pull` (the full all-deps pull fails on internal-feed
packages like `abseil`, which a Hydra delegate doesn't need) landed it at:

```
D:/pyxis_external/usd-deps/usd   (PXR_MINOR_VERSION 25, PXR_PATCH_VERSION 11)
  include/pxr/{base,imaging,usd,usdImaging,...}   pxr.h, hd/renderDelegate.h,
                                                  hd/sceneIndex.h, hd/rendererPlugin.h,
                                                  usdImaging/stageSceneIndex.h
  lib/usd_*.lib  (92 import libs; note the usd_ prefix vs vcpkg's bare names)
  pxrConfig.cmake   ->  find_package(pxr) works
```

`pyxis_hydra_omni` builds out-of-tree against this USD root (its own CMake
configure, `find_package(pxr PATHS D:/pyxis_external/usd-deps/usd`), linking
Pyxis's prebuilt USD-free `pyxis_renderer` public surface. The Kit-side
image/semaphore *import* code is already proven (device B in
`GpuInteropRoundTrip`), so the remaining work is the USD/Hd delegate wiring +
extension packaging — not interop risk.

The shippable artifact remains the **extension** `omni.hydra.pyxis`; the
`kit_base_editor` app is only the dev host (see "Extension, not application").

**UPDATE (2026-05-23): Stage 3 "C1" done — delegate compiles + links against
nv-usd 25.11.** `sources/pyxis_hydra_omni/` (a real `HdRenderDelegate` +
`HdRendererPlugin` registration + `plugInfo.json`) builds to
`pyxis_hydra_omni.dll` with clang-cl against Kit's nv-usd. Notes from getting it
to link:
- Do **not** use `pxrConfig.cmake` — it `find_dependency()`s
  TBB/OpenSubdiv/MaterialX/Imath/Python3 `*Config.cmake` files the standalone
  package doesn't ship. The `usd_*.lib` are DLL import libs (deps resolved inside
  the USD DLLs) and all headers are bundled, so direct include/lib wiring is
  self-contained. USD libs carry a `usd_` prefix; `tbb.lib` pulls `tbb12.lib` via
  pragma so `lib/` must be on the link search path.
- nv-usd 25.11 is a **py312** build: headers pull `<pyconfig.h>` and inlined Tf
  glue references Python symbols, so Python 3.12 (the same one Kit ships) is a
  build dep — a different minor would ABI-mismatch.
- nv-usd's `HdRendererPlugin::IsSupported` uses NVIDIA's extended signature
  (`HdRendererCreateArgs const&, std::string* reasonWhyNot`), not stock USD's
  `bool gpuEnabled`.

**UPDATE (2026-05-23): Stage 3 "C2" done — delegate registers + is discoverable
in a live Kit process.** The extension is defined entirely in-repo
(`sources/pyxis_hydra_omni/extension/`): a prebuilt-native + Python-registration
extension (no Kit-premake build). On startup its Python module calls
`Plug.Registry().RegisterPlugins(<ext>/bin/resources)`. A headless `kit.exe`
smoke (enable `omni.hydra.pyxis`, `omni.usd.libs` dep only) confirms:

```
[omni.hydra.pyxis.extension] Registered Pyxis Hydra delegate ...; type discoverable = True
PYXIS_C2 registered=... type_found=True
```

i.e. `Plug.Registry.FindTypeByName("HdPyxisOmniRendererPlugin")` returns the type
inside a running Kit/USD process — so a full viewport app surfaces "Pyxis" in the
renderer list. `build.ps1` assembles the full extension (toml + Python + DLL +
plugInfo) into the Kit extension folder, stripping the wizard's C++ scaffold.

**UPDATE (2026-05-23): Stage 3 "C4" render data-path proven on hardware.**
`tests/unit/PyxisRenderToExportedImage.cpp` stands up the real renderer stack
(headless device + `GpuScene` + `PyxisRenderer`), allocates the exportable
RGBA16F target via `GpuInteropExporter`, clears it to a magenta sentinel, renders
a frame (a minimal triangle so `CommitResources` builds a TLAS), and reads it
back — asserting `PyxisRenderer` **overwrote the sentinel with finite radiance**,
i.e. it rendered directly into the Win32-shareable texture. Combined with C3
(a second device imports + reads such a texture), the **complete §4 viewport
handoff is verified end-to-end on the RTX 5000**: Pyxis renders → exportable
image → Kit-side device imports + reads, zero host copy. All 6 interop/render
unit tests pass together. Remaining C4 work is Hydra-side plumbing only: the
`HdRenderPass` calling this proven sequence, FSD prim adapters
(mesh/camera/material → `GpuScene`), and backing Kit's `HdRenderBuffer` with the
imported image.

**UPDATE (2026-05-23): Stage 3 "C4" plumbing — renderer linked into the
extension.** `sources/pyxis_hydra_omni/Private/PyxisEngine.{h,cpp}` is a USD-free
render driver that owns a Pyxis Vulkan device + `GpuScene` + `PyxisRenderer` +
`GpuInteropExporter` and runs the proven render→export→signal sequence; the
`HdRenderPass::_Execute` drives it. `pyxis_hydra_omni.dll` now **compiles +
links** against nv-usd 25.11 *and* the prebuilt USD-free `pyxis_renderer` /
`pyxis_platform` (Release/MD import libs). `build.ps1` stages the 45 runtime DLLs
+ path-tracer shaders so the extension is runtime-ready. Build gotchas resolved:
- The extension must compile at **C++23** (public headers use `std::expected` /
  `std::span`); nv-usd 25.11 builds fine at 23.
- Do **not** put vcpkg's bare `include/` on the path — it carries vcpkg USD 26.3,
  which silently shadowed nv-usd 25.11 and produced a delegate vtable against
  `pxrInternal_v0_26_3` that failed to link against the 25.11 libs. Only the
  `include/hlslpp` subdir is added; nv-usd is the sole `pxr/` provider.

Remaining for C4-full (runtime, needs the viewport app): verify the extension
loads + `PyxisEngine::Initialize` stands up its device inside the Kit process and
renders; back Kit's `HdRenderBuffer` with `GpuInteropImporter`; and wire the FSD
prim adapters (`CreateRprim`/`HdMesh` Sync → `GpuScene::CreateMesh`/
`AppendInstance`, camera → `SetCamera`) so it renders the USD scene rather than
just the empty-scene background. The render data-path itself is already proven on
hardware (C3 + C4 data-path).

**UPDATE (2026-05-23): Stage 3 "C4-full" — delegate renders a USD stage
end-to-end, VERIFIED on hardware (no Kit).** The FSD prim adapters are wired:
`HdPyxisOmniMesh::Sync` → `GpuScene::CreateMesh`/`AppendInstance` (fan-triangulated),
`HdPyxisOmniCamera::Sync` → `GpuScene::SetCamera` (matrix transpose per §10),
with material/light/render-buffer as functional stubs; the delegate owns the
`PyxisEngine` and shares its `GpuScene` with prims via `HdPyxisOmniRenderParam`.
A headless harness (`sources/pyxis_hydra_omni/tests/HdEngineSmoke.cpp`,
`run_smoke.ps1`) drives a USD stage through the delegate exactly as a Kit
viewport would — `UsdImagingStageSceneIndex` → `HdRenderIndex` → `HdEngine` —
and **passes on the RTX 5000**:

```
HdEngineSmoke: meshCount=1 instanceCount=1 readback=1 (1280x720)
PASS: HdEngine drove the Pyxis delegate end-to-end.
```

i.e. the full chain works: USD stage → scene index → `HdPyxisOmniMesh::Sync` →
`GpuScene` → render pass → `PyxisEngine::RenderFrame` → `CommitResources`
(BLAS/TLAS built, `instanceCount=1`) → exportable image read back.

**Packaging fix surfaced by this:** `build.ps1` must NOT stage vcpkg's USD 26.3
`usd_*.dll` (or `tbb12.dll`) into the extension — Kit provides nv-usd 25.11, and
shipping the same-named 26.3 DLLs shadows it and breaks the delegate
(entry-point mismatch vs the 25.11 ABI). Fixed: only `pyxis_renderer`/`platform`
+ non-USD deps are staged.

**UPDATE (2026-05-23): material + light adapters done & verified.**
`HdPyxisOmniMesh` now resolves its bound material — `GetMaterialResource` →
parse the `UsdPreviewSurface` network (diffuseColor/metallic/roughness/emissive/
opacity) → `OpenPBRMaterialDesc` → `GpuScene::AcquireMaterial` → set on the
instance. `HdPyxisOmniLight` (distant/dome/rect) → `GpuScene::AddLight` (color +
intensity from light params, direction/position from the transform). The harness
authors a bound material + a distant light; the full chain passes:

```
HdEngineSmoke: meshCount=1 instanceCount=1 materialCount=1 lightCount=1 readback=1
PASS
```

Correctness fix: material bindings only resolve through the **full**
`UsdImagingCreateSceneIndices` chain (binding-resolution + flatten filters), not
the bare `UsdImagingStageSceneIndex` — the harness uses the former.

So the **complete v1 Hydra ingest+render chain (geometry + camera + material +
light) is verified end-to-end on hardware, without Kit.** The only work that
genuinely needs a *running* Kit viewport to validate (so it cannot be proven in
this headless setup): aliasing Kit's `HdRenderBuffer` to `GpuInteropImporter` so
Kit composites Pyxis's exported image (the import half is proven, C3), engine
resize-to-viewport, and packaging the `pyxis.viewer` app (deliverable 2; the
`repo template new` wizard is interactive). Code for the buffer aliasing can be
written; verification needs the viewport.

**UPDATE (2026-05-23): KEY FINDING — Kit 110's viewport cannot select an
external USD Hydra delegate.** The extension loads + registers correctly in a
live Kit editor (`type discoverable = True` confirmed in `pyxis.editor`), but
"Pyxis" cannot be chosen as the viewport renderer because:
- `omni.kit.viewport.menubar.render`'s `hd_renderer_list.py` **hardcodes** the
  selectable engines to `rtx, iray, index, pxr` — no API to add a named entry.
- The only USD-Hydra-delegate slot is the **`pxr`** engine (`omni.hydra.pxr`),
  which is **not shipped in Kit 110** (present hydra exts: `rtx`,
  `scene_delegate`, `usdrt_delegate`). So `pxr` is greyed out / unselectable.

Registering a USD `HdRendererPlugin` (our `plugInfo.json`) makes the delegate
discoverable to *usdview-style HdEngine hosts* (proven by `HdEngineSmoke`), but
Kit's viewport populates its menu from Kit's own compiled engine registry
(`rtx.hydra.dll`), not USD's plugin registry. This is a Kit-platform constraint,
not a delegate defect.

Paths to actually display Pyxis in a Kit window (pick one, post-this-RFC):
1. **Custom viewport extension** — a Kit extension with its own viewport widget
   that runs our HdEngine and blits the exported image into a Kit UI texture via
   `GpuInteropImporter` (interop already proven, C3). Bypasses the renderer menu.
   *Recommended* — tractable, reuses verified interop.
2. **Kit C++ engine plugin** — implement Kit's internal `IHydraEngine` (as
   `rtx.hydra.dll` does) wrapping the delegate. Proper but uses closed Kit APIs.
3. **usdview / headless / Mode-A file workflow** — already functional.

The delegate + render + ingest chain remains fully verified; only Kit's
viewport-renderer *selection* is closed in 110.

**UPDATE (2026-05-23): Option B (Kit C++ engine plugin) confirmed infeasible
with the public SDK.** `omni::usd::UsdManager::registerHydraEngineFactory(name,
IHydraEngineFactoryPtr)` is a *public* registration hook, BUT the interface it
requires — `omni::usd::hydra::IHydraEngineFactory` and the `IHydraEngine` it
produces — is only **forward-declared** in the dev headers
(`class IHydraEngineFactory;`); the method definitions live in NVIDIA's internal
source (what `rtx.hydra.dll` implements) and are not shipped. So a conformant
engine cannot be written against the public Kit 110 SDK. B needs NVIDIA's
internal Hydra-engine SDK / RTX renderer source. → Option A (custom viewport
extension) is the feasible in-viewport path; C (usdview/headless/file) already
works.

### Option B unblock plan (chosen path — parked on NVIDIA internal SDK)

**What to request from NVIDIA** (Omniverse developer relations / partner program
/ kit-sdk source access): the definitions of
`omni::usd::hydra::IHydraEngineFactory` and the `IHydraEngine` it creates — the
interface `rtx.hydra.dll` implements. The *registration* hook
(`UsdManager::registerHydraEngineFactory`) is already public; only the interface
to inherit is missing.

**Lead that may reduce the ask:** `usdrt::hydra::ISimpleEngine`
(`.../kit-kernel/.../dev/fabric/include/usdrt/hydra/engine/ISimpleEngine.h`) IS
fully defined and is a render engine (`render_abi(stageId, params)`,
`setCameraState_abi`, `setRenderViewport_abi`, `setLightingState_abi`,
`isConverged_abi`, pause/resume). Ask NVIDIA whether implementing + registering a
`SimpleEngine` (omni.core `IObject`) is the supported route for a custom viewport
renderer in Kit 110 — if so, B is feasible with the *public* fabric SDK.

**Integration design (ready to execute once the interface is in hand):**
1. New Carbonite plugin `omni.hydra.pyxis.engine` (Kit-premake C++, against the
   engine interface) that on startup calls
   `UsdManager::registerHydraEngineFactory("pxr", PyxisHydraEngineFactory)` — the
   `"pxr"` slot is empty in Kit 110, so registering it lights up the existing
   (currently greyed) "pxr" entry in the viewport renderer menu. (Or register a
   new name and add it to `exts."omni.kit.viewport.menubar.render".autoManage.enabledList`.)
2. `PyxisHydraEngine::render_abi` / Execute → drives the existing `PyxisEngine`
   (device + GpuScene + PyxisRenderer + exporter, already built) and uses
   `GpuInteropImporter` (already proven, C3) to present the rendered image into
   the engine's output surface; `setCameraState`/`setLightingState` feed
   `GpuScene::SetCamera`/`AddLight`.
3. Scene data already flows through the existing `HdPyxisOmni*` adapters
   (mesh/camera/material/light → `GpuScene`, verified end-to-end).

**Already done, so B is a small finish:** the renderer, GPU interop (export +
import), the FSD prim adapters, and the reproducible build are all complete and
hardware-verified. B reduces to (a) the engine-interface shim + (b) presenting
into Kit's surface — both unblocked the moment the interface header arrives.

**UPDATE (2026-05-23): live experiment in `pyxis.editor` confirms B's blocker +
clarifies the ISimpleEngine role.** Forcing `--/renderer/enabled=rtx,pxr`
(+ `autoManage.canRemove=false`) makes **"Pyxis" appear and be selectable** in
the Render menu (alongside "Pixar Storm") — Kit enumerates it via
`Tf.Type("HdRendererPlugin").GetAllDerivedTypes()` (probe logged
`count=2 pyxis=True plugins=[HdStormRendererPlugin, HdPyxisOmniRendererPlugin]`).
But selecting it fails:
```
UsdContext::createViewport - unable to find suitable engine for config
```
i.e. the viewport needs a Kit **hydra engine factory** for the `pxr` path, and
none is registered in Kit 110 (only RTX's, in `rtx.hydra.dll`). This is exactly
the `registerHydraEngineFactory(IHydraEngineFactory)` blocker — `IHydraEngineFactory`
is closed. Crucially, **`ISimpleEngine` does NOT satisfy `createViewport`**: it's
a *standalone* Hydra host (omni.core `IObject`, createable, with
`setRendererPlugin`), not the viewport's engine-factory type. So ISimpleEngine
does **not** unblock the menu/viewport path — it only unblocks **Option A**
(create a SimpleEngine, `setRendererPlugin("HdPyxisOmniRendererPlugin")`, render,
blit its AOV into a custom Kit panel). Net: enumeration + selection work; the
*viewport render backend* is the closed piece. → Pursue B via NVIDIA's internal
`IHydraEngineFactory`, or do A with ISimpleEngine for an in-Kit Pyxis panel.

**UPDATE (2026-05-23): host-facing render path complete + verified — Option A is
unblocked.** `HdPyxisOmniRenderPass::_Execute` now reads the render-pass-state's
AOV bindings and **composites Pyxis's rendered color into the host's bound color
`HdRenderBuffer`** (RGBA16F memcpy; RGBA8 / Float32 conversions) — the exact
presentation surface every Hydra host reads (usdview, `UsdImagingGL`, a custom
Kit panel). `HdEngineSmoke` was extended to bind a color render buffer + a
framing `UsdGeomCamera` and **passes on RTX 5000**:
```
engineNonZeroPixels=921600  aovNonZeroPixels=921600  (1280x720)   PASS
```
i.e. Pyxis renders the full frame (camera + distant light + UsdPreviewSurface
material) AND writes every pixel into the host buffer. (The earlier black output
was a missing camera, not the compositing — root-caused.)

**Consequence:** the delegate is now a complete, host-agnostic Hydra renderer —
geometry/camera/material/light ingest + render + present-to-AOV, all verified
headlessly. Any Hydra host that binds a color buffer gets Pyxis pixels. **Option
A (custom Kit panel)** reduces to: a Python `omni.ui` window that drives an
HdEngine with the Pyxis delegate (as `HdEngineSmoke` does) + an AOV-bound
render-pass-state, and displays the resulting `HdRenderBuffer` (CPU upload to an
`omni.ui.ByteImageProvider`, or `GpuInteropImporter` for zero-copy). The only
remaining closed piece is the Kit *viewport's* engine-factory (Option B).

**UPDATE (2026-05-23): tooling consolidated into the repo + test coverage
expanded.** All Omniverse build artifacts now live under `<repo>/build/omniverse`
(gitignored) — the Kit SDK clone, nv-usd, and Packman cache; no sibling external
folder. CMake `PXR_USD_ROOT` and all scripts are repo-relative. Added
`_tools/omniverse/check.ps1` — a prerequisite verifier (clang-cl, cmake, ninja,
git, vcpkg, GPU, Vulkan + `external_memory_capabilities`, Kit SDK, nv-usd 25.11
import libs, Python 3.12, prebuilt renderer, shaders) that prints OK/MISSING +
fix hints and exits non-zero on a missing requirement. Tests:
- gtest (`build/dev`): GPU-interop suite now 10 cases (`GpuInteropRoundTrip` +
  `GpuInteropExport` formats/uuid/multi-resource/timeline + `GpuInteropImport`
  create / invalid-memory-handle / invalid-semaphore-handle) + the C4
  `PyxisRenderToExportedImage` — all pass on RTX 5000, skip on CPU-only CI.
- ctest (`build/omni`): `HdEngineSmoke` registered (runs via `run_smoke.ps1`),
  asserting the composited AOV is **byte-identical** to the engine render.

**Reproducible build (`_tools/omniverse/`).** A clean clone can't build the Kit
deliverables alone (SDK is Packman-only), but `setup.ps1` (acquire nv-usd 25.11 +
Python 3.12 non-interactively) then `build.ps1` (configure + compile + stage into
the extension) reproduce deliverable 1; both verified to run clean. `deps/nv-usd.packman.xml`
pins the USD package. See `_tools/omniverse/README.md`.

## Deliverables

1. **`omni.hydra.pyxis`** — the Hydra render-delegate **extension**. The reusable
   artifact: drop into any Kit app (USD Composer, custom Kit apps) to select
   Pyxis as the renderer. Built by `_tools/omniverse/build.ps1`.
2. **A packaged Omniverse editor** — the `kit_base_editor` app + the extension
   (Pyxis as default renderer), via `repo package`. A turnkey "Pyxis Viewer."
   "Full" here means base-editor grade (viewport, stage tree, property panel,
   basic authoring) — *not* USD-Composer-grade authoring, which is a heavier
   app-assembly effort orthogonal to Pyxis. We embed Pyxis as the renderer in a
   Kit editor app; we do not build an editor from scratch.

## Resolved decisions (2026-05-23)

The three open questions below are resolved by maintainer direction:

1. **Version pin = Omniverse Kit 110.1.1 → OpenUSD 25.11.** Kit 110.1.1 is the
   latest release (2026-05-12); it builds against OpenUSD **25.11** and drives
   Hydra through the **Fabric Scene Delegate (FSD)** with multi-GPU rendering.
   USD 25.11 is one minor below Pyxis's vcpkg 26.3, so the shim's USD call-site
   port is small.
2. **Pin scope = targeted.** The main project stays on vcpkg USD 26.3; only the
   new `pyxis_hydra_omni` target builds against Kit's nv-usd 25.11 (via Packman).
   `pyxis_renderer`/`pyxis_platform` stay USD-free (§1) and are shared verbatim.
3. **Viewport handoff = GPU interop, mandatory. No CPU readback path.** Vulkan
   `VK_KHR_external_memory_win32` exports the Pyxis-rendered color/depth/AOV
   images; the Kit device imports them. Synchronization via an exported
   `VK_KHR_external_semaphore_win32` timeline semaphore. The CPU-readback option
   is struck from §4.
4. **Scope = full implementation + complete tests** (gtest unit + ctest golden),
   not a sketch.

**Verified environment (2026-05-23, dev workstation):** RTX 5000 Ada Laptop
(16 GB, driver 581.80), Vulkan 1.4.341 advertising
`VK_KHR_external_memory_capabilities` + `VK_KHR_external_semaphore_capabilities`,
clang 22 (clang-cl), CMake 4.3, vcpkg at `C:\Users\vlegrand\vcpkg`. Pyxis already
exposes the two interop escape hatches: `IDeviceManager::GetVulkanContext()`
(`VulkanContext.h` → raw `VkInstance`/`VkPhysicalDevice`/`VkDevice`/queue) and
NVRHI `getNativeObject(VK_Image | VK_DeviceMemory)`. No external-memory use
exists yet, so this RFC owns that surface.

**Verification strategy:** the Pyxis export side + a **two-device in-process
round-trip test** (export from Pyxis's `VkDevice`, import into a second
`VkDevice`, blit, pixel-compare) is buildable and runnable on the workstation
GPU **without the Kit SDK** — that de-risks the hardest piece independently. Only
the Kit-side import + extension is gated on the Packman SDK pull.

## Summary

Define how an NVIDIA Omniverse Kit editor (USD Composer / Kit-based app) drives
Pyxis as its renderer. Three integration modes are identified — **(A) decoupled
file**, **(B) live file / Nucleus**, **(C) in-viewport Hydra delegate**. A and B
require essentially no renderer change and are not relitigations of scope; **C is
a genuine scope expansion** (§1 ingestion contract, §42 "not a network/farm
renderer") and is the reason this RFC exists. The recommendation is to ship A
immediately, B as a thin follow-up, and treat C as a post-v1 track gated on this
RFC's acceptance because C's cost is dominated by a USD-ABI dependency fork, not
renderer work.

## Motivation

Pyxis already ships `hdPyxis` — a Hydra 2.0 render delegate
(`HdPyxisRendererPlugin : HdRendererPlugin`, registered via `plugInfo.json`,
talking only to the `GpuScene` / `PyxisRenderer` public API per §7, §18, §25).
Omniverse Kit editors load renderers **as Hydra render delegates**. So "use
Omniverse with Pyxis" is far less greenfield than it sounds — the question is
not *whether* Pyxis can be a Hydra host's renderer (it is designed to be one,
§6) but *which* Omniverse-specific realities (USD ABI, Fabric, viewport buffer
handoff, Kit packaging) the existing delegate must additionally satisfy.

Who's blocked: nobody on the v1 critical path. A/B strengthen the existing
ingest-parity invariant (§25.O.3) by adding an Omniverse-authored fixture. C is
a forward-looking capability with no v1 milestone dependency.

## Detailed design

### Three modes (they share almost no implementation)

| Mode | User action | Where Pyxis runs | New code |
|---|---|---|---|
| A. Decoupled / file | Author in USD Composer, save `.usd`/`.usdc` | Pyxis loads file (`usd_ingest` or `hdPyxis` in usdview) | ~none |
| B. Live file / Nucleus | Author + save; Pyxis hot-reloads | Separate Pyxis process watching the layer | small |
| C. In-viewport delegate | Pick "Pyxis" in the viewport renderer dropdown | Inside the Kit process, as a loaded Hydra delegate | large |

### The dominating constraint: USD ABI alignment

A Hydra render delegate is a DLL loaded **into the host's process**. It must be
compiled against the **exact same USD build** the host uses — same version, same
compiler/CRT, same `TfType`/`Sdf`/`Hd` symbol layout. There is no ABI stability
across USD versions or build flavors.

- **Pyxis today**: vcpkg OpenUSD **26.3**, `--no-python`, no Boost, MaterialX on
  (`vcpkg.json`, §6).
- **Omniverse Kit**: ships its own **nv-usd / OpenUSD fork** (Kit 106/107-era is
  roughly USD 24.x, *with* Python, *with* Omniverse schemas, NVIDIA toolchain).

These do not match. Consequences:

- **Mode A/B**: irrelevant. USD is a file format; the two USD builds never share
  a process. This is *why* A/B are cheap.
- **Mode C**: this mismatch **is the project**. A vcpkg-USD-26.3 `pyxis_hydra.dll`
  cannot load into a Kit-USD-24.x process. C requires a **second build of the
  Hydra shim compiled against Omniverse's USD SDK** (`omni.usd` libs via Packman).
  See "Build & packaging" below.

### Mode A/B architecture (data plane is a file)

```
Omniverse USD Composer ──.usd/.usdc──▶ Pyxis process
  (Nucleus / local FS)                   pyxis_usd_ingest (one-shot)  OR
       ▲                                 pyxis_hydra in usdview
       └──── Mode B: file/Nucleus change ──▶ re-ingest
```

No public-surface change. The §29.4.a resolver chain handles asset resolution;
Nucleus is either an `omniverse://` `ArResolver` plugin or a local Nucleus sync
(zero Pyxis code). "Live" in Mode B = re-run the one-shot importer on change
(§O.2 — `pyxis_usd_ingest` has no `UsdNotice` listener by design), not
incremental updates.

### Mode C architecture (data plane is shared process memory)

```
Kit process:
  Viewport ▶ UsdImagingStageSceneIndex (via Fabric/FSD)
           ▶ HdRenderIndex ──renderer──▶ hdPyxis (DLL built vs Kit's USD)
                                            │ public GpuScene API
                                            ▼
                                     pyxis_renderer (own NVRHI/Vulkan device)
                                            │ renders into
  Viewport ◀── HdRenderBuffer (color/depth) ◀── CPU readback OR GPU interop
```

`hdPyxis` already does almost all of this for usdview (§7, §25.K). The delta
from the existing delegate is exactly four items:
1. Built against Kit's USD (see ABI constraint above).
2. Survives the **Fabric Scene Delegate** (below).
3. Hands pixels to **Kit's viewport** rather than usdview's (below).
4. Packaged as a **Kit extension** (below).

### Fabric Scene Delegate (Omniverse-specific)

Modern Kit drives Hydra through **Fabric (USDRT) / the Fabric Scene Delegate
(FSD)** rather than feeding `UsdImagingStageSceneIndex` directly. Because FSD
still emits standard `HdSceneIndex` data, a pure Scene-Indices delegate consumes
Fabric-sourced data transparently — and Pyxis's delegate is mandated pure
Scene Indices (§6, no legacy `UsdImagingDelegate`). ✅ already aligned.

Watch-items:
- Prim *types* and *dirty bits* via Fabric can differ in granularity → the §24
  DirtyBits→GpuScene table needs a Fabric-fed conformance pass.
- Omniverse adds non-standard schemas (MDL `OmniPBR`/`OmniGlass`, `omni:` lights,
  physics). These miss Pyxis's supported-types list (`mesh/camera/*Light/material`,
  §7) and are dropped + logged in `unsupported_features.json`. MaterialX
  (`open_pbr_surface`/`standard_surface`) *does* flow through
  `pyxis_material_translation`; MDL does not and is post-v1 (§42).

### Viewport handoff (Mode C's second-hardest problem)

Pyxis renders with NVRHI/Vulkan into **its own device** (§32 — it does not share
Kit's RTX device; two Vulkan devices in one process is fine, the only contact
point is the buffer copy). Kit composites results it gets via `HdRenderBuffer`.

**Decision: GPU interop, no CPU readback** (resolved 2026-05-23). Pyxis renders
into NVRHI `VkImage`s whose backing `VkDeviceMemory` is allocated *exportable*
(`VkExportMemoryAllocateInfo`, `VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32`).
The exported Win32 `HANDLE` is imported by Kit's Vulkan device
(`VkImportMemoryWin32HandleInfoKHR`) as the storage for an
`HdRenderBuffer`-backing image, so Kit composites Pyxis pixels with **zero host
copy**. Cross-device ordering uses an exported timeline semaphore
(`VK_KHR_external_semaphore_win32`): Pyxis signals value N when the frame's
images are ready; Kit waits on N before sampling.

Mechanism summary:

| Concern | Mechanism |
|---|---|
| Image storage | `VkExportMemoryAllocateInfo` OPAQUE_WIN32 on the AOV `VkDeviceMemory`; Win32 `HANDLE` via `vkGetMemoryWin32HandleKHR` |
| Device match | `VkPhysicalDeviceIDProperties::deviceUUID` must match between Pyxis & Kit devices; if not (multi-GPU), fall back is an error (no silent CPU copy) |
| Sync | exported timeline semaphore; Pyxis signals, Kit waits (`vkGetSemaphoreWin32HandleKHR`) |
| Layout | images created `VK_IMAGE_LAYOUT_GENERAL`-compatible with explicit queue-family `EXTERNAL` ownership transfer |
| AOV formats | per §25.I.1 — color RGBA16F, depth R32F, normal/albedo/id as listed |

This extends the `CopyToHydraRenderBufferPass` contract (§25.I.1): in the Kit
build the "copy" is a no-op alias (imported memory *is* the render target).
A device-UUID mismatch or missing extension is a hard `ErrorKind` (logged,
delegate reports unsupported) — never a silent readback fallback.

### Extension, not application

`pyxis_hydra_omni` ships as a **Kit extension** (e.g. `omni.hydra.pyxis`) that
registers Pyxis as a **Hydra render delegate**. It is loaded *into* existing Kit
applications — USD Composer, or any Kit app with a viewport — where the user
selects "Pyxis" from the renderer dropdown. Pyxis does **not** ship a standalone
Kit application. The `kit_base_editor` *app* used during bring-up is purely a
**development host**: it pulls the Kit SDK (incl. nv-usd 25.11) and provides a
viewport to load the extension into. The shippable artifact is the extension,
which third parties drop into their own Kit apps.

### Build & packaging (Mode C)

```
pyxis_renderer / pyxis_platform / pyxis_material_translation   ← USD-free (§1), built once
        ▲                                   ▲
pyxis_hydra (vcpkg USD 26.3)        pyxis_hydra_omni (Kit nv-usd via Packman)
   → usdview / Pyxis app                → Kit extension (omni.hydra.pyxis)
```

`pyxis_renderer` being USD-free by mandate (§1, §30.3) is the architectural
payoff that makes C feasible — only the thin Hydra shim recompiles. New CMake
target `pyxis_hydra_omni` (SHARED): same sources as `pyxis_hydra`, USD roots
pointed at the Kit SDK USD via Packman, `/EHsc` + RTTI (USD needs it, §30).
Package as a Kit extension (`extension.toml` + `plugInfo.json` resolving to
`pyxis_hydra_omni.dll` + renderer/platform DLLs). This build is out-of-tree from
the main vcpkg toolchain and pinned to a specific Kit SDK SHA, mirroring §49.

### Phased implementation steps

- **Phase A** (days, mostly validation): round-trip a USD-Composer-authored
  scene through `usd_ingest` and `hdPyxis`; run ingest-parity (§25.O.3) on an
  Omniverse fixture; catalogue dropped schemas; add an Omniverse golden test.
- **Phase B** (1–2 wk): layer/file watcher → re-ingest; optional `omniverse://`
  `ArResolver` (or rely on Nucleus local-sync).
- **Phase C** (post-M9, multi-month, gated on this RFC):
  - C0 — this RFC accepted (Kit version pin, supported-schema matrix, success criteria).
  - C1 — `pyxis_hydra_omni` loads in a minimal Kit app, appears in renderer list (ABI work).
  - C2 — first pixels into the viewport via CPU readback (§25.I.1); validate color+depth.
  - C3 — drive via FSD; re-validate §24 DirtyBits; confirm live edits propagate `Dirty<*>`.
  - C4 — viewport camera + `normal`/`albedo`/`id` AOVs (§25.I.1).
  - C5 (optional) — GPU interop, only if readback profiles as the bottleneck.

## Alternatives considered

1. **Reuse the existing `pyxis_hydra.dll` directly inside Kit (no second build).**
   Rejected: violates the USD-ABI rule — a vcpkg-USD-26.3 DLL cannot load into a
   Kit-USD-24.x process. Non-negotiable; this is the central reason C is expensive.
2. **Align Pyxis's vcpkg USD pin to Kit's USD version so one build serves both.**
   Rejected: pins the whole project's USD to Omniverse's fork cadence (older,
   Python-on, Boost history), regressing §6 decisions and dragging every other
   target. Bumping USD MAJOR is itself an RFC item (§44.1).
3. **Share Kit's RTX/Vulkan device instead of standing up Pyxis's own.**
   Rejected: §32 makes Pyxis own and destroy its NVRHI device; cross-engine
   device sharing is far more fragile than a buffer copy and buys nothing for a
   progressive path tracer.
4. **Skip the in-viewport delegate entirely; ship only A/B.**
   Genuinely viable and the conservative default — A/B deliver most of the
   "author in Omniverse, render in Pyxis" value at a fraction of C's cost. This
   RFC keeps C as a *gated, optional* track precisely so this remains the
   fallback if C0's cost estimate proves prohibitive.

## Drawbacks / risks

Ranked:
1. **USD ABI mismatch** — make-or-break for C; the bulk of C's cost is incurred
   *before* the first pixel. Mitigation: isolate to `pyxis_hydra_omni`, pin Kit hard.
2. **Kit version churn** — every Kit upgrade may force a USD rebuild. Treat
   Omniverse support as a versioned matrix, not "latest."
3. **Fabric data-granularity surprises** — validate the §24 dirty-bit map
   empirically under FSD (C3).
4. **Schema gaps** (MDL, `omni:` prims) — bounded by drop-and-log, but sets the
   expectation that Pyxis renders the USD/MaterialX subset, not Omniverse's full
   proprietary surface.
- No public-API or regression-image impact for A/B. C adds no public surface
  either (the delegate already only calls §18 API); its risk is build/packaging
  and the viewport copy path, not ABI of the frozen PODs.

## Migration & impact

- **A**: no milestone change — it is M4–M5 already done, re-pointed at
  Omniverse-authored input. Adds one golden fixture (§35, no RFC needed for the
  fixture itself).
- **B**: small standalone feature; no milestone dependency.
- **C**: post-v1 track. Depends on the full HdEngine pipeline (M5+ per the M4
  note in CLAUDE.md), which the current M4 stub does not yet provide (stub
  RenderPass/RenderBuffer). Should not start before M9. Owners: renderer +
  build/CI (new out-of-tree Kit-pinned target).

## Open questions

Resolved (see "Resolved decisions" at top): Kit/USD pin (110.1.1 / 25.11),
targeted pin scope, GPU-interop-not-readback, full-impl-with-tests.

Still open:
- Does the Pyxis & Kit `deviceUUID` reliably match on the target hardware? On a
  single-GPU workstation (verified RTX 5000) yes; on Optimus/multi-GPU laptops
  Kit may select a different adapter. Mitigation is the hard-error path, but a
  diagnostic that names both UUIDs is desirable.
- Do we ship the `omniverse://` `ArResolver` (Phase B) or mandate Nucleus
  local-sync to keep Pyxis USD-resolver-plugin-free?
- Does B's "re-ingest on change" satisfy the intended interactivity, or is C's
  per-edit liveness required — i.e. is B a real product or a stepping stone?
