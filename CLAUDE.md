# Pyxis — Project Context for Claude

**Source of truth**: [plan_final.md](plan_final.md) (5664 lines, ~298 KB). Section numbers below (§N) refer to that file. **Read the relevant section in full before answering substantive questions** — never reason from this summary alone when the plan has the answer.

---

## What Pyxis is

C++23 real-time path tracer (NVRHI/Vulkan, Slang shaders, Windows-only v1) inspired by Autodesk Aurora. Primary target: render Disney Moana Island USD scene end-to-end on a single Windows workstation.

**Not**: production renderer, network/farm renderer, animation playback, full color management, volumetrics. Out-of-scope additions go through the §44 RFC process.

---

## Architectural rule #1 — Final-state design ships day 0

There is no "v1 shim" phase. The Flecs ECS, four-layer stack, public API surface, system pipeline, folder layout, **and both ingest adapters** ship from M0. v1 reductions are *features* on the §42 deferred list (subdivision, volumes, curves, animation, texture compression, …), not architecture.

## Four-layer stack (§1)

```
pyxis_app  (executable: viewer + headless)
   ├─ pyxis_hydra        ──┐ ingest adapters; only one active per run
   └─ pyxis_usd_ingest   ──┤ via app.ingest = "hydra" | "usd_direct"
                           ▼
                    pyxis_renderer   (SceneWorld = flecs::world; GpuScene; RenderGraph)
                           ▼
                    pyxis_platform   (NVRHI device, Vulkan, GLFW, spdlog, Tracy)
```

**Ingestion-agnostic**: renderer's public API (§18) is the only contract. Both adapters produce byte-identical EXRs against the same scene (§25.O.3) — this is a P0 invariant.

---

## CMake targets (§3)

| Target | Type | Notes |
|---|---|---|
| `pyxis_platform` | SHARED | spdlog/Tracy single-instance |
| `pyxis_renderer` | SHARED | Flecs linked PRIVATE; no Flecs in `Public/` |
| `pyxis_renderer_shaders` | custom | Slang→SPIR-V via ShaderMake |
| `pyxis_material_translation` | STATIC | UsdPreviewSurface/MaterialX/RenderMan → OpenPBR; shared by both ingest adapters |
| `pyxis_hydra` | SHARED | Hydra 2.0 render delegate (Scene Indices, **not** legacy `UsdImagingDelegate`) |
| `pyxis_usd_ingest` | SHARED | Direct `UsdStage` walker, **no Hydra dep**; one-shot importer (§O.2) |
| `pyxis` | EXE | viewer + headless; ingest selected at startup |
| `pyxis_unit_tests` | EXE | gtest |
| `pyxis_regression` | python/ctest | spawns `pyxis.exe --headless`, image-diffs |

---

## Folder layout (§2) — quick map

```
sources/pyxis_<module>/Public/Pyxis/<Module>/...   # narrow public surface
sources/pyxis_<module>/Private/...                  # everything else
```

Renderer's public surface is exhaustive (§18.1) — `Forward.h`, `RendererApi.h`, `Error.h`, `GpuScene.h`, `PyxisRenderer.h`, `Profiler.h`, `Descs/*.h`. **Anything else is `Private/` and inaccessible to ingest adapters or app.** Reviewers reject PRs that widen this.

`SceneWorld` lives in `pyxis_renderer/Private/Scene/` with subfolders `Components/` (one POD per header), `Systems/` (free functions), `Queries/` (cached `flecs::query_t*`), `Observers/`, plus `World.h`, `Phases.h`, `HandleBimap.h`, `Pipeline.cpp`.

---

## Public API contract (§18) — non-negotiables

- Public POD descriptors are byte-frozen: `sizeof`/`alignof`/offsets/padding are part of the contract. Adding a field consumes a trailing `_reserved*` slot (§22.3) or it's a MAJOR break.
- **No STL containers cross the DLL boundary.** Inputs are `std::span<const T>` / `std::string_view` (borrowed). Outputs that own a string use the inline `ErrorMessage` / `ScopeName` PODs.
- `std::span` / `std::string_view` are **input-only**; never persisted, never returned. The renderer copies what it needs internally.
- Strong-handle enums (`MeshHandle`, `MaterialHandle`, `TextureHandle`, `InstanceHandle`, `LightHandle`) — `enum class : uint32_t`, `Invalid = 0`, layout: 24-bit slot + 8-bit generation (§19.7).
- Methods returning `Expected<T>` are `[[nodiscard]]`. `void`-returning verbs silently ignore stale handles, counted in `FrameStats::staleHandleDrops` (§18.5).

---

## Coding rules (§30) — normative; reviewers reject violations

