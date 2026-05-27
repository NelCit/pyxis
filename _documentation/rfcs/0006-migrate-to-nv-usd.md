# RFC 0006 — Migrate the whole project from vcpkg OpenUSD 26.3 to Kit nv-usd 25.11

- **Status**: Accepted (owner-approved 2026-05-23)
- **Supersedes**: the §6 decision to standalone-build vcpkg OpenUSD 26.3; narrows
  RFC 0004's "only the omni shim uses nv-usd" to "the whole project uses nv-usd".

## Summary

Re-base every USD-consuming module of Pyxis onto **Kit's nv-usd 25.11** (the same
OpenUSD that Omniverse Kit 110.1.1 ships), replacing the vcpkg OpenUSD 26.3 the
standalone currently links. The renderer (`pyxis_renderer`) and platform
(`pyxis_platform`) remain **USD-free** and are unaffected.

## Motivation

`pyxis_hydra` (the Hydra render-delegate ingest adapter) and `pyxis_hydra_omni`
(the Omniverse delegate) are the **same delegate forked across two USD versions**
— the only reason for the fork is ABI: a DLL built against vcpkg USD 26.3
(`pxrInternal_v0_26_3`) cannot load in Kit, which requires nv-usd 25.11. The fork
has already cost duplicated bugs (a parallel-sync `GpuScene` race and a
free-camera black-render bug were fixed in the omni copy but are still latent in
`pyxis_hydra`).

Standardising on **one USD** collapses the fork: `pyxis_hydra` compiles once and
loads in both the standalone host and Kit. `pyxis_hydra_omni` reduces to thin
Kit-only glue (`PyxisEngine` device/renderer driver + USD-plugin registration),
not a second delegate. It also keeps Pyxis's USD identical to the Omniverse
ecosystem it targets.

## Consequences (accepted)

- **+Python 3.12 + Boost**: nv-usd 25.11 is a py312 build; its headers pull
  `<pyconfig.h>` and Tf's python glue, and it ships Boost. The standalone — today
  deliberately `--no-python`, no Boost (§6) — gains these as runtime/link deps.
  v1's "zero embedded Python" stance (§47) is revisited: Python becomes a USD
  *implementation* dependency, not a Pyxis scripting surface.
- **Regression baselines re-bless**: byte-identical golden EXRs (§33.7) were
  generated under USD 26.3; nv-usd 25.11 can shift tessellation / material
  resolution, so every baseline image is regenerated and reviewed once.
- **Build**: USD is removed from `vcpkg.json`; the USD-consuming targets wire
  nv-usd from `build/omniverse/usd-deps/usd` (same source RFC 0004 already uses),
  via a shared CMake module. OpenSubdiv / OpenVDB / MaterialX (vcpkg ports that
  fed `pyxis_usd_ingest`) must be satisfied from nv-usd's bundled set or
  re-pinned; audited in Phase 2.
- **`AssetLocator` Resources override** (added for Kit) stays — harmless for the
  standalone (empty override → exe-dir behavior).

## Non-goals

- The renderer/platform stay USD-free (§1) — no change.
- No change to the public renderer API (§18) or `GpuScene` contract.

## Phased plan

**Phase 1 — foundation.** Extract `pyxis_hydra_omni`'s nv-usd wiring (include/lib
dirs, the `usd_*` link set, python312) into a reusable CMake module
(`cmake/PyxisNvUsd.cmake`, `pyxis_use_nv_usd(target)`). **Done.**

**ABI-coupling constraint (important):** USD types cross every link edge between
the USD-consuming modules — `pyxis_hydra` links `pyxis_material_translation`;
`pyxis_usd_ingest` links it too; `pyxis_app` links both adapters. You CANNOT have
one of them on nv-usd while a linked sibling is on vcpkg USD 26.3 (mismatched
`HdMaterialNetwork` / `TfToken` / `VtArray` ABIs → corruption). So they migrate as
**one coupled set**, not one-at-a-time.

**Phase 2 — flip the USD-consuming set together.** In a single step, re-point
`pyxis_material_translation`, `pyxis_hydra`, and `pyxis_usd_ingest` at nv-usd via
`pyxis_use_nv_usd` (drop their `find_package(pxr)` + vcpkg `usd*`/`hd*` target
links). Fix 26.3→25.11 API deltas (the omni fork already compiles equivalent
delegate code against 25.11, so deltas should be small). Resolve the
OpenSubdiv/OpenVDB/MaterialX story (nv-usd bundles some; others stay vcpkg only if
they don't exchange USD types). Build the three green. **Done** — all three build
clean against nv-usd 25.11. Deltas: nv-usd's Tf python glue leaks `#define PLATFORM
"win32"` (pyconfig.h) → `#undef PLATFORM` guard in `LogCategories.h`; OpenSubdiv
must come from nv-usd's bundled v3_6_0 (link its `osdCPU.lib`, drop vcpkg
opensubdiv — its headers would shadow + mismatch ABI).

