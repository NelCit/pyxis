---
name: milestone-exit-criteria
description: Verify a Pyxis PR or branch satisfies the §41 exit criteria for a target milestone (M0..M11) — required files exist, deliverables wired, fixtures added/updated where §35 requires it. Invoke when closing out a milestone, when reviewing a PR labeled for a milestone, or when the user asks "are we done with M_N?". Reports a punch list: done vs missing.
---

# milestone-exit-criteria

§41 lays out per-milestone deliverables and exit criteria. This skill walks the criteria for a target milestone and reports which are done (file exists + plausible content), which are partial, and which are missing.

The user must specify which milestone to check (M0, M1, M2, …, M11). If unspecified, default to whichever milestone the current branch name suggests (e.g. `milestone/m1-viewer-triangle` → M1).

## Master checklist (per §41)

### M0 — Skeleton

- [ ] `CMakeLists.txt`, `_cmake/Compiler.cmake`, `_cmake/Vulkan.cmake`
- [ ] `sources/pyxis_platform/{Public,Private}/...` (Device + Logging + FileSystem only)
- [ ] `sources/pyxis_app/Main.cpp`
- [ ] clang-cl toolchain wired, `/W4 /WX`, vcpkg or fetch-content
- [ ] `VkDeviceManager::create()` returns `Expected<...>`; logs adapter, driver, VRAM
- [ ] Exit codes: `0` ok, `2` device init fail, `3` config fail
- [ ] Flecs vendored via vcpkg pinned baseline
- [ ] `SceneWorld::Init` constructs `flecs::world`, registers `Components/`, registers `PYXIS_PHASE_*` no-op pipeline, tears down cleanly
- [ ] Flecs Explorer launches in Debug (`http://localhost:27750`)
- [ ] Unit test: `SceneWorldInit`

### M1 — Viewer triangle

- [ ] GLFW window, NVRHI swapchain
- [ ] `RenderGraph` + `IRenderPass`
- [ ] Hard-coded `TrianglePass`
- [ ] ImGui (gated on `PYXIS_DEBUG_TOOLS`)
- [ ] `Profiler` skeleton, `CommandListMarker`
- [ ] Window draws a triangle at 60+ FPS
- [ ] ImGui dockable panel with FPS

### M2 — Headless triangle

- [ ] `VkDeviceManagerHeadless`
- [ ] `HeadlessMode`
- [ ] `parameters.json` loader
- [ ] EXR writer (tinyexr)
- [ ] CLI parser
- [ ] `pyxis --headless --config tests/fixtures/headless_triangle.json` writes deterministic EXR
- [ ] Rerun gives byte-identical output (§33.7 preconditions met)

### M3 — Slang path-trace box

- [ ] `Slang.cmake` + ShaderMake driver
- [ ] `ShaderInterop.slang` with `PYXIS_INTEROP_STRUCT`
- [ ] raygen / closesthit / miss for one cube
- [ ] `BlasCache`, `TlasBuilder`
- [ ] Accumulation + tonemap passes
- [ ] First non-trivial Flecs systems wired: `System_UploadDirtyMaterials`, `System_BuildDirtyBlas`, `System_RebuildTlas` running in their phases inside `CommitResources`
- [ ] Cube created via `GpuScene::CreateMesh` / `AppendInstance` (real ECS pipeline)
- [ ] Hardcoded scene: cube + camera + distant light, no Hydra
- [ ] Viewer + headless render the cube; accumulation converges
- [ ] Row-major matrix unit test green

### M3.5 — Default startup scene

- [ ] `Resources/scenes/default.usd` exists
- [ ] `Resources/scenes/default_sky.exr` exists
- [ ] §29.4.a chain implemented (CLI > config > recent > bundled; recent deferred to M4+)
- [ ] `--print-default-scene-path` CLI exit-mode wired
- [ ] `scene.resolved.source = ...` spdlog line emitted at startup
- [ ] Resources copied next to pyxis.exe at build time

### M4 — Hydra delegate stub + USD-direct stub