- **C++23**, clang-cl 17+, `/std:c++latest /W4 /WX /permissive-`, `/EHs-c-` in renderer/platform (no exceptions), `/EHsc` in Hydra (USD needs it).
- **Forbidden**: RTTI in renderer/platform (`/GR-`), exceptions across DLL boundaries, STL streams, raw `new`/`delete` outside thin RAII.
- **Naming** (§30.2): `PascalCase` types/free funcs/member funcs, `_camelCase` private fields, `camelCase` POD public fields/locals, `UPPER_SNAKE_CASE` compile-time constants (no prefix; `MAX_FRAMES_IN_FLIGHT`, `HANDLE_SLOT_BITS`), `PascalCase` enum-class constants (no prefix; `MeshHandle::Invalid`), `PYXIS_SCREAM` macros, single flat `pyxis::` namespace. Acronyms count as words (`BlasCache`, not `BLASCache`).
- **No singletons** except `pyxis::Logging::Get()` (§33.10) and Tracy's client. `Profiler` is constructor-injected.
- **No allocations in pass `Execute()`** — preallocate in `Declare()` / on-resize.
- **Public headers must not** include `<windows.h>`, transitively pull `pxr/...` (renderer is USD-free), or expose NVRHI types beyond opaque `IDevice*` / `ICommandList*`.
- **`PYXIS_ERROR(kind, fmt, ...)`** is the canonical `Error` constructor. `PYXIS_TRY` propagates `Expected<T>` failures up.
- **Three error tiers**: `PYXIS_ASSERT` / `PYXIS_VERIFY` (programmer error) → `Expected<T, Error>` (recoverable) → `PYXIS_FATAL` (fatal, after flushing logs).

### Flecs conventions (§30.11) — `Private/Scene/` only

- Components are POD structs (no `std::vector`, no `std::string`); variable-length data lives in `Private/GpuScene/` tables, referenced by handle.
- `Dirty<T>` is a zero-size tag component. Cleared in `System_ClearDirtyFlags` after each phase.
- Systems are free functions named `System_VerbObject` in `Private/Scene/Systems/`.
- **Queries are cached at registration time** — building a query inside a per-frame system body is a PR-blocking violation.
- Prefer pair relationships `(Instance, MaterialOf, mat)` over entity-field components.
- Custom phase pipeline (`PYXIS_PHASE_*`); built-in `flecs::OnUpdate` is **not** used.
- **Single-writer mutation**: only the render thread calls `world.entity()`/`set()`/`destruct()`. Ingest threads push `MutationCommand` records onto a `moodycamel::ConcurrentQueue` drained at the start of `CommitResources`.

---

## Threading (§31) — three logical threads

| Thread | Owns | Talks via |
|---|---|---|
| Render thread | NVRHI device, swapchain, ImGui, `RenderFrame`, `CommitResources` | drains upload-completion queue |
| Ingest thread | active adapter's stage state | writes to `GpuScene` via public API |
| Asset I/O pool (`N = clamp(hw_concurrency/2, 2, 8)`) | texture/mesh decode, MikkTSpace, EXR/PNG | produces `PendingUpload` records |

Tracy zones use dotted prefixes by component: `ingest.hydra.*`, `ingest.usd.*`, `ingest.shared.*`, `assets.*`, `render.*`, `app.*`.

---

## Frames-in-flight (§33.1)

- `MAX_FRAMES_IN_FLIGHT = 3` is the compile-time cap (sizes every per-frame ring).
- `GpuSceneCreateDesc::framesInFlight` is the *active* runtime count: 2 default, 3 in headless for byte-identical EXR (§33.7).

---

## Determinism (§33.7, §35, §36.5)

- Headless mode pins frame index, RNG seed (rejected if `seed == 0`), instance ordering (`SdfPath`-sorted), texture upload order, jitter sequence.
- Byte-identical EXR is scoped to: RTX 4080, pinned NVIDIA Game Ready Driver range, pinned Vulkan SDK, Win 11 23H2/24H2. Outside that matrix → per-test RMSE/MAE tolerance.

---

## Render graph (§9) — linear, no DAG culling, no aliasing

GPU-execution-only. CPU staging / upload draining / AS builds happen earlier, inside `GpuScene::CommitResources(commandList)` — these are *not* render-graph passes. v1 graph: `PathTrace → Accumulation → ToneMap → AovResolve → DebugView → CopyToHydraBuffer → Present`. See §9.1 (`ToneMapPass` worked example) for the canonical pass shape: ctor takes `IDevice* + ShaderLibrary&`, `Declare` registers refs, `Execute` is allocation-free, profiler scopes mandatory.

## Bindless layout (§5)