**Phase 3 — app + deps.** Migrate `pyxis_app`; add Python 3.12 + Boost to the
runtime/staging; remove `usd` (and now-redundant USD-only ports) from
`vcpkg.json`. Standalone `pyxis.exe --headless` renders the default scene + World
Lobby green. **Done.** `pyxis_app` + `pyxis_unit_tests` use the includes+libs-only
`pyxis_nv_usd_includes_and_libs` (the app stays on the exception-free perimeter —
USD headers are catch-free, so they compile under `_HAS_EXCEPTIONS=0` exactly as
against vcpkg). nv-usd's `lib/usd` plugin tree + `lib`/`bin` DLLs + python312/3.dll
stage next to `pyxis.exe`. `usd` + `opensubdiv` removed from `vcpkg.json`. World
Lobby renders headless (951 meshes, 155 materials, 30 lights); 266 unit tests pass.

Investigated the World Lobby look-diff vs the old (vcpkg) build before re-blessing
(rigorous isolation): material translation byte-identical (122/122 desc dumps),
material bindings byte-identical (941/941), platform.dll byte-identical render
(swap test, RMSE 0). The entire ~0.26 RMSE is **USD-core geometry/normals** on the
reflective floor + glass — nv-usd 25.11 is what Omniverse Kit renders, so it is the
artist-faithful result, not a regression. The old regression baseline was
independently stale (vcpkg `main` missed it by ~14×). Baseline re-blessed under
nv-usd; regression PASS (RMSE 0).