- [ ] `pyxis_hydra` shared lib, `plugInfo.json`, `HdPyxisRendererPlugin`
- [ ] `HdPyxisRenderDelegate` stub (mesh + camera + renderBuffer types only)
- [ ] `HdPyxisRenderPass` + `HdPyxisRenderTask` calling `PyxisRenderer::RenderFrame`
- [ ] `pyxis_usd_ingest` shared lib, `StageWalker` + `Geom/Mesh` + `Camera`
- [ ] `pyxis_material_translation` static lib, used by both adapters
- [ ] `HydraEngine` and `UsdDirectEngine` selectable via `app.ingest`
- [ ] `usdview` can pick the delegate
- [ ] Tiny USD renders identically across standalone Pyxis (both adapters) and `usdview`
- [ ] Regression image diff Hydra-vs-USD-direct = 0 (run `/ingest-parity-check`)

### M5 — UsdPreviewSurface → OpenPBR

- [ ] `UsdPreviewSurfaceToOpenPBR` adapter
- [ ] `MaterialTable` deduplication (XXH3_64 hash)
- [ ] `TextureCache` with stb/tinyexr decode
- [ ] GPU mip-generation pass
- [ ] OpenPBR shader (single hit group, branchless on `MaterialFlag` bits)
- [ ] Textured cube via UsdPreviewSurface renders correctly headless and viewer
- [ ] Unit test for OpenPBR conversion field-by-field

### M6 — Native instancing

- [ ] `HdPyxisInstancer`
- [ ] Instance flattening (nested instancers)
- [ ] BLAS sharing rule (one BLAS per `MeshHandle`)
- [ ] `instanceId` / `materialId` AOVs
- [ ] 10k-instance scene at interactive rates on lab GPU
- [ ] AOVs match expected mapping

### M7 — Lighting

- [ ] Distant + dome (lat-long EXR + alias table) + rect lights
- [ ] Importance sampling in raygen
- [ ] NEE
- [ ] MIS with BRDF
- [ ] Cornell-box-equivalent scene with all three light types converges within RMSE tolerance

### M8a — Moana subset render

- [ ] `tests/fixtures/moana_subset/` shipped (one hero asset, < 1M tris, ≤ ~50 unique materials)
- [ ] `UsdImagingStageSceneIndex` ingestion against the subset
- [ ] Subset renders headless and viewer
- [ ] Profile written
- [ ] Used as nightly regression seed

### M8b — Full Moana load (no OOM)

- [ ] Scene-index filters: flatten + prototype-propagating + material-binding
- [ ] Large-mesh chunked staging
- [ ] Lazy texture decode at scale
- [ ] Progress logging
- [ ] `unsupported_features.json` writer
- [ ] Budget tracker hard caps active
- [ ] Full Moana opens to first commit without OOM on 24 GB GPU
- [ ] Any single camera frame renders (visual correctness not yet required)

### M9 — Moana visually correct

- [ ] Dome-light alignment
- [ ] UDIM sampling
- [ ] Normals/tangents fallbacks
- [ ] Double-sided
- [ ] Emissive triangles
- [ ] MaterialX coverage gaps closed for hero assets
- [ ] Visually recognizable, color-correct hero camera frame
- [ ] AOV outputs valid; accumulation converges

### M10 — Moana headless + regression

- [ ] Python regression harness (`tests/regression/`, `_tools/run_regression.py`)
- [ ] Fixtures
- [ ] CI pipeline (build + unit + tiny regressions, ≤ 10 min wall-clock)
- [ ] Nightly subset-Moana job
- [ ] Per-test KPIs CSV emitted

### M11 — Profiling polish

- [ ] Full spdlog summary tables
- [ ] ImGui profiler panels
- [ ] JSON / CSV reports
- [ ] Tracy
- [ ] Profiling overhead < 1% in Release

## Cross-cutting checks (every milestone)

- §35 fixture rule: any new public-API verb or new MaterialX coverage path must add or update a fixture in `tests/fixtures/`.
- §22 version bump if `Public/` changed.
- `_tools/golden_exports.txt` updated if exports changed.

## Output

```
## Milestone M_N exit criteria

### Done
- [x] CMakeLists.txt, _cmake/Compiler.cmake exist
- [x] ...

### Missing
- [ ] tests/fixtures/headless_triangle.json — required for M2 exit
- [ ] _cmake/Vulkan.cmake — referenced by §41 M0

### Partial
- ~ VkDeviceManager::create() exists but does not return Expected<>; refactor required (§30.6)

## Verdict
M_N: NOT READY (3 missing, 1 partial)
```

Don't auto-fix — produce the punch list. The user decides what to address.