Single `BindlessLayout`: `RawBuffer_SRV(space=1)` (256 slots — vertex/index/material pages) + `Texture_SRV(space=2)` (~80 000 slots — Moana hero textures + UDIM tiles).

## OpenPBR (§11) — canonical material

All inputs (UsdPreviewSurface, MaterialX `open_pbr_surface` / `standard_surface` shim, RenderMan fallback) convert on CPU to `OpenPBRMaterialDesc`, hashed via `XXH3_64bits`, deduplicated, packed into 16-byte-aligned `OpenPBRMaterialGPU`. **One** generic OpenPBR closesthit shader in v1, branchless on `MaterialFlag` bits. Per-material specialization (multi-hitgroup) is post-v1.

## Slang interop (§10, §23)

- `ShaderInterop.slang` is the only file shared between C++ and shaders. `PYXIS_INTEROP_STRUCT` macro; `#ifdef __cplusplus` swaps `hlslpp::float4`/`float4x4`/etc. aliases.
- **Row-major matrices everywhere**. Multiplication is row-vector: `pos_clip = mul(pos_world, viewProj)`.
- Default cbuffer layout (16-byte vector alignment, `std140`-equivalent). `static_assert(sizeof(...) % 16 == 0)` mandatory.
- ShaderMake driven; permutations via `-D NAME={0,1}`; `-matrix-layout-row-major -O3 -profile sm_6_6 -target spirv -emit-spirv-directly`.

---

## BLAS / TLAS (§16) — split by mesh size

- `triCount < 64k`: `PREFER_FAST_TRACE` only (no `ALLOW_UPDATE`, no compaction — animation post-v1).
- `triCount >= 64k`: `PREFER_FAST_TRACE | ALLOW_COMPACTION`, no `ALLOW_UPDATE`.
- TLAS rebuilt every frame if dirty; refit otherwise. Two-tier (static + dynamic), sharded by `(SdfPath hash mod K)` for >16M instances (§16.5). 24-bit `instanceCustomIndex` cap (16 777 215).

---

## Versioning & ABI (§22)

- SemVer `MAJOR.MINOR.PATCH`. v1 ships as `1.0.0`. Pre-1.0 (M0–M9) has no stability guarantees.
- MAJOR = source-compat break. MINOR = additive only (new methods, new fields in trailing `_reserved` slots, enum values appended). PATCH = bugfix-only.
- Renaming/removing a public symbol = two-minor deprecation window + MAJOR removal.
- CI runs `_tools/check_exports.py` (dumpbin + golden diff) and a symbol-version map check.

---

## Profiling (§34) — two regimes

- **Load-time** (one-shot per scene load, coarse) → spdlog summary, Chrome-tracing JSON, viewer Load Timeline panel.
- **Per-frame** (every frame, 240-frame ring, fine) → Performance panel, CSV, Tracy (optional), in-process Profiler panel.

KPIs (1080p hero camera, RTX 4080, post-warm): `pass.PathTrace < 12ms`, `frame.cpu.commitResources < 2ms`, p99/p50 < 1.4. Time-to-first-image on Moana subset (M8a) < 30s on lab machine.

**Measure before you optimise.** PRs without profile evidence are rejected.

---

## Milestones (§38, §41)

| | Milestone | Done when |
|---|---|---|
| M0 | Skeleton | `pyxis.exe` opens NVRHI/Vulkan device; Flecs world + phase pipeline registered; `SceneWorldInit` test green |
| M1 | Viewer triangle | swapchain, RenderGraph, ImGui, profiler scopes |
| M2 | Headless triangle | `--headless --config` writes deterministic EXR |
| M3 | Slang path-trace box | one cube, BLAS+TLAS, raygen/closesthit/miss, accum + tonemap |
| M3.5 | Default startup scene | `Resources/scenes/default.usda` resolves through §29.4.a chain |
| M4 | Hydra delegate stub + USD-direct stub | usdview picks the delegate; both adapters render the same tiny `.usda` byte-identically |
| M5 | UsdPreviewSurface→OpenPBR | textured cube, OpenPBR shader |
| M6 | Native instancing | 10k-instance scene, BLAS sharing, instance/material AOVs |
| M7 | Lighting | dome + distant + rect; NEE + MIS |
| M8a | Moana subset render | one island region (≤1M tris, ≤~50 mats), nightly regression seed |
| M8b | Full Moana load | full USD opens to first commit without OOM on 24 GB GPU |
| M9 | Moana visually correct | dome alignment, UDIM, normals/tangents fallbacks, emissive |
| M10 | Moana headless + regression | nightly subset-Moana regression green |
| M11 | Profiling polish | full reports + ImGui panels; profiling overhead < 1% Release |