**Phase 4 — collapse the fork.** Make `pyxis_hydra`'s delegate the single source
of truth (port the omni fork's materials/lights + the race-mutex + camera-from-
state fixes into it). Point the Kit target at `pyxis_hydra`'s delegate sources;
delete `HdPyxisOmni{RenderDelegate,Mesh,Camera,Light,RenderParam}`. Re-bless all
regression baselines under nv-usd and review the diffs once. **Done.**
`pyxis_hydra`'s delegate was promoted from the M4 injected-renderer stub to the
full **self-owning** model: it owns a `pyxis::hydra::PyxisEngine` (own Vulkan
device + GpuScene + PyxisRenderer + GpuInteropExporter), real mesh/camera/light
Sync + UsdPreviewSurface material resolve, the parallel-Sync race-mutex on
`HdPyxisRenderParam::SceneMutex`, and a render pass that drives `RenderFrame` +
composites into the host color AOV (camera taken from `renderPassState`, so the
viewport's free camera works). The app's `IngestUsd[hydra]` path is unaffected (it
shares StageWalker, never the delegate). `pyxis_hydra_omni` reduced to Kit glue
(`PyxisExtModule`, `PyxisHydraHost`, `PyxisViewportBridge/Panel`, `kit_stubs`) that
COMPILES `pyxis_hydra`'s delegate/engine/plugin sources directly; the 6 forked
`HdPyxisOmni*` / `PyxisEngine` files were deleted. Verified: standalone
`pyxis_hydra` builds + 266 unit tests pass; the Kit module builds against the Kit
SDK and all three headless Kit tests pass through the unified delegate
(`pyxis_plugin_load_test` resolves `HdPyxisRendererPlugin`; `HdEngineSmoke` drives a
stage → GpuScene → render → AOV composite; `MultiCycleVramTest` runs 3× World Lobby
(942 instances) create/render-250-frames/destroy with clean teardown). Remaining:
the in-Kit viewport GUI confirmation (a manual eyeball, not a headless gate).

## Verification gates

Each phase ends green before the next: Phase 1 — `pyxis_hydra` builds + its unit
tests; Phase 3 — standalone headless World Lobby renders (image review); Phase 4
— Kit viewport renders via the shared delegate + the standalone regression suite
passes against re-blessed baselines.

## Rollback

The migration is staged on a branch; vcpkg USD 26.3 wiring is removed only in
Phase 3. Phases 1–2 keep the standalone buildable on vcpkg USD until the cutover,
so the work can be abandoned cheaply before Phase 3.

## Persistent engine across pxr renderer switches

The closed `omni.hydra.pxr` engine DESTROYS the `HdPyxisRenderDelegate` when the
user switches the viewport renderer away from Pyxis and CONSTRUCTS A FRESH ONE on
switch-back. The delegate originally created its own `PyxisEngine` (own Vulkan
device + `GpuScene` + textures) in `Init()` and tore it down in the destructor, so
every switch-back rebuilt everything — the user saw `PyxisEngine: initialised` +
`UploadPendingTextures: 149 textures` each time.

**Fix.** The `PyxisEngine` now lives in a process-static holder
(`PersistentPyxisState`, file-local in `HdPyxisRenderDelegate.cpp`) that outlives
any single delegate instance. This is a deliberate, RFC-sanctioned exception to the
§30 "no singletons except `Logging::Get()` / Tracy" rule: it is scoped to the
Kit-hosted delegate (the standalone/headless paths never use it), gated by
`pyxis:persistEngine` / `PYXIS_OMNI_NO_PERSIST`, and exists only because the closed
pxr engine's destroy-on-switch behaviour leaves no other way to avoid a full
~10 GB device + texture rebuild per renderer switch. On switch-back the delegate
BORROWS the resident
engine instead of building a new one; the destructor leaves a borrowed engine
resident (no `waitForIdle`/teardown). Hydra builds a fresh render index on
switch-back and re-syncs ALL prims as `AllDirty`, so dirty-bits can't be used to
skip work — instead `HdPyxisMesh::EmitToScene` content-hashes each prim's
geometry + UVs + world transform + resolved `MaterialHandle` and:
- unseen prim -> `CreateMesh` + `AppendInstance`, record the handles + hash;
- hash unchanged -> REUSE (skip both calls entirely — the fast switch-back path);
- hash changed -> `UpdateMesh` + `UpdateInstanceTransform`/`UpdateInstanceMaterial`
  in place (no duplicate geometry; falls back to `DestroyMesh`+`CreateMesh` if the
  update fails).
Materials/textures re-acquire as cache hits for free because `AcquireMaterial`
(by hash) and `AcquireTexture` (by key) already dedup against the surviving
`GpuScene`. The dedup map is keyed by `SdfPath` and shared across delegate
instances via the static holder — that sharing is what makes switch-back reuse
work. Map access is serialized on `HdPyxisRenderParam::SceneMutex` like every
other `GpuScene` mutation (Hydra syncs prims in parallel).

**VRAM tradeoff.** A persisted engine PINS the full scene (textures + geometry +
BLAS) even while the viewport shows another renderer. Pyxis has its OWN Vulkan
device per §32, so this residency is ADDITIVE to RTX's own scene residency and can
OOM on very large scenes. The toggle below disables persistence for that case.

**Toggle.** Render setting `pyxis:persistEngine` (bool, default true), read by the
delegate via `HdRenderDelegate::GetRenderSetting` (USD-native — no carb dependency,
so the standalone/usdview build still compiles). Env override
`PYXIS_OMNI_NO_PERSIST` (if set) forces persistence OFF without UI — for the
VRAM-constrained / large-scene case and for headless. When persistence is OFF the
delegate owns the engine in a `unique_ptr` and tears it down on destruction
(pre-RFC behaviour), and `EmitToScene` bypasses the dedup map.

**Stage-change reset.** Opening a DIFFERENT scene must not leave the previous
stage's ghost prims in the persisted engine. The Kit extension
(`omni/hydra/pyxis/__init__.py`) stamps the stage identity into render setting
`pyxis:stageToken` on `OPENED`/`ASSETS_LOADED`; on the first prim of a sync pass
the delegate compares it to the resident token and, on a change, calls
`engine->Scene()->Clear()` + clears the prim map before the new stage populates.
If the token never arrives (stays empty) the reset degrades gracefully to "no
reset" — same-stage reuse still works; switching scenes while persisting would
then need a manual toggle. The exact carb root that maps to the delegate's
`GetRenderSetting` is the AVxcelerate convention
`/persistent/app/hydra/delegates/HdPyxisRendererPlugin/<token>`; the extension
ALSO stamps `/pxr/HdPyxisRendererPlugin/<token>` as a fallback, and the live test
will confirm which one propagates.

**Verification.** Headless cannot verify this (Storm's GL path is dead under
`--no-window`, so the pxr-hosting switch flow can't run); live in-Kit verification
is required — switch the viewport renderer Pyxis -> RTX -> Pyxis and confirm the
second activation does NOT re-print `PyxisEngine: initialised` /
`UploadPendingTextures` (run with `PYXIS_OMNI_DBG=1` to see the borrow/owned +
stage-token decisions).