---

## Section index (where to look in plan_final.md)

| Topic | § |
|---|---|
| Architecture overview | 1 |
| Folder layout | 2 |
| CMake targets | 3 |
| Third-party deps | 4 |
| Vulkan setup, required extensions | 5, 5.b |
| Device managers (`VkDeviceManager` + `VkDeviceManagerHeadless`) | 5.c |
| OpenUSD / Hydra setup | 6 |
| Hydra render delegate class structure | 7 |
| `SceneWorld` (Flecs ECS) | 8 |
| RenderGraph + passes (incl. `ToneMapPass` example, `RenderGraph::Compile`) | 9 |
| Slang shader organisation | 10 |
| OpenPBR material architecture, `MaterialFlag` | 11 |
| RNG / sampler strategy (PCG32, Sobol+Owen, jitter, RR + clamp order) | 12 |
| Texture cache, UDIM | 13 |
| Geometry: vertex layout, buffer-pool segmentation | 14, 14.5 |
| Instancing | 15 |
| BLAS/TLAS (incl. sharding) | 16, 16.5 |
| Memory budgets | 17 |
| **Public API canonical reference** | 18 |
| Public API additions (capabilities, cancellation, progress, Clear, EXR save, picking, handle bits, AOV nullability) | 19 |
| Error catalogue | 20 |
| RenderSettings, quality knobs | 21 |
| Versioning / ABI / deprecation | 22 |
| Slang↔C++ interop rules | 23 |
| Hydra DirtyBits → GpuScene mapping | 24 |
| Hydra adapter (A–N) + USD-direct adapter (O) | 25 |
| Configuration system, JSON schema | 26, 27 |
| Frame pacing knobs | 28 |
| First-run UX, accessibility, ImGui panels, default scene, feature toggles, AOV inspector, Save Scene As USD | 29 |
| **C++ coding rules (incl. Flecs conventions §30.11)** | 30 |
| Threading model | 31 |
| NVRHI device sharing, destruction rules | 32 |
| GPU sync, frames-in-flight, pipeline cache, Aftermath, cross-DLL logging | 33 |
| Profiling regimes & KPIs | 34 |
| Testing / image regression / headless | 35 |
| Test infra (`.clang-format`, `.clang-tidy`, sanitizers, fuzzing, GPU determinism, perf regression, MaterialX coverage, dirty USD) | 36 |
| CI quality gates | 37 |
| Milestones | 38 |
| Step-by-step implementation order | 39 |
| Phased delivery (final-arch reasoning) | 40 |
| **Post-v1 scene-loading roadmap (long)** | 40.5 |
| Architectural reference patterns | (between §40 and §41) |
| Per-phase detailed plan (M0..M11) | 41 |
| **Strict "do not build yet" list** | 42 |
| Reserved hooks (light linking, motion blur, DoF, LUT tone-map, geometry kinds) | 43 |
| RFC process | 44 |
| Repo governance (CONTRIBUTING, CODEOWNERS, triage) | 45 |
| Build supply chain (vcpkg, repro builds, signing) | 46 |
| Crash reporting, logs, telemetry stance (zero telemetry v1) | 47 |
| Compliance / privacy / licensing | 48 |
| CMake architecture detail | 49 |
| Documentation deliverables | 50 |

---

## Decisions baked into the plan (do not relitigate without an RFC §44)

- Two ingest adapters day 0; both produce byte-identical EXRs.
- OpenPBR is canonical; one generic closesthit in v1.
- Hydra 2.0 / Scene Indices only; legacy `UsdImagingDelegate` not used.
- `pyxis_usd_ingest` is one-shot; no `UsdNotice` listener (live USD updates outside Hydra are post-v1).
- One executable, two modes; headless built on `VkDeviceManagerHeadless`.
- Linear RenderGraph in v1.
- BLAS by `MeshHandle` (strict prototype sharing); compaction default-on for ≥64k tris.
- Image is the only regression artefact v1.
- Subdivision, volumes, curves, displacement, texture compression deferred (§42).
- License: Apache 2.0.

---

## When asked to write code or design something

1. **Find the relevant section(s)** in plan_final.md first; cite them.
2. **Respect the public surface (§18)** — never introduce a new public type without an RFC.
3. **Respect coding rules (§30)** — naming, error handling, Flecs conventions.
4. **Respect threading (§31)** — single-writer Flecs, no GPU calls outside render thread.
5. **Match the architectural reference patterns** at the bottom of the plan (before §41).
6. **If a request would violate a normative rule**, say so and point to the section, rather than silently going along.

The plan is a written-down design; deviating from it without justification creates churn. When in doubt, ask which section to follow.
