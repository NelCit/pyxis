# Pyxis — Engineering Plan

A C++23 real-time path tracer inspired by Autodesk Aurora. NVRHI/Vulkan, Slang shaders, Windows-only v1.
Primary target: render the Amazon Lumberyard Bistro USD scene end-to-end on an 8 GB-class GPU.

**Design philosophy: the architecture below is the final-state design and ships day 0.**
There is no v1-shim phase: the Flecs ECS world, the four-layer stack, the public API
surface, the system pipeline schedule, the folder layout, **and both ingest adapters**
(`pyxis_hydra` and `pyxis_usd_ingest`) are all built as final from M0. v1 reductions are
limited to the deferred-feature list in §42 (subdivision, volumes, curves, animation,
texture compression, etc.). Phasing is a delivery decision about *which features are
wired into systems and passes*, not a decision about the architecture itself — see
§38/§39/§41.

**What Pyxis is *not*.** Pyxis is not a production renderer. It is not a
Arnold/RenderMan/Cycles competitor. It is a USD-native real-time path tracer for
*previewing* production-shaped USD assets at editorial quality on a single Windows workstation.
No network rendering, no AOVs-for-comp pipeline, no production color management,
no volumetrics, no animation playback v1. Anything outside that scope goes through the
RFC process (§45) before being added.

---

## TL;DR

One executable, two modes (viewer / headless), driven by `parameters.json` + CLI overrides.
Viewer mode is the product surface; **headless mode is test infrastructure** (image
regression, perf regression, bisect, profiling) — not a server / render-farm offering;
farm rendering is explicitly out of scope (§42).
**Final pipeline (day 0):** *ingest adapter* — chosen at startup via
`app.ingest = "hydra" | "usd_direct"` — → Pyxis renderer public API → **Flecs ECS
`SceneWorld`** → GpuScene tables (bindless) → NVRHI/Vulkan ray tracing → accumulation
→ tone-map → AOVs. Both adapters ship from M0 and share the same regression suite.
Profiling, reporting and config are first-class infra shared by both modes; ImGui and
spdlog are *output* backends, never owners.

---

# Part I — Architecture & Setup

## 1. Overall Architecture


Final layered architecture (all four layers exist from day 0, but some are stubbed in v1):

1. **Platform layer** — `pyxis_platform` (SHARED, so spdlog/Tracy registries are
   single-instance across the process)
   - `VkDeviceManager` (presenting) and `VkDeviceManagerHeadless` (no swapchain), modeled on the standard NVRHI / `donut`-style device-manager split.
   - Window/input (GLFW), filesystem (`std::filesystem`), logging (spdlog), Tracy.
2. **Renderer core** — `pyxis_renderer` (single public API; final shape, day 0)
   - Public API: `GpuScene`, `PyxisRenderer`, `Profiler`, POD `Descs/` (§18).
   - Internal: **`SceneWorld`** (Flecs ECS — `flecs::world` from M0) with components,
     systems, queries and a registered pipeline (§8). Plus `MaterialSystem`,
     `TextureCache`, `BlasCache`, `TlasBuilder`, `UploadQueue`, `DeletionQueue`,
     `RenderGraph`, all render passes, AOV management. **No Hydra, no ImGui, no USD.**
3. **Ingest adapters** — siblings, all clients of the renderer API. **Both ship day 0.**
   - `pyxis_hydra` — Hydra render delegate (`HdPyxisRenderDelegate`) so usdview,
     Houdini Solaris, etc. can drive Pyxis. OpenPBR conversion from
     UsdPreviewSurface / MaterialX / RenderMan.
   - `pyxis_usd_ingest` — opens `UsdStage` directly, **no Hydra dependency**, walks
     prims via `UsdPrimRange` + `UsdGeomImageable`/`UsdShade`/`UsdLux` and calls the
     same `GpuScene` verbs. Same OpenPBR conversion code (factored into a shared
     `pyxis_material_translation` static lib).
   - Selection is a startup config: `app.ingest = "hydra" | "usd_direct"`. The
     renderer cannot tell which one drove it.
   - Whichever adapter is selected at startup is the **only** code in that run that
     sees `pxr/...`.
4. **Application** — `pyxis_app` (executable)
   - Owns config loader, mode selection, USD stage opening, ingest-adapter selection,
     viewer UI (ImGui), headless driver, output writing, profiling reporters.

```
                ┌─────────────────── Application ───────────────────┐
                │  config • CLI • viewer/headless • UI • reporting  │
                └───────────────┬───────────────────────┬───────────┘
                                │                       │
                  day 0   ┌──────▼──────┐         ┌──────▼─────────┐  day 0
                         │ pyxis_hydra │         │ pyxis_usd_ingest│
                         └──────┬──────┘         └──────┬──────────┘
                                │  Pyxis renderer public API (§18) │
                                ▼                       ▼
                         ┌─────────────── pyxis_renderer ───────────────┐
                         │  SceneWorld (flecs::world — day 0)           │
                         │     ├─ entities: Mesh, Material, Texture,    │
                         │     │   Instance, Light, Camera              │
                         │     ├─ components: Transform, MaterialRef,   │
                         │     │   Visibility, BlasRef, Dirty<T>, …     │
                         │     └─ systems: BLAS build, TLAS build,      │
                         │         material upload, texture upload      │
                         │  GpuScene tables (bindless, NVRHI handles)   │
                         │  RenderGraph + passes + Profiler             │
                         └──────────────┬───────────────────────────────┘
                                        ▼
                                 nvrhi (Vulkan)
                                        ▲
                                        │
                                 pyxis_platform
```

**Ingestion-agnostic rule.** The renderer's public API (§18) is the only contract.
Every ingest adapter — `pyxis_hydra`, `pyxis_usd_ingest`, anything added later — is a
client of that API and produces the same `MeshHandle` / `MaterialHandle` /
`InstanceHandle` graph. The ECS world inside `pyxis_renderer` is an implementation
detail and must not leak through any public header.

**Final-state, day 0.** v1 ships the **complete** architecture above — the public API
(§18), the Flecs ECS `SceneWorld` (§8), the four-layer dependency direction, the system
pipeline order, the folder layout, **and both ingest adapters** (`pyxis_hydra` and
`pyxis_usd_ingest`) at parity. The only v1 *reductions* are deferred *features* listed
in §42 (subdivision, volumes, curves, animation, texture compression, etc.). These are
missing functionality, not missing architecture: when they land they slot in as new
components / systems / passes inside the existing skeleton.

Key rule: `pyxis_renderer` does not include `pxr/...` headers; all USD/Hydra concepts
are translated at the ingest-adapter boundary into renderer handles.

---

## 2. Repository / Folder Organization


```
Pyxis/
├─ CMakeLists.txt
├─ plan.md                        # this plan
├─ NuGet.config                   # optional, only if a NuGet package step is needed
├─ _cmake/                        # CMake helpers (Slang.cmake, Vulkan.cmake, Compiler.cmake, Install.cmake, Version.cmake)
│  ├─ All.cmake, Compiler.cmake, Slang.cmake, Utils.cmake, Version.cmake
├─ _tools/
│  ├─ run_bistro_headless.cmd
│  ├─ run_regression.py
├─ _documentation/
│  ├─ overview.md, openpbr.md, hydra_delegate.md, profiling.md, parameters.md
├─ thirdparty/                    # vendored or fetched
│  ├─ nvrhi/, shadermake/, slang/, openusd/,
│  ├─ imgui/, tracy/, spdlog/, hlslpp/, glfw/, nlohmann_json/, stb/, tinyexr/, mikktspace/
├─ resources/
│  ├─ shaders/ (Slang sources — single tree of `.slang` files compiled by ShaderMake)
│  ├─ baselines/                  # tiny baselines only; large ones live outside the repo
│  └─ test_scenes/                # tiny .usda fixtures
├─ sources/
│  ├─ pyxis_platform/
│  │  ├─ Public/
│  │  │  └─ Pyxis/Platform/
│  │  │     ├─ Forward.h, PlatformApi.h
│  │  │     ├─ Device/                # IDeviceManager, DeviceCreationParams, AdapterInfo, Resolution
│  │  │     ├─ Window/                # IWindow, WindowDesc, InputEvent
│  │  │     ├─ Logging/               # Log (spdlog facade), LogCategories, LogConfig
│  │  │     ├─ Profiling/             # TracyShim, CpuTimer
│  │  │     ├─ FileSystem/            # Path, FileIo, AssetLocator
│  │  │     ├─ Memory/                # ProcessMemory (RSS), Alignment
│  │  │     ├─ Threading/             # ThreadName, TaskGroup
│  │  │     ├─ Vulkan/                # VulkanRequirements, VulkanDebug
│  │  │     ├─ Diagnostics/           # Aftermath, NsightCapture (CMake-gated)
│  │  │     └─ Time/                  # Clock
│  │  └─ Private/
│  │     ├─ Device/                   # VkDeviceManager, VkDeviceManagerHeadless
│  │     ├─ Window/                   # GlfwWindow
│  │     ├─ Logging/                  # SpdlogBackend
│  │     ├─ FileSystem/               # AssetLocator impl
│  │     ├─ Vulkan/                   # VulkanInstance, VulkanPhysicalDevice, VulkanFeatures
│  │     ├─ Diagnostics/              # Aftermath, NsightCapture impls
│  │     └─ Memory/                   # ProcessMemoryWin32
│  ├─ pyxis_renderer/
│  │  ├─ Public/                              # narrow API surface only
│  │  │  └─ Pyxis/Renderer/
│  │  │     ├─ Forward.h                      # handles + fwd decls (MeshHandle, MaterialHandle, ...)
│  │  │     ├─ RendererApi.h                  # PYXIS_RENDERER_API export macro
│  │  │     ├─ Error.h                        # Error, ErrorKind, std::expected alias
│  │  │     ├─ GpuScene.h                     # Create/Destroy/Update verbs (the API)
│  │  │     ├─ PyxisRenderer.h                # RenderFrame/Resize/LastFrameProfile
│  │  │     ├─ Profiler.h                     # Profiler facade
│  │  │     └─ Descs/                         # POD descriptors only
│  │  │        ├─ MeshDesc.h, OpenPBRMaterialDesc.h, TextureKey.h
│  │  │        ├─ InstanceDesc.h, CameraDesc.h, LightDesc.h
│  │  │        ├─ RenderSettings.h, RenderTargets.h
│  │  │        └─ FrameStats.h, FrameProfile.h
│  │  └─ Private/
│  │     ├─ Scene/                # SceneWorld = flecs::world (day 0)
│  │     │   ├─ World.h/.cpp                  # SceneWorld facade; init/teardown; phase pipeline
│  │     │   ├─ Phases.h                      # PYXIS_PHASE_* user phases (Upload, Build, Tlas, Bindless, Clear)
│  │     │   ├─ HandleBimap.h/.cpp            # MeshHandle ↔ flecs::entity bidirectional map
│  │     │   ├─ Components/                   # POD components — one header per component
│  │     │   │   ├─ Geom.h, Transform.h, Visibility.h
│  │     │   │   ├─ MaterialRef.h, MeshRef.h, BlasRef.h
│  │     │   │   ├─ MaterialGpu.h, TextureGpu.h
│  │     │   │   ├─ LightParams.h
│  │     │   │   └─ Dirty.h                   # Dirty<T> tag components (zero-size)
│  │     │   ├─ Systems/                      # free-function systems, registered by SceneWorld
│  │     │   │   ├─ UploadDirtyTextures.cpp
│  │     │   │   ├─ UploadDirtyMaterials.cpp
│  │     │   │   ├─ ExtractDirtyMeshes.cpp
│  │     │   │   ├─ BuildDirtyBlas.cpp
│  │     │   │   ├─ RebuildTlas.cpp
│  │     │   │   ├─ UpdateBindlessTable.cpp
│  │     │   │   ├─ ClearDirtyFlags.cpp
│  │     │   │   └─ Pipeline.cpp              # registers all systems in deterministic order
│  │     │   ├─ Queries/                      # cached flecs::query_t* per archetype
│  │     │   │   └─ QueryCache.h/.cpp
│  │     │   └─ Observers/                    # OnRemoveMesh.cpp, refcount + DeletionQueue hookup
│  │     ├─ GpuScene/             # GpuScene impl, bindless tables, BlasCache, TlasBuilder
│  │     ├─ Materials/            # OpenPBRMaterialGPU (GPU layout), hashing, deduplication
│  │     ├─ Passes/               # PathTracePass, AccumulationPass, ToneMapPass, AovResolvePass, DebugViewPass, CopyToHydraBufferPass
│  │     ├─ RenderGraph/          # Graph, Pass, Resource, Barrier, Profiling integration
│  │     ├─ Shaders/              # Slang compilation, ShaderLibrary, hot-reload
│  │     ├─ Memory/               # StagingRing, UploadQueue, DeletionQueue, BudgetTracker
│  │     ├─ Profiler/             # ProfilerData, FrameProfile internals, GpuTimestampPool
│  │     ├─ Renderers/            # PyxisRenderer impl (orchestrates passes for a frame)
│  │     └─ Utils/                # SdfPathHandle, Hash, Span, small POD helpers
│  ├─ pyxis_material_translation/             # shared static lib used by both ingest adapters
│  │  ├─ Public/Pyxis/MaterialTranslation/    # UsdPreviewSurface→OpenPBR, MaterialX→OpenPBR, RenderMan→OpenPBR
│  │  └─ Private/
│  ├─ pyxis_hydra/                            # ingest adapter — Hydra render delegate (day 0)
│  │  └─ Private/
│  │     ├─ Delegate/             # HdPyxisRenderDelegate, HdPyxisRenderParam, plugInfo.json
│  │     ├─ Rprim/                # HdPyxisMesh
│  │     ├─ Sprim/                # HdPyxisCamera, HdPyxisDistantLight, HdPyxisDomeLight, HdPyxisRectLight, HdPyxisMaterial
│  │     ├─ Bprim/                # HdPyxisRenderBuffer
│  │     ├─ Instancer/            # HdPyxisInstancer
│  │     ├─ RenderPass/           # HdPyxisRenderPass + HdPyxisRenderTask
│  │     └─ Sync/                 # DirtyBits handling, CommitResources
│  ├─ pyxis_usd_ingest/                       # ingest adapter — direct USD walker, no Hydra (day 0)
│  │  └─ Private/
│  │     ├─ StageWalker/          # UsdPrimRange traversal, time-sample sampling (single-frame v1)
│  │     ├─ Geom/                 # UsdGeomMesh / UsdGeomXform / UsdGeomPointInstancer → MeshDesc/InstanceDesc
│  │     ├─ Light/                # UsdLuxDistantLight/DomeLight/RectLight → LightDesc
│  │     ├─ Material/             # binds pyxis_material_translation; UsdShade resolution
│  │     ├─ Camera/               # UsdGeomCamera → CameraDesc
│  │     └─ Snapshot/             # one-shot IngestSnapshot; live UsdNotice listener deferred to post-v1 (§40.4)
│  └─ pyxis_app/
│     ├─ Private/
│     │  ├─ Application.h/.cpp    # mode-agnostic core
│     │  ├─ ViewerMode.h/.cpp     # window/swapchain/input/ImGui
│     │  ├─ HeadlessMode.h/.cpp   # offline render loop, image writer
│     │  ├─ Config/               # parameters.json schema, loader, CLI override
│     │  ├─ HydraEngine/          # owns UsdStage, UsdImagingStageSceneIndex, HdEngine (used when app.ingest = "hydra")
│     │  ├─ UsdDirectEngine/      # owns UsdStage + drives pyxis_usd_ingest (used when app.ingest = "usd_direct")
│     │  ├─ UI/                   # ImGui panels (Settings, Features, Debug Tools, Performance, Scene Stats, Inspector, Profiler, Material Report, Texture Cache, GPU Stats, Console, Scene, Load Timeline) — see §29.3
│     │  └─ Reporting/            # JSON/CSV writers for profiling and stats
│     └─ Main.cpp                 # parses CLI, dispatches to ViewerMode or HeadlessMode
└─ tests/
   ├─ unit/                       # gtest, in-tree, no Hydra/USD required for most
   └─ regression/                 # external; subprocess-runs pyxis.exe --headless
```

Coding-style is consistent throughout: `Pascal/CamelCase` types, `_camelCase` private members, headers under
`Private/`, public API under `Public/`, `shaders/` with a `shaders.cfg` style file (here a Slang
manifest), clang-cl `/W4 /WX` (no MSVC `cl.exe`).

---

## 3. Main CMake Targets


| Target | Type | Purpose |
|---|---|---|
| `pyxis_platform` | SHARED | Vulkan/NVRHI device managers, OS, file I/O, logging (SHARED so spdlog/Tracy registries are single-instance across all DLLs) |
| `pyxis_renderer` | SHARED | Renderer core: public API, `SceneWorld` (`flecs::world`), GpuScene, RenderGraph, OpenPBR, profiler. Links `flecs` PRIVATE — no Flecs header escapes through `Public/`. |
| `pyxis_renderer_shaders` | custom target | Slang compilation, depends on `pyxis_renderer` |
| `pyxis_material_translation` | STATIC | UsdPreviewSurface / MaterialX / RenderMan → OpenPBR translation. Linked PRIVATE by both ingest adapters; never linked by `pyxis_renderer`. |
| `pyxis_hydra` | SHARED | Hydra render delegate ingest adapter (day 0). Loadable as a USD plugin. |
| `pyxis_usd_ingest` | SHARED | Direct USD ingest adapter (day 0). Opens `UsdStage`, no Hydra. |
| `pyxis` | EXECUTABLE | Single application binary; viewer + headless; selects ingest adapter at startup |
| `pyxis_unit_tests` | EXECUTABLE | gtest unit tests |
| `pyxis_regression` | python/ctest harness | Spawns `pyxis.exe --headless`, image-diff |

CMake helpers under `_cmake/`: `Slang.cmake` (compiles `.slang` into SPIR-V), `Vulkan.cmake`,
`Version.cmake`, `Compiler.cmake` (selects clang-cl, sets `/W4 /WX`, `/external:I` for thirdparty; equivalent `-Wall -Wextra -Werror` path kept available for clang frontend), `Install.cmake`.

---

## 4. Third-Party Dependencies


| Dep | Why |
|---|---|
| **OpenUSD (pxr)** | USD stage, `UsdImagingStageSceneIndex` + Hd 2.0 scene-index filters, SdfPath, UsdLux, UsdShade |
| **NVRHI** | RHI; Vulkan backend only. Built with `NVRHI_WITH_RTXMU=ON` so BLAS memory + compaction route through RTXMU (§16). |
| **RTXMU** | RTX Memory Utility — BLAS suballocation pool, scratch pool, async compaction. Vendored as a submodule inside NVRHI's source tree; no separate vcpkg / FetchContent declaration needed. |
| **Vulkan SDK / Vulkan-Headers** | Vulkan 1.3, VK_KHR_ray_tracing_pipeline, VK_KHR_acceleration_structure, VK_EXT_descriptor_indexing |
| **Slang** | Shader language + compiler; single source for raygen/closesthit/miss/compute |
| **ShaderMake** | NVIDIA's shader build tool; drives Slang/DXC compilation, permutation expansion (`-D NAME={0,1}`), SPIR-V output, and dependency tracking |
| **GLFW** | Viewer-mode window + input (Windows) |
| **ImGui (docking)** | Viewer mode UI; integrate via NVRHI's ImGui helper |
| **spdlog** | Logging across all modes; profiling summary backend |
| **Tracy** | CPU profiling; optional GPU integration. **Pin client and server to the same minor version** (e.g. `v0.11.x`); a mismatched server silently drops captures. The pinned version is recorded in `vcpkg.json` and verified at startup (`Profiler` logs `tracy::ClientVersion()` so capture sessions can be cross-checked). |
| **nlohmann/json** | `parameters.json` parsing, JSON profiling/report output |
| **hlslpp** | Math types (HLSL-shaped C++ vectors/matrices, SIMD-accelerated). HLSL/Slang-friendly semantics; mirrors the GPU types so the C++/shader interop stays 1:1. **No glm.** |
| **stb_image / tinyexr** | Texture decode (LDR + EXR/HDR) |
| **MikkTSpace** | Tangent-space generation for normal maps |
| **MaterialX** | Used by USD; needed for MaterialX → OpenPBR conversion |
| **gtest** | Unit tests |
| **DirectXShaderCompiler** | Optional, only if Slang uses it as backend; usually not needed |
| **Flecs** | ECS for `SceneWorld`. Permissively-licensed (MIT). Pinned via `vcpkg.json` baseline. Linked PRIVATE into `pyxis_renderer` only — no Flecs header is exposed through `Public/`. Flecs Explorer (REST/web UI on port 27750) is enabled in **Debug** builds only via the `flecs[rest]` vcpkg feature, gated behind `PYXIS_DEBUG_TOOLS`. |
| **moodycamel-concurrentqueue** | Lock-free MPMC queue used to ferry ingest-thread mutations to the render thread (§31). |

Excluded in v1: OpenImageIO (USD pulls a viable subset), OpenColorIO (basic color
mgmt only), OIDN/OptiX denoiser (deferred).

---

## 5. Windows / Vulkan Setup


- Windows 10/11, x64 only.
- Vulkan SDK 1.3.x; require: ray tracing pipeline, acceleration structure, deferred host ops,
  buffer device address, descriptor indexing (runtime + non-uniform), scalar block layout,
  **timeline semaphores** (NVRHI's frame submission keys on timeline values — the
  command-list ring + `DeletionQueue` flush both index by timeline value, not VkFence),
  host query reset, **synchronization2** (mandatory — NVRHI uses the new pipeline-stage /
  access-mask enums; Sync1 fallback is **not** supported), dynamic rendering (not strictly
  required since NVRHI handles this).
- One `VkDevice`, one graphics queue, one async-compute queue (optional v1), one transfer queue
  for staging uploads.
- Bindless: a single large `BindlessLayout` with `RawBuffer_SRV(space=1)` and
  `Texture_SRV(space=2)`. Capacities are asymmetric: `Texture_SRV(s=2)` reserves
  ~80 k slots (Bistro uses ~2–3 k slots in v1; the remaining capacity is headroom
  for post-v1 production-class scenes with UDIM tiles at scale, and for room for
  growth), while `RawBuffer_SRV(s=1)` reserves only **256** slots (vertex/index/
  material pages from §14.5 occupy the first ~7; the remainder is headroom for
  future scratch / cluster / animation tables). Both numbers are upper bounds, not
  steady-state usage. Materials index into both.
- Allocators: rely on NVRHI's internal allocator (it uses VMA). Track our own GPU memory
  budget independently for reporting.
- **Pipeline cache** (§33.8): `VkPipelineCache` persisted to
  `%LOCALAPPDATA%/Pyxis/PipelineCache/<pipelineCacheUUID>-<slangVersion>.bin`;
  loaded at startup, rebuilt on mismatch. The primary key is
  `VkPhysicalDeviceProperties::pipelineCacheUUID` — it already encodes device + driver
  + ABI atomically and is the only documented invalidation token. The Slang compiler
  version is a secondary safety key (we own the SPIR-V it produces). Without this,
  first-run path-tracing PSO creation costs 5–30 s on a fresh CI machine.
- **Swapchain & display** (viewer mode): swapchain format pinned to
  `B8G8R8A8_UNORM_SRGB` v1 (HDR `R10G10B10A2_UNORM` + ST.2084 explicitly out of scope).
  Per-monitor DPI awareness (`PerMonitorAwareV2` via app manifest); ImGui scales by
  monitor DPI; render resolution is *backbuffer pixels*, never DIPs. Multi-GPU: highest-VRAM
  ray-tracing-capable adapter is selected, the choice is logged, and `--adapter <index>`
  overrides at the CLI.
- **Headless mode (CI / regression / profiling, *not* a server product).** `VkDeviceManagerHeadless`
  does **not** load GLFW; no `glfwInit()` is reachable from `--headless`. The mode exists so
  the image-regression harness (§35), the perf-regression harness (§34/§37) and bisect tooling
  can drive `pyxis.exe` from a CI runner that has no display, no logged-in desktop session
  and no focused window — not because Pyxis ships as a render farm or server-side service.
  Network-distributed / farm rendering is explicitly out of scope (§42). CI smoke-tests run
  `pyxis --headless` inside a Windows Server 2022 container with no display driver.
- RenderDoc and Nsight Graphics supported. **Aftermath / Nsight Capture** via CMake flag
  (Debug-only, Windows-only, copying SDK headers into the build tree). Crash-diagnostics flow
  is documented in §33.9.

### 5.b Required Vulkan extensions / features (canonical list)

Device creation fails fast with `ErrorKind::FeatureMissing` if any of these are absent.

| Extension / feature | Reason |
|---|---|
| Vulkan 1.3 core (or 1.2 + `VK_KHR_synchronization2` + `VK_KHR_dynamic_rendering` + `VK_KHR_maintenance4`) | NVRHI Sync2 path; > 4 GiB single allocation |
| `VK_KHR_acceleration_structure` | BLAS / TLAS |
| `VK_KHR_ray_tracing_pipeline` | RT PSO + closesthit |
| `VK_KHR_deferred_host_operations` | required by `VK_KHR_acceleration_structure` |
| `VK_KHR_buffer_device_address` | required by AS + bindless raw-buffer indexing |
| `VK_EXT_descriptor_indexing` (or 1.2 core) + `runtimeDescriptorArray` + `descriptorBindingPartiallyBound` + `shaderSampledImageArrayNonUniformIndexing` + `shaderStorageBufferArrayNonUniformIndexing` | bindless 80 k-slot table, non-uniform indexing in path-trace closesthit |
| `VK_EXT_scalar_block_layout` (or 1.2 core) | optional; not required v1 (Pyxis uses default cbuffer layout matching `hlslpp` — §23) but kept enabled when available for forward compatibility |
| `VK_KHR_timeline_semaphore` (or 1.2 core) | NVRHI command-list ring + `DeletionQueue` |
| `VK_EXT_host_query_reset` (or 1.2 core) | profiler timestamp-pool reset off the GPU |
| `VK_KHR_maintenance4` (or 1.3 core) | allocations > 4 GiB (vertex/index pools §14.5) |
| `VK_EXT_memory_budget` | runtime VRAM budget feedback (§17) |
| `VK_EXT_pipeline_creation_cache_control` (optional) | reject blocking pipeline creation on cache miss |
| `VkPhysicalDeviceFeatures::shaderInt64` (Vulkan 1.0 core feature) | RNG seed-mix in path-trace closesthit (§12.1) |
| `VK_NV_aftermath` (optional, gated by `PYXIS_ENABLE_AFTERMATH`) | crash dumps |

Fallback policy: there is none in v1. The engine targets RTX 30/40 + recent NVIDIA / AMD
ray-tracing drivers; missing any required feature is a hard error with a clear log
entry naming the failing feature.

### 5.c Device managers — `VkDeviceManager` & `VkDeviceManagerHeadless`

Two concrete classes in `pyxis_platform/Device/`, sharing one interface. Modeled
on NVIDIA's `donut::app::DeviceManager` split (the canonical Vulkan-headless +
Vulkan-windowed pair shipped with the donut sample framework). They are the *only*
code in
Pyxis that touches raw Vulkan or GLFW; every other module sees `nvrhi::IDevice*`.

**Common base — `IDeviceManager`** owns:

- The `VkInstance` (validation layers gated by Debug + `--vk-validation`).
- Adapter selection (highest-VRAM RT-capable; `--adapter <i>` override).
- The `VkDevice` + queues (graphics / compute / transfer; one queue family per role where the driver supports it, otherwise the closest fit).
- The `nvrhi::IDevice*` wrapping that VkDevice — *owned* here, *borrowed* by every
  consumer (§32).
- Per-frame fence / timeline-semaphore plumbing (`MAX_FRAMES_IN_FLIGHT = 3`).
- Aftermath / Nsight hookup (Debug only, §33.9).
- Lifetime: ctor creates instance + device; dtor `vkDeviceWaitIdle` then tears down.

```cpp
class IDeviceManager {
public:
  virtual ~IDeviceManager();
  [[nodiscard]] nvrhi::IDevice*  GetDevice()           const noexcept;  // borrowed
  [[nodiscard]] uint32_t         GetFramesInFlight()   const noexcept;
  virtual void BeginFrame() = 0;     // viewer: vkAcquireNextImageKHR; headless: no-op
  virtual void EndFrame()   = 0;     // viewer: vkQueuePresentKHR;    headless: signal+optional readback
  // ... fences, timeline semaphores, validation toggle, adapter info
};
```

**`VkDeviceManager` — viewer mode (presenting).** Adds everything tied to a window /
display:

- Owns the GLFW window (`glfwCreateWindow`, monitor + DPI awareness, drag-drop
  callbacks).
- Owns the `VkSurfaceKHR` created from GLFW via `glfwCreateWindowSurface`.
- Owns the `VkSwapchainKHR`, with present-mode honoring §28's `viewer.presentMode`
  (`fifo` / `mailbox` / `immediate` / `fifoRelaxed`) and `viewer.targetFps`.
- Recreates the swapchain on resize / DPI change / present-mode change.
- Frames-in-flight default = 2.
- Enables `VK_KHR_surface` + `VK_KHR_swapchain` on top of §5.b's required list.
- Render passes (§9) and the renderer core (§18) never see swapchain images
  directly — they write to NVRHI textures and `EndFrame()` blits to the acquired
  swapchain image.

**`VkDeviceManagerHeadless` — CI / regression / profiling mode.** Strips everything
display-related:

- **No GLFW.** Not linked, not initialized; no `glfwInit()` call site exists in
  this class. A CI runner without a display driver cannot accidentally fault.
- **No swapchain.** `VK_KHR_swapchain` is *not* requested at instance/device
  creation, so headless can run on stripped-down driver stacks (e.g. Windows
  Server 2022 containers).
- **No present.** `EndFrame()` waits on the frame fence and, when the harness
  asks, issues a readback of `RenderTargets::color` to a host-mapped buffer for
  EXR/PNG writing.
- **Frames-in-flight pinned to 3** for determinism (§33.7) — the byte-identical
  EXR claim only holds in headless because viewer mode is allowed to vary
  frames-in-flight for latency.
- **Deterministic camera-jitter sequence** is enforced here (§12.4); viewer mode
  allows runtime jitter changes.
- Validation layers still allowed (and enabled in CI Debug runs).

**Why two classes instead of an `if (headless)` switch.** It makes the no-display
invariant *physically* true (no GLFW symbol is reachable from `--headless`, no
linker pull-in of display-stack DLLs); it makes the swapchain extension list
different at instance/device creation, not just at use-site; and it follows the
standard `donut::app::DeviceManager` split, which is widely recognised in NVRHI
codebases. The renderer holds an `IDeviceManager*` and never branches on mode —
branching lives in `pyxis_app::Main.cpp`:

```cpp
std::unique_ptr<IDeviceManager> dm = cli.headless
    ? std::make_unique<VkDeviceManagerHeadless>(cfg)
    : std::make_unique<VkDeviceManager>(cfg);
```

Tree (§2):

```
pyxis_platform/Device/
  IDeviceManager.h
  VkDeviceManager.h/.cpp           // viewer
  VkDeviceManagerHeadless.h/.cpp   // CI / regression / profiling
```

---

## 6. OpenUSD / Hydra Setup


- Pin OpenUSD to a specific commit (target: v25.x release branch tip; SHA recorded in
  `_cmake/Thirdparty.cmake` — §49). Build USD with: `--no-python` (we won't embed
  Python), MaterialX support enabled, Hd / UsdImaging enabled, Vulkan/Imaging not required.
- USD assets resolved via `ArResolver`; respect relative paths and USD asset resolver
  contexts; honor `USD_DEFAULT_RESOLVER_SEARCH_PATH`-style env vars.
- For larger production-class packages (post-v1): do NOT preload all USDs. Use lazy population (default UsdImaging). Bistro at v1 fits comfortably in memory.
- **Use Hydra 2.0 / Scene Indices** (`UsdImagingStageSceneIndex` + scene-index filters), not
  the legacy `UsdImagingDelegate`. Rationale: Scene Indices is the supported forward path in
  modern USD, gives a flat, queryable scene representation, removes the legacy adapter
  registry, plays better with MaterialX-as-canonical material networks, and matches what
  Aurora-class renderers target. We accept a slightly less mature production-mileage in
  exchange for a future-proof base.
- Compose: `UsdImagingStageSceneIndex` → flatten + prototype-propagating + material-binding
  filters → `HdsiSceneGlobalsSceneIndex` → our renderer's input. We feed it to a
  `HdRenderIndex` whose render delegate is `HdPyxisRenderDelegate`.
- Drive frame via `HdEngine::Execute(renderIndex, tasks)` with a single `HdRenderTask` we
  control. We do not call `UsdImagingGLEngine`.
- Config flag `hydra.enableSceneIndex` is removed; Scene Indices is the only path. A future
  legacy fallback is not planned.

---

## 7. Hydra Render Delegate Class Structure


```
HdPyxisRenderDelegate : HdRenderDelegate
  ├─ HdPyxisRenderParam : HdRenderParam   (holds GpuScene*, Profiler*, frame index, dirty flags)
  ├─ TfTokenVector GetSupportedRprimTypes()  → { mesh }
  ├─ TfTokenVector GetSupportedSprimTypes()  → { camera, distantLight, domeLight, rectLight,
  │                                             material, extComputation? (skip v1) }
  ├─ TfTokenVector GetSupportedBprimTypes()  → { renderBuffer }
  ├─ Create/Destroy{Rprim,Sprim,Bprim,Instancer,RenderPass}
  ├─ HdResourceRegistrySharedPtr GetResourceRegistry()  (a tiny one)
  ├─ HdAovDescriptor GetDefaultAovDescriptor(TfToken)   (color/depth/normal/albedo/id)
  └─ CommitResources(HdChangeTracker*)                  (drains uploads, builds BLAS/TLAS)

Rprims:
  HdPyxisMesh : HdMesh        (one per Hydra mesh prim; resolves to a GpuMeshHandle)
Sprims:
  HdPyxisCamera, HdPyxisDistantLight, HdPyxisDomeLight, HdPyxisRectLight, HdPyxisMaterial
Bprims:
  HdPyxisRenderBuffer : HdRenderBuffer (allocates/maps NVRHI textures)
Instancer:
  HdPyxisInstancer : HdInstancer
RenderPass:
  HdPyxisRenderPass : HdRenderPass  (delegates to PyxisRenderer)
```

Plugin descriptor: `plugInfo.json` registers `HdPyxisRendererPlugin : HdRendererPlugin` so
external Hydra hosts (usdview etc.) can discover the delegate; viewer/headless explicitly load
it via `HdRendererPluginRegistry`.

---

# Part II — Renderer Internals

## 8. Renderer / Internal Scene Structure


`GpuScene` is the public-facing renderer database (§18). Behind it lives `SceneWorld`,
the internal scene representation. **`SceneWorld` is a `flecs::world` from day 0** —
there is no v1 shim, no plain-tables fallback. Nothing in this section is part of the
public surface (§18); ingest adapters never touch these types. Flecs is linked PRIVATE
into `pyxis_renderer` and no Flecs header escapes through `Public/`.

### 8.1 Canonical shape (Flecs ECS)

```
SceneWorld  (flecs::world)
  ├─ Entities
  │   ├─ Mesh       (one per unique geometry; tagged Mesh)
  │   ├─ Material   (one per unique OpenPBRMaterialDesc hash; tagged Material)
  │   ├─ Texture    (one per TextureKey)
  │   ├─ Instance   (one per drawable; relates to Mesh + Material via pairs)
  │   ├─ Light      (distant/dome/rect; tagged with light kind)
  │   └─ Camera     (the active camera entity)
  ├─ Components
  │   ├─ Geom { vertexRange, indexRange, triCount, hasNormals, hasTangents, hasUV }
  │   ├─ Transform { hlslpp::float4x4 world }
  │   ├─ Visibility { bool }
  │   ├─ MaterialRef { flecs::entity material }   // pair (Instance, MaterialOf)
  │   ├─ MeshRef     { flecs::entity mesh }       // pair (Instance, MeshOf)
  │   ├─ BlasRef     { BlasHandle }               // attached to Mesh
  │   ├─ MaterialGpu { uint32_t gpuSlot }         // attached to Material
  │   ├─ TextureGpu  { uint32_t bindlessSlot }    // attached to Texture
  │   ├─ Dirty<Transform>, Dirty<Topology>, Dirty<Material>, Dirty<Visibility>
  │   └─ LightParams (variant: Distant | Dome | Rect)
  └─ Systems (run during CommitResources, in this dependency order)
      1. UploadDirtyTextures      query: Texture, Dirty<Texture>
      2. UploadDirtyMaterials     query: Material, Dirty<Material>
      3. ExtractDirtyMeshes       query: Mesh, Dirty<Topology>     → vertex/index uploads
      4. BuildDirtyBlas           query: Mesh, Dirty<Topology>     → BlasCache
      5. RebuildTlas              query: Instance, Transform, Visibility, MeshRef
                                  (when `K > 1` per \u00a715.5, the system internally
                                   partitions instances by `(instance.id hash mod K)`
                                   and builds K shards in one batched submit)
      6. UpdateBindlessTable      query: Texture, Dirty<TextureGpu>
      7. ClearDirtyFlags          remove all Dirty<*> components
```

Each `GpuScene` public method is exactly one ECS write:

| Public method (§18) | ECS effect |
|---|---|
| `CreateMesh(MeshDesc)` | `world.entity().set<Geom>(...).add<Dirty<Topology>>()` |
| `AcquireMaterial(OpenPBRMaterialDesc)` | hash-lookup → existing entity, else `entity().set<MaterialDesc>(...).add<Dirty<Material>>()` |
| `AcquireTexture(TextureKey)` | hash-lookup → existing entity, else `entity().set<TextureKey>(...).add<Dirty<Texture>>()` |
| `AppendInstance(InstanceDesc)` | `entity().set<Transform>(...).set<Visibility>{true}.add(MeshOf, mesh).add(MaterialOf, mat).add<Dirty<Topology>>()` (TLAS dirty) |
| `UpdateInstanceTransform(h, m)` | `e.set<Transform>(m).add<Dirty<Transform>>()` |
| `SetInstanceVisibility(h, v)` | `e.set<Visibility>{v}.add<Dirty<Visibility>>()` |
| `DestroyInstance(h)` | `e.destruct()` (TLAS rebuild flag set globally) |
| `SetCamera(CameraDesc)` | write components on the singleton camera entity |
| `AddLight(LightDesc)` | `entity().set<LightParams>(...).add<Dirty<Light>>()` |
| `CommitResources(cl)` | `world.progress()` runs the system pipeline above |

### 8.2 Concrete v1 implementation (day-0 Flecs)

`SceneWorld` owns a `flecs::world` and is constructed once per `PyxisRenderer`. Concrete
day-0 contract:

- **Initialisation order** (in `SceneWorld::Init`):
  1. Construct `flecs::world` (with REST iface enabled in Debug only).
  2. Register every component via `world.component<T>()` (one call per component header in
     `Private/Scene/Components/`). Component IDs are stable for the lifetime of the world.
  3. Register pair relationships: `MeshOf`, `MaterialOf` as Flecs tags used in pairs
     `(Instance, MeshOf, mesh)` / `(Instance, MaterialOf, material)`.
  4. Register the custom phase pipeline: `PYXIS_PHASE_UploadTextures` → `_UploadMaterials`
     → `_ExtractMeshes` → `_BuildBlas` → `_RebuildTlas` → `_UpdateBindless` → `_ClearDirty`.
     Pipeline is custom (not `flecs::PreUpdate`/`OnUpdate`) so order is **deterministic
     and explicit** — a mismatch on phase order is a regression-suite failure.
  5. Register all systems (free functions in `Private/Scene/Systems/`) into their phase
     via `world.system<...>().kind(phase).run(System_Verb)`.
  6. Register observers (`OnRemove<MeshRef>` etc.) for refcount + `DeletionQueue` hookup.
- **Handle↔entity mapping** lives in `HandleBimap`:
  - `std::vector<flecs::entity> _byHandleSlot;`  // indexed by `handle.value - 1`, O(1).
  - Reverse lookup is rare (only on observer-driven invariants) and uses
    `flecs::entity::get<HandleTag>()`.
  - Generation counter is encoded in the upper bits of the 32-bit handle so use-after-
    destroy is detected at `Resolve()` time and reported as `ErrorKind::InvalidHandle`.
- **Query caching**: every system caches its `flecs::query_t*` once at registration time
  (`Private/Scene/Queries/QueryCache.h`). **Building a query inside a hot loop is a
  PR-blocking violation** (§30.11).
- **Mutation submission**: ingest threads do **not** call `world.entity(...)` directly.
  They push `MutationCommand` records into a `moodycamel::ConcurrentQueue`; the render
  thread drains the queue at the start of `CommitResources` and applies all writes on
  the world (Flecs is not threadsafe for arbitrary multi-writer access; single-writer
  is the contract).
- **`CommitResources(cl)` body** is essentially `world.progress();` (Flecs runs the
  custom pipeline, in order, and yields back). Pre/post hooks integrate with `Profiler`
  via `PYXIS_GPU_SCOPE`/`PYXIS_CPU_SCOPE`.
- **Debugging**: in Debug builds, `flecs::rest::Init` brings up Flecs Explorer at
  `http://localhost:27750`. The `Profiler` panel exposes a button that prints the
  current archetype set + system schedule to spdlog (handy when a regression image
  diff is non-zero only on certain frames).
- **Component layout discipline**: every component is a POD struct with a fixed layout
  (no `std::vector`, no `std::string`); variable-length data lives in dedicated
  `Private/GpuScene/` tables and is referenced by handle. This keeps Flecs archetypes
  small and predictable, and lets the same component types ship as GPU-side scalar-
  layout structs through `PYXIS_INTEROP_STRUCT` (§23).

### 8.3 SdfPath identity (ingest-adapter side)

SdfPath identity is owned by the **ingest adapter**, never by the renderer:

```
SdfPathHandleMap   (lives in pyxis_hydra / pyxis_usd_ingest)
  unordered_map<SdfPath, MeshHandle>
  unordered_map<SdfPath, MaterialHandle>
  unordered_map<SdfPath, InstanceHandle>
  unordered_map<SdfPath, LightHandle>
  unordered_map<SdfPath, TextureHandle>
```

`pyxis_renderer` neither knows nor cares that SdfPath exists.

---

## 9. RenderGraph and Pass Structure


A simple linear graph in v1 (no automatic culling); explicit dependencies, automatic NVRHI
barriers, GPU timestamp queries auto-injected per pass.

Frame graph (v1). The graph is **GPU-execution-only**: every CPU staging,
upload-record draining and acceleration-structure build happens earlier in the
frame inside `GpuScene::CommitResources(commandList)` (§18.5, §33.4) which the
Application runs on the same command list before submitting the graph. The graph
has no `TlasUpdatePass` / `TextureUploadPass` of its own — those are explicitly
*not* render-graph passes:

```
[BeginFrame]
   ├── (CommitResources runs here, on the same CL, before any graph pass) ──
   ├─ PathTracePass          (raygen/closesthit/miss; writes radiance, AOVs)
   ├─ AccumulationPass       (compute; running average if enableAccumulation)
   ├─ DenoisePass            (optional, off in v1)
   ├─ ToneMapPass            (compute; writes LDR target)
   ├─ AovResolvePass         (writes depth/normal/albedo/materialId/instanceId AOVs)
   ├─ DebugViewPass          (compute; selects which buffer to display)
   ├─ CopyToHydraRenderBufferPass  (copies into Hydra HdRenderBuffer outputs)
   └─ PresentPass            (viewer mode only; copy to swapchain)
[EndFrame]
```

Each pass implements:

```cpp
class IRenderPass {
  virtual std::string_view Name() const = 0;
  virtual void Declare(RenderGraph::Builder&) = 0;
  virtual void Execute(nvrhi::ICommandList*, const PassContext&) = 0;
};
```

`PassContext` exposes `GpuScene*`, `RenderSettings*`, `ProfilerScope*`, `frameIndex`,
`framesInFlight` (runtime active count, `\u2264 MAX_FRAMES_IN_FLIGHT` \u2014 \u00a730.1). ImGui is
not visible from passes.

### 9.1 Worked example — `ToneMapPass`

A complete, idiomatic compute pass. Small enough to fit on a screen, exercises every
contract: header layout, `Declare` resource registration, `Execute` body, NVRHI binding
sets, ShaderInterop constants, profiler scopes, error reporting, and the registration
site inside `PyxisRenderer`.

**Header — `Private/Passes/ToneMapPass.h`**

```cpp
#pragma once

#include <Pyxis/Renderer/Forward.h>           // PyxisRenderer fwd, RenderSettings fwd
#include "RenderGraph/IRenderPass.h"
#include "Shaders/ShaderInterop/ToneMap.hlsli"  // shared C++/Slang struct

#include <nvrhi/nvrhi.h>
#include <hlslpp/hlsl++.h>

namespace pyxis {

class ShaderLibrary;
class RenderGraph;

// Compute pass. Reads HDR radiance (after Accumulation), writes LDR sRGB target.
// Tone-map operator is selected per frame from RenderSettings::toneMap.
class ToneMapPass final : public IRenderPass {
 public:
  ToneMapPass(nvrhi::IDevice* device, ShaderLibrary& shaders);
  ~ToneMapPass() override;

  ToneMapPass(const ToneMapPass&)            = delete;
  ToneMapPass& operator=(const ToneMapPass&) = delete;

  std::string_view Name() const override { return "ToneMap"; }
  void Declare(RenderGraph::Builder& builder) override;
  void Execute(nvrhi::ICommandList* cl, const PassContext& ctx) override;

 private:
  nvrhi::IDevice*               _device     = nullptr;     // not owned
  nvrhi::ShaderHandle            _shader;
  nvrhi::BindingLayoutHandle     _bindingLayout;
  nvrhi::ComputePipelineHandle   _pipeline;

  // Ring of binding sets: one per frame in flight (MAX_FRAMES_IN_FLIGHT, §33.1).
  // The compile-time cap is MAX_FRAMES_IN_FLIGHT = 3; the runtime active count
  // is `GpuSceneCreateDesc::framesInFlight` (default 2, headless 3 — §33.1).
  // Recreated on resize when SRV/UAV identities change.
  std::array<nvrhi::BindingSetHandle, MAX_FRAMES_IN_FLIGHT> _bindingSets{};

  // Resource handles registered with the graph in Declare(); resolved each Execute().
  RgTextureRef _hdrInput{};   // ToneMap reads
  RgTextureRef _ldrOutput{};  // ToneMap writes
};

} // namespace pyxis
```

**Implementation — `Private/Passes/ToneMapPass.cpp`**

```cpp
#include "Passes/ToneMapPass.h"

#include "RenderGraph/RenderGraph.h"
#include "Shaders/ShaderLibrary.h"
#include "Profiler/ProfilerInternal.h"

#include <Pyxis/Renderer/Descs/RenderSettings.h>
#include <spdlog/spdlog.h>

namespace pyxis {

ToneMapPass::ToneMapPass(nvrhi::IDevice* device, ShaderLibrary& shaders)
    : _device(device) {
  PYXIS_ASSERT(device != nullptr);

  _shader = shaders.LoadCompute("ToneMap.cs");
  PYXIS_VERIFY(_shader, "ToneMap.cs failed to load");

  nvrhi::BindingLayoutDesc layoutDesc;
  layoutDesc.visibility = nvrhi::ShaderType::Compute;
  layoutDesc.bindings = {
      nvrhi::BindingLayoutItem::PushConstants(0, sizeof(ToneMapConstants)),
      nvrhi::BindingLayoutItem::Texture_SRV(0),
      nvrhi::BindingLayoutItem::Texture_UAV(0),
  };
  _bindingLayout = _device->createBindingLayout(layoutDesc);

  nvrhi::ComputePipelineDesc pipelineDesc;
  pipelineDesc.CS              = _shader;
  pipelineDesc.bindingLayouts  = { _bindingLayout };
  _pipeline = _device->createComputePipeline(pipelineDesc);
}

ToneMapPass::~ToneMapPass() = default;

void ToneMapPass::Declare(RenderGraph::Builder& builder) {
  // Inputs/outputs are referenced by stable graph names (§9). Concrete textures are
  // owned by PyxisRenderer; the graph resolves the ref at Execute() time.
  _hdrInput  = builder.Read ("Radiance.HDR.Accumulated", nvrhi::ResourceStates::ShaderResource);
  _ldrOutput = builder.Write("Color.LDR",                 nvrhi::ResourceStates::UnorderedAccess);
}

void ToneMapPass::Execute(nvrhi::ICommandList* cl, const PassContext& ctx) {
  PYXIS_GPU_SCOPE(cl, "pass.ToneMap");
  PYXIS_CPU_SCOPE("ToneMapPass::Execute");

  nvrhi::ITexture* hdr = ctx.graph->Resolve(_hdrInput);
  nvrhi::ITexture* ldr = ctx.graph->Resolve(_ldrOutput);

  const uint32_t frameSlot = ctx.frameIndex % ctx.framesInFlight;  // runtime, ≤ MAX_FRAMES_IN_FLIGHT
  auto&          set       = _bindingSets[frameSlot];

  // Rebuild the binding set lazily when the resolved texture identity changes
  // (resize, AOV reformat). Identity check is a simple pointer compare cached
  // by the graph; details elided.
  if (!set || ctx.graph->BindingsInvalidated(_hdrInput, _ldrOutput, frameSlot)) {
    nvrhi::BindingSetDesc bindings;
    bindings.bindings = {
        nvrhi::BindingSetItem::PushConstants(0, sizeof(ToneMapConstants)),
        nvrhi::BindingSetItem::Texture_SRV(0, hdr),
        nvrhi::BindingSetItem::Texture_UAV(0, ldr),
    };
    set = _device->createBindingSet(bindings, _bindingLayout);
  }

  ToneMapConstants constants{};
  constants.exposure   = ctx.settings->exposure;
  constants.toneMapOp  = static_cast<uint32_t>(ctx.settings->toneMap);
  constants.outputSize = uint2(ldr->getDesc().width, ldr->getDesc().height);

  nvrhi::ComputeState state;
  state.pipeline    = _pipeline;
  state.bindings    = { set };
  cl->setComputeState(state);
  cl->setPushConstants(&constants, sizeof(constants));

  // 8x8 thread groups; matches numthreads in ToneMap.cs.
  constexpr uint32_t TILE_SIZE = 8;
  cl->dispatch((constants.outputSize.x + TILE_SIZE - 1) / TILE_SIZE,
               (constants.outputSize.y + TILE_SIZE - 1) / TILE_SIZE,
               1);
}

} // namespace pyxis
```

**Shared C++ ↔ Slang struct — `Private/Shaders/ShaderInterop/ToneMap.hlsli`**

```hlsl
#pragma once
#include "ShaderInterop.slang"   // PYXIS_INTEROP_STRUCT, shared C++/Slang types

PYXIS_INTEROP_STRUCT(ToneMapConstants)
{
    float    exposure;
    uint32_t toneMapOp;        // 0=Linear, 1=ACES, 2=Reinhard
    uint2    outputSize;
};
```

**Slang entry point — `Private/Shaders/ToneMap.cs.slang`**

```hlsl
#include "ShaderInterop/ToneMap.hlsli"

[[vk::push_constant]] ConstantBuffer<ToneMapConstants> gConstants;
Texture2D<float4>       gHdr     : register(t0);
RWTexture2D<float4>     gLdr     : register(u0);

float3 ToneMap_Aces(float3 c) { /* ... */ return c; }
float3 ToneMap_Reinhard(float3 c) { return c / (1.0 + c); }

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    if (any(tid.xy >= gConstants.outputSize)) return;

    float3 hdr = gHdr[tid.xy].rgb * exp2(gConstants.exposure);

    float3 ldr;
    switch (gConstants.toneMapOp) {
        case 1:  ldr = ToneMap_Aces(hdr);     break;
        case 2:  ldr = ToneMap_Reinhard(hdr); break;
        default: ldr = hdr;                   break;
    }

    gLdr[tid.xy] = float4(saturate(ldr), 1.0);
}
```

**Registration site — inside `PyxisRenderer::Init`**

```cpp
// Order matters: ToneMap consumes Accumulation's output, AovResolve consumes ToneMap's.
// Note: BLAS / TLAS / texture / mesh uploads are NOT graph passes — they run inside
// GpuScene::CommitResources before the graph executes (§18.5, §33.4).
_renderGraph.AddPass(std::make_unique<PathTracePass>       (_device, _shaders, _scene));
_renderGraph.AddPass(std::make_unique<AccumulationPass>    (_device, _shaders));
_renderGraph.AddPass(std::make_unique<ToneMapPass>         (_device, _shaders));   // <-- here
_renderGraph.AddPass(std::make_unique<AovResolvePass>      (_device, _shaders));
_renderGraph.AddPass(std::make_unique<CopyToHydraRenderBufferPass>(_device));
```

**What this example illustrates (and that every other pass must follow):**

1. **Constructor takes `nvrhi::IDevice*` + `ShaderLibrary&`**, never the device manager
   or the application. Passes are unaware of viewer/headless mode.
2. **Pipeline + binding-layout creation happens once** in the constructor; nothing
   GPU-allocating happens in `Execute` (§30.10).
3. **`Declare` only registers resource refs**; it never reads `RenderSettings`. Graph
   barriers are a function of the registered states only.
4. **`Execute` is allocation-free**: binding-set rebuild is conditional on a resolved
   identity change (resize/AOV reformat), the constants struct is on the stack, the
   dispatch is computed from the resolved texture's `getDesc()`.
5. **Profiler scopes are mandatory**: one `PYXIS_GPU_SCOPE` (`pass.<Name>`) and one
   `PYXIS_CPU_SCOPE` per pass. The KPI budgets in §34 reference `pass.ToneMap` by name.
6. **C++/Slang struct is shared via `PYXIS_INTEROP_STRUCT`** (§23). Layout is the
   default HLSL cbuffer/std140-equivalent rhythm (16-byte vector alignment —
   convention), row-major; never duplicate the struct in the shader.
7. **Frame-in-flight ring of binding sets**: one entry per `MAX_FRAMES_IN_FLIGHT` slot
   (compile-time cap = 3, §33.1); only the first `framesInFlight` slots are alive at
   runtime. A frame's binding set is never recycled while still in use by the GPU.
8. **No exceptions, no RTTI, no `std::filesystem` calls in `Execute`** (§30.1).
9. **The pass does not reach into `GpuScene` or Flecs**; tone-mapping is purely a
   per-pixel transform on graph-managed textures. Passes that *do* touch the scene
   (e.g. `PathTracePass`) take a `const GpuScene&` reference and call only its public
   accessors.

For raytracing-style passes (`PathTracePass`), the only differences are: an
`nvrhi::rt::PipelineDesc` with raygen/miss/hitgroup shaders + an SBT, a `cl->dispatchRays`
in `Execute`, and a binding set that includes the bindless raw-buffer/texture tables
from `GpuScene`.

### 9.2 `RenderGraph` — concrete implementation

The graph framework itself fits in ~500 LOC and lives at
`pyxis_renderer/Private/RenderGraph/`. It is *not* a generic FrameGraph: it is a
linear, allocation-free, single-threaded recorder with three responsibilities —
**resource registry**, **barrier insertion**, **profiler-scope wrapping**. There
is no DAG culling, no aliasing, no async-compute split (§9 opening rule).

**File layout**

```
Private/RenderGraph/
  IRenderPass.h          // shown above; pass interface
  RgRefs.h               // RgTextureRef, RgBufferRef — opaque handles
  RenderGraph.h/.cpp     // RenderGraph + RenderGraph::Builder
  PassContext.h          // struct passed to Execute()
```

**Resource refs** (`RgRefs.h`)

```cpp
namespace pyxis {

// Opaque indices into RenderGraph's resource table. NOT pointers; valid only
// for the graph instance that produced them. Comparable, hashable, default
// construction = invalid.
enum class RgTextureRef : uint32_t { Invalid = 0 };
enum class RgBufferRef  : uint32_t { Invalid = 0 };

} // namespace pyxis
```

**`PassContext`** — the only thing `Execute` is allowed to read state from.

```cpp
struct PassContext {
  GpuScene*         scene          = nullptr;     // borrowed
  RenderSettings*   settings       = nullptr;     // borrowed; mutable for reactive knobs
  RenderGraph*      graph          = nullptr;     // for Resolve(), BindingsInvalidated()
  uint64_t          frameIndex     = 0;           // monotonic; never wraps in v1
  uint32_t          framesInFlight = 2;           // runtime, ≤ MAX_FRAMES_IN_FLIGHT (§33.1)
  uint2             renderResolution{};           // backbuffer pixels (not DIPs, §5)
};
```

**`RenderGraph` and `RenderGraph::Builder`**

The graph has two lifecycle phases plus per-frame execute:

- **Construction** (`PyxisRenderer::Init`): `AddPass(std::unique_ptr<IRenderPass>)`
  appends to a `std::vector` in registration order. No pass is `Declare`'d yet.
- **Compile** (`PyxisRenderer::Init`, after all `AddPass` calls; also re-run on
  resize / AOV-flag change): walks the pass list once, calls `Declare` on each,
  builds the resource table and the per-pass barrier list. Validates that every
  `Read` is preceded by a `Write` of the same ref; missing producer is
  `ErrorKind::RenderGraphMissingProducer`.
- **Execute** (`PyxisRenderer::RenderFrame`): walks the pass list once, opens a
  CPU+GPU profiler scope per pass, emits the cached barriers, calls
  `pass.Execute(commandList, ctx)`. Allocation-free in the steady state.

```cpp
namespace pyxis {

class RenderGraph final {
public:
  explicit RenderGraph(nvrhi::IDevice* device, Profiler* profiler) noexcept;
  ~RenderGraph();

  RenderGraph(const RenderGraph&)            = delete;
  RenderGraph& operator=(const RenderGraph&) = delete;

  // ---- Construction ----
  void AddPass(std::unique_ptr<IRenderPass> pass);

  // ---- Compile (idempotent; called from Init and from OnResize) ----
  [[nodiscard]] Expected<void> Compile();

  // External resources (color/AOV targets, swapchain image proxy, internal
  // accumulation buffer) are imported into the registry by name. Imports are
  // refreshed in OnResize before Compile re-runs.
  [[nodiscard]] RgTextureRef ImportTexture(std::string_view name,
                                           nvrhi::ITexture* tex,
                                           nvrhi::ResourceStates initialState) noexcept;
  [[nodiscard]] RgBufferRef  ImportBuffer (std::string_view name,
                                           nvrhi::IBuffer*  buf,
                                           nvrhi::ResourceStates initialState) noexcept;

  // ---- Execute (one call per frame, after GpuScene::CommitResources) ----
  void Execute(nvrhi::ICommandList* cl, const PassContext& ctx);

  // ---- Pass-callable helpers (used inside Execute) ----
  [[nodiscard]] nvrhi::ITexture* Resolve(RgTextureRef ref) const noexcept;
  [[nodiscard]] nvrhi::IBuffer*  Resolve(RgBufferRef  ref) const noexcept;

  // True iff the resolved texture identity for any of the given refs has
  // changed since the pass last rebuilt its binding set for `frameSlot`.
  // Pass implementations call this to lazy-rebuild binding sets only on
  // resize / AOV reformat (see §9.1 ToneMapPass::Execute).
  [[nodiscard]] bool BindingsInvalidated(RgTextureRef ref, uint32_t frameSlot) noexcept;
  [[nodiscard]] bool BindingsInvalidated(RgTextureRef a, RgTextureRef b,
                                         uint32_t frameSlot) noexcept;

  // ---- Builder, used only inside IRenderPass::Declare ----
  class Builder final {
   public:
    [[nodiscard]] RgTextureRef Read (std::string_view name, nvrhi::ResourceStates state);
    [[nodiscard]] RgTextureRef Write(std::string_view name, nvrhi::ResourceStates state);
    [[nodiscard]] RgBufferRef  ReadBuffer (std::string_view name, nvrhi::ResourceStates state);
    [[nodiscard]] RgBufferRef  WriteBuffer(std::string_view name, nvrhi::ResourceStates state);
   private:
    friend class RenderGraph;
    RenderGraph* _graph = nullptr;
    uint32_t     _passIndex = 0;
  };

private:
  // Pass entry: pass ptr + cached barriers computed at Compile() time.
  struct PassEntry {
    std::unique_ptr<IRenderPass>          pass;
    std::vector<nvrhi::TextureBarrier>    textureBarriers;
    std::vector<nvrhi::BufferBarrier>     bufferBarriers;
  };

  // Resource entry: identity + last-known state. Resolved at Execute() time.
  struct TexEntry {
    std::string             name;
    nvrhi::ITexture*        texture          = nullptr;        // borrowed
    nvrhi::ResourceStates   currentState     = nvrhi::ResourceStates::Common;
    uint32_t                lastWritePass    = UINT32_MAX;
    uint64_t                identityRevision = 0;              // bumped on import refresh
  };
  struct BufEntry { /* mirror of TexEntry */ };

  nvrhi::IDevice*        _device   = nullptr;   // borrowed
  Profiler*              _profiler = nullptr;   // borrowed
  std::vector<PassEntry> _passes;
  std::vector<TexEntry>  _textures;
  std::vector<BufEntry>  _buffers;
  bool                   _compiled = false;

  // Per-frame-slot, per-resource identity revisions remembered by passes via
  // BindingsInvalidated; flat dense table to keep Execute() allocation-free.
  std::array<std::vector<uint64_t>, MAX_FRAMES_IN_FLIGHT> _seenRevisions{};
};

} // namespace pyxis
```

**Compile-time barrier resolution.** During `Compile`, the graph walks the pass
list in registration order. For each pass it asks the pass to `Declare` and
records `(ref, requiredState, kind=Read|Write)` tuples. After collecting all
declarations the graph walks them once more and emits a barrier on a pass
whenever a resource's `currentState` differs from the required state, then
updates `currentState`. The barriers are stored on the `PassEntry` so `Execute`
just emits a pre-computed list.

```cpp
Expected<void> RenderGraph::Compile() {
  for (uint32_t i = 0; i < _passes.size(); ++i) {
    Builder b{ this, i };
    _passes[i].pass->Declare(b);
  }
  // Walk Declarations in order; emit barriers when state must change.
  for (uint32_t i = 0; i < _passes.size(); ++i) {
    for (const auto& decl : _passes[i].textureDecls) {
      auto& tex = _textures[ToIndex(decl.ref)];
      if (tex.currentState != decl.state) {
        _passes[i].textureBarriers.push_back({ tex.texture, tex.currentState, decl.state });
        tex.currentState = decl.state;
      }
      if (decl.kind == DeclKind::Read && tex.lastWritePass == UINT32_MAX) {
        return PYXIS_ERROR(ErrorKind::RenderGraphMissingProducer,
                           "pass {} reads {} which no earlier pass writes",
                           _passes[i].pass->Name(), tex.name);
      }
      if (decl.kind == DeclKind::Write) tex.lastWritePass = i;
    }
    /* same for buffers */
  }
  _compiled = true;
  return {};
}
```

**Execute is dumb on purpose.** `Execute` records the pre-computed barrier
batch, opens profiler scopes, calls the pass, repeats. No DAG walk, no
"is-this-pass-still-needed" check, no resource aliasing.

```cpp
void RenderGraph::Execute(nvrhi::ICommandList* cl, const PassContext& ctx) {
  PYXIS_ASSERT(_compiled);
  for (auto& entry : _passes) {
    PYXIS_CPU_SCOPE_DYN(entry.pass->Name());                    // Tracy zone
    pyxis::Profiler::ScopedGpu gpu(*_profiler, cl, entry.pass->Name()); // §34
    if (!entry.textureBarriers.empty() || !entry.bufferBarriers.empty()) {
      cl->setEnableAutomaticBarriers(false);
      for (const auto& b : entry.textureBarriers) cl->setTextureState(b);
      for (const auto& b : entry.bufferBarriers)  cl->setBufferState(b);
      cl->commitBarriers();
      cl->setEnableAutomaticBarriers(true);
    }
    PassContext localCtx = ctx;
    localCtx.graph = this;
    entry.pass->Execute(cl, localCtx);
  }
}
```

**Why automatic-barriers-off then back on.** NVRHI's automatic-barrier mode is
correct but conservative. The graph turns it off only for the cached barriers
it already computed, then turns it back on so the pass body itself (which the
graph cannot statically inspect — e.g. a raygen that writes to bindless UAVs
inside the closesthit) still benefits from NVRHI's tracking. This dual-mode
pattern matches the standard `donut`-style RenderGraph layout used in NVRHI sample code.

**Resize / AOV-flag change.** `PyxisRenderer::OnResize(uint2 newRes)`:

1. Recreate every imported texture (`color`, `radiance`, `normal`, `albedo`,
   `motionVector`, `materialId`, `instanceId`) at the new resolution, AOV
   bits respected.
2. Call `_renderGraph.ImportTexture(name, ptr, initialState)` for each — this
   bumps the `identityRevision` of the matching `TexEntry`.
3. Call `_renderGraph.Compile()` to rebuild the cached barrier lists. (This is
   cheap — a few hundred microseconds even on production-shaped scenes; it does not allocate the pass
   table.)
4. Pass binding sets rebuild lazily on the *next* `Execute` because
   `BindingsInvalidated` returns true on identity-revision mismatch.

The same flow handles `RenderTargets::enabledAovs` changing between frames
(§19.8): the graph recompiles, the pass list is unchanged, dead AOV writes
become writes to a 1×1 placeholder texture (cheaper than re-plumbing the pass
list).

**Error surface.** `Compile()` returns `Expected<void>` with these codes (added
to the §20 catalogue):

- `RenderGraphMissingProducer` — a pass `Read`s a name no earlier pass `Write`s.
- `RenderGraphDuplicateImport` — the same name is imported twice.
- `RenderGraphUnknownRef` — `Resolve(ref)` called on a ref from another graph
  instance (programmer error; `PYXIS_VERIFY` in Debug, returns `nullptr` in
  Release).

`Execute` is `void` and never fails: every error class it could hit
(state mismatch, dangling ref) is a programmer error caught in Debug.

**Threading.** The graph is **render-thread-only** (§31). `AddPass`, `Compile`,
`Execute` and every `IRenderPass` method run on the render thread. Ingest
threads never touch the graph; they push mutations onto `GpuScene`'s
moodycamel queue (§4) which the render thread drains inside
`GpuScene::CommitResources` *before* the graph executes (§18.5).

**What the graph deliberately does not do, and where it would change to.**

| Feature | Status | Trigger to add |
|---|---|---|
| DAG topology / culling | Not v1 | First time we have ≥ 2 distinct render outputs (split-screen, picking-on-demand) |
| Resource aliasing | Not v1 | First time peak GPU > VRAM budget (§17) |
| Async-compute split | Not v1 | First profile showing a clear compute-only bubble in the trace |
| Multiple command queues | Not v1 | Same trigger as async-compute |
| Sub-pass merging (Vulkan render passes) | Not v1 | We are RT-only; merging buys nothing for compute + dispatchRays |
| Conditional pass enable | Yes (today) | `AddPass` is called or skipped at construction (`if (settings.aov.normal) graph.AddPass(...)`) |

Every line above is an RFC trigger, not a v1 milestone (§44).

---

## 10. Shader Organization (Slang)


**Matrix layout convention: row-major, everywhere.**

- Slang is invoked with `-matrix-layout-row-major` (the C++ → Slang shared interop relies on it,
  and `hlslpp` matrices are row-major in memory). All `float4x4`/`float3x4` constants are
  written and read as row-major; no per-shader `column_major` qualifiers anywhere.
- C++ side: `hlslpp::float4x4` rows are stored as `float4` rows; uploaded byte-for-byte to
  constant/structured buffers; consumed in shaders as row-major `float4x4`.
- Multiplication convention is **column-vector** (textbook math, `v' = M·v`):
  `pos_clip = mul(viewProjMatrix, pos_world)` — matrix on the left, vector on the right.
  Translation lives in the **last column** of every transform matrix. Camera / instance
  code, the `ShaderInterop` header and any debug viz must follow this; do not mix
  `mul(M, v)` and `mul(v, M)` styles. (Earlier drafts of this plan specified
  row-vector convention; the M3 cube-render commit flipped to column-vector across the
  codebase because it matches the standard linear-algebra notation used in
  graphics literature, GLSL's default `mat * vec`, and what most contributors
  reach for first.)
- BLAS/TLAS instance transforms are `3x4` row-major affine (`VkTransformMatrixKHR` is
  natively row-major with translation in the last column — same shape as our
  column-vector matrices, dropped to 3 rows since the bottom `[0,0,0,1]` is implicit).
- Compile-time guard in `Compiler.cmake`: a unit test asserts a known transform matches
  byte-for-byte between a C++-built `hlslpp::float4x4` and a Slang-side reference, to
  catch any accidental column-major regression.


```
resources/shaders/
  ├─ shaders.cfg                    (variant manifest, plain text, one entry per shader permutation)
  ├─ common/
  │  ├─ ShaderInterop.slang         (#includable from C++; defines OpenPBRMaterialGPU, GpuInstance, constants)
  │  ├─ Sampling.slang              (RNG, low-discrepancy)
  │  ├─ Colorspace.slang
  │  └─ Random.slang
  ├─ openpbr/
  │  ├─ OpenPBR.slang               (BRDF lobes: diffuse, specular GGX, metal, transmission, coat, emission)
  │  └─ MaterialEvaluator.slang
  ├─ pathtracing/
  │  ├─ PathTrace.slang             (raygen)
  │  ├─ ClosestHit.slang
  │  ├─ AnyHit.slang
  │  └─ Miss.slang
  ├─ post/
  │  ├─ Accumulation.slang
  │  ├─ ToneMap.slang
  │  ├─ AovResolve.slang
  │  └─ DebugView.slang
  └─ utils/
     └─ Copy.slang
```

- Build via `Slang.cmake` driving **ShaderMake**: each entry compiles to SPIR-V `.spv`;
  permutations declared in `shaders.cfg` follow the `-D NAME={0,1}` pattern that ShaderMake
  consumes natively. ShaderMake
  handles dependency tracking, permutation expansion, and incremental rebuilds; it invokes
  the Slang compiler (or DXC where required) under the hood.
- Shared header `ShaderInterop.slang` is also included from C++ via the
  `PYXIS_INTEROP_STRUCT(name)` macro shim defined in §23 (the §9.1 worked example
  uses it). The legacy alias `PYXIS_SHADER_INTEROP` is removed; do not introduce it.
  This is where
  `OpenPBRMaterialGPU`, `GpuInstance`, `CameraConstants`, `PathTraceConstants` live.
- Slang-specific modules (`import openpbr.OpenPBR;`) preferred over textual includes.
- Hot-reload (viewer mode only): triggered **explicitly** by an ImGui button
  *Reload Shaders* in the debug overlay (`PYXIS_DEBUG_TOOLS`); there is **no automatic
  file watcher** in v1 (avoids partial-save races on Windows IDEs). The button
  spawns a detached `std::jthread` ("shader-reload worker") that (1) recompiles
  every `.slang` entry point via Slang's session API off the render thread, (2)
  on success swaps the SPIR-V blobs and PSOs atomically at the next frame boundary
  (under a single mutex held by `ShaderLibrary`) and resets accumulation, (3) on
  **any** compile failure logs the diagnostic to spdlog, surfaces it in the
  ImGui status bar, and **keeps the previous PSOs live** — the running frame is
  never interrupted by a bad shader edit. Headless mode does not expose this path.

---

## 11. OpenPBR Canonical Material Architecture


OpenPBR is the canonical model. Every supported input is converted on the CPU side to a single
`OpenPBRMaterialDesc`, hashed, deduplicated, packed into `OpenPBRMaterialGPU`, and uploaded.

```cpp
struct OpenPBRMaterialDesc {
  // Base
  hlslpp::float3  baseColor       = {0.8f, 0.8f, 0.8f};
  float           baseWeight      = 1.0f;
  float           metalness       = 0.0f;
  float           roughness       = 0.5f;
  // Specular
  float           specularWeight  = 1.0f;
  float           specularIor     = 1.5f;
  hlslpp::float3  specularColor   = {1, 1, 1};
  // Transmission
  float           transmissionWeight = 0.0f;
  hlslpp::float3  transmissionColor  = {1, 1, 1};
  // Coat
  float           coatWeight      = 0.0f;
  float           coatRoughness   = 0.05f;
  float           coatIor         = 1.5f;
  // Emission
  hlslpp::float3  emissionColor   = {0, 0, 0};
  float           emissionWeight  = 0.0f;
  // Geometry
  float           opacity         = 1.0f;
  float           normalStrength  = 1.0f;
  bool            doubleSided     = false;
  // Subsurface (deferred)
  float           subsurfaceWeight = 0.0f;
  hlslpp::float3  subsurfaceColor  = {1, 1, 1};
  // Texture slots (TextureHandle::Invalid if absent)
  TextureHandle   baseColorMap, metallicMap, roughnessMap, normalMap,
                  emissionMap, opacityMap, transmissionMap, coatRoughnessMap;
  // Provenance \u2014 the enum is declared at namespace scope in OpenPBRMaterialDesc.h (see \u00a728.4);
  // here we just reference it.
  Source           source     = Source::Default;
  // Diagnostics-only SdfPath. Borrowed at the call site; the renderer copies the
  // first 63 bytes into an internal POD before queueing the mutation across
  // threads (§18.5 / §31). Not hashed.
  std::string_view sourcePrim;
};

struct OpenPBRMaterialGPU {  // 16-byte aligned, packed for the GPU; cbuffer layout (§23).
  uint32_t       baseColorTex, metallicTex, roughnessTex, normalTex;          // row 0
  uint32_t       emissionTex, opacityTex, transmissionTex, coatRoughnessTex;  // row 1
  hlslpp::float3 baseColor;          float baseWeight;                        // row 2
  hlslpp::float3 specularColor;      float roughness;                         // row 3
  hlslpp::float3 emissionColor;      float metalness;                         // row 4
  hlslpp::float3 transmissionColor;  float opacity;                           // row 5
  float          specularIor, specularWeight, transmissionWeight, normalStrength; // row 6
  float          coatWeight, coatRoughness, coatIor;
  float          _reserved_coatNormalStrength = 0.0f;                         // row 7 — §22.3 reserved-slot
  uint32_t       flags;             // bitmask of MaterialFlag (see §11.6)
  uint32_t       _reserved0 = 0, _reserved1 = 0, _reserved2 = 0;              // row 8 — §22.3 reserved-slots
};
static_assert(sizeof(OpenPBRMaterialGPU) % 16 == 0,
              "OpenPBRMaterialGPU must be 16-byte aligned (§23 cbuffer layout)");
```

### 11.6 `MaterialFlag` enum (GPU `flags` bits)

Declared once in `Private/Materials/MaterialFlag.h`; mirrored in `ShaderInterop.slang`
so the closesthit shader's branchless dispatch reads the same bits.

```cpp
enum class MaterialFlag : uint32_t {
  None               = 0,
  DoubleSided        = 1u << 0,
  HasBaseColorMap    = 1u << 1,
  HasNormalMap       = 1u << 2,
  HasMetallicMap     = 1u << 3,
  HasRoughnessMap    = 1u << 4,
  HasEmissionMap     = 1u << 5,
  HasOpacityMap      = 1u << 6,
  HasTransmissionMap = 1u << 7,
  HasCoatRoughnessMap = 1u << 8,
  AlphaTested        = 1u << 9,   // opacityThreshold > 0
  CoatEnabled        = 1u << 10,  // coatWeight > 0
  TransmissionEnabled= 1u << 11,  // transmissionWeight > 0
  Emissive           = 1u << 12,  // emissionWeight > 0 OR emissionMap valid
};
```

The closesthit shader uses `(flags & CoatEnabled)` etc. to skip lobe evaluation
entirely — "branchless on flags" means a single conditional per lobe, predicated on
bit-test, never a string of `if (material has X)` lookups against texture handles.
```

Conversion rules (summary table — full mappings in `_documentation/openpbr.md`):

| Input | Field | OpenPBR target |
|---|---|---|
| UsdPreviewSurface | `diffuseColor` | `baseColor` |
| UsdPreviewSurface | `metallic` | `metalness` |
| UsdPreviewSurface | `roughness` | `roughness` |
| UsdPreviewSurface | `useSpecularWorkflow=1, specularColor` | `metalness=0`, `specularColor` |
| UsdPreviewSurface | `ior` | `specularIor` |
| UsdPreviewSurface | `clearcoat` | `coatWeight` |
| UsdPreviewSurface | `clearcoatRoughness` | `coatRoughness` |
| UsdPreviewSurface | `emissiveColor` | `emissionColor`, `emissionWeight=1` if non-zero |
| UsdPreviewSurface | `opacity`, `opacityThreshold` | `opacity`, alpha-test flag |
| UsdPreviewSurface | `normal` | `normalMap` |
| UsdPreviewSurface | `displacement` | logged unsupported, skipped v1 |
| MaterialX `standard_surface` | base/specular/coat/emission/transmission | translation shim into OpenPBR |
| MaterialX `open_pbr_surface` | full coverage (canonical mapping) | identity copy |
| MaterialX node graphs (procedural / arbitrary nodes) | logged unsupported, skipped v1 | fallback to constants pulled from connected default values |
| RenderMan `PxrSurface` etc. | match common channels (diffuse/specular/clearcoat/glass/glow) | best-effort + fallback |
| Unknown / unsupported | — | fallback gray material; entry in `unsupported_features.json` |

Hashing: `OpenPBRMaterialDesc` is hashed with **`XXH3_64bits`** (xxhash 0.8+) over
numeric fields + texture handles, in declared field order; equal hashes share a
`MaterialHandle`. The `sourcePrim` field is **not** hashed (diagnostic-only). Texture-handle
changes invalidate the hash.

Shader strategy: **one** generic OpenPBR closest-hit shader in v1, branchless on `flags`. Material
specialization (multiple hit groups) is deferred.

---

## 12. RNG / Sampler Strategy


Determinism rule (v1): `(seed, frameIndex, pixelXY, sampleIndex, bounceIndex)` uniquely
selects a 32-bit random word. Same inputs → byte-identical EXR.

### 12.1 Default sampler — PCG32 streams

- One `PCG32` instance per shader invocation, **never** a global one.
- State seeded at the top of the closesthit shader as:
  ```hlsl
  uint streamId = HashCombine(
      pixelXY.y * width + pixelXY.x,   // pixel
      sampleIndex,                       // sample inside frame
      bounceIndex);                      // depth
  uint64_t state = SplitMix64(uint64_t(seed) ^ uint64_t(frameIndex) << 32 ^ streamId);
  ```
  where `HashCombine` is `pcg_hash` (Bob Jenkins-style mix) and `SplitMix64` is the
  standard one-line splitmix.
- Each `Random()` call advances the PCG32 state with the canonical multiplier-increment
  pair; fast (one IMUL + one IADD + one rotate on GPU).
- This satisfies the "no two bounces share a stream" requirement that prevents the
  characteristic "correlated noise" path-tracer artefact.

### 12.2 Optional Sobol+Owen (`RenderSettings::lowDiscrepancySampling = true`)

- Per-pixel scrambled Owen-shuffled Sobol; 256-dimensional table baked at startup
  (~1 MiB structured buffer). Dimension index advances one per call site
  (camera-jitter → dim 0–1, lens → dim 2–3, primary BSDF → dim 4–5, NEE light pick →
  dim 6, NEE BSDF → dim 7–8, then PCG32 fallback past dim 256).
- Owen scramble seed = `HashCombine(pixelXY, seed)`; gives per-pixel decorrelation
  while keeping the low-discrepancy property within a pixel.
- Cost: 4–6 % vs PCG32 on RTX 4080. Convergence: ~1.5–2× fewer samples for the same
  RMSE on smooth diffuse surfaces; roughly even on glossy hair / fuzz.

### 12.3 Why not `xorshift` / `wang_hash` only

`wang_hash(streamId)` alone correlates between bounces (small Hamming-distance
streamIds produce small Hamming-distance first outputs); PCG32 fixes this by
carrying a 64-bit state across `Random()` calls. Reviewers reject any
shader-internal `Random()` that hashes per-call without a stateful stream.

### 12.4 Camera jitter sequence

Per-frame camera-space jitter is **decoupled from the path-tracer RNG** so accumulation
correctness depends on it explicitly:

- Default: scrambled-Halton (2,3) with per-frame Cranley-Patterson rotation, seeded
  by `RenderSettings::seed`. 1 MiB pre-baked table (`shaders/common/Halton.slang`).
- When `RenderSettings::lowDiscrepancySampling = true`, jitter takes Sobol dimensions
  0–1 (§12.2) instead of Halton; the rest of the path is shared.
- Headless mode pins the jitter sequence to `seed` so two runs at the same
  `samplesPerFrame` produce byte-identical EXRs.

### 12.5 Russian-roulette + firefly clamp interaction

The order is **fixed and normative** to avoid bias:

1. Sample BSDF / light, compute throughput-weighted radiance for this bounce.
2. Apply `fireflyClampLuminance` to the *returned* radiance (caps spikes from
   missed-NEE specular caustics).
3. *Then* apply Russian-roulette weighting (`russianRouletteStartBounce`).

Clamping after RR would scale the clamp threshold by the survival probability and
introduce variance; clamping before BSDF sample would change the integrand. The
shader carries `static_assert`-style comments at each call site referencing this
section.

---

## 13. Texture Loading & Caching for a Large USD Scene


- `TextureCache` is keyed on `(resolvedPath, role, colorspace)`.
  Roles: `BaseColor`, `NormalMap`, `RoughnessMetallic`, `Emission`, `Opacity`, `EnvLatLong`.
- Lazy load: textures referenced by an `OpenPBRMaterialDesc` are only opened on first use,
  then submitted to an asynchronous decode + upload pipeline.
- Mip generation: GPU mip generation via a compute pass for non-pre-mipped formats; EXR can be
  pre-mipped on disk if available (some production assets ship `.tex` and `.exr`; `.tex` is RenderMan-only —
  we look for `.exr` siblings, otherwise rasterize a fallback).
- sRGB vs linear: derived from role; `BaseColor`, `Emission` are sRGB; `NormalMap`,
  `RoughnessMetallic` are linear. Color management is `basic` in v1 (gamma 2.2 / linear sRGB).
- Format conversion: 8-bit textures uploaded as-is in their native format
  (`R8G8B8A8_UNORM` / `R8G8B8A8_UNORM_SRGB`); 16/32-bit float → `R16G16B16A16_SFLOAT` /
  `R32G32B32A32_SFLOAT`. **GPU-side block compression (BC7 / BC5 / etc.) is deferred**;
  we accept the larger texture footprint v1 and rely on `textures.maxResolution` to
  cap memory.
- UDIM: detected by `<UDIM>` token in resolved path. v1 strategy: **one bindless
  `Texture2D` per UDIM tile** (varying tile sizes are common in production assets — hero
  meshes ship at 4K, distant rocks at 512). Bistro doesn't use UDIM at v1, so this path is
  exercised by a small synthetic UDIM fixture (§35); the architecture is in place for
  post-v1 production-class scenes. The shader looks up
  `(materialId, udimTile) → bindlessSlot` via a per-material UDIM lookup table
  (small structured buffer, ~256 entries per material). Rejected alternative: a flat
  `Texture2DArray` requires every layer at the asset's max resolution, which inflates
  texture memory by roughly 4× with no quality benefit.
- Missing texture: fallback color from `parameters.json` `textures.missingTextureColor`.
- Bindless: each `TextureHandle` is a slot index in a single `Texture_SRV` bindless table.
- Resolution clamp: `textures.maxResolution` clamps decode size; LOD0 capped, lower mips kept.
- Eviction: simple LRU on staging-side decoded data. GPU resident textures are not evicted in v1;
  we report budget breaches.

---

## 14. Geometry Loading Strategy for Very Large Meshes


- Meshes streamed mesh-by-mesh via Hydra `HdMesh::Sync` → `GpuScene::createOrUpdateMesh`.
- `HdMeshUtil::ComputeTriangleIndices` for triangulation; `ComputeNumPoints`/normals helpers.
- Generate normals when missing (`pxOsd` smoothing, or face normals fallback).
- Tangents: MikkTSpace if `normalMap` is present **and** UVs exist; otherwise screen-space
  derivatives in shader.
- Single big vertex pool + single big index pool per `GpuScene`, **sliced into pages
  of ≤ 4 GiB** so each page fits inside `VkPhysicalDeviceLimits::maxStorageBufferRange`
  (4 GiB on most desktop hardware). Bindless SRVs are issued one per page; the mesh's
  `Geom` component carries `(pageIndex, vertexOffset, indexOffset)`. Page count is
  bounded (Bistro fits in 1 vertex page, 1 index page on an 8 GB card; production-class scenes fit in ≤ 2 vertex pages, ≤ 1 index page on a 24 GB card). Requires
  Vulkan 1.3 or `VK_KHR_maintenance4` for the > 4 GiB allocation; both are mandatory
  in §5.

### 14.5 Buffer-pool segmentation (concrete)

| Pool | Page size cap | Max pages v1 | Bindless slot count |
|---|---|---|---|
| Vertex pool | 4 GiB | 4 | 4 (`RawBuffer_SRV(s=1, slot=0..3)`) |
| Index pool  | 4 GiB | 2 | 2 (`RawBuffer_SRV(s=1, slot=4..5)`) |
| Material GPU pool | 256 MiB | 1 | 1 |
| Texture bindless table | n/a | n/a | 80 000 (`Texture_SRV(s=2)`) |

`MeshHandle` resolves to `(pageIndex, vertexByteOffset, indexByteOffset, triCount,
 vertexCount)`. Closesthit shaders index the vertex/index pages by `pageIndex` via the
bindless raw-buffer table, then offset by the byte ranges — the standard NVRHI
bindless pattern,
extended to multiple pages.
- Vertex layout: position (R32G32B32_FLOAT), packed normal (R10G10B10A2_SNORM, with
  the SNORM `w` interpreted as `>= 0 ? +1 : -1` for handedness), packed tangent
  (R10G10B10A2_SNORM, same handedness convention in `w`), UV0 (R16G16_FLOAT). Decoder
  in shader: `normal_xyz = packed.xyz; handedness_w = packed.w >= 0 ? 1.0 : -1.0;`.
  The two SNORM-low bits of `w` available beyond ±1 are wasted by design; the codec is
  picked for symmetry with NVIDIA's `Falcor` / `Donut` conventions and matches what
  RTXMU expects. Extra primvars optional.
- Degenerate triangles dropped during topology conversion; counted in stats.
- Static-geometry assumption (v1): neither ingest adapter re-uploads positions
  after first sync. The public API does expose `GpuScene::UpdateMesh` (§18.5);
  internally it is implemented as `DestroyMesh` + `CreateMesh` in v1 (the `MeshHandle`
  stays valid; the BLAS for that mesh is rebuilt). It exists for tests and future
  animation work, not because v1 scenes exercise it. Material parameters, by contrast,
  *can* be changed in-place via `UpdateMaterial` (§18.5) without disturbing the
  BLAS / TLAS.
- Skipping for v1: subdivision (rendered as polymesh hulls in v1 — the actual
  `pxOsd::Tokens->none` token is set inside the Hydra adapter, see §25.B);
  curves/points/volumes/nurbs likewise deferred. **Subdivision skipping is acceptable**
  for first Bistro visuals.

---

## 15. Instancing Strategy


- Honor `HdInstancer` (native Hydra instancing), including nested instancing.
- For each Rprim: walk `instancerId` chain, accumulate `instanceTransforms`,
  flatten into the `InstanceTable`. Cache the flattened transform array per dirty cycle.
- One BLAS per **prototype mesh**; many TLAS instances reference it — the canonical instanced-foliage / scattered-prop case Bistro and any production-shaped scene rely on.
- BLAS sharing rule: BLAS keyed on `MeshHandle`. If the same SdfPath mesh is consumed by N
  instancers, all share one BLAS. Unique BLAS only when the underlying mesh data differs
  (different topology hash).
- Instance ID: assigned monotonically; written into `gl_InstanceCustomIndexEXT` for picking
  and the `instanceId` AOV.
- Instance visibility: combined with prim visibility; invisible instances simply omitted
  from TLAS to save memory.
- Avoid duplicating mesh data: never call `createOrUpdateMesh` twice for the same prototype;
  the SdfPath→MeshHandle map enforces this.

---

## 16. BLAS / TLAS Build Strategy


**Memory + lifecycle delegated to RTXMU.** Pyxis links NVRHI with
`NVRHI_WITH_RTXMU=ON`, which routes every BLAS through NVIDIA's
[RTXMU](https://github.com/NVIDIAGameWorks/RTXMU) (RTX Memory Utility,
vendored as a submodule inside NVRHI). The public NVRHI API
(`createAccelStruct`, `buildBottomLevelAccelStruct`,
`buildTopLevelAccelStruct`) is unchanged — RTXMU sits behind it and
handles three things we'd otherwise hand-roll:

1. **BLAS suballocation pool.** RTXMU packs many BLAS into a small
   number of large `VkBuffer`s rather than one buffer per BLAS. Cuts
   the descriptor-set + allocation churn on production-scale scenes
   dramatically (Bistro = ~10³ unique BLAS; full production-class scenes can hit ~10⁴).
2. **Scratch pool.** RTXMU sizes + reuses a single growable scratch
   buffer; we never allocate one ourselves. `vkCmdBuildAccelerationStructuresKHR`
   reads from the pool's slot picked by RTXMU.
3. **Asynchronous compaction.** When a BLAS built with
   `ALLOW_COMPACTION` finishes on the GPU (NVRHI feeds RTXMU the
   build IDs at queue-submit time), RTXMU enqueues the compaction
   copy + post-build-info query automatically. The old uncompacted
   memory is reclaimed once the copy retires. **No
   query-size-then-copy code lives on the Pyxis side.**

What stays in Pyxis-side code: the **build-flag policy** below. RTXMU
honors whatever flags we pass to NVRHI; it doesn't second-guess them.

- BLAS build-flag policy is **split by mesh size**:
  - **Small / mid meshes (`triCount < 64 k`)** — `PREFER_FAST_TRACE` only, no
    `ALLOW_UPDATE`, no `ALLOW_COMPACTION`. v1 has no skinning, no animation
    (§42); paying the ~10–15 % traversal cost of `ALLOW_UPDATE` for a feature
    that doesn't exist would be pure waste. Skipping compaction is cheap because
    the population of small meshes is small. When animation lands post-v1, this
    bucket flips to `PREFER_FAST_TRACE | ALLOW_UPDATE`.
  - **Heavy meshes (`triCount >= 64 k`)** — `PREFER_FAST_TRACE | ALLOW_COMPACTION`,
    no `ALLOW_UPDATE`. Compaction routinely shaves 25–55 % off BLAS memory on
    dense foliage / vegetation. The Vulkan spec permits both bits at build time, but a
    compacted AS may not be updated afterwards (`vkCmdCopyAccelerationStructureKHR`
    with `MODE_COMPACT_KHR` discards the update-source data) — since v1 ships no
    in-place position updates anyway, omitting `ALLOW_UPDATE` simplifies
    bookkeeping.
  - The 64 k threshold is configurable via `geometry.blasCompactionTriThreshold`
    (default 65 536); profile-driven changes are allowed (see §34's "measure first"
    rule).
  - Boolean toggle `geometry.compactBLAS` (§27) is a *master switch*: when `false`,
    `ALLOW_COMPACTION` is stripped from the build flags before the NVRHI call
    (debug mode — RTXMU only compacts ASes whose build flags allowed it). The
    `ALLOW_UPDATE` flag is not user-tunable in v1 — the size-split policy fully
    determines it (never set in v1, since neither bucket needs it).
- TLAS:
  - Rebuilt every frame in v1 if any instance dirty; refit (`MODE_UPDATE`) otherwise.
    The TLAS itself is built with `ALLOW_UPDATE` (TLAS update is cheap and routinely
    used; this is independent of the BLAS policy above).
  - **TLAS is not RTXMU-managed.** RTXMU's pool only suballocates
    BLAS; the TLAS allocation goes through NVRHI's standard path
    against a single dedicated `VkBuffer` sized to peak instance
    count.
  - Optional: split TLAS into (static, dynamic) once dynamic exists; not in v1.
- Build queues: BLAS + TLAS builds submitted on the graphics queue
  (NVRHI doesn't expose async build cleanly v1; RTXMU doesn't change
  this — it's a memory-management library, not a queue-management
  one). Profiled with GPU timestamps.

**Limitation acknowledged: no Opacity Micromaps in v1.** RTXMU 0.30+
explicitly does not support OMMs (`Feature::OpacityMicroMaps` is
gated off when `NVRHI_WITH_RTXMU=ON`). Pyxis doesn't ship OMMs in
v1 — alpha-tested foliage at production scale is a post-v1 polish item
(§42 "displacement / alpha tessellation"). If/when OMMs land, the
choice is to either drop RTXMU and hand-roll BLAS memory, or wait
for upstream RTXMU OMM support.

### 16.5 TLAS partitioning policy (post-v1 production-scale headroom)

`VkAccelerationStructureInstanceKHR` ships a 24-bit `instanceCustomIndex` — a hard
16 777 215 cap on instances inside one TLAS. Bistro at v1 sits comfortably under this cap
(~10⁴ instances), so the sharding path described below is **dormant in v1** — `K=1`,
single static + single dynamic TLAS. The architecture is preserved as headroom for
post-v1 production-class scenes whose flattened instance count exceeds the cap (e.g.
Moana-class assets at ~28 M instances after instancer flattening of foliage and beach
detail). Strategy:

1. **Two-tier TLAS**: one *static* TLAS holding all `Static` instances (built once,
   never refit) and one *dynamic* TLAS holding `Dynamic` instances (rebuilt each
   frame). The closesthit shader unifies them via two `RayQuery` invocations
   selecting the closer hit. This bumps the per-bounce traversal cost by ~2 % on
   RTX 4080, well within §2 KPIs.
2. **Static-TLAS sharding by `(SdfPath` hash mod K`)** when instance count > 16 M.
   Each shard is its own `TLAS_k`, all bound bindlessly; the closesthit performs K
   `RayQuery::Proceed` calls and keeps the closest hit. K is chosen so each shard
   holds < 12 M instances (headroom). Bistro (M8a–M10) uses K=1; post-v1 production-scale scenes
   would use K=2 or K=3.
3. **Cull-then-flatten**: instancer flattening is performed *after* a coarse
   per-instancer-region visibility cull driven by `parameters.json.hydra.purpose`
   and a configurable `geometry.maxInstancesPerTlas` cap. Anything beyond the cap
   is dropped with `ErrorKind::TlasInstanceLimitExceeded` logged once and
   `FrameStats::degraded = true`.
4. **Failure mode**: if a TLAS build returns `TlasInstanceLimitExceeded` or
   `AccelStructBuildFailed`, or if dependent BLAS allocation hits
   `BlasBudgetExceeded`, the renderer surfaces `Expected<void>` from
   `CommitResources` and the application falls back to whatever TLAS was previously
   valid (the swap is double-buffered).

---

## 17. Memory Management for Large Production Scenes


- A `BudgetTracker` aggregates: vertex/index, textures, BLAS, TLAS, scratch, staging,
  AOVs, render targets, **nested-instancer flatten cache** (per-`(SdfPath, time)` keyed,
  invalidated only on `Dirty<Instancer>`; budget cap **2.5 GiB** v1 — Bistro's flatten is
  well under 100 MiB, and the cap is sized as headroom for post-v1 production-class scenes
  (a full Moana-class flatten is ~2.2 GiB so the cap leaves slack for the dirty-replace path).
  Reported in spdlog and ImGui. **BLAS + scratch counters are sourced from
  RTXMU's pool stats** (`rtxmu::VkAccelStructManager::GetStats()` exposed
  through NVRHI's `getDeviceMemoryStats` hook) rather than computed by
  Pyxis — RTXMU owns those allocations now (§16).
- Hard caps configurable in `parameters.json` (texture max resolution, etc.).
- Staging ring buffer (e.g., 256 MB) reused frame-to-frame for uploads; oversize uploads
  fall back to one-shot allocations. **Outstanding one-shot bytes are capped** at
  `4 × stagingRing` (default 1 GiB); once the cap is reached the ingest thread blocks
  on a CV until the render thread retires enough one-shots through `DeletionQueue`.
  Without this cap, fifty 800 MB texture decodes in flight would balloon staging to
  40 GB before any frame retires.
- Deletion queue with N-frames-in-flight guard.
- Soft fallback: if budget breached during loading, log a warning, mark scene
  `degraded=true`, continue.
- Peak captures: peak-during-load, peak-during-first-frame, peak-steady-state.

---

# Part III — Public Surface & Contracts

## 18. Public API Surface (Renderer Core) — Canonical Reference


This section is the **single source of truth** for the `pyxis_renderer` public API. Any
type, header or method not listed here is `Private/` and inaccessible from `pyxis_hydra`,
`pyxis_usd_ingest`, or `pyxis_app`. The narrowness is enforced architecturally by CMake
(`target_include_directories(pyxis_renderer PUBLIC Public/ PRIVATE Private/)`) and
normatively by §30.3.

### 18.1 Header inventory (exhaustive)

| Header | Kind | Contents |
|---|---|---|
| `Public/Pyxis/Renderer/RendererApi.h` | macro | `PYXIS_RENDERER_API` (dllexport/dllimport) |
| `Public/Pyxis/Renderer/Forward.h` | fwd | handles + class fwd decls (§18.2) |
| `Public/Pyxis/Renderer/Error.h` | type | `Error`, `ErrorKind`, `Expected<T>` alias (§18.3) |
| `Public/Pyxis/Renderer/GpuScene.h` | class | scene mutation API (§18.5) |
| `Public/Pyxis/Renderer/PyxisRenderer.h` | class | frame rendering API (§18.6) |
| `Public/Pyxis/Renderer/Profiler.h` | class | profiler facade, opaque (§18.7) |
| `Public/Pyxis/Renderer/Descs/MeshDesc.h` | POD | §18.4 |
| `Public/Pyxis/Renderer/Descs/OpenPBRMaterialDesc.h` | POD | §11, §18.4 |
| `Public/Pyxis/Renderer/Descs/TextureKey.h` | POD | §18.4 |
| `Public/Pyxis/Renderer/Descs/InstanceDesc.h` | POD | §18.4 |
| `Public/Pyxis/Renderer/Descs/CameraDesc.h` | POD | §18.4 |
| `Public/Pyxis/Renderer/Descs/LightDesc.h` | POD (tagged variant) | §18.4 |
| `Public/Pyxis/Renderer/Descs/RenderSettings.h` | POD | §21, §18.4 |
| `Public/Pyxis/Renderer/Descs/RenderTargets.h` | POD | AOV bindings, §18.4 |
| `Public/Pyxis/Renderer/Descs/FrameStats.h` | POD | counters returned by `LastFrameStats` |
| `Public/Pyxis/Renderer/Descs/FrameProfile.h` | POD snapshot | timings returned by `LastFrameProfile` |
| `Public/Pyxis/Renderer/Descs/GpuSceneCreateDesc.h` | POD | constructor params for `GpuScene` |
| `Public/Pyxis/Renderer/Descs/RendererCreateDesc.h` | POD | constructor params for `PyxisRenderer` |

Explicitly **not public**: `MeshTable`, `MaterialTable`, `TextureTable`, `BlasCache`,
`TlasBuilder`, `UploadQueue`, `DeletionQueue`, `OpenPBRMaterialGPU` (the GPU layout —
distinct from the public `OpenPBRMaterialDesc` CPU descriptor), `RenderGraph`,
`IRenderPass`, every concrete pass class, `ShaderLibrary`, `DescriptorTableManager`,
`ProfilerData`, `GpuTimestampPool`, `SceneWorld`, all Flecs components and systems.

### 18.2 Forward declarations and handles

```cpp
// Public/Pyxis/Renderer/Forward.h
#pragma once
#include <cstdint>

namespace nvrhi { class IDevice; class ICommandList; }

namespace pyxis {

// Strong handles (§30.4). Opaque to consumers; the renderer maps these to
// `flecs::entity` via an internal HandleBimap (§8.2).
enum class MeshHandle     : uint32_t { Invalid = 0 };
enum class MaterialHandle : uint32_t { Invalid = 0 };
enum class TextureHandle  : uint32_t { Invalid = 0 };
enum class InstanceHandle : uint32_t { Invalid = 0 };
enum class LightHandle    : uint32_t { Invalid = 0 };

// POD descriptors (full definitions in Descs/*.h, summarised in §18.4).
struct MeshDesc;
struct OpenPBRMaterialDesc;
struct TextureKey;
struct InstanceDesc;
struct CameraDesc;
struct LightDesc;
struct RenderSettings;
struct RenderTargets;
struct FrameStats;
struct FrameProfile;
struct GpuSceneCreateDesc;
struct RendererCreateDesc;

// Error (§18.3, §20).
enum class ErrorKind : uint16_t;
struct Error;
template <class T> using Expected = std::expected<T, Error>;

// Public classes.
class GpuScene;
class PyxisRenderer;
class Profiler;

} // namespace pyxis
```

### 18.3 Error type

```cpp
// Public/Pyxis/Renderer/Error.h
#pragma once
#include <expected>
#include <array>
#include <string_view>
#include <cstdint>

namespace pyxis {
enum class ErrorKind : uint16_t { /* see §20 */ };

// ABI-safe inline-owning string. Crosses DLL boundaries safely; never
// allocates; truncates with a trailing ellipsis past the buffer size.
// Built by PYXIS_ERROR(...) at the failure site.
struct ErrorMessage {
  static constexpr size_t CAPACITY = 240;
  std::array<char, CAPACITY> data{};
  uint16_t                    size = 0;   // not including null terminator
  [[nodiscard]] std::string_view View() const noexcept { return { data.data(), size }; }
};

struct Error {
  ErrorKind    kind;
  ErrorMessage message;   // human-readable, sdfPath included where relevant
  ErrorMessage source;    // "file:line" via PYXIS_ERROR(...)
};
template <class T> using Expected = std::expected<T, Error>;
} // namespace pyxis
```

### 18.4 Public POD descriptors (full definitions)

All public PODs are 16-byte-aligned where they map to GPU layouts, otherwise plain
aggregates. They contain only `hlslpp` math types, primitive types, strong handles,
and `std::span` / `std::string_view` (for input-only references).

```cpp
// Public/Pyxis/Renderer/Descs/MeshDesc.h
struct MeshDesc {
  std::span<const hlslpp::float3> positions;       // required
  std::span<const uint32_t>       indices;         // required, triangle list
  std::span<const hlslpp::float3> normals;         // optional; empty → generated
  std::span<const hlslpp::float4> tangents;        // optional; empty → MikkTSpace if normalMap present
  std::span<const hlslpp::float2> uv0;             // optional
  std::string_view                debugName;       // for markers, profile reports
};

// Public/Pyxis/Renderer/Descs/TextureKey.h
struct TextureKey {
  enum class Role  : uint8_t { BaseColor, NormalMap, RoughnessMetallic,
                               Emission, Opacity, EnvLatLong };
  enum class Color : uint8_t { sRGB, Linear };
  std::string_view resolvedPath; // ArResolver-resolved absolute path; borrowed (input-only).
                                 // The renderer copies what it needs into its internal cache key.
  Role             role        = Role::BaseColor;
  Color            colorspace  = Color::sRGB;
};

// Public/Pyxis/Renderer/Descs/OpenPBRMaterialDesc.h — see §11 for full layout.
// Public-API summary:
struct OpenPBRMaterialDesc {
  hlslpp::float3 baseColor    = {0.8f, 0.8f, 0.8f};
  float          baseWeight   = 1.0f;
  float          metalness    = 0.0f;
  float          roughness    = 0.5f;
  // ... specular / transmission / coat / emission / geometry blocks (see §11)
  TextureHandle  baseColorMap = TextureHandle::Invalid;
  TextureHandle  metallicMap, roughnessMap, normalMap, emissionMap,
                 opacityMap, transmissionMap, coatRoughnessMap;
  enum class Source : uint8_t { UsdPreviewSurface, MaterialX, RenderManFallback, Default };
  Source         source       = Source::Default;
  // Diagnostics-only SdfPath. Borrowed at the call site; the renderer copies the
  // first 63 bytes into an internal POD before queueing the mutation across
  // threads (see §18.5 / §31) so the view never outlives the caller's stack.
  // Not hashed.
  std::string_view sourcePrim;
};

// Public/Pyxis/Renderer/Descs/InstanceDesc.h
struct InstanceDesc {
  MeshHandle        mesh        = MeshHandle::Invalid;
  MaterialHandle    material    = MaterialHandle::Invalid;
  hlslpp::float4x4  worldFromLocal{};   // row-major (§10)
  bool              visible     = true;
  std::string_view  debugName;
};

// Public/Pyxis/Renderer/Descs/CameraDesc.h
struct CameraDesc {
  hlslpp::float4x4  viewFromWorld{};
  hlslpp::float4x4  projFromView{};
  float             focalLengthMm    = 35.0f;
  float             apertureFStop    = 0.0f;       // 0 = pinhole
  float             focusDistance    = 1.0f;
  float             nearClip         = 0.01f;
  float             farClip          = 10000.0f;
};

// Public/Pyxis/Renderer/Descs/LightDesc.h
struct LightDesc {
  enum class Kind : uint8_t { Distant, Dome, Rect };
  Kind             kind  = Kind::Distant;
  hlslpp::float3   color = {1, 1, 1};
  float            intensity = 1.0f;
  // Distant
  hlslpp::float3   direction = {0, -1, 0};
  // Dome
  TextureHandle    envMap    = TextureHandle::Invalid;
  // Rect
  hlslpp::float3   position  = {0, 0, 0};
  hlslpp::float3   axisU     = {1, 0, 0};
  hlslpp::float3   axisV     = {0, 1, 0};
  bool             doubleSided = false;
};

// Public/Pyxis/Renderer/Descs/RenderSettings.h — see §21.
struct RenderSettings {
  uint32_t  width            = 1920;
  uint32_t  height           = 1080;
  uint32_t  samplesPerFrame  = 1;
  uint32_t  maxBounces       = 6;
  // RNG seed (§12). `seed = 0` is a sentinel: in viewer mode it derives a per-frame
  // seed from the frame counter (intentionally non-deterministic so accumulation
  // doesn't lock onto an artefact); in headless mode the loader rejects `seed = 0`
  // and forces the caller to pick an explicit value, so EXR output is byte-identical
  // across runs (§33.7).
  uint32_t  seed             = 0;
  bool      enableAccumulation = true;
  bool      enableToneMapping  = true;
  float     exposure         = 0.0f;
  // Quality knobs (§21.2) — path-tracer hygiene.
  uint32_t  accumulationFrameLimit  = 0;     // 0 = unbounded; headless writes EXR when reached
  uint32_t  russianRouletteStartBounce = 3;  // RR kicks in at this depth
  float     fireflyClampLuminance = 50.0f;   // clamp returned radiance per bounce; 0 = disabled
  bool      lowDiscrepancySampling = false;  // true = Sobol+Owen; false = PCG32 (§12)
  enum class ToneMap   : uint8_t { Linear, Aces, Filmic };
  enum class DebugView : uint8_t { Color, Albedo, Normal, Depth,
                                   InstanceId, MaterialId };
  ToneMap   toneMap   = ToneMap::Aces;
  DebugView debugView = DebugView::Color;
  uint32_t  enabledAovs = 0x7F;        // bitmask of AovFlag values (§18.4 RenderTargets)
};

// Public/Pyxis/Renderer/Descs/RenderTargets.h
enum class AovFlag : uint32_t {
  None         = 0,
  Color        = 1u << 0,
  Depth        = 1u << 1,
  Normal       = 1u << 2,
  Albedo       = 1u << 3,
  MotionVector = 1u << 4,
  MaterialId   = 1u << 5,
  InstanceId   = 1u << 6,
};

struct RenderTargets {
  // NVRHI texture refs supplied by the caller (Hydra Bprims, swapchain target,
  // or headless writer). Renderer never allocates these.
  nvrhi::ITexture* color        = nullptr;  // RGBA16F, required
  nvrhi::ITexture* depth        = nullptr;  // R32F,    optional
  nvrhi::ITexture* normal       = nullptr;  // RGB16F,  optional
  nvrhi::ITexture* albedo       = nullptr;  // RGBA16F, optional
  nvrhi::ITexture* motionVector = nullptr;  // RG16F,   optional
  nvrhi::ITexture* materialId   = nullptr;  // R32_UINT, optional
  nvrhi::ITexture* instanceId   = nullptr;  // R32_UINT, optional
};

// Public/Pyxis/Renderer/Descs/FrameStats.h
struct FrameStats {
  uint64_t meshCount, materialCount, textureCount, instanceCount, lightCount;
  uint64_t blasCount, blasBytes, tlasBytes;
  uint64_t vertexBytes, indexBytes, textureBytes;
  uint64_t pendingUploads, pendingBlasBuilds;
  uint64_t staleHandleDrops; // Destroy*/Update* on a recycled or Invalid handle (§18.5)
  bool     degraded;       // true if any soft-fallback fired this frame
};

// Public/Pyxis/Renderer/Descs/FrameProfile.h
struct FrameProfile {
  // Opaque-by-design snapshot. Backends (spdlog/JSON/CSV/ImGui) read fields
  // through accessor methods; binary layout may evolve.
  enum class ScopeKind : uint8_t { Cpu, Gpu };

  // Inline owning name buffer. Same ABI rationale as ErrorMessage (§18.3, §18.9):
  // crosses DLL boundaries safely and keeps the snapshot self-contained, so a
  // FrameProfile returned by LastFrameProfile() stays valid indefinitely — it does
  // not point into Profiler-owned storage that rotates on the next EndFrame.
  struct ScopeName {
    static constexpr size_t CAPACITY = 56;
    std::array<char, CAPACITY> data{};
    uint8_t                     size = 0;
    [[nodiscard]] std::string_view View() const noexcept { return { data.data(), size }; }
  };
  struct PassTiming {
    ScopeName name;          // e.g. "pass.PathTrace", "render.commitResources"
    ScopeKind kind;          // Cpu or Gpu
    double    durationMs;    // CPU wall time or GPU timestamp delta
    uint32_t  depth;         // nesting depth for hierarchical display
  };
  std::span<const PassTiming> passes;     // engine passes + user CpuScope/GpuScope, in submission order
  double   cpuFrameMs;                    // BeginFrame → EndFrame wall time
  double   gpuFrameMs;                    // first-to-last GPU timestamp on the frame's command lists
  uint64_t frameIndex;
};

// Public/Pyxis/Renderer/Descs/GpuSceneCreateDesc.h
struct GpuSceneCreateDesc {
  uint32_t bindlessCapacity   = 80'000;
  uint32_t stagingMib         = 256;
  uint32_t framesInFlight     = 2;
  bool     compactBlas        = true;
};

// Public/Pyxis/Renderer/Descs/RendererCreateDesc.h
struct RendererCreateDesc {
  uint32_t initialWidth  = 1920;
  uint32_t initialHeight = 1080;
  std::string_view shaderSearchPath;   // for hot-reload (viewer only)
};
```

### 18.5 `GpuScene` — scene mutation API

The verbs every ingest adapter (`pyxis_hydra` and `pyxis_usd_ingest` from day 0,
anything added later) drives a frame with.

Threading: `producer-consumer` — mutation calls may be issued on the ingest thread; only
`CommitResources` touches the GPU and must run on the render thread.
Internally the `Impl` enqueues every mutation onto a multi-producer / single-consumer
lock-free queue (`moodycamel::ConcurrentQueue`, vendored via vcpkg) drained at the
start of `CommitResources`. Multiple ingest threads (Hydra `Sync`, USD-direct
`StageWalker`, asset-I/O completions) may produce concurrently; the render thread is
the sole consumer. Plain `std::vector::push_back` from any non-render thread is
forbidden — reviewers reject any direct container mutation from a non-render thread.

```cpp
// Public/Pyxis/Renderer/GpuScene.h
#pragma once
#include <Pyxis/Renderer/RendererApi.h>
#include <Pyxis/Renderer/Forward.h>
#include <Pyxis/Renderer/Error.h>
#include <Pyxis/Renderer/Descs/GpuSceneCreateDesc.h>
#include <hlsl++.h>

namespace pyxis {

class PYXIS_RENDERER_API GpuScene final {
public:
  GpuScene(nvrhi::IDevice* device, Profiler& profiler, const GpuSceneCreateDesc&);
  ~GpuScene();
  GpuScene(const GpuScene&) = delete;
  GpuScene& operator=(const GpuScene&) = delete;
  GpuScene(GpuScene&&) noexcept;
  GpuScene& operator=(GpuScene&&) noexcept;

  // ── Mesh ───────────────────────────────────────────────────────────────
  [[nodiscard]] Expected<MeshHandle> CreateMesh(const MeshDesc& meshDesc);
  // In-place mesh replacement. v1: implemented internally as DestroyMesh+CreateMesh;
  // the BLAS for `meshHandle` is rebuilt and any TLAS instance referencing it is
  // re-linked. The handle stays valid across the call so InstanceHandles remain stable.
  // Future: zero-copy vertex/index update where topology is unchanged.
  [[nodiscard]] Expected<void>       UpdateMesh(MeshHandle meshHandle, const MeshDesc& meshDesc);
  void                               DestroyMesh(MeshHandle meshHandle);
  // Liveness probe — useful for ingest adapters validating their SdfPath→Handle map
  // after USD edits, and for tests asserting cleanup. Not required to drive a frame.
  [[nodiscard]] bool                 HasMesh(MeshHandle meshHandle) const;

  // ── Material ───────────────────────────────────────────────────────────
  // Lazy acquirer: never fails at the call site. Per-material conversion errors
  // surface during the next CommitResources via FrameStats::degraded + a one-shot
  // spdlog entry; the offending material falls back to the default gray material.
  [[nodiscard]] MaterialHandle       AcquireMaterial(const OpenPBRMaterialDesc& materialDesc); // dedupes by hash
  void                               UpdateMaterial(MaterialHandle materialHandle, const OpenPBRMaterialDesc& materialDesc);
  void                               DestroyMaterial(MaterialHandle materialHandle);
  [[nodiscard]] bool                 HasMaterial(MaterialHandle materialHandle) const;

  // ── Texture ────────────────────────────────────────────────────────────
  // Lazy acquirer: never fails at the call site. Texture decode + upload happens
  // asynchronously on the I/O pool; decode failures surface via FrameStats::degraded
  // + a one-shot spdlog entry, and the offending texture is replaced by the
  // missing-texture color.
  [[nodiscard]] TextureHandle        AcquireTexture(const TextureKey& textureKey); // dedupes; lazy decode
  void                               DestroyTexture(TextureHandle textureHandle);
  [[nodiscard]] bool                 HasTexture(TextureHandle textureHandle) const;

  // ── Instance ───────────────────────────────────────────────────────────
  [[nodiscard]] Expected<InstanceHandle> AppendInstance(const InstanceDesc& instanceDesc);
  void                               UpdateInstanceTransform(InstanceHandle instanceHandle, const hlslpp::float4x4& worldFromLocal);
  void                               UpdateInstanceMaterial (InstanceHandle instanceHandle, MaterialHandle materialHandle);
  void                               SetInstanceVisibility  (InstanceHandle instanceHandle, bool visible);
  void                               DestroyInstance        (InstanceHandle instanceHandle);
  [[nodiscard]] bool                 HasInstance(InstanceHandle instanceHandle) const;

  // ── Camera & lights ────────────────────────────────────────────────────
  void                               SetCamera (const CameraDesc& cameraDesc);
  [[nodiscard]] LightHandle          AddLight  (const LightDesc& lightDesc);
  void                               UpdateLight(LightHandle lightHandle, const LightDesc& lightDesc);
  void                               RemoveLight(LightHandle lightHandle);

  // ── Frame boundary ─────────────────────────────────────────────────────
  // Drains uploads, builds dirty BLAS, rebuilds/refits TLAS. See §16, §33.4.
  // Threading: render thread only.
  // May fail (e.g. BlasBudgetExceeded, TlasInstanceLimitExceeded, OutOfMemoryGpu).
  [[nodiscard]] Expected<void>           CommitResources(nvrhi::ICommandList* commandList);

  // ── Stale-handle policy ────────────────────────────────────────────────
  // Destroy* and Update* verbs above return `void` by design: stale handles
  // (handle whose generation has already been recycled, or InstanceHandle::Invalid)
  // are silently ignored and counted in `FrameStats::staleHandleDrops` (§18.4).
  // Callers that need a hard guarantee probe with `HasMesh` / `HasMaterial` /
  // `HasTexture` / `HasInstance` first.

  // ── Introspection ──────────────────────────────────────────────────────
  [[nodiscard]] FrameStats           LastFrameStats() const;

private:
  struct Impl;
  std::unique_ptr<Impl> _impl;
};

} // namespace pyxis
```

### 18.6 `PyxisRenderer` — frame rendering API

```cpp
// Public/Pyxis/Renderer/PyxisRenderer.h
#pragma once
#include <Pyxis/Renderer/RendererApi.h>
#include <Pyxis/Renderer/Forward.h>
#include <Pyxis/Renderer/Descs/RenderSettings.h>
#include <Pyxis/Renderer/Descs/RenderTargets.h>
#include <Pyxis/Renderer/Descs/RendererCreateDesc.h>
#include <Pyxis/Renderer/Descs/FrameProfile.h>

namespace pyxis {

class PYXIS_RENDERER_API PyxisRenderer final {
public:
  PyxisRenderer(nvrhi::IDevice* device,
                GpuScene&       scene,
                Profiler&       profiler,
                const RendererCreateDesc&);
  ~PyxisRenderer();
  PyxisRenderer(const PyxisRenderer&) = delete;
  PyxisRenderer& operator=(const PyxisRenderer&) = delete;

  // Renders one frame. Caller's responsibility: GpuScene::CommitResources
  // has been called for this frame already. Threading: render thread only.
  //
  // motionVector AOV: when `RenderTargets::motionVector` is bound, the renderer
  // caches the prev-frame `(viewFromWorld, projFromView)` matrices internally
  // at the top of every `RenderFrame` and computes per-pixel screen-space motion
  // as `screenSpace(prevClipFromWorld * worldHitPos) -
  //     screenSpace(currClipFromWorld * worldHitPos)` in pixel units (the world
  // hit position is constant across the two projections; the camera moved).
  // The first frame after construction, after `Resize`, or after `ResetAccumulation`
  // produces all-zero vectors (no prev-frame matrices yet). Animation §42.
  void                            RenderFrame(nvrhi::ICommandList*  commandList,
                                              const RenderSettings& renderSettings,
                                              const RenderTargets&  renderTargets);

  // Resize internal AOV / accumulation buffers. Resets accumulation. May fail if the
  // requested size exceeds the GPU memory budget or the device's max texture extent.
  [[nodiscard]] Expected<void>    Resize(uint32_t width, uint32_t height);

  // Resets accumulation without resizing (camera/material change, etc.).
  void                            ResetAccumulation();

  [[nodiscard]] FrameProfile      LastFrameProfile() const;

private:
  struct Impl;
  std::unique_ptr<Impl> _impl;
};

} // namespace pyxis
```

### 18.7 `Profiler` — opaque facade

```cpp
// Public/Pyxis/Renderer/Profiler.h
#pragma once
#include <Pyxis/Renderer/RendererApi.h>
#include <Pyxis/Renderer/Descs/FrameProfile.h>
#include <string_view>

namespace nvrhi { class IDevice; class ICommandList; }

namespace pyxis {

class PYXIS_RENDERER_API Profiler final {
public:
  // Construction: GPU timing is enabled iff a non-null device is supplied. A
  // CPU-only profiler (`Profiler{nullptr}`) is valid for unit tests that don't
  // touch the GPU. Default arguments are forbidden on the public API (§30.5),
  // so callers must pass `nullptr` explicitly.
  explicit Profiler(nvrhi::IDevice* device);
  ~Profiler();
  Profiler(const Profiler&) = delete;
  Profiler& operator=(const Profiler&) = delete;

  // RAII CPU scope. Names follow the dotted lower-case convention (§34).
  class PYXIS_RENDERER_API CpuScope final {
  public:
    explicit CpuScope(Profiler& profiler, std::string_view name);
    ~CpuScope();
    CpuScope(const CpuScope&) = delete;
    CpuScope& operator=(const CpuScope&) = delete;
  };

  // RAII GPU scope. Brackets a region on the supplied command list with
  // begin/end timestamp queries and an NVRHI debug marker. The renderer
  // uses the same primitive internally for every render-graph pass; the
  // public type lets ingest/app code add their own GPU regions
  // (e.g. "hydra.sync", "app.imguiOverlay") that show up alongside
  // engine passes in `FrameProfile::passes`.
  class PYXIS_RENDERER_API GpuScope final {
  public:
    GpuScope(Profiler& profiler, nvrhi::ICommandList* commandList, std::string_view name);
    ~GpuScope();
    GpuScope(const GpuScope&) = delete;
    GpuScope& operator=(const GpuScope&) = delete;
  };

  // Frame boundary — called by the Application once per frame.
  // BeginFrame: rotates the GPU timestamp pool, opens the frame's CPU root.
  // EndFrame  : resolves the previous frame's timestamps (latency = framesInFlight)
  //             and snapshots them into the FrameProfile returned by LastFrameProfile.
  void BeginFrame();
  void EndFrame();

  // Read-only snapshot of the most recently resolved frame.
  // Includes both CPU scopes and GPU scopes (engine passes + user-issued GpuScopes).
  [[nodiscard]] FrameProfile LastFrameProfile() const;

  // Output sinks (spdlog / JSON / CSV / ImGui) are registered by the Application;
  // the renderer only produces data, it never writes files or draws ImGui itself.
  // Concrete sink registration API is kept Private/ until M11.

private:
  struct Impl;
  std::unique_ptr<Impl> _impl;
};

} // namespace pyxis
```

### 18.8 Closure check (every ingest verb has a home)

Every operation an ingest adapter (Hydra, USD-direct) needs to drive a frame is a method
on `GpuScene` or `PyxisRenderer` — *no* `Private/` type is required:

| Ingest need | Public method |
|---|---|
| Create a mesh prototype | `GpuScene::CreateMesh` |
| Replace mesh data (in-place handle-stable) | `GpuScene::UpdateMesh` (v1 internally = DestroyMesh+CreateMesh; handle stays valid) |
| Acquire / dedupe a material | `GpuScene::AcquireMaterial` |
| Tweak material parameters in-place | `GpuScene::UpdateMaterial` |
| Acquire a texture (lazy decode) | `GpuScene::AcquireTexture` |
| Spawn an instance | `GpuScene::AppendInstance` |
| Move an instance | `GpuScene::UpdateInstanceTransform` |
| Rebind an instance's material | `GpuScene::UpdateInstanceMaterial` |
| Hide / show an instance | `GpuScene::SetInstanceVisibility` |
| Remove an instance | `GpuScene::DestroyInstance` |
| Probe handle liveness (cleanup tests, USD→Handle map sanity) | `HasMesh` / `HasMaterial` / `HasTexture` / `HasInstance` |
| Set / move the camera | `GpuScene::SetCamera` |
| Add / update / remove a light | `GpuScene::AddLight` / `UpdateLight` / `RemoveLight` |
| Push pending GPU work for the frame | `GpuScene::CommitResources` |
| Render a frame | `PyxisRenderer::RenderFrame` |
| Resize output | `PyxisRenderer::Resize` |
| Reset accumulation on settings change | `PyxisRenderer::ResetAccumulation` |
| Bracket a CPU region with timing + Tracy zone | `Profiler::CpuScope` |
| Bracket a GPU region with timestamps + NVRHI marker | `Profiler::GpuScope` |
| Read frame stats / profile | `LastFrameStats` / `LastFrameProfile` |

The Hydra layer imports only `Public/Pyxis/Renderer/*.h` (and `Descs/*.h`). NVRHI appears
only as opaque interface pointers (`nvrhi::IDevice*`, `nvrhi::ICommandList*`); no USD or
NVRHI-private headers escape through the public surface.

### 18.9 ABI / stability rules

- The API is C++ ABI; consumers must link against the matching `pyxis_renderer.dll`
  built with the same compiler/runtime. Shipping multiple versions side-by-side is not
  supported in v1.
- **No `std::string` / `std::vector` / `std::map` / any other STL container appears in
  any public POD descriptor or public method signature.** STL containers cross-DLL are
  undefined behaviour the moment debug-iterator / SCL settings differ between
  consumer and `pyxis_renderer.dll`. Inputs are taken as `std::string_view` /
  `std::span<const T>` (borrowed). Outputs that need to own a string
  (`Error::message`, `Error::source`) use `pyxis::ErrorMessage` — a fixed-size,
  `std::array`-backed POD. This is the ABI-safety lock; reviewers reject any PR that
  adds an STL container to a public POD or method signature.
- **PIMPL is mandatory on every public class.** `GpuScene` and `Profiler` hold
  their `std::vector<Entry>` tables, per-frame ring slots, and NVRHI buffer /
  texture / acceleration-structure / timer-query handles behind a
  `struct Impl;` forward-declared in the header and `std::unique_ptr<Impl>`
  (or raw `Impl*` with explicit out-of-line dtor) as the only data member.
  The header forward-declares `nvrhi::IDevice` / `nvrhi::ICommandList` and
  any other NVRHI type that appears as an opaque pointer in method
  signatures; it never includes `<nvrhi/nvrhi.h>`, `<vector>`, `<array>`,
  `<string>`, or any other STL container header. This protects consumers
  (`pyxis_app.exe`, `pyxis_hydra.dll`, `pyxis_usd_ingest.dll`, future
  out-of-tree integrations) from `_HAS_EXCEPTIONS` / debug-iterator / SCL
  flag mismatches that would otherwise corrupt vector / unique_ptr
  layouts silently — the trade-off is one extra heap allocation per
  public-class instance, which is dwarfed by the per-frame GPU work.
  Render-side accessors that return a borrowed `nvrhi::*` pointer (e.g.
  `GpuScene::GetTlas()` returning `nvrhi::rt::IAccelStruct*`) are
  permitted because the consumer round-trips the pointer into another
  binding desc without dereferencing it; the full NVRHI header is
  included only by the .cpp that actually reads the type's members.
- Public POD descriptors are **frozen at the byte level**: `sizeof`, `alignof`, member
  offsets and padding are part of the contract. Adding a member — even at the end —
  changes `sizeof` and is therefore a major-version break unless the type carries an
  explicit trailing-`reserved` field (see §12).
- `std::span<const T>` and `std::string_view` are **allowed only in input-only
  parameters** (e.g. `MeshDesc::positions`, `OpenPBRMaterialDesc::sourcePrim`,
  `Profiler::CpuScope` ctor). The renderer copies whatever it needs into
  Private storage before the call returns; views never appear in return types,
  in fields persisted past a call, or in any public POD that is stored anywhere
  (`Error::message` and `FrameProfile::PassTiming::name` use the inline
  `ErrorMessage`/`ScopeName` PODs precisely because of this rule).
  **One narrow exception**: `FrameProfile::passes` is a `std::span<const
  PassTiming>` that points into Profiler-owned storage (the slot's
  resolved-pass list inside `Profiler::Impl`) guaranteed stable until
  the next `BeginFrame()`. Callers who want a longer-lived snapshot
  copy the span's contents into their own storage — the contract is
  documented on the type. The §18.9 review still rejects new
  spans-out-of-public-API by default; this is a single grandfathered
  case, not a new pattern.
- Public method signatures are append-only between minor versions; renames or removals
  are major-version breaks.
- Strong-handle enum underlying types are fixed at `uint32_t` and their `Invalid = 0`
  contract is permanent.

### 18.10 Worked example — how a consumer uses the API

The two public classes are constructed by the Application, then handed to whichever
ingest adapter is active. Below is the full lifecycle for a viewer-style consumer
(applies identically to a Hydra render-delegate or a USD-direct importer).

#### Setup (once)

```cpp
#include <Pyxis/Renderer/GpuScene.h>
#include <Pyxis/Renderer/PyxisRenderer.h>
#include <Pyxis/Renderer/Profiler.h>
#include <Pyxis/Renderer/Descs/MeshDesc.h>
#include <Pyxis/Renderer/Descs/InstanceDesc.h>
#include <Pyxis/Renderer/Descs/CameraDesc.h>
#include <Pyxis/Renderer/Descs/LightDesc.h>
#include <Pyxis/Renderer/Descs/RenderSettings.h>
#include <Pyxis/Renderer/Descs/RenderTargets.h>

using namespace pyxis;

nvrhi::IDevice* device = /* from pyxis_platform */;

// Pass `device` so the profiler can record GPU timestamps in addition to
// CPU scopes. Use the default-constructed (`nullptr`) form for headless
// CPU-only tests.
// `Profiler` has no default constructor (§30.5 forbids default args on public API);
// pass an explicit `nullptr` for unit-test-only / CPU-only profiling instead.
Profiler            profiler{ device };
GpuScene            scene   { device, profiler, GpuSceneCreateDesc{} };
PyxisRenderer renderer{ device, scene, profiler,
                              RendererCreateDesc{ .initialWidth = 1920,
                                                  .initialHeight = 1080 } };
```

#### Populating the scene (any thread, ingest-side)

```cpp
// 1. Mesh — a single triangle.
const hlslpp::float3 positions[] = {{0,0,0},{1,0,0},{0,1,0}};
const uint32_t       indices[]   = {0,1,2};

MeshDesc meshDesc {
  .positions = positions,
  .indices   = indices,
  .debugName = "triangle",
};
auto meshResult = scene.CreateMesh(meshDesc);
if (!meshResult) { /* log meshResult.error().message and bail */ }
MeshHandle meshHandle = *meshResult;

// 2. Material — OpenPBR red plastic. AcquireMaterial dedupes by hash.
OpenPBRMaterialDesc materialDesc {
  .baseColor = {0.8f, 0.1f, 0.1f},
  .roughness = 0.4f,
  .source    = OpenPBRMaterialDesc::Source::Default,
};
MaterialHandle materialHandle = scene.AcquireMaterial(materialDesc);

// 3. Instance — place the mesh at origin with the material bound.
InstanceDesc instanceDesc {
  .mesh           = meshHandle,
  .material       = materialHandle,
  .worldFromLocal = hlslpp::float4x4::identity(),
  .visible        = true,
  .debugName      = "triangle.0",
};
auto instanceResult = scene.AppendInstance(instanceDesc);
if (!instanceResult) { /* log instanceResult.error().message and bail */ }
InstanceHandle instanceHandle = *instanceResult;

// 4. Camera + a distant light.
CameraDesc cameraDesc { /* viewFromWorld, projFromView ... */ };
scene.SetCamera(cameraDesc);

LightDesc sunDesc {
  .kind      = LightDesc::Kind::Distant,
  .color     = {1.0f, 0.95f, 0.9f},
  .intensity = 5.0f,
  .direction = {-0.3f, -1.0f, -0.2f},
};
LightHandle sunHandle = scene.AddLight(sunDesc);
```

#### Per-frame loop (render thread)

```cpp
RenderTargets targets {
  .color = swapchainColor,         // nvrhi::ITexture* from the platform
  .depth = swapchainDepth,
};
RenderSettings settings {
  .width  = 1920, .height = 1080,
  .samplesPerFrame = 1,
  .maxBounces      = 6,
  .toneMap         = RenderSettings::ToneMap::Aces,
  .exposure        = 0.0f,
};

while (running) {
  profiler.BeginFrame();

  // (A) Ingest adapter pushes per-frame mutations through the public verbs.
  //     Example: spin the instance around Y. The matrix math is the
  //     consumer's responsibility — Pyxis only takes a row-major float4x4.
  const hlslpp::float4x4 spin = hlslpp::float4x4::rotation_y(time);
  scene.UpdateInstanceTransform(instanceHandle, spin);

  // (B) Drain pending GPU work — uploads, BLAS builds, TLAS refit.
  //     Pull a pre-allocated command list from a per-frame ring sized to
  //     `MAX_FRAMES_IN_FLIGHT`; only `framesInFlight` slots are live at runtime.
  //     Recycle each entry after the frame's fence has signalled.
  nvrhi::ICommandList* commandList = _frameCommandLists[frameIndex % framesInFlight];
  commandList->open();
  if (auto r = scene.CommitResources(commandList); !r) {
    // Soft fallback: log and skip the frame; the prior TLAS double-buffer is still
    // valid (§16.5). Do NOT discard the [[nodiscard]] result — §30.4 forbids it.
    pyxis::Logging::Get().Warn("CommitResources failed: {}", r.error().message.View());
    commandList->close();
    profiler.EndFrame();
    continue;
  }

  // (C) Execute the frame — the public "execute" entry point.
  //     User-issued GPU scopes (e.g. an ImGui overlay pass on the same
  //     command list) are bracketed with Profiler::GpuScope and show up
  //     alongside engine passes in FrameProfile.passes.
  {
    Profiler::GpuScope gpuScope{ profiler, commandList, "app.frame" };
    renderer.RenderFrame(commandList, settings, targets);
  }

  commandList->close();
  device->executeCommandList(commandList);
  // (Caller is responsible for fencing & reusing `commandList` after `framesInFlight` frames.)

  profiler.EndFrame();

  // (D) Optional: read introspection for HUD / logs.
  FrameStats   stats   = scene.LastFrameStats();
  FrameProfile profile = renderer.LastFrameProfile();
}
```

#### Reactive events

```cpp
// Window resize → resize AOVs and reset accumulation.
void OnWindowResized(uint32_t w, uint32_t h) {
  if (auto r = renderer.Resize(w, h); !r) {
    // Budget / max-extent failure: log, keep the previous size, mark degraded.
    pyxis::Logging::Get().Warn("Resize failed: {}", r.error().message.View());
    frameStats.degraded = true;
    return;
  }
  // Resize() implicitly resets accumulation; ResetAccumulation() is for
  // settings/material/camera changes that don't change the buffer size.
}

// User toggled a debug AOV → invalidate accumulation but keep buffers.
void OnDebugViewChanged() {
  renderer.ResetAccumulation();
}

// Asset removed.
void OnPrimRemoved() {
  scene.DestroyInstance(instanceHandle);
  scene.DestroyMesh(meshHandle);
  // Material/texture refcount themselves; unused entries are reclaimed
  // during the next CommitResources via the deletion queue.
}
```

#### Mapping to ingest adapters

| Adapter | Where setup lives | Where mutations are issued |
|---|---|---|
| `pyxis_app` (viewer / headless) | `Application::Init` constructs `GpuScene` + `PyxisRenderer` | App's update loop |
| `pyxis_hydra` | constructed once per `HdRenderDelegate` | inside `HdRenderPass::Sync` and `HdEngine::Execute` — calls the same `GpuScene` verbs |
| `pyxis_usd_ingest` | constructed by Application; given a `UsdStage*` for ingest only | `StageWalker::Walk` runs once at startup, emits an `IngestSnapshot`, releases the stage. All subsequent mutations come through the Pyxis renderer API (§O.2). |

The renderer never knows which adapter is driving it — every adapter speaks the same
public verbs, which is why the architecture is ingestion-agnostic (§1, §40).

---

## 19. Public API — Additions to Round Out v1


These additions plug the gaps a host integrator hits within the first day of
embedding Pyxis. They are part of the §18 public surface from M1 onward.

### 19.1 `PyxisCapabilities` — queryable feature flags

```cpp
// Public/Pyxis/Renderer/Descs/PyxisCapabilities.h
struct PyxisCapabilities {
  // Versioning (mirrors §22.2 macros at the resolved DLL).
  uint32_t versionEncoded;        // (major<<24)|(minor<<16)|patch
  // Adapters available.
  bool     hasHydraAdapter;
  bool     hasUsdDirectAdapter;
  // Feature gates (toggle as the renderer grows).
  bool     supportsDenoiser;
  bool     supportsBC7Textures;
  bool     supportsSubdivision;
  bool     supportsVolumes;
  bool     supportsCurves;
  bool     supportsLightLinking;
  bool     supportsMotionBlur;
  bool     supportsDof;
  // Bindless capacities (§5.b).
  uint32_t bindlessTextureSlots;
  uint32_t bindlessRawBufferSlots;
  // Acceleration limits.
  uint32_t maxInstancesPerTlas;
  uint32_t blasCompactionTriThreshold;
  // Frames in flight.
  uint32_t maxFramesInFlight;     // == MAX_FRAMES_IN_FLIGHT
  uint32_t defaultFramesInFlight; // == GpuSceneCreateDesc default
  // Trailing reserved (§22.3).
  uint32_t _reserved0 = 0, _reserved1 = 0, _reserved2 = 0, _reserved3 = 0;
};

[[nodiscard]] PYXIS_RENDERER_API PyxisCapabilities GetCapabilities() noexcept;
```

`GetCapabilities()` is process-wide and never fails; hosts call it once at
startup to switch UI affordances on/off. Adding a feature flag is a MINOR
bump (§22.1); never remove flags.

### 19.2 Cancellation token

`CommitResources` and the initial USD walk can take minutes on production-class scenes (Bistro is faster, ~tens of seconds). A cancellation
token lets hosts abort cleanly:

```cpp
// Public/Pyxis/Renderer/CancellationToken.h
class PYXIS_RENDERER_API CancellationToken final {
public:
  CancellationToken();
  void RequestCancel() noexcept;       // any thread
  [[nodiscard]] bool IsCancelled() const noexcept;
private:
  std::atomic<bool> _cancelled{false};
};

// GpuScene gets a CancellationToken-aware overload (added; old form kept).
[[nodiscard]] Expected<void> CommitResources(nvrhi::ICommandList* commandList,
                                             const CancellationToken& cancel);
```

- Both overloads ship in 1.x; the no-token form forwards to the token-aware
  form with a never-cancelled sentinel. At the next MAJOR (§22.1) the no-token
  form is removed and the token-aware form becomes the only signature.

- Long-running internal loops (BLAS-batch build, texture-decode, instancer
  flatten) check `cancel.IsCancelled()` at granular boundaries (every BLAS,
  every 256 textures, every instancer node).
- On cancellation, `CommitResources` returns
  `ErrorKind::CommitCancelled` (added to §20); whatever was already committed
  is valid and the next `CommitResources` resumes the leftover work.
- The ingest adapters (`pyxis_hydra`, `pyxis_usd_ingest`) accept the same
  token at construction and check it from their stage-walk loops, so `Ctrl+C`
  in headless mode aborts within a second.

### 19.3 Progress callback

```cpp
// Public/Pyxis/Renderer/Descs/ProgressCallback.h
struct ProgressEvent {
  enum class Stage : uint8_t {
    StageOpen, StageWalk, MaterialTranslation,
    TextureDecode, MeshExtraction, BlasBuild, TlasBuild,
    Idle
  };
  Stage          stage;
  uint64_t       itemsCompleted;
  uint64_t       itemsTotal;          // 0 if unknown
  ScopeName      currentItem;         // 56-byte POD; e.g. "/World/Hero/Mesh"
  double         elapsedMs;
};

using ProgressCallback = void (*)(const ProgressEvent&, void* userData);

// GpuScene::SetProgressCallback (no-throw).
void SetProgressCallback(ProgressCallback cb, void* userData) noexcept;
```

- Called from whichever thread is doing the work; hosts must marshal to UI
  themselves.
- Throttled to ≤ 30 events/sec per stage; bursty stages drop intermediate
  events but never drop the final `itemsCompleted == itemsTotal` event.
- Setting `cb = nullptr` unsubscribes.

### 19.4 `GpuScene::Clear()`

Re-loading a different USD without reconstructing the renderer:

```cpp
// Drops every Mesh / Material / Texture / Instance / Light, releases all
// GPU resources via the DeletionQueue, and resets handle generations. The
// next CreateMesh starts at handle slot 0 again. The TLAS becomes empty.
// Threading: render thread only.
void Clear() noexcept;
```

This is the difference between "swap scenes in 200 ms" and "tear down and
rebuild the renderer in 5 s". Hydra hosts that call
`HdRenderIndex::RemoveRprim` on every prim already exercise the moral
equivalent; making it one verb is far less error-prone.

### 19.5 Single-frame EXR API (viewer "save current frame")

```cpp
// On PyxisRenderer.
[[nodiscard]] Expected<void> SaveFrameExr(std::string_view path) const;
[[nodiscard]] Expected<void> SaveFramePng(std::string_view path,
                                          bool toneMapped) const;
```

- Uses the same EXR / PNG writers as headless mode (`tinyexr`, `stb`).
- Operates on the most recently completed frame's `RenderTargets::color` (or
  the tone-mapped LDR target for PNG); blocks until GPU readback completes
  (one frame's worth of latency).
- Errors via `Expected<void>` on path / quota / encode failure.
- Headless `--screenshot` CLI flag exposes the same path for tooling.

### 19.6 Picking — `PickAt(x, y)`

Hosts always ask for "give me the instance under the cursor". Plan kept this
deferred; v1 ships a thin wrapper around the `instanceId` AOV so clients don't
have to roll their own readback:

```cpp
struct PickResult {
  InstanceHandle instance = InstanceHandle::Invalid;
  MaterialHandle material = MaterialHandle::Invalid;
  hlslpp::float3 worldHit = {};
  float          distance = 0.0f;
  bool           isValid  = false;
};

// On PyxisRenderer.
[[nodiscard]] Expected<PickResult> PickAt(uint32_t x, uint32_t y) const;
```

- v1 implementation: read back one pixel from the `instanceId` and `materialId`
  AOVs (both must be enabled — otherwise returns
  `ErrorKind::PickRequiresAovs`). Optional richer pick (a single ray
  through the camera with full BSDF eval) is a post-v1 RFC.
- Threading: render thread only; blocks until readback completes (~1 frame).
- The §42 "Picking outside instance-ID AOV" deferral now means: full-quality
  picking with material parameters is post-v1; the basic instance hit lookup
  ships v1.

### 19.7 `InstanceHandle` generation bits — pinned

§8.2 mentioned generation bits; pinning the layout:

```cpp
// 32-bit handle layout (applies to MeshHandle / MaterialHandle / TextureHandle /
// InstanceHandle / LightHandle):
//   bits  0..23   slot index  (16 777 216 unique slots — generous headroom; Bistro uses ~10⁴, matches a TLAS-cap-sized scene for post-v1 production-class)
//   bits 24..31   generation  (256 reuses before wrap; on wrap the slot is
//                              quarantined and the handle is allocated from a
//                              fresh slot).
constexpr uint32_t HANDLE_SLOT_BITS       = 24;
constexpr uint32_t HANDLE_GENERATION_BITS = 8;
constexpr uint32_t HANDLE_SLOT_MASK       = (1u << 24) - 1u;
constexpr uint32_t HANDLE_GENERATION_MASK = ~HANDLE_SLOT_MASK;
```

`HandleBimap` quarantines a slot whose generation reaches 255 and never reuses
it, so generation rollover cannot silently re-issue a stale handle. A unit
test exhausts a slot and asserts the quarantine.

### 19.8 `RenderTargets` AOV nullability — pinned

The contract for null vs. enabled:

- `RenderTargets::color` is always required; `nullptr` is
  `ErrorKind::AovColorRequired`.
- For every other AOV (depth, normal, albedo, motionVector, materialId,
  instanceId): if `enabledAovs & Flag` is set **and** the corresponding
  `RenderTargets::*` pointer is `nullptr`, `RenderFrame` returns
  `ErrorKind::AovTargetMissing` synchronously *before* opening the command
  list. No partial frame.
- If `enabledAovs & Flag` is *not* set, the corresponding pointer is ignored
  whether null or not.
- `materialId` and `instanceId` AOVs share the requirement that the
  closesthit shader writes them; they cannot be silently dropped under high
  load. A path-tracer stub frame (zero bounces) still writes them.

---

## 20. Error Catalog (Initial)


The `Error` struct is defined in §18.3 (with `ErrorMessage` for ABI-safe ownership);
this section enumerates the `ErrorKind` values only.

```cpp
// Error struct — see §18.3 for the canonical declaration. Not duplicated here.
enum class ErrorKind : uint16_t {
  Ok = 0,
  // Configuration
  ConfigMissingField, ConfigBadType, ConfigBadEnum, ConfigOutOfRange,
  // I/O
  FileNotFound, FilePermissionDenied, FileCorrupt,
  // USD
  UsdStageOpenFailed, UsdPrimNotFound, UsdMaterialUnsupported,
  // Texture
  TextureDecodeFailed, TextureFormatUnsupported, TextureBudgetExceeded,
  // Shader
  ShaderCompileFailed, ShaderLinkFailed, ShaderEntryPointMissing,
  // Vulkan / NVRHI
  DeviceLost, FeatureMissing, OutOfMemoryGpu, ValidationError,
  // Acceleration structures
  BlasBudgetExceeded, AccelStructBuildFailed, TlasInstanceLimitExceeded,
  // AOV
  AovFormatUnsupported, AovUnknownToken, AovColorRequired, AovTargetMissing,
  // Handles
  InvalidHandle, HandleStaleGeneration,
  // Lifecycle / cancellation (added §19)
  CommitCancelled,
  // Capability gates (added §19 / §43)
  PickRequiresAovs, GeometryKindUnsupported,
  // I/O quota (added §47)
  FileQuotaExceeded,
  // Render graph (added §9.2)
  RenderGraphMissingProducer, RenderGraphDuplicateImport, RenderGraphUnknownRef,
  // Generic
  InvalidArgument, InvalidState, NotImplemented,
};
```

- `PYXIS_ERROR(kind, fmt, ...)` is the canonical construction macro: it formats the
  message into `Error::message`, fills `Error::source` with `__FILE__ ":" __LINE__`
  via the same `ErrorMessage` POD (truncated to fit), and returns by value.
- `PYXIS_TRY(expr)` propagates an `Expected<T>` failure upward unchanged (no
  wrapping, no rewriting); call sites that *want* to attach extra context
  re-construct via `PYXIS_ERROR`.
- The Hydra layer translates renderer `Error` into Hydra-friendly diagnostics
  (`HdRenderDelegate::GetRenderStats()` / `TF_WARN`).

---

## 21. RenderSettings Without Coupling ImGui to Render Passes


```
RenderSettings  (POD struct; lives in pyxis_renderer)
   │
   ▼
PyxisRenderer + passes  (read-only consumers)
   ▲
   │  (writes via the Application)
SettingsPanel (ImGui) — in pyxis_app, NOT in pyxis_renderer
```

- `RenderSettings` is a plain struct: resolution, samplesPerFrame, maxBounces, seed,
  enableAccumulation, enableToneMapping, exposure, debugView enum, enabled AOV mask, etc.
- Mutated only by the Application (from JSON, CLI, or ImGui). Renderer reads it at the
  start of `RenderFrame()` and detects deltas to decide accumulation reset.
- ImGui panel and JSON serialisation are **plain hand-written code per POD**, not a
  reflection layer. Concretely:
  - **JSON**: each editable POD lives next to a `NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(...)`
    macro line (one per struct) that lists every field exactly once. `nlohmann::json`
    handles serialise / deserialise / default-on-missing automatically; the cost of
    adding a field is one extra token in the macro.
  - **ImGui**: a free function `void DrawImGui(RenderSettings& s)` (and one per other
    editable POD) lives in `pyxis_app/UI/`, calls `ImGui::SliderFloat` / `ImGui::Combo`
    / etc. one widget per field, and groups them visually with `ImGui::CollapsingHeader`
    + `ImGui::BeginDisabled` for dependent toggles. Because it is plain code, tooltips,
    enable / disable based on other fields, units, custom validators, and per-field
    help text are all trivial — no DSL, no introspection POD, no template
    metaprogramming.
  - **Tests**: a single golden-JSON regression for each POD round-trips load/save and
    asserts byte-identical text; the JSON macro line means "new field added without a
    test update" is caught immediately by the test diff.

  Adding a setting therefore touches three places — the struct, the macro line, the
  `DrawImGui` function — all in the same file pair. This is more typing than a
  reflection-driven panel for the first ten fields, and noticeably less typing than
  any reflection scheme by the time the panel needs grouping and conditional widgets.

### 21.2 Quality / determinism knobs

These live on `RenderSettings` and propagate into the path-trace closesthit shader
through `ShaderInterop.slang`. All have safe defaults; none are required for first
frame.

| Field | Default | Effect |
|---|---|---|
| `accumulationFrameLimit` | `0` | When non-zero, accumulation freezes after N frames; in headless mode the EXR is written and `pyxis` exits 0. |
| `russianRouletteStartBounce` | `3` | Russian-roulette termination starts at this bounce depth; 0 disables RR (full `maxBounces` walk). Lower values → noisier but faster. |
| `fireflyClampLuminance` | `50.0` | Per-bounce returned-radiance clamp in scene-linear sRGB; 0 disables. Removes the long-tail spikes typical of MIS-without-NEE on dense foliage. |
| `lowDiscrepancySampling` | `false` | When true, replaces the PCG32 sampler with Sobol+Owen (§12); 4–6 % CPU cost on the camera-sample stage, ~1.5–2× faster convergence on smooth surfaces. |
| `seed` | `0` | RNG stream seed; zero → derived from frame count for non-deterministic viewer mode, fixed for headless. |

Reviewers reject any new "quality knob" added directly to a pass without going through
`RenderSettings`.

---

## 22. Versioning, ABI Releases, and Deprecation


### 22.1 Version scheme — Semantic Versioning 2.0.0

`pyxis_renderer.dll` and the public API (§18) follow **SemVer**: `MAJOR.MINOR.PATCH`.

- **MAJOR** — any change that breaks `Public/` source compatibility (signature
  removed/renamed, POD `sizeof` change without explicit reserved-field consumption,
  enum value renumber, handle-bit-layout change).
- **MINOR** — additive only: new public methods, new POD fields *only* in a trailing
  `reserved` slot reserved at the previous major (§18.9), new `ErrorKind` values at
  the end of the enum, new optional `RenderTargets` AOV slot.
- **PATCH** — bugfix-only; no API changes, no behaviour changes that move regression
  RMSE outside the per-test tolerance.

v1 ships as `1.0.0`. Anything before that is `0.MINOR.PATCH` with no stability
guarantees (M0–M9 are pre-release). M10 cuts `1.0.0-rc.0`, M11 cuts `1.0.0`.

### 22.2 `PYXIS_VERSION_*` macro contract

Generated by `_cmake/Version.cmake` from a single `version.txt` at the repo root, in
both header and CMake form:

```cpp
// Public/Pyxis/Renderer/Version.h
#define PYXIS_VERSION_MAJOR   1
#define PYXIS_VERSION_MINOR   0
#define PYXIS_VERSION_PATCH   0
#define PYXIS_VERSION_STRING "1.0.0"
#define PYXIS_VERSION_ENCODED ((PYXIS_VERSION_MAJOR << 24) | (PYXIS_VERSION_MINOR << 16) | (PYXIS_VERSION_PATCH))
// Note: the git SHA is intentionally NOT a preprocessor macro — baking it into
// a header would force every translation unit to recompile on every commit and
// would defeat ccache. It is exposed only as a runtime symbol below, resolved
// from a single .cpp generated by `_cmake/Version.cmake`.
namespace pyxis {
[[nodiscard]] PYXIS_RENDERER_API uint32_t   GetVersionEncoded() noexcept;        // returns PYXIS_VERSION_ENCODED at compile of the .dll
[[nodiscard]] PYXIS_RENDERER_API const char* GetVersionString() noexcept;        // "1.0.0+abc1234"
[[nodiscard]] PYXIS_RENDERER_API const char* GetVersionGitSha() noexcept;        // 40-char SHA, or "unknown" for dirty builds
}
```

`Application::Init` calls `GetVersionEncoded()` and asserts `(macro_at_compile >> 16) ==
(runtime >> 16)` — i.e. the consumer compiled against a major+minor that matches the
loaded DLL. PATCH mismatch is logged at info level only.

### 22.3 Deprecation window

- Renaming or removing a public symbol takes **two minor releases** to land:
  1. **N**: introduce the new symbol; mark the old one
     `[[deprecated("use Foo instead; removed in 2.0")]]`. Both must work and be tested.
  2. **N+1**: leave deprecated, log usage at info level if `PYXIS_DEBUG_TOOLS`.
  3. **MAJOR bump**: remove.
- Adding a POD field uses the **trailing reserved slot** declared at the previous
  MAJOR: `uint32_t _reserved0 = 0; uint32_t _reserved1 = 0;` are present from 1.0.0
  on every public POD. They become typed members in 1.MINOR; their previous default
  (`0`) is the documented zero-init meaning.
- Enum values are **append-only**. Reordering or renumbering is a MAJOR.
- A symbol-version map (`pyxis_renderer.def`) is generated from the public headers
  and diffed in CI; a non-additive diff fails the build unless the PR title contains
  `[abi-break]` and bumps `version.txt`.

### 22.4 Symbol export discipline

- Every public class is decorated with `PYXIS_RENDERER_API`. Every public free
  function gets the same decoration.
- The CI step `_tools/check_exports.py` runs `dumpbin /exports pyxis_renderer.dll`,
  filters to non-mangled or undecorated public symbols, and diffs against
  `_tools/golden_exports.txt`. A diff fails the build unless the PR also updates
  the golden file (and `version.txt`'s minor or major).

---

## 23. Slang ↔ C++ Interop Rules


- `ShaderInterop.slang` is the *only* file shared between C++ and shaders. It must compile
  under both Slang and clang-cl. `#ifdef __cplusplus` swaps types via type aliases:
  ```c
  #ifdef __cplusplus
    #include <hlsl++.h>
    using float2   = hlslpp::float2;
    using float3   = hlslpp::float3;
    using float4   = hlslpp::float4;
    using float4x4 = hlslpp::float4x4;
    using float3x4 = hlslpp::float3x4;
    using uint     = uint32_t;
    // Both sides declare the plain struct identically; the GPU-side
    // ConstantBuffer<T> binding is declared *separately* in the shader
    // (e.g. `ConstantBuffer<CameraConstants> g_camera : register(b0);`).
    #define PYXIS_INTEROP_STRUCT(name) struct name
  #else
    #define PYXIS_INTEROP_STRUCT(name) struct name
  #endif

  // Usage:
  PYXIS_INTEROP_STRUCT(CameraConstants) {
      float4x4 viewFromWorld;
      float4x4 projFromView;
      float3   eyeWorld;
      float    _pad0;
  };
  ```
- Every GPU struct defined in `ShaderInterop.slang` carries `static_assert(sizeof(...) % 16 == 0)`
  on the C++ side and matching `[[vk::binding]]` annotations.
- All matrices are **row-major** (§10). Row-vector multiplication only.
- No `bool` in interop structs; use `uint`. No `#pragma pack`; rely on natural alignment +
  explicit padding (`uint _pad[N];`).
- ShaderMake invocation includes:
  `-matrix-layout-row-major -O3 -profile sm_6_6 -target spirv -emit-spirv-directly`
  for SPIR-V output. Debug builds add `-g -O0`. The PCG seed-mix in §12.1 uses
  `uint64_t` arithmetic; Slang lowers this to SPIR-V's `Int64` capability, so
  `VkPhysicalDeviceFeatures::shaderInt64` (Vulkan 1.0 core feature) **must** be
  enabled at device creation. §5.b lists it in the required-features set.
- **Layout convention.** Pyxis uses Slang/HLSL's *default* cbuffer
  layout (`std140`-equivalent: 16-byte vector alignment) on both sides of the
  interop. The C++ side uses `hlslpp::float3` / `hlslpp::float4x4` etc., which
  are designed to match HLSL `float3`/`float4x4` byte-for-byte. This is the same
  pattern that has shipped successfully across NVRHI/D3D12/Vulkan codebases for years and
  is the reason Pyxis does **not** pass `-fvk-use-scalar-layout`. The benefit
  is one shared rhythm (`float3` packs with a trailing scalar into a 16-byte
  row on both sides) and zero conversion code in the upload path; the cost is
  that struct authors must respect 16-byte vector alignment, which the
  `static_assert(sizeof(...) % 16 == 0)` rule above catches mechanically.

---

# Part IV — Scene Ingest

## 24. Hydra DirtyBits → GpuScene Update Mapping


| HdChangeTracker dirty bit | Action in delegate | GpuScene operation |
|---|---|---|
| `DirtyTransform` | `mesh->Sync` reads `GetTransform` | update `InstanceTable[instanceId].transform`; mark TLAS dirty; reset accumulation |
| `DirtyPoints` | re-extract positions | replace vertex range; mark BLAS dirty, mark TLAS dirty |
| `DirtyTopology` | re-extract face counts/indices | replace index range; rebuild BLAS; rebuild TLAS |
| `DirtyNormals` | re-extract or regenerate | replace normal range; (BLAS unaffected) |
| `DirtyPrimvar` | UV/tangent/displayColor | replace primvar ranges |
| `DirtyMaterialId` | rebind material | update `InstanceTable[i].materialIdx` |
| `DirtyVisibility` | hide/show | toggle in `InstanceTable`; mark TLAS dirty |
| `DirtyInstanceIndex` / `DirtyInstancer` | re-flatten instances | rebuild instance set for this prim; mark TLAS dirty |
| `DirtyDoubleSided` | flag update | update material flag |
| `DirtyRenderTag` (`purpose`) | filter by config | drop/include from TLAS |
| `DirtySubdivTags` | v1 ignored | log once |
| `DirtyWidths` | curves only | n/a v1 |
| `DirtyParams` (Sprim camera/light) | update camera or light entry | reset accumulation |
| Material network change (`HdMaterial::Sync`) | rerun OpenPBR conversion | rehash → maybe new MaterialHandle |
| `HdRenderBuffer::Sync` (Bprim) | resize | reallocate AOV texture |

For a static scene load (Bistro), only the **initial** sync triggers heavy paths
(`DirtyTopology`/`DirtyPoints`/`DirtyMaterialId`/`DirtyInstancer`); subsequent frames mostly
toggle `DirtyTransform`/`DirtyParams` for the camera and reset accumulation.

---

## 25. Scene Imaging — Adapter Detail (Hydra adapter §25.A–N; USD-direct adapter §25.O)


### A. Hydra primitive support
- Rprim: `HdPrimTypeTokens->mesh` only. Skip points, basisCurves, volume, NURBS in v1.
- Sprim: `camera`, `distantLight`, `domeLight`, `rectLight`, `material`. Skip
  `extComputation`, `simpleLight`, `cylinderLight`, `sphereLight` (mapped to rect/distant fallbacks
  if scenes use any).
- Bprim: `renderBuffer` for AOVs. Skip texture-resource-as-Bprim (we manage textures
  ourselves through the `TextureCache`).
- Instancer: native + nested.

### B. Mesh imaging
- Points: `HdTokens->points`.
- Topology: `GetMeshTopology()`; triangulate with `HdMeshUtil`. Quads / ngons supported via
  fan triangulation.
- Normals: authored (`HdTokens->normals`) preferred; otherwise generated smooth (averaged).
- UVs: `HdTokens->st` primary; secondary UVs read but unused v1.
- Subsets: `GetGeomSubsets()` consumed. Each subset → its own InstanceTable entry sharing the
  same mesh+BLAS but a different material assignment via index buffer offset+count.
- Per-face material binding: handled by subsets. v1 does not support per-face arbitrary binding
  outside subsets.
- Double-sided: stored on material; closesthit shader respects it.
- Visibility: filters in/out of TLAS.
- Purpose: configurable via `parameters.json` `hydra.purpose`. Default `["default","render"]`.
- Refinement: `pxOsd::Tokens->none` (adaptive=0) at the Hydra-adapter boundary;
  subdivision **deferred** — subdiv hero meshes will look faceted on silhouette in v1.
  Acknowledged; tracked as known limitation. (The renderer itself stays USD-free per
  §1/§30.3; this token is only consumed inside `pyxis_hydra`.)
- Bounds: `GetExtent()` used for culling stats only (path tracing doesn't frustum-cull).
- Degenerate tris: dropped in topology conversion, counted.
- Large mesh upload: chunked staging copy; mesh > 256 MB split across multiple staging fills.
- Static assumption: positions never updated after first sync in v1.
- Will-break-Bistro-if-skipped: subsets (yes — many production assets rely on per-face material binding
  through subsets), instancer (yes), UVs (most materials), normals fallback (yes, Bistro ships
  authored normals on most meshes).

### C. Instancing
- Native Hydra instancer: full support.
- Point instancer (`UsdGeomPointInstancer`): exposed via Hydra as native instancer — covered
  by the same code path.
- Nested instancing: flatten transforms recursively.
- BLAS sharing: by MeshHandle.
- Per-instance transforms: `instanceTransforms` primvar; concatenated with parent.
- Instance IDs: deterministic given a sorted SdfPath order — important for golden-image tests.

### D. Transform & hierarchy
- World transforms only consumed (Hydra already flattens for Rprims).
- SdfPath identity: `unordered_map<SdfPath, Handle, SdfPath::Hash>` in the Hydra layer.
- Static transform: detected by absence of `DirtyTransform` since first sync.
- TLAS invalidated on any transform dirty; we set a single dirty flag and rebuild once at
  `CommitResources`.
- Camera: `HdCamera` → `CameraConstants` (proj + view matrices, focal length, lens, etc.).

### E. Materials & OpenPBR conversion (cf. §11)
- Adapter chain: `UsdPreviewSurfaceAdapter`, `MaterialXAdapter`, `RenderManFallbackAdapter`.
- Per Hydra material: produce `OpenPBRMaterialDesc` → hash → dedupe → upload.
- Texture slot extraction during conversion; texture-cache requests issued immediately
  (lazy decode) so the texture-handle is non-null and stable.
- Fallback material logged into `unsupported_features.json`.

### F. Textures (cf. §13)
- Path resolution via `HdResourceRegistry`/`Sdf` asset resolver, then handed to TextureCache.

### G. Lighting
- Distant light: direction + spectrum; importance-sampled.
- Dome light: env map (lat-long EXR); importance sampling table built from the map.
- Rect light: position + axes + emission.
- Mesh emissive: handled implicitly by `OpenPBRMaterialDesc::emission*`; sampled via
  emissive-triangle alias table. v1: simple uniform sampling over emissive triangles.
- Bistro needs: dome light (sky), distant light (sun), and rect/area lights for interior fixtures.

### H. Volumes & atmosphere
- Detect any `HdVolume` Rprim. v1: log once, skip from TLAS.
- Optional homogeneous fog approximated via `parameters.json` via render settings (constant
  extinction). Acknowledged that any authored volumetric look will be lost — deferred.

### I. AOVs
- Required: `color` (RGBA16F).
- Optional: `depth` (R32F linear view-space depth), `normal` (RGB16F world-space),
  `albedo` (RGBA16F), `materialId` (R32_UINT), `instanceId` (R32_UINT).
- Accumulation buffer: separate RGBA32F.
- Tone-mapped is `color`; linear `albedo`/`normal`.
- Hydra integration: `HdPyxisRenderBuffer::Map/Unmap` → backed by NVRHI texture; the
  `CopyToHydraRenderBufferPass` copies from internal AOVs into Hydra-owned buffers.
- Sync: NVRHI handles barriers; we just declare reads/writes in the RenderGraph.

#### I.1 AOV format negotiation (Hydra ↔ Pyxis)

Hydra clients (usdview, custom hosts) request AOVs via `HdAovDescriptor::format`.
`HdPyxisRenderDelegate::GetDefaultAovDescriptor` advertises Pyxis's preferred internal
formats; if the client overrides them, `CopyToHydraRenderBufferPass` performs the
format conversion. Mapping table:

| Hydra `HdAovTokens` | Pyxis internal format | Hydra-side accepted formats | Conversion in copy pass |
|---|---|---|---|
| `color` | RGBA16F | RGBA16F (preferred), RGBA32F, RGBA8 sRGB | float → float (cast) or float → 8-bit sRGB encode |
| `depth` | R32F (linear view-space) | R32F (preferred), D24_UNORM_S8_UINT | float → packed depth |
| `normal` | RGB16F (world-space, normalised) | RGB16F (preferred), RGBA16F (alpha=0), RGB10A2_UNORM | identity / pad / pack |
| `albedo` | RGBA16F (linear, unpremultiplied) | RGBA16F (preferred), RGBA8 sRGB | identity / 8-bit sRGB encode |
| `motionVector` | RG16F (screen-space px/frame, prev→curr) | RG16F (preferred), RG32F | identity / 16→32 widen |
| `materialId` | R32_UINT | R32_UINT (preferred), R32F (cast) | identity / int→float reinterpret |
| `instanceId` | R32_UINT | R32_UINT (preferred), R32F (cast) | identity / int→float reinterpret |

Unknown AOV tokens or unsupported formats fail with `ErrorKind::AovFormatUnsupported`,
logged once per token, and are skipped (not silently substituted). The default
descriptor advertises the *preferred* column.

#### I.2 Color management pipeline (explicit)

v1 is **scene-linear sRGB end-to-end**. No ACES / OCIO. Concretely:

1. **Texture decode** — sRGB-encoded inputs (`Role::BaseColor`, `Role::Emission`)
   are decoded to linear at upload time via the texture format
   (`R8G8B8A8_UNORM_SRGB`); linear inputs (`Role::NormalMap`,
   `Role::RoughnessMetallic`, EXR `BaseColor`) stay in their authored space.
2. **Path-tracing radiance** is linear scene-referred. No transform applied during
   accumulation.
3. **Tone-map** maps linear scene radiance → linear display radiance
   (`RenderSettings::toneMap`: `Linear` = identity, `Aces` = the standard ACES RRT+ODT
   approximation, `Filmic` = Hable's curve).
4. **Output encode**:
   - EXR (`output.image`): written **linear**, no transform. This is the regression
     artifact — always linear so RMSE compares apples to apples.
   - PNG / swapchain: **sRGB-encoded** (gamma 2.2 if `toneMap=Linear`, ACES ODT
     output curve otherwise).
5. **ACES-tagged content caveat**: some production assets ship ACEScg-tagged textures.
   v1 ignores the tag and treats them as sRGB-or-linear-by-role; reference images will
   look slightly desaturated versus a true ACES pipeline. This is logged in
   `unsupported_features.json` and tracked as a post-1.0 task. A future
   `render.outputColorSpace = "linearSrgb" | "acesCg"` config field is *reserved* in
   `parameters.json` but pinned to `linearSrgb`. Bistro is sRGB-tagged, so this
   caveat does not affect v1 reference images.

### J. Dirty bits — see §24.

### K. Render delegate lifecycle
- Construction: registers the renderer with `HdRendererPluginRegistry`. Creates `GpuScene`
  shared with the application.
- `CreateRprim`/`DestroyRprim` etc.: thin allocators for `HdPyxisMesh` (and friends), do **not**
  upload to GPU yet.
- `CommitResources`: drains the upload queue, rebuilds dirty BLAS, rebuilds/refits TLAS.
  This is the **only** place GPU work for scene topology happens. Each Sync just marks
  ranges dirty.
- `CreateRenderPass`: returns `HdPyxisRenderPass` which calls `PyxisRenderer::RenderFrame`.

#### K.1 `HdPyxisRenderTask` — actual frame driving

The Application drives frames via `HdEngine::Execute(renderIndex, &tasks)` with **one**
task, `HdPyxisRenderTask` (subclass of `HdTask`). It is the single place where the Hydra
phase model and the Pyxis public API meet:

```cpp
// pyxis_app/Private/HydraEngine/HdPyxisRenderTask.h
class HdPyxisRenderTask final : public HdTask {
public:
  HdPyxisRenderTask(SdfPath const& id,
                    pyxis::GpuScene*      scene,
                    pyxis::PyxisRenderer* renderer,
                    pyxis::Profiler*      profiler);

  // Phase 1 — Sync. Runs on the ingest thread (§31). Reads the task's
  // HdTaskContext params (camera id, viewport, AOV bindings, RenderSettings
  // delta) and stages them into local fields. No GPU work, no GpuScene mutation
  // here — individual Rprim Sync calls already pushed mutations through GpuScene
  // before this task runs.
  void Sync(HdSceneDelegate*, HdTaskContext*, HdDirtyBits*) override;

  // Phase 2 — Prepare. v1: no-op. Reserved for future per-frame setup that
  // doesn't need the command list (e.g. AS prebuild scheduling decisions).
  void Prepare(HdTaskContext*, HdRenderIndex*) override;

  // Phase 3 — Execute. Runs on the render thread (§31). The single place where
  // Pyxis touches the GPU for this frame:
  //   1. profiler.BeginFrame()
  //   2. open command list (per-frame ring — §18.10)
  //   3. scene->CommitResources(commandList)        // public API
  //   4. renderer->RenderFrame(commandList, settings, targets)  // public API
  //   5. close + execute command list
  //   6. profiler.EndFrame()
  void Execute(HdTaskContext*) override;

  TfTokenVector const& GetRenderTags() const override; // ["default","render"] from config

private:
  pyxis::GpuScene*      _scene;
  pyxis::PyxisRenderer* _renderer;
  pyxis::Profiler*      _profiler;
  pyxis::RenderSettings _settings;       // captured in Sync, consumed in Execute
  pyxis::RenderTargets  _targets;        // resolved from AOV bindings (§25.I.1)
  CameraDescCpu         _cameraSnapshot; // captured in Sync
};
```

Key invariants:
- The task **only** calls public Pyxis API (`GpuScene`, `PyxisRenderer`, `Profiler`).
  No `Private/` type leaks into the Hydra layer.
- `Sync` never touches the GPU.
- `Execute` is the only path that opens a command list; per-Rprim `HdPyxisMesh::Sync`
  has already pushed `GpuScene::CreateMesh` / `UpdateInstanceTransform` etc. via the
  thread-safe submission queue (§18.5).

### O. `pyxis_usd_ingest` — USD-direct prim traversal contract (no Hydra)

Mirror of §24 / §25 for the `pyxis_usd_ingest` adapter. Same `MeshDesc` /
`InstanceDesc` / `OpenPBRMaterialDesc` outputs as the Hydra adapter; same
`pyxis_material_translation` shared library; **same instance ordering** so
regression EXRs match byte-for-byte across adapters.

#### O.1 `StageWalker` traversal
- `StageWalker::Walk(UsdStage*, const Configuration&)` does a single depth-first
  traversal of `stage->Traverse()`, sorted by `SdfPath` (`std::stable_sort`).
- Per prim:
  - `UsdGeomMesh` → `MeshDesc` via `UsdGeomXformCache` for transforms,
    `UsdGeomPrimvarsAPI` for normals/UVs, `HdMeshUtil`-equivalent local triangulator
    (we vendor a 200-line minimal triangulator: fan-triangulate ngons, drop
    degenerates, count). No `pxOsd` adaptive subdivision in v1 (matches §25.B).
  - `UsdGeomPointInstancer` → fully flatten to per-prototype `InstanceDesc`
    array (same flatten cache as Hydra side, keyed on `(prim path, time)`).
  - `UsdGeomXformable` non-mesh non-instancer → contributes to the `Xformable`
    transform stack; not emitted itself.
  - `UsdShadeMaterial` → `OpenPBRMaterialDesc` via
    `pyxis_material_translation::FromUsdShade(...)`. Same code path the Hydra
    delegate calls.
  - `UsdLuxLightAPI` → `LightDesc` (distant, dome, rect supported v1).
- Output: a single `IngestSnapshot` POD pushed onto the
  `moodycamel::ConcurrentQueue` (§18.5).

#### O.2 Update model — API-only outside Hydra

Pyxis supports exactly **two** update channels, chosen by which library the
host links:

| Host links | Editing surface | Update mechanism |
|---|---|---|
| `pyxis_hydra` (Hydra delegate) | host mutates the `UsdStage` (DCC slider, script, simulator) | Hydra's own listener — `UsdImagingStageSceneIndex` translates `UsdNotice::ObjectsChanged` into `HdSceneIndex` prim-dirty events, delivered to `HdPyxisMesh::Sync`. The §24 dirty-bits table fires. We never register our own `TfNotice`. |
| `pyxis_usd_ingest` (direct ingest, no Hydra) | host calls the Pyxis renderer API (`UpdateInstanceTransform`, `UpdateMaterial`, `DestroyMesh`, …) | direct API calls into `GpuScene`. No listener; the renderer does not hold a `UsdStage*` after ingest and does not subscribe to `UsdNotice`. |

`pyxis_usd_ingest` is therefore a **one-shot importer**: `StageWalker::Walk`
reads the stage, emits an `IngestSnapshot`, and from that moment the stage
may even be released. Subsequent edits go through the Pyxis API. This keeps
the renderer USD-agnostic at runtime (§1, §30.3) and avoids a second
"USD-as-live-channel" path that would duplicate Hydra's job.

**What this means for hosts:**
- DCCs (Solaris, Maya-USD, usdview, Katana, Blender-USD): use `pyxis_hydra`.
  Edit USD as usual; Hydra delivers the diffs.
- Headless farm workers, custom non-DCC tools, in-process scripts, sims:
  use `pyxis_usd_ingest` for the initial scene load, then drive frame-to-
  frame mutations through the Pyxis API. If a host *also* wants to
  authoritatively edit a `UsdStage` and have Pyxis follow, the right
  answer is "link `pyxis_hydra`" — adding a non-Hydra `UsdNotice` listener
  inside Pyxis would duplicate `UsdImagingStageSceneIndex` for no
  customer-visible benefit.

The dirty-bit / notice-to-`GpuScene` mapping in §24 therefore stands as the
single update contract; no parallel notice-driven mapping lives inside
`pyxis_usd_ingest`.

#### O.3 Determinism contract

Both adapters MUST emit instances in `SdfPath`-sorted order. The shared regression
harness (§35) opens the same Bistro scene through `pyxis_hydra` and `pyxis_usd_ingest`
and asserts RMSE = 0 between the two output EXRs. Any divergence is a P0 bug.

### L. GpuScene design — see §8.

### M. Vulkan/NVRHI specifics
- Acceleration-structure feature enablement at device creation.
- Scratch buffer sized to max-of-batch.
- Compaction default-on.
- Bindless via `RawBuffer_SRV(space=1)` + `Texture_SRV(space=2)`.
- Large buffer allocations go through NVRHI; for very large vertex/index pools, we use
  multiple buffers of ≤ 2 GiB each on platforms where it matters; Bistro stays well under.
- Staging strategy: ring + one-shot for oversize.
- Queue sync: graphics + transfer + compute; a single `CommandList` per pass v1.
- Image layouts: NVRHI tracks via `keepInitialState`/`setInitialState`; we use the
  the standard NVRHI image-layout convention (`keepInitialState` / `setInitialState`).
- Debug labels via `commandList->beginMarker(...)` per pass.
- Aftermath / Nsight Graphics Capture optional, Debug-only Windows-only via CMake flag.

### N. Production-scene risks (mitigations)
| Risk | Mitigation |
|---|---|
| Startup time (USD pop) | progress reporting via spdlog, lazy texture decode, profiled |
| Many instanced assets | strict BLAS sharing by MeshHandle |
| Heavy texture memory | resolution clamp, lazy decode, missing-texture fallback (no BC compression v1) |
| Complex material networks | OpenPBR conversion + fallback material; unsupported nodes logged |
| Volumetrics | logged + skipped v1 |
| Unsupported USD features | catalog in `out/scene_unsupported_features.json` |
| Memory budget | budget tracker, configurable caps |
| Long BLAS build | batch + compaction + GPU timestamp report |
| Need for progress | per-stage spdlog with prim counts |
| Need for cache stats | TextureCache.report(), MaterialTable.report() |
| Need for fallbacks | gray material, magenta texture |
| Downscaling | `textures.maxResolution`, future `geometry.maxTrisPerMesh` |

---

# Part V — Application, Configuration, Presentation, UX

## 26. Configuration System (`parameters.json`)


- Single source of truth: `parameters.json`. CLI overrides specific fields.
- CLI: `--config <path>`, `--headless`, `--scene <path>`, `--camera <sdfPath>`,
  `--width <int>`, `--height <int>`, `--samples <int>`, `--output <path>`, `--profile <path>`.
  When `--scene` is omitted, scene resolution falls back through
  `parameters.json` → most-recent-existing recent scene → the bundled
  default scene (§29.4.a); `pyxis.exe` therefore always has something
  to render at startup.
- Loader: `nlohmann::json` → `Configuration` struct. Validate against the
  shipped JSON Schema document `pyxis_app/Resources/parameters.schema.json` (see §49)
  (required fields, types, enum values). Print a clean spdlog summary at startup.
- The **resolved effective config** (after CLI overrides) is written to
  `output.effectiveConfig` so any run is reproducible.
- Same loader is used for viewer, headless, profiling and tests — no special test config.

Example configs in `_documentation/parameters.md`:
- `parameters.bistro_viewer.json`
- `parameters.bistro_headless.json`
- `parameters.regression_tiny.json`
- `parameters.profile_benchmark.json`

The full example given in `NEW_ENGINE.md` is the canonical reference for field names and is
included verbatim in the docs.

---

## 27. Configuration Schema (Concrete)


```jsonc
{
  "app":          { "ingest": "hydra" },
  "scene":        { "path": "...", "camera": "/World/cameras/main", "purpose": ["default","render"] },
  "render":       { "width": 1920, "height": 1080, "samplesPerFrame": 1, "maxBounces": 6,
                    "seed": 0, "enableAccumulation": true, "exposure": 0.0,
                    "toneMap": "aces", "debugView": "color",
                    "accumulationFrameLimit": 0, "russianRouletteStartBounce": 3,
                    "fireflyClampLuminance": 50.0, "lowDiscrepancySampling": false,
                    "aovs": ["color","depth","normal","albedo","motionVector","instanceId","materialId"] },
  "textures":     { "maxResolution": 4096, "missingTextureColor": [1.0, 0.0, 1.0, 1.0] },
  "geometry":     { "compactBLAS": true, "blasCompactionTriThreshold": 65536,
                    "maxInstancesPerTlas": 12000000 },
  "hydra":        { "purpose": ["default","render"] },
  "profiling":    { "enabled": true, "tracy": false,
                    "outputJson": "out/profile.json", "outputCsv": "out/profile.csv" },
  "output":       { "image": "out/frame.exr", "ldr": "out/frame.png",
                    "effectiveConfig": "out/effective.json" },
  "diagnostics":  { "validationLayer": true, "aftermath": false },
  "limits":       { "stagingMib": 256, "bindlessCapacity": 80000, "framesInFlight": 2,
                    "budgetGiB": { "textures": 14.0, "blasTlas": 6.0,
                                   "staging": 0.25, "aov": 0.5, "scratch": 1.0,
                                   "headroom": 2.0 } }
}
```

- `app.ingest` selects the ingest adapter at startup: `"hydra"` (default,
  `pyxis_hydra.dll`) or `"usd_direct"` (`pyxis_usd_ingest.dll`). Both adapters are
  shipped in v1; their EXR outputs must match byte-for-byte on the regression
  fixtures (§25.O.3).
- `limits.budgetGiB` is normative for a 16 GiB target GPU (RTX 4080); the sum
  leaves headroom for swapchain + ImGui + driver overhead. v1 hero scene (Bistro) fits
  comfortably under 8 GiB; the 16 GiB target leaves slack for post-v1 production-class
  scenes. On smaller GPUs the application scales every category proportionally.
- `Configuration` C++ struct mirrors this 1:1 with `[[nodiscard]] static std::expected<Configuration, Error> parse(const Json&);`
- Schema validation: required-field check, type check, enum check, range check. One pass.
- The shipped `parameters.schema.json` is the authoritative document; the C++ parser
  validates against it at startup so any drift between code and schema is caught
  immediately.
- Headless mode `mkdir -p`s the parent directories of `output.image`, `output.ldr`,
  `output.effectiveConfig` and `profiling.output*` before opening the file; missing
  directories must never abort a long render.
- CLI overrides: each CLI arg maps to a JSON pointer; applied after parse, before validate.

---

## 28. Frame Pacing & Presentation Knobs


Viewer mode adds a small set of knobs, all mutable at runtime through ImGui
and persisted in `parameters.json`:

```jsonc
"viewer": {
  "presentMode":     "fifo",        // "fifo" | "mailbox" | "immediate" | "fifoRelaxed"
  "targetFps":        0,            // 0 = no cap (defer to presentMode); else frame-pace to this
  "powerSaveMs":      4,            // sleep this many ms between frames when window is unfocused
  "interactiveLatencyMode": false   // true → IMMEDIATE while the camera is moving, FIFO when still
}
```

There is intentionally **no** separate `vsync` boolean: it would be a redundant
alias for `presentMode == "fifo"` and create an ambiguity when both are set.
Hosts that need a one-click vsync toggle bind it directly to `presentMode`.

- `presentMode` is mapped 1:1 to Vulkan present modes; falls back to
  `fifo` if the requested mode is unavailable, with a one-shot log line.
- `targetFps > 0` enables a CPU-side limiter that sleeps to the next frame
  boundary; combined with `mailbox` it caps power draw without tearing.
- `interactiveLatencyMode = true` is the recommended "production-scene viewer" setting:
  during camera motion the renderer drops to `IMMEDIATE` to minimise lag,
  and snaps back to `FIFO` when the camera is idle for ≥ 250 ms. This is
  off by default because tearing during motion confuses screen-recording.
- HDR display path is **deferred** (§5 already excludes HDR swapchain
  formats v1). Reintroduction is an RFC; the hook is the
  `RenderTargets::color` already being RGBA16F internally.

---

## 29. First-Run UX & Accessibility


### 29.1 First-run UX

- The installed tree contains **`pyxis_app/Resources/parameters.default.json`**
  (canonical defaults, ships alongside `pyxis.exe`). Loader resolution is an
  **overlay**, not first-found-wins:
  1. Start from the embedded defaults (always present, baked into the binary
     and also written to `<exe-dir>/parameters.default.json` for inspection).
  2. Overlay `<exe-dir>/parameters.default.json` on disk (lets a sysadmin
     ship a site-wide override without touching per-user files).
  3. Overlay `%LOCALAPPDATA%/Pyxis/parameters.json` (per-user, optional).
  4. Overlay `--config <path>` (explicit; highest precedence).
  - Each overlay is a key-by-key merge: missing keys keep the previous value,
    present keys replace it. Arrays are replaced, not concatenated.
  - Pyxis never writes back to any of these files. The merged effective
    config is written to `output.effectiveConfig` per §26 for diagnostics.
- `pyxis --print-default-config` writes the embedded defaults JSON to stdout
  (so a user can pipe it to a file and edit) without opening any device.
- `pyxis --validate-config <path>` parses + schema-validates a config and
  exits 0 / 3 with a readable error trail. Useful in CI hooks.
- First run with no `parameters.json` in the user folder logs a single
  info-level line "using bundled defaults from <path>"; never silent.

### 29.2 Accessibility (viewer mode)

ImGui-based panels honour the following conventions:

- **Font scaling**: `RenderSettings::ui.fontScale` (1.0 default) plus
  `Ctrl + +` / `Ctrl + -` shortcuts. Per-monitor DPI awareness (§5) already
  scales the base size; the user knob multiplies on top.
- **Colorblind-safe AOV palettes**: `DebugView::Albedo`, `Normal`,
  `MaterialId`, `InstanceId` use palettes from
  `_documentation/colorblind_palettes.md` (sourced from ColorBrewer 8-class
  qualitative + Wong's 8-color palette). The palette is selectable via
  `RenderSettings::ui.colorblindPalette ∈ { "default", "deuteranopia",
  "protanopia", "tritanopia", "monochrome" }`. Default is the Wong 8-color
  palette which is safe across deuteranopia and protanopia.
- **Colour vs grayscale fallback**: every ImGui panel that renders a color
  swatch also renders a numeric label, so a screen-reader or grayscale
  capture reads correctly. A unit test renders the AOV palettes through a
  Brettel/Viénot CVD simulation and asserts pairwise ΔE ≥ 12 across the
  three CVD types.
- **Keyboard navigation**: every ImGui panel is reachable via `Tab` + arrow
  keys; no mouse-only widgets.
- **Reduced motion**: `RenderSettings::ui.reducedMotion = false` default;
  when true, ImGui transitions and the spinner-while-loading animation are
  disabled.
- An **accessibility statement** is shipped at
  `_documentation/accessibility.md` linking to the relevant
  `parameters.json` fields and shortcuts.

### 29.3 Viewer ImGui panels

The viewer ships a fixed set of ImGui panels, all dockable, all toggleable
from the `View` menu, all mutually independent. The panel set is closed in
v1 — adding a panel is an RFC because it widens the QA surface.

| Panel | Purpose | Backed by |
|---|---|---|
| **Settings** | Live-edits the `RenderSettings` POD (§17): bounce count, samples-per-frame, AOV selector, debug-view selector, tone-map operator, exposure, gamma, ui font/colorblind palette, frame-pacing knobs (§28). All edits write back into the in-memory config; an explicit "Save" button writes `%LOCALAPPDATA%/Pyxis/parameters.json`. | `RenderSettings` |
| **Features** | RenderGraph feature toggles (§29.5): pathTracing / accumulation / NEE / MIS / toneMap / TAA / motionVectors / per-AOV exports / Tracy. Greys out toggles whose dependencies are unmet. Each toggle triggers a `RenderGraph::Compile` on the next frame; the compile-cache makes flipping back instant. | `RenderSettings::features`, `RenderGraph::Compile` |
| **Debug Tools** *(`PYXIS_DEBUG_TOOLS` only)* | A small developer-iteration panel separate from Settings. Hosts: **Reload Shaders** button (§10 — recompiles all `.slang` entry points off the render thread, hot-swaps PSOs at the next frame boundary, resets accumulation; bad edits keep previous PSOs live and surface the diagnostic in a status line); **Reload RenderGraph** (forces a `Compile` even with no feature change — useful when iterating on pass-declaration code); **Validation toggle** (turns Vulkan validation layers on/off without restart); **Force device idle** (calls `vkDeviceWaitIdle` for clean profiler captures). Compiled out of Release builds entirely. | `ShaderLibrary`, `RenderGraph` |
| **Performance** | Per-frame CPU + GPU timing of every render-graph pass, with a 240-frame rolling history (sparkline + min/avg/p99). Shows: total frame ms, CPU sync wait, GPU pass list with per-pass `BeginMarker` ms (from `Profiler::FrameProfile::PassTiming`), TLAS-build ms, BLAS-build ms, upload-queue bytes flushed, RenderGraph compile-cache hit/miss. A "Tracy connect" button prints the listen URL (§9.4). A "Copy CSV" button serialises the rolling history to clipboard for spreadsheet diffing. | `Profiler` (§9.5) |
| **Scene Stats** | Informative counts about the loaded scene: meshes (count, total triangles, total vertex MB, total index MB), materials (count, OpenPBR variant histogram by `MaterialFlag`), textures (count, total VRAM MB, decoded MB, format histogram, largest 10 textures by bytes), instances (count, visible, TLAS instance bytes), lights (count by kind), AS memory (BLAS MB, TLAS MB, scratch high-water). Refreshed every 30 frames; each row is a click-to-sort table. | `GpuScene`, `TextureCache`, `BlasCache`, `TlasBuilder` |
| **Inspector** | Render-target / AOV viewer with channel selector, range knobs, pixel picker, pinned probes with sparklines, and per-texture EXR/PNG save (§29.6). | `RenderGraph::Resolve` |
| **Profiler** | Tracy-equivalent zone view *without* an external connection; renders the live `FrameProfile::PassTiming` ring as a flame-bar. Useful when Tracy is unavailable (closed networks, customer machines). | `Profiler` |
| **Material Report** | Lists every `MaterialHandle` with its `OpenPBRMaterialDesc::Source` (`UsdPreviewSurface` / `MaterialX` / `RenderManFallback` / `Default`) and the diagnostic `sourcePrim` SdfPath. Filters: source kind, flag bits, "fallback only". Click → highlights instances using that material in the next AOV `MaterialId` render, and opens a side editor that calls `DrawImGui(OpenPBRMaterialDesc&)` (§21) on the selected entry; edits dirty the material's hash and round-trip through "Save Scene As USD" (§29.7). | `GpuScene` material table |
| **Texture Cache** | Lists every cached texture with role (`BaseColor`, `Normal`, …), colorspace, dimensions, format, decoded bytes, residency status. Eviction is manual via "Evict selected" (no LRU in v1, §42). Click → highlights instances sampling that texture. | `TextureCache` |
| **GPU Stats** | Adapter info (vendor, model, driver, VRAM total/used), Vulkan extensions actually enabled, `framesInFlight`, swapchain present mode actually negotiated, `VK_EXT_calibrated_timestamps` availability, NVRHI heap residency. | `VkDeviceManager` |
| **Console** | spdlog tail (last 1024 lines), severity filter, "Copy errors" button. Also surfaces the last 32 `Error::Kind` / `ErrorMessage` records emitted by the renderer (§20). | `spdlog` ringbuffer sink |
| **Scene** | "Open USD…" file dialog (§29.4) and "Save Scene As USD…" (§29.7); shows the currently-loaded stage's resolved root-layer path, asset-resolver context, load-set / population mask, prim count. Has a "Reload" button that re-runs ingest on the current path. | active adapter |
| **Load Timeline** | One-shot Gantt waterfall of the most recent scene-load (§34.1): per-thread swim lanes (asset I/O pool members, ingest thread, render thread), per-phase bars (USD open, prim discovery, mesh extraction, texture decode, BLAS / TLAS first build), end-of-load summary table. Read-only; persists across frames so it can be inspected after the scene starts rendering, and refreshes only on the next scene load (§29.4). | `ProfilerData` load-time scopes |

All panels are read-mostly except Settings (writes `RenderSettings`), Texture
Cache (writes `Evict`), and Scene (triggers reload). None of them ever
touches the GPU directly; they call documented API verbs only. Panel
state (open/closed, docking layout, column widths) persists in
`%LOCALAPPDATA%/Pyxis/imgui.ini` per ImGui convention.

### 29.4 Runtime scene loading

Viewer mode supports loading a different USD stage at runtime *without*
reconstructing the renderer. The flow:

1. User picks a `.usd` / `.usda` / `.usdc` / `.usdz` via the **Scene** panel's
   "Open USD…" button (a native Win32 file-open dialog; same code path as
   `--scene` CLI). The selected path is validated against
   `parameters.json` `paths.allowedRoots` (§31 sandbox rule).
2. The viewer enqueues a `SceneSwapRequest` onto the ingest thread's
   command queue. The render thread keeps drawing the current scene at
   present-mode pacing while the request is pending — no UI freeze.
3. On the ingest thread:
   a. `PyxisRenderer::WaitIdle()` drains in-flight uploads.
   b. `GpuScene::Clear()` (§19.4) drops every Mesh / Material / Texture /
      Instance / Light, releases GPU resources via `DeletionQueue`, resets
      handle generations.
   c. The active adapter is rebound to the new stage:
      - `pyxis_hydra`: `HdRenderIndex::Clear()` then attach the new
        `UsdImagingStageSceneIndex` over the new `UsdStage`.
      - `pyxis_usd_ingest`: discard the old `IngestSnapshot`, run
        `StageWalker::Walk` on the new stage, emit a fresh snapshot.
   d. Ingest streams the new prims; the renderer paints first pixels
      against the new TLAS as soon as `CommitResources` returns.
4. Accumulation buffers are reset (§12.4) — the new scene starts from
   sample 0.
5. The path is appended to a "Recent Files" list (capped at 8) persisted
   in `%LOCALAPPDATA%/Pyxis/recent_scenes.json`.

Failure modes:
- Path outside `paths.allowedRoots` → `Error::FilePermissionDenied`
  (§20 sandbox rule), no state change, error banner in the Console panel.
- USD open / composition error → `Error::UsdStageOpenFailed`, the **previous
  scene remains rendered** (we only call `Clear()` after the new stage
  successfully composes — fail-safe ordering).
- Cancellation: a second "Open USD…" while one is in-flight cancels the
  pending request before `Clear()` (cheap), or completes the in-flight
  swap then queues the new one (after `Clear()`); user cannot end up with
  a half-loaded stage.

This is **not** "hot-swap with continuity" (post-v1, §42) — accumulation
resets, the camera resets to the new stage's primary `UsdGeomCamera` if
present, the previous scene's textures and BLAS are fully released. It is
"swap scenes in 200 ms instead of restarting the process", which is what
the §19.4 `Clear()` primitive was designed to enable.

Headless mode does not expose runtime scene loading — each headless
invocation processes one `--scene` and exits (§3, §27).

### 29.4.a Default startup scene

Launching `pyxis.exe` with **no `--scene` argument** must produce a
renderable image, not a black window or an error. Pyxis ships a tiny
canonical USD scene as `pyxis_app/Resources/scenes/default.usd` and
loads it by default.

**Resolution order at startup**:
1. `--scene <path>` CLI argument, if present.
2. `parameters.json` `paths.scene`, if non-empty.
3. Most-recent entry in `%LOCALAPPDATA%/Pyxis/recent_scenes.json` whose
   file still exists, if `viewer.reopenLastScene = true` (default).
4. The bundled **default scene** at
   `<exe-dir>/Resources/scenes/default.usd`.

The bundled file is small (< 8 KB USDA text under a `.usd` extension —
USD's loader auto-detects the format), version-controlled, and
binary-identical across builds so it is also a useful smoke-test asset.

**Default scene contents** (`default.usd`):

| Element | Detail |
|---|---|
| Up axis | `Y` |
| Meters per unit | `1.0` |
| Default prim | `/PyxisDefault` |
| Ground | `UsdGeomMesh` plane (10 m × 10 m, 2 tris) at `/PyxisDefault/Ground` with a neutral grey OpenPBR material (`baseColor = 0.5`, `roughness = 0.7`) |
| Hero geometry | three `UsdGeomMesh` primitives at `/PyxisDefault/Sphere{Plastic,Metal,Glass}` — a 0.5 m unit sphere instanced three times via `UsdGeomPointInstancer` with three different OpenPBR materials (a coloured plastic, a polished metal, a transmissive glass). Showcases the three main BSDF lobes §11 implements. |
| Sky / dome light | `UsdLuxDomeLight` at `/PyxisDefault/Lights/Sky` referencing a tiny 64×32 lat-long EXR shipped at `Resources/scenes/default_sky.exr` (a procedural overcast-sky gradient, ~30 KB). Provides indirect lighting + a recognisable horizon. |
| Sun | `UsdLuxDistantLight` at `/PyxisDefault/Lights/Sun`, intensity tuned so the metal sphere shows a clean specular highlight. |
| Camera | `UsdGeomCamera` at `/PyxisDefault/Cameras/main` framing the three spheres + a slice of ground. 35 mm focal length, pinhole. |
| Animation | none (single time sample). |

**Why these contents**:
- Exercises every v1 renderer feature on the boot path: bindless
  textures (the dome EXR), TLAS with > 1 instance, all three
  light kinds we ship (§25.G), all three OpenPBR BSDF lobes,
  point-instancer flattening (§25.C), tone-mapping (§9.1).
- Loads in well under a second on any reference machine — the M0
  `pyxis.exe` smoke test still completes promptly.
- Visually obvious if anything is wrong: a flat-shaded grey window,
  a magenta sphere, or a missing horizon line each indicates a
  specific subsystem regression.
- Provides a stable composition for the M1–M3 viewer regression
  thumbnail (§35) without needing the Bistro asset locally.

**Behaviour**:
- The default scene is read-only on disk; "Save Scene As USD…" (§29.7)
  on top of it works by writing an overlay sublayer the user can save
  anywhere allowed by `paths.allowedRoots`. The bundled file itself is
  never overwritten.
- A one-line spdlog info message at load time identifies which path
  resolved (`scene.resolved.source = cli | config | recent | bundled`)
  so support tickets can confirm what was rendered.
- Headless `--headless` with no `--scene` still resolves through the
  same chain; CI can therefore run a smoke render of the default scene
  with zero scene arguments.
- `pyxis --print-default-scene-path` prints the absolute path of the
  bundled default and exits, useful for tooling.

### 29.5 Feature toggles & RenderGraph variant selection

Pyxis ships **one** RenderGraph definition; "features" are passes inside it
that can be enabled or disabled per frame via `RenderSettings::features`
flags (§17). The viewer exposes them through a **Features** ImGui panel.
This is *not* runtime graph editing — the set of toggleable features is
closed and pre-validated; what the panel does is flip booleans the
`RenderGraph::Compile` step (§9.2) reads when deciding which producers /
consumers to link.

| Feature flag (in `RenderSettings::features`) | Default | Effect on graph |
|---|---|---|
| `pathTracing` | on | mandatory; cannot be turned off (the panel shows it greyed) |
| `accumulation` | on | enables temporal accumulation; turning off resets every frame |
| `nee` | on | next-event estimation lights; off → BRDF-only sampling |
| `mis` | on | multiple-importance sampling between BRDF and lights |
| `denoise` | off | reserved (post-v1, §42) — panel shows but disabled |
| `toneMap` | on | enables `ToneMapPass`; off → swapchain receives raw HDR clamped (debug) |
| `taa` | off | temporal AA pass (deferred — see §15) |
| `motionVectors` | off | enables motion-vector AOV pass (required if `taa` or `denoise`) |
| `aovs.albedo / normal / depth / motion / instanceId / materialId` | off | enables the corresponding `AovExportPass` (§17.5) |
| `tracy` | off | drives `Profiler` Tracy emit |

Rules:
- Toggling a feature triggers `RenderGraph::Compile()` to re-run on the next
  frame; the compile result is cached by feature-mask hash so flipping
  back is instant (§9.2 cache).
- A toggle that would create a missing producer is rejected at the panel
  level (the checkbox is greyed with a tooltip explaining the dependency).
  The compiler also re-validates: an invalid combination still produces
  `Error::RenderGraphMissingProducer` rather than a silent miscompile.
- Feature state is part of the persisted `RenderSettings` and round-trips
  through `parameters.json`. Headless mode reads the same flags from the
  config; **the panel is viewer-only but the data is shared**.
- Determinism is preserved: §32's regression harness pins
  `RenderSettings::features` so toggling in the viewer cannot drift
  golden-image baselines.

### 29.6 AOV / render-target inspector

A **Inspector** panel (separate from Settings, separate from the
swapchain view) renders any internal render-graph texture full-frame, with
a pixel picker. Useful for debugging "why is this region black" without
recompiling.

Capabilities:
- **Texture selector** lists every `RgTextureRef` declared in the active
  graph: `Radiance.HDR.Accumulated`, `Albedo`, `Normal.View`, `Depth`,
  `MotionVectors`, `InstanceId`, `MaterialId`, `LDR.ToneMapped`, plus any
  `AovExportPass` outputs. Plus the imported swapchain (read-only).
- **Channel mode**: `RGB`, `R`, `G`, `B`, `A`, `Luminance`,
  `LinearDepth(viewZ)`, `LogDepth`, `MotionVectorRGB(visualize as 2D
  vectors with color wheel)`, `Normal(remap [-1,1]→[0,1])`,
  `IdHash(stable random color per integer id)`.
- **Range / exposure**: `min`, `max` floats, plus an **auto-range** button
  that scans the texture once on the readback path and clamps to
  [p1, p99]. For HDR textures (`R32G32B32A32_SFLOAT`,
  `R16G16B16A16_SFLOAT`) the default range is auto.
- **Pixel picker**: hovering shows `(x, y)` and the raw + decoded value
  (e.g. `Depth: 0.97312 → viewZ 14.3 m`; `MotionVectors: (0.0034, -0.0011)
  px = (4.6, -1.5) at 1080p`; `InstanceId: 0x0000_001F → instance #31`).
  Click to **pin** a probe — pinned probes persist across frames in a
  side list, each with its own historical sparkline (last 240 frames).
- **Save AOV**: per-texture button writes the current frame to EXR (HDR
  textures) or PNG (LDR), via the §19.5 single-frame save API
  (`SaveFrameExr` / `SaveFramePng`); paths sandboxed against
  `paths.allowedRoots`.
- **Cost model**: rendering the inspector copies the chosen graph texture
  into a small staging texture (1× per visible-frame), so the inspector
  itself adds < 0.2 ms even on the largest HDR target. Pixel picker
  reads from a 4-byte readback buffer with a 2-frame latency (avoids
  pipeline stalls).
- **Implementation note**: the inspector does not own a render-graph
  pass; it consumes the graph via the `RenderGraph::Resolve(RgTextureRef)
  → nvrhi::ITexture*` accessor (§9.2) inside an ImGui callback that
  records its own command list ordered after `Execute`.

This is the renderer's primary debug surface — anything that can be put
in an AOV can be inspected, picked, and dumped without recompiling.

### 29.7 Save current state as USD

The viewer's **File → Save Scene As USD…** entry serialises the *current
runtime state* into a USD layer that Pyxis can re-load with byte-identical
geometry, materials, lights, and camera.

What is captured:

| State category | Mechanism |
|---|---|
| Camera (current `CameraDesc`) | `UsdGeomCamera` at `/Pyxis/Cameras/main`, populated from `viewFromWorld` / `projFromView` / `focalLengthMm` / `apertureFStop` / `focusDistance` / clips |
| Modified materials (any `OpenPBRMaterialDesc` whose hash differs from its ingest-time hash) | `UsdShade` material network at `/Pyxis/Materials/<MaterialHandle>` using **`OpenPBRSurface`** as the shader id — round-trips losslessly with §11 since both sides speak OpenPBR. Unmodified materials are referenced from the source layer (no copy). |
| Material → instance bindings (rebound at runtime via Material Report) | `material:binding` rel updates on the affected prims |
| Modified instance transforms | `xformOp:transform` written on the instance prim (overrides the source) |
| Lights (current `LightDesc[]`) | `UsdLuxDistantLight` / `UsdLuxDomeLight` / `UsdLuxRectLight` under `/Pyxis/Lights/`; for dome lights, `inputs:texture:file` points at the *resolved* env-map path |
| Render settings snapshot | `customLayerData["pyxis:renderSettings"]` dictionary (mirrors §17 fields); not standard USD but harmless to other tools and consumed by Pyxis on re-load |
| Active feature mask (§29.5) | `customLayerData["pyxis:features"]` |

Layer authoring:
- Output layer is a single `.usda` (default) or `.usdc` (binary; user
  toggle) authored as an **overlay sublayer** referencing the original
  scene's root-layer asset path. Unchanged prims are not duplicated;
  only diffs and new prims are written.
- If the original scene was an in-memory anonymous stage (rare),
  the saved file is a flattened standalone layer.
- `/Pyxis/` namespace prefix avoids clashing with the user's own scene
  hierarchy. The Pyxis re-load path treats the prefix as authoritative
  (cameras under it override the source's primary camera, etc.).
- Asset-resolver context is preserved: relative texture paths in
  modified materials are written relative to the saved layer's
  directory.

Round-trip guarantees:
- Re-loading the saved layer in Pyxis is **idempotent**: the next "Save
  Scene As USD…" against the unchanged re-loaded state writes a layer
  whose textual diff vs the previous save is empty (modulo timestamps
  in `customLayerData`).
- Re-loading in *another* USD-aware tool (usdview, Houdini Solaris)
  shows the geometry, the camera, and any USD-standard lights
  correctly. `OpenPBRSurface` networks render correctly in any
  Hydra-2.0 renderer that speaks OpenPBR; older renderers fall back
  to `UsdPreviewSurface` via the §11 conversion which is also written
  alongside (`pyxis:fallbackPreviewSurface`).
- **Not** captured: profiler history, ImGui panel state, recent-files
  list, AOV inspector probes — those are viewer ephemera, not scene
  state.

Failure modes:
- Output path outside `paths.allowedRoots` → `Error::FilePermissionDenied`,
  no file written.
- Disk full / write error → `Error::FileCorrupt` (we never partially
  overwrite an existing file; the writer goes to `<path>.tmp` then
  atomically renames on success).
- Unsupported scene element (e.g. material kind that conversion rejects)
  → entry is skipped with a per-prim log line and the saved layer is
  marked `customLayerData["pyxis:savePartial"] = true`. Pyxis re-loading
  a partial save behaves identically to loading any incomplete USD.

This is **not** a USD authoring tool (no UI for adding new prims, no
sublayer composition editor). It is "freeze the current viewer state to
disk so I can ship it to a colleague" — the primary use cases are:
saving a debug repro of a customer issue, persisting a hand-tweaked
lookdev session, and round-tripping into the regression harness as a
deterministic input.

Headless mode exposes the same path via `--save-scene <out.usda>` (writes
once, after the first frame, then exits).

---

# Part VI — Coding Rules, Threading, GPU Synchronisation

## 30. C++ Coding Rules


These rules are normative; reviewers reject PRs that violate them. Inspired by Aurora's coding
standards and modern C++ Core Guidelines, narrowed for this codebase.

### 30.1 Language and dialect
- **C++23** required (for `std::expected`, `std::print`-readiness, `if consteval`,
  `[[assume]]`, deducing `this`). Toolchain: clang-cl 17+ / MSVC STL with C++23 mode
  (`/std:c++latest`). Allowed C++20 features remain available: concepts, `<ranges>`
  (read-only), `<span>`, `<bit>`, designated initializers, `consteval`, three-way
  comparison, `<format>`.
- **Forbidden**: RTTI in renderer/platform (`/GR-` on those targets — Hydra layer needs RTTI
  for `pxr` so it stays on `/GR`); exceptions across DLL boundaries (catch and convert at
  the boundary); STL streams (`<iostream>`); raw `new` / `delete` outside thin RAII wrappers.
- **Compiler**: clang-cl, MSVC ABI. Flags: `/std:c++latest /W4 /WX /external:I` for thirdparty,
  `/permissive-`, `/Zc:preprocessor`, `/Zc:__cplusplus`, `/Zc:inline`, `/utf-8`,
  `/EHsc` (Hydra layer) or `/EHs-c-` (renderer/platform — no exceptions).
- Warnings whitelisted only via per-target push/pop; never globally.

### 30.2 Naming
| Kind | Convention | Example |
|---|---|---|
| Type (class/struct/enum/alias) | `PascalCase` | `GpuScene`, `MeshHandle` |
| Free function | `PascalCase` | `LoadParameters` |
| Member function | `PascalCase` | `AppendInstance` |
| Static method | `PascalCase` | `BlasCache::EstimateScratchSize` |
| Public field of POD struct | `camelCase` | `RenderSettings::maxBounces` |
| Private field | `_camelCase` | `_uploadQueue` |
| Local variable | `camelCase` | `triangleCount` |
| Constant (compile-time, including `constexpr`/`static constexpr` members) | `UPPER_SNAKE_CASE` | `MAX_BINDLESS_TEXTURES`, `HANDLE_SLOT_BITS` |
| Enum constant (`enum class` member) | `PascalCase` (no prefix) | `MaterialFlag::DoubleSided`, `MeshHandle::Invalid` |
| Macro | `PYXIS_SCREAM` (UPPER_SNAKE_CASE with `PYXIS_` prefix) | `PYXIS_DEBUG_TOOLS` |
| Namespace | `lowercase` | `pyxis` (single, flat — no per-module sub-namespace) |
| File | `PascalCase.{h,cpp}` matches primary type | `GpuScene.h` |
| Slang/HLSL identifier | `camelCase` for vars and entry points (`raygenMain`), `PascalCase` for structs | `OpenPBRMaterialGPU` |

Rationale: PascalCase free / member functions match the engine-style codebases this
project draws from (Aurora, donut, the `Application::Run` / `SceneRenderer::Render`
shape) and read uniformly across renderer, ingest, and platform code.
`SceneMetadata::AppendMesh` style and Aurora's API. `camelCase` is reserved for *data*
(local variables, POD fields, loop indices), so a quick visual scan separates code (verbs)
from state (nouns). Slang shader entry points stay `camelCase` because that's the HLSL/Slang
convention and matches the SPIR-V tooling expectations.

- **No Hungarian, no `m_` prefixes, no `s_` for statics.** Statics use file-local
  anonymous namespaces.
- **Acronyms count as words**: `BlasCache` not `BLASCache`, `GpuScene` not `GPUScene`,
  `aovId` not `AOVId`. Exception: 2-letter acronyms keep both letters cased (`UI`, `IO`).
- One primary type per header; matching `.cpp`. Helpers get `*Internal.h` if unavoidable.
- **No short / abbreviated identifiers.** Variables, parameters, and loop counters
  must be at least 3 characters. The role should read off the name —
  `commandList`, `deviceManager`, `result`, `width`, `height`, `event`, `iter` —
  not `cl`, `dm`, `r`, `w`, `h`, `e`, `it`. The build enforces this via
  clang-tidy's `readability-identifier-length` check; PRs reintroducing
  short names fail the gate.
  - **Sole exemption: `i` / `j` / `k` for loop indices.** Universal C-family
    convention; encoded as `IgnoredLoopCounterNames: '^[ijk]$'` in `.clang-tidy`.
  - Where a name would have been a 1- or 2-letter abbreviation tied to a
    type (e.g. `nvrhi::ICommandList* cl`), prefer the type's natural noun
    (`commandList`); when the type itself is a handle, pair them as
    `commandListHandle` (the `nvrhi::CommandListHandle`) plus `commandList`
    (the borrowed pointer obtained via `.Get()`).

### 30.3 Headers and includes
- **The public surface is deliberately narrow.** A header is `Public/` only if a consumer
  outside the target needs it. The renderer's public surface is exhaustively:
  `Forward.h`, `RendererApi.h`, `Error.h`, `GpuScene.h`, `PyxisRenderer.h`,
  `Profiler.h`, and the POD `Descs/*.h` they take as parameters
  (`MeshDesc`, `OpenPBRMaterialDesc`, `TextureKey`, `InstanceDesc`, `CameraDesc`,
  `LightDesc`, `RenderSettings`, `RenderTargets`, `FrameStats`, `FrameProfile`).
  Everything else — `MeshTable`, `BlasCache`, `TlasBuilder`, all passes, the RenderGraph,
  the upload/deletion queues, `OpenPBRMaterialGPU` (GPU layout), `ShaderLibrary`,
  `DescriptorTableManager`, NVRHI handles inside `GpuScene` — lives in `Private/` and is
  inaccessible to `pyxis_hydra` and `pyxis_app`.
- The verbs the Hydra layer needs (material change, camera move, instance move,
  `CreateInstance`, `CreateTexture`, `CreateMesh`, light add/update, visibility toggle,
  `CommitResources`, `RenderFrame`) are all methods on `GpuScene` /
  `PyxisRenderer` — no other public type is required to drive a frame.
- Public headers must compile with `/W4 /WX` against consumer code, must not include
  `<windows.h>`, must not transitively pull `pxr/...` (the renderer is USD-free), must not
  expose NVRHI implementation types beyond opaque forward-declared interface pointers
  (`nvrhi::IDevice*`, `nvrhi::ICommandList*`), and must not include any other third-party
  header.
- Private headers in `Private/`. May include anything visible to that target.
- Include order, blank line between groups:
  1. Matching header (in `.cpp`).
  2. Pyxis public headers.
  3. Pyxis private headers.
  4. NVRHI / Vulkan headers.
  5. Third-party.
  6. Standard library.
- All includes use `<>` for thirdparty and public Pyxis API, `""` for the same-target
  private headers.
- `#pragma once` only; no include guards.
- No transitive includes — every `.cpp` includes what it uses.

### 30.4 Types and ownership
- **Smart pointers**: `std::unique_ptr<T>` for sole ownership; `std::shared_ptr<T>` only when
  truly shared (NVRHI `RefCountPtr` for NVRHI handles — that's the library's idiom).
  No `std::shared_ptr<T>` for "I don't know who owns it"; refactor instead.
- **Out-parameters**: forbidden. Return by value (`std::optional<T>`, `std::expected<T, Error>`,
  or aggregate). Status codes: `std::expected` (C++23, mandatory — see §30.1).
- **References vs pointers**: non-owning references for "must exist" arguments; raw pointers
  for nullable. Pointers never own.
- **`const`-correctness**: every member function that does not mutate visible state is `const`;
  every parameter that is not mutated is `const T&` (or `T` for trivially copyable, ≤ 16 bytes).
- **POD structs** (config, GPU layouts, `RenderSettings`): default-constructible, all members
  public, no non-trivial constructors, only data. Use designated initializers at use sites.
- **Strong types** for handles: `MeshHandle`, `MaterialHandle`, etc. — `enum class` over
  `uint32_t` with `Invalid = 0`. No bare `uint32_t` indices crossing module boundaries.
- `final` classes by default if not designed for inheritance.
- `[[nodiscard]]` on every factory and every `std::expected`-returning function.

### 30.5 Functions
- ≤ 50 lines as a guideline; > 100 requires reviewer justification.
- ≤ 5 parameters; group with a `Desc` struct beyond that. Aurora-style:
  `class Foo { Foo(const FooDesc& desc); };`.
- No default arguments on virtual functions; no default arguments on public API.
- Pure functions live in anonymous namespaces in `.cpp`.
- `noexcept` on every move ctor/op, every dtor, and every function in the renderer/platform
  no-exceptions perimeter.

### 30.6 Error handling
Three tiers, applied per-call-site:

| Tier | Mechanism | Use for |
|---|---|---|
| **Programmer error** | `PYXIS_ASSERT(cond, fmt, ...)` (Debug-only); `PYXIS_VERIFY(cond, fmt, ...)` (always-on, fires in Release too — use for invariants whose violation must abort the process even in shipped builds) | Pre-conditions, invariants. Never recoverable. |
| **Recoverable runtime error** | `std::expected<T, Error>` returned, propagated up | I/O, USD parse, shader compile, Vulkan-feature missing |
| **Fatal hardware/driver** | log; flush every spdlog sink (`pyxis::Logging::Get().Flush()` \u2014 \u00a730.10); flush Tracy; then `std::terminate` via `PYXIS_FATAL` | OOM after fallback, lost device, validation error |

- No silent failures, ever. Every `std::expected` failure is logged at the boundary that
  decides to handle it, with category + sdfPath + cause.
- The renderer never throws; the Hydra layer may translate USD/MaterialX exceptions to
  `std::expected` at its boundary.

### 30.7 Comments and documentation
- Doxygen-style on public headers (`///`). Private files: `//` only when the *why* is not
  obvious; never restate the *what*.
- Every public class header: one paragraph stating purpose, lifetime, and threading model.
- Every public function: one line + parameter notes for non-obvious ones.
- TODO format: `// TODO(<owner>, <ticket-or-section>): ...`. PR reviewer rejects untagged TODOs.

### 30.8 Const-correctness, immutability, value semantics
- Prefer immutable POD descriptors (`MeshDesc`, `MaterialDesc`, `RenderSettings`) created
  once and consumed read-only.
- Public APIs never expose mutable references to internal containers.
- Move semantics on everything that owns memory; rule of zero by default; explicit rule of
  five with `noexcept` if a destructor exists.

### 30.9 Concurrency rules
- Every public class header documents thread-safety as one of:
  `single-threaded`, `thread-safe`, `producer-consumer (described)`, `frame-sync (called only
  on render thread)`.
- No global mutable state. Singletons forbidden with two narrow exceptions:
  (a) `pyxis::Logging::Get()` returns the single platform-owned logger (§33.10);
  (b) the Tracy client is process-global by Tracy's design. Everything else is
  passed by reference — the `Profiler` is owned by `Application`.
- Atomics only via `std::atomic<T>`; never via volatile.

### 30.10 Memory and allocation
- No allocations in render-pass `Execute()` — preallocate in `Declare()` / on-resize.
- One frame-scoped scratch arena (`FrameAllocator`) for per-frame transient CPU data.
- No `new T[]`; use `std::vector<T>` with `reserve()` or `std::pmr` arenas for hot paths.

### 30.11 Flecs conventions

These rules apply only inside `pyxis_renderer/Private/Scene/`. No Flecs type appears in
any `Public/` header.

- **Components are POD structs** in `Private/Scene/Components/<Name>.h`, one type per
  header. No `std::vector`, no `std::string`, no virtuals — fixed-size data only.
  Variable-length data lives in dedicated `Private/GpuScene/` tables and is referenced
  by handle. This keeps Flecs archetypes small and predictable.
- **`Dirty<T>` is a tag component** (zero-size). Add via `e.add<Dirty<Topology>>()`,
  remove via `e.remove<Dirty<Topology>>()`. Systems query for `Dirty<T>` and clear it in
  `System_ClearDirtyFlags` after the per-phase work is committed.
- **Systems are free functions** in `Private/Scene/Systems/<Verb>.cpp`, named
  `System_VerbObject` (e.g. `System_BuildDirtyBlas`, `System_RebuildTlas`). They take
  the iterator/query parameters Flecs provides and nothing else; all renderer-wide
  state (NVRHI device, GpuScene tables, UploadQueue) is reached via the `SceneWorld`
  context pointer set at registration time.
- **Queries are cached** at registration time in `Private/Scene/Queries/QueryCache.h`.
  Calling `world.query_builder<...>()` inside a per-frame system body is a PR-blocking
  violation; queries built per frame allocate and defeat archetype caching.
- **Prefer relationships (pairs)** `(Instance, MaterialOf, materialEntity)` over
  components-with-entity-fields. Pairs are first-class in Flecs queries; entity-field
  components are not, and lead to manual joins.
- **Observers** (`world.observer<...>().event(flecs::OnRemove)`) are reserved for
  cross-component invariants: refcount drops to zero, `DeletionQueue` scheduling, BLAS
  release. Do not use observers as a substitute for systems.
- **Phase pipeline is custom and explicit.** Built-in `flecs::OnUpdate` etc. are not
  used; all systems run inside the `PYXIS_PHASE_*` pipeline registered in
  `Private/Scene/Phases.h`. Reordering phases or inserting between them requires an
  RFC (§44).
- **Mutation is single-writer.** Only the render thread calls `world.entity(...)`,
  `e.set<T>(...)`, or `e.destruct()`. Ingest threads push `MutationCommand` records
  into the `moodycamel::ConcurrentQueue` drained at the start of `CommitResources`.
- **Flecs Explorer (REST UI)** is gated behind `PYXIS_DEBUG_TOOLS` (Debug builds only),
  served on `http://localhost:27750` via the `flecs[rest]` vcpkg feature.

---

## 31. Threading Model


Three logical threads (some may share an OS thread on small machines):

| Thread | Owns | Talks to |
|---|---|---|
| **Main / render thread** | NVRHI device, swapchain, ImGui, command-list submission, `PyxisRenderer::RenderFrame`, `GpuScene::CommitResources` | reads from upload-completion queue |
| **Ingest thread** | active adapter's stage state — `UsdStage` plus either (`HdRenderIndex` + scene-index filters + `HdEngine::Execute(Sync)`, when adapter = `pyxis_hydra`) or `StageWalker` (one-shot, when adapter = `pyxis_usd_ingest`; the stage is released after the snapshot is emitted) | writes to `GpuScene` via the renderer's public API; uploads buffers via the upload pipeline |
| **Asset I/O pool** (`N = std::clamp(std::thread::hardware_concurrency() / 2, 2u, 8u)` workers) | texture decode, mesh-primvar extraction, MikkTSpace, EXR/PNG decode | produces `PendingTextureUpload` / `PendingMeshUpload` records into a thread-safe queue |

Rules:

- The **ingest thread** owns the active adapter's stage state — either
  `UsdStage` + `HdRenderIndex` + scene-index filters + `HdEngine::Execute(Sync)`
  (`pyxis_hydra`), or `UsdStage` + `StageWalker` running once to emit an
  `IngestSnapshot` (`pyxis_usd_ingest`; no `UsdNotice` listener — post-v1
  per §40.4 / §42). Both call `GpuScene::CreateMesh` etc. — these are documented
  `producer-consumer`: enqueue CPU-side records, no GPU calls. Only
  `CommitResources()` (called on the render thread) touches the GPU.
- **Tracy zone naming convention** (so a single capture is readable across modes):
  top-level dot-prefix names every scope by component:
  - `ingest.hydra.*` — Hydra adapter (`hydra.init`, `hydra.primDiscovery`,
    `hydra.firstSync`, `hydra.sync`).
  - `ingest.usd.*` — USD-direct adapter (`usd.stageOpen`, `usd.stageWalk`,
    `usd.notice`).
  - `ingest.shared.*` — `pyxis_material_translation` work shared by both adapters
    (`material.network.extraction`, `material.openpbr.conversion`).
  - `assets.*` — I/O pool (`assets.texture.pathResolve`, `assets.texture.decode`,
    `assets.mesh.extraction`, `assets.mesh.triangulation`, `assets.primvar.extraction`).
  - `render.*` — render thread (`render.commitResources`, `render.frame.cpu`,
    `render.frame.firstFrame`, `render.frame.timeToFirstImage`, `render.geometry.upload`,
    `render.texture.upload`, `render.blas.build`, `render.tlas.build`).
  - `app.*` — application UI / config / reporting (`app.imgui`, `app.config.load`,
    `app.report.write`).
  The names listed in §34 conform to this scheme; older callsites without a
  component prefix are renamed in M11.
- **Asset I/O is fork-join per `CommitResources`**: the ingest thread submits decode jobs
  and waits in a barrier before requesting `CommitResources`. Texture decode never blocks
  the render thread.
- **No shared mutable state** between threads except via the documented queues and atomics.
- **ImGui is render-thread-only**, behind `PYXIS_DEBUG_TOOLS`.
- **Tracy zones** must use the same thread the work runs on; never re-tag work in another
  zone.

---

## 32. Lifetime, Threading, and Device Sharing


### 32.1 NVRHI device sharing

§18's constructors accept `nvrhi::IDevice*` precisely so hosts that already
have an NVRHI device (Houdini / Maya plugins, sample apps embedding Pyxis side-by-side) can pass it in. v1 contract:

- The device is **borrowed**, never owned. Pyxis does not release the device
  in `~GpuScene` or `~PyxisRenderer`. The host must outlive both.
- The device must satisfy §5.b's required features. `GpuScene`'s constructor
  performs the feature query and returns
  `ErrorKind::FeatureMissing` if the borrowed device falls short — never
  mutate the host's device, never enable extensions on it.
- Both `pyxis_platform`'s `VkDeviceManager` and an externally-supplied device
  must agree on the validation-layer state; the constructor logs which path
  is in use.
- The host owns command-list submission. `PyxisRenderer::RenderFrame` and
  `GpuScene::CommitResources` *populate* a command list provided by the
  caller; they never call `device->executeCommandList`. (This was already
  implied by §18's signatures; making it normative here.)
- A "Pyxis-owned device" path is preserved for the standalone `pyxis.exe`:
  `pyxis_platform::CreateOwnedDevice(...)` returns a
  `unique_ptr<nvrhi::IDevice, OwnedDeviceDeleter>` that *does* destroy on
  scope-exit.

### 32.2 Thread-safety of destruction

- `~GpuScene()` and `~PyxisRenderer()` are **render-thread-only** and require
  every in-flight frame to have completed (the host must wait on its own
  per-frame fences). Failing this is a programmer error: `PYXIS_VERIFY` fires
  in Debug; Release behaviour is undefined.
- Calling any `GpuScene` mutation verb from a non-render thread *during*
  destruction (e.g. an ingest thread that hasn't been joined) is undefined
  behaviour. Hosts must join their ingest workers before destroying
  `GpuScene`.
- `Profiler` destruction follows the same rule.
- The `CancellationToken` (§19.2) is the recommended way to drain ingest
  threads cleanly before destruction.

---

## 33. Detailed GPU Synchronization & Resource Rules


NVRHI handles most barriers; the rules below are what we add on top.

### 33.1 Frames in flight
- `inline constexpr uint32_t MAX_FRAMES_IN_FLIGHT = 3;` is the compile-time upper bound
  used to size every per-frame ring (command lists, binding sets, staging buffers,
  deletion lists, query pools). It is the dimension of every `std::array<..., MAX_FRAMES_IN_FLIGHT>`
  in the codebase.
- `GpuSceneCreateDesc::framesInFlight` (default 2; 3 for headless determinism) is the
  *active* count actually in flight at runtime; the renderer asserts
  `framesInFlight <= MAX_FRAMES_IN_FLIGHT` at construction.
- One semaphore per active frame.
- `DeletionQueue::flush(currentFrame - framesInFlight)` runs at start of frame.

### 33.2 Resource state
- All persistent resources created with `keepInitialState = true` and an explicit
  `initialState` matching first use. NVRHI tracks transitions per command list.
- For resources reused across frames (vertex/index/material buffers), state is
  `nvrhi::ResourceStates::ShaderResource` at frame end. We assert this in Debug.
- **Vertex/index buffers consumed by BLAS builds** transition
  `ShaderResource ↔ AccelStructBuildInput` inside `System_BuildDirtyBlas`; they
  return to `ShaderResource` for the rest of the frame. NVRHI handles the actual
  barrier insertion when both states are declared on the same command list.
### 33.3 Bindless table updates
- `DescriptorTableManager` writes are batched in `CommitResources`. No mid-frame writes.
- Writes only to slots that were vacant at the *start* of `framesInFlight` frames ago,
  guaranteed by routing destruction through `DeletionQueue`.

### 33.4 Acceleration structures
- BLAS builds: `CommitResources` issues `BuildAccelStructs()` followed by the compaction
  sub-pass for builds whose flag set contains `ALLOW_COMPACTION`. Both happen on the
  graphics queue with explicit timestamps.
- TLAS rebuild: always on the graphics queue, after BLAS, before `PathTracePass`.
- Scratch memory: a single dedicated scratch buffer sized to
  `max(blasBatchScratch, tlasScratch)`. Never aliased with vertex/index/staging.

### 33.5 Staging
- One ring (default 256 MiB), one `std::atomic<uint64_t>` head, one tail per frame.
- `EnqueueUpload(srcBytes, dstBuffer, offset)` returns `std::expected<UploadId, Error>`.
- Oversize uploads (> ringSize / 4) bypass the ring and use a one-shot allocation freed
  via `DeletionQueue`.

### 33.6 Command-list discipline
- One command list per pass in v1; passes don't share command lists.
- All passes call `cl->beginMarker(name)` / `endMarker()` (RAII wrapper `CommandListMarker`).
- Validation layer enabled in Debug; CI fails on any validation-layer message tagged
  `Error` or `Warning` with severity ≥ `Performance`.

### 33.7 Determinism
- Headless mode pins frame index, RNG seed, instance ordering (sorted SdfPath), texture
  upload order. Same input → byte-identical EXR.

### 33.8 Pipeline cache (cold-start mitigation)
- `VkPipelineCache` persisted to
  `%LOCALAPPDATA%/Pyxis/PipelineCache/<pipelineCacheUUID>-<slangVersion>.bin`.
  Primary key: `VkPhysicalDeviceProperties::pipelineCacheUUID` (encodes device +
  driver + ABI atomically; the only Vulkan-blessed invalidation token). Secondary
  key: Slang compiler version, since we own the SPIR-V it produces.
- Loaded at device creation; on mismatch / corrupt blob the cache is rebuilt silently.
- Also covers ray-tracing-pipeline state objects (RT PSO creation can take 10–30 s on a
  cold driver). Without this every clean CI machine eats minutes per first run.

### 33.9 Crash diagnostics (Aftermath)
- Aftermath / Nsight Capture is gated by CMake (`PYXIS_ENABLE_AFTERMATH=ON`,
  Debug-only, Windows-only). When enabled:
  1. SDK headers copied into the build tree (the standard Aftermath integration pattern); `nvrhi-vulkan` is built with Aftermath markers.
  2. Every render pass calls `cl->beginMarker(name)` (RAII `CommandListMarker`) so
     Aftermath captures localise the failure to a pass.
  3. The path-tracing closesthit shader writes a per-pixel-tile breadcrumb into a
     small UAV `u_pyxisBreadcrumb : RWStructuredBuffer<uint>` sized
     `tileCount = ceil(width / 16) * ceil(height / 16)`. Each closesthit invocation
     does an `InterlockedMax` on its tile slot of
     `(passId << 24) | (frameId << 8) | bounceIndex`, never a non-atomic write — the
     prior "single uint with raw store from many shader threads" design was a race.
     On `DeviceLost` the buffer is read back and the saturated tiles are logged so
     the failing region of the framebuffer is identifiable.
  4. On `nvrhi::IDevice` reporting `DeviceLost`, the Aftermath GPU dump (`*.nv-gpudmp`)
     is written to `%LOCALAPPDATA%/Pyxis/Crashes/<timestamp>/`, the breadcrumb is logged,
     and the process exits with code `4` (device-lost).
- Without Aftermath enabled, the breadcrumb UAV is omitted (no shader cost) and
  `DeviceLost` simply logs + exits 4.

### 33.10 Cross-DLL logging contract (single spdlog registry)

spdlog uses thread-local default-registry storage; if `pyxis_renderer.dll`,
`pyxis_hydra.dll` and `pyxis_usd_ingest.dll` each statically link spdlog (vcpkg's
default), each DLL gets its **own** registry and `spdlog::default_logger()` returns a
different sink per DLL. The single-source-of-truth rule for v1:

- **spdlog is linked as a SHARED library**, not `spdlog_header_only`. vcpkg builds
  spdlog static by default on `x64-windows`; we override per-port in `vcpkg.json`
  via `"spdlog": { "features": [], "$linkage": "dynamic" }` (vcpkg's per-port
  linkage override) **and** set `BUILD_SHARED_LIBS=ON` for the spdlog FetchContent
  fallback. CI runs `dumpbin /exports pyxis_platform.dll | findstr spdlog` and fails
  if spdlog symbols are statically inlined into `pyxis_platform.dll`.
- `pyxis_platform` exposes `pyxis::Logging::Get()` returning the *one* `pyxis::Logger&`
  (a thin wrapper around `std::shared_ptr<spdlog::logger>` plus the dotted-category
  conventions from §34). The wrapper is the public type — spdlog itself never appears
  in any `Public/` header outside `pyxis_platform`.
- **No code outside `pyxis_platform` ever calls `spdlog::default_logger()` directly,
  nor uses `SPDLOG_INFO(...)` macros.** Other DLLs reach the logger through
  `pyxis::Logging::Get()`. `Profiler` is constructor-injected because its lifetime
  is bound to the GPU device and it must be created/destroyed in lockstep with
  `nvrhi::IDevice`; the logger by contrast is process-scoped and outlives the
  device, so a free-function accessor is the symmetric choice. The accessor is
  the single documented exception to the §30.9 "no singletons" rule (along with
  Tracy's client).
- Tracy's client is linked SHARED for the same reason; same rule applies.
- Reviewers reject any `spdlog::log(...)` / `SPDLOG_INFO(...)` usage inside
  `pyxis_renderer/Private`, `pyxis_hydra/Private`, `pyxis_usd_ingest/Private`.

---

# Part VII — Quality & Process

## 34. Profiling & Instrumentation


Pyxis distinguishes **two profiling regimes** that share infrastructure but
differ in goal, granularity, and report shape:

| Regime | Goal | Frequency | Granularity | Primary report |
|---|---|---|---|---|
| **Load-time profiling** (§34.1) | shrink time-to-first-image, surface ingest bottlenecks | one-shot per scene load | coarse (USD open, prim discovery, mesh extraction, texture decode batches, BLAS/TLAS build) | timeline / waterfall view, end-of-load summary table |
| **Per-frame profiling** (§34.2) | shrink steady-state frame ms, surface per-pass regressions | every frame (240-frame rolling history) | fine (per render-graph pass GPU timestamps, per-system Flecs CPU scopes, commitResources phases) | rolling sparklines + flame bar, p50/p99 over window, CSV export |

Both regimes write into the **same** `ProfilerData` / `FrameProfile`
structures and share the backend pipeline below; the difference is which
scopes light up and how the data is presented.

### Architecture (shared)
- `Profiler` is a singleton-equivalent passed by reference (`Profiler&`); ownership lives in `Application` and every consumer takes a borrowed reference.
- Owns `ProfilerData` (CPU scope tree) and `FrameProfile` (per-frame GPU pass timings).
- Backends are pluggable: spdlog summary, ImGui panel, JSON writer, CSV writer, Tracy,
  external regression sink. The profiler **does not depend on** any backend.

```
ProfilingScope (CPU, RAII)   ──►   ProfilerData
ProfilingPass (GPU, RAII)    ──►   GpuTimestampPool ──► FrameProfile

ProfilerData + FrameProfile  ──►   spdlog summary
                              ──►   ImGui panel (viewer)
                              ──►   JSON / CSV (headless)
                              ──►   Tracy (optional)
                              ──►   external test harness (regression KPI)
```

ImGui and spdlog do not own profiling data. They are output backends.

### 34.1 Load-time profiling

**Scope.** Everything that runs once when a scene is opened (cold start) or
re-opened (`§29.4` runtime swap). Goal is to drive **time-to-first-image**
down by surfacing which phase dominates, not to micro-optimise hot loops.

**Named CPU scopes** (must exist from day 1):
- `ingest.usd.stageOpen`, `ingest.usd.populationMask`, `ingest.usd.composition`
- `ingest.hydra.init`, `ingest.hydra.primDiscovery`, `ingest.hydra.firstSync`
  *(or `ingest.usd_ingest.stageWalk` when adapter = `pyxis_usd_ingest`)*
- `assets.mesh.extraction`, `assets.mesh.triangulation`, `assets.primvar.extraction`,
  `assets.mesh.mikktSpace`
- `ingest.shared.material.network.extraction`,
  `ingest.shared.material.openpbr.conversion`
- `assets.texture.pathResolve`, `assets.texture.decode` (per-decode + total)
- `render.texture.upload`, `render.geometry.upload`,
  `render.blas.build` (per-mesh + total), `render.tlas.build` (first build),
  `render.commitResources` (first call)
- `render.frame.firstFrame`, `render.frame.timeToFirstImage`

**Reports.**
- **spdlog end-of-load summary** (always emitted): one table with each
  named scope's wall-time, % of total load, peak parallelism. Goes to
  log file and to the Console panel.
- **`out/load_profile.json`** (headless `--profile` or viewer "Save load
  profile"): full Chrome-tracing-format event stream of every scope,
  loadable in `chrome://tracing` / Perfetto for waterfall analysis.
- **Viewer ImGui `LoadTimelineView`**: a Gantt-style waterfall of the
  load phases plus per-thread swim lanes (asset I/O pool members, ingest
  thread, render thread). Read-only; persists last load even after the
  next frame starts.

**KPIs gated on load-time profiles** (§34.3).

### 34.2 Per-frame profiling

**Scope.** Everything that runs every frame in steady state. Goal is to
hold the per-pass GPU and per-system CPU budgets and catch regressions
the moment they land.

**GPU pass timestamps** (timestamp pairs around each render-graph pass):
- `pass.PathTrace`, `pass.Accumulation`, `pass.Denoise`, `pass.ToneMap`,
  `pass.AovResolve`, `pass.DebugView`, `pass.CopyToHydraBuffer`,
  `pass.PresentBlit`
- For each pass we record: resolution, sample count, dispatch size, output format.
- Steady-state-only: `accel.TlasBuild` (refit cost on dirty), `accel.BlasBuild`
  (only when a mesh changes; usually 0 ms post-warm),
  `texture.UploadCopy` (only when textures stream in mid-flight).

**CPU per-frame scopes:**
- `frame.cpu` (whole frame), `frame.cpu.flecsTick`,
  `frame.cpu.commitResources`, `frame.cpu.renderGraph.compile`,
  `frame.cpu.renderGraph.execute`, `frame.cpu.recordCommandLists`,
  `frame.cpu.imguiBuild`, `frame.cpu.present.wait`
- One scope per Flecs system that runs in the render-frame schedule
  (auto-named from the system identifier so adding a system surfaces
  immediately).

**Window.** 240 frames (≈ 4 s at 60 fps) rolling, kept in a fixed-size
ring. p50 / p95 / p99 / max are recomputed each frame from the ring.

**Reports.**
- **Performance panel** (§29.3): live sparklines, p99/p50, per-pass flame
  bar.
- **`out/frame_profile.csv`** (headless or viewer "Copy CSV"): one row
  per frame, one column per scope. Drop into a spreadsheet, diff across
  builds.
- **Tracy** (optional, off by default): every CPU and GPU scope feeds
  Tracy when `RenderSettings::features.tracy = true`. Tracy server
  version is pinned in `vcpkg.json` (§4) and verified at startup.
- **`Profiler` panel** (§29.3): zone view rendered from the live ring;
  the in-process backup for when Tracy is unavailable.

**KPIs gated on per-frame profiles** (§34.3).

### 34.3 Profile-driven optimisation policy

Pyxis follows one rule: **measure, then optimise.** Both regimes feed it:

- No micro-optimisation lands without a Tracy/Nsight zone or a
  `FrameProfile` / `ProfilerData` entry proving the cost. PRs that say
  "this might be faster" without a number attached are rejected.
- **Per-frame KPIs** (1080p hero camera, RTX 4080 reference, post-warm):
  - `pass.PathTrace` < 12 ms
  - `pass.Accumulation` + `pass.ToneMap` + `pass.AovResolve` < 2 ms combined
  - `frame.cpu.commitResources` < 2 ms steady state
  - p99 / p50 frame ratio < 1.4 (catches stalls / GC)
- **Load-time KPIs**:
  - `render.frame.timeToFirstImage` for Bistro (M8a) < 15 s on the
    lab machine
  - `assets.texture.decode` parallelism ≥ 6 of 8 worker threads sustained
  - `render.blas.build` ≥ 4 builds in flight at peak
- Any commit that regresses a pinned KPI by > 10 % is reverted by
  default; explicit override requires a paragraph in the PR description
  and one extra reviewer.
- **Anti-patterns forbidden without profile evidence**: pre-batching BLAS
  builds beyond the simple per-frame queue, growing the bindless
  capacity above 80k, hand-vectorising hot loops, replacing
  `std::vector` with custom allocators, adding shader specialisation
  constants.

### 34.4 Memory profile (cross-cutting)

Tracked once at end-of-load and once per second in steady state:
- CPU RSS (Windows `GetProcessMemoryInfo`).
- GPU memory (`VkPhysicalDeviceMemoryBudgetPropertiesEXT` if available).
- Per-bucket: vertex, index, texture (decoded vs resident), BLAS, TLAS,
  scratch, staging, AOV / RT.
- **Three watermark snapshots** kept for the run: at end-of-load, at
  first frame, at steady-state (taken 60 s after first frame). The
  spread between load-peak and steady-state surfaces leaked staging
  buffers and undeleted upload scratch.

---

## 35. Testing, Image-Regression, Headless


### Architecture
- Unit tests (`pyxis_unit_tests`, gtest): math, OpenPBR conversion, hashing,
  topology helpers, dirty-bit handling, config parser. Tiny scenes via in-memory `.usda`.
- Regression tests live in `tests/regression/` as a Python harness invoking
  `pyxis.exe --headless --config <fixture>`. They are not linked into the renderer.
- The renderer never embeds the test framework.

### Regression flow
1. Test harness picks a fixture directory (scene + `parameters.json` + baseline).
2. Spawns `pyxis.exe --headless --config <fixture>/parameters.json`.
3. Renderer loads → renders deterministically → writes EXR to `output.image`.
4. Harness loads EXR + baseline EXR → computes RMSE / MAE / PSNR / SSIM (FLIP later).
5. Per-test tolerance decides pass/fail.
6. On failure: write diff EXR + stash logs as artifacts.

### Headless requirements
- No window, no ImGui, no input.
- Deterministic settings: fixed resolution, camera, sample count, seed, exposure, tone map,
  max bounces. Regression fixtures **must** set `accumulationFrameLimit > 0` and `seed > 0`
  explicitly; the headless config loader rejects either being zero (§18.4 `RenderSettings::seed`,
  §21.2 `accumulationFrameLimit`) so EXR output is byte-identical across runs.
- Output to EXR (linear) and optional PNG (tone-mapped, sRGB).
- Returns 0 on success, non-zero on failure.
- Same renderer core as viewer.

### External harness requirements
- Subprocess execution; capture stdout/stderr; check exit code; diff image; write artifacts.
- No access to renderer internals; image-only regression artifact in v1.

### Test scenes
- Tiny in-tree `.usda`: triangle, quad-triangulation, missing-normals quad, authored-normals
  quad, UV cube, multi-subset cube, double-sided plane, native-instanced cubes,
  point-instanced rocks, BLAS-sharing scene, UsdPreviewSurface, MaterialX standard_surface,
  MaterialX open_pbr_surface, RmanFallback, missing-texture, unsupported-node.
- Bistro: full Bistro USD (interior + exterior) used for nightly regression at a hero
  camera. Full production-class scenes (Moana, ALab, etc.) are post-v1 / local-manual only.
- Camera selection via `scene.camera` SdfPath.

#### Test-fixture intent (what each fixture proves)

| Fixture | Proves |
|---|---|
| `triangle.usda` | Vulkan device + raygen + first pixel works |
| `quad_triangulated.usda` | `HdMeshUtil::ComputeTriangleIndices` on a quad |
| `missing_normals_quad.usda` | Smooth-normal generation fallback |
| `authored_normals_quad.usda` | Authored-normal pass-through |
| `uv_cube.usda` | UV0 primvar extraction + tangent generation (MikkTSpace) |
| `cube_with_subsets.usda` | Per-face material binding via `GeomSubsets` |
| `double_sided_plane.usda` | `doubleSided` flag honoured in closesthit |
| `instanced_cubes.usda` | Native `HdInstancer` + BLAS sharing by `MeshHandle` |
| `point_instancer_rocks.usda` | `UsdGeomPointInstancer` flattening |
| `nested_instancers.usda` | Recursive instancer transform composition |
| `blas_sharing.usda` | One BLAS, N instances — verifies no duplicate builds |
| `usd_preview_complete.usda` | Every UsdPreviewSurface field round-trips into OpenPBR |
| `mtlx_open_pbr_surface.usda` | Canonical OpenPBR identity pass-through |
| `mtlx_standard_surface.usda` | StdSurface→OpenPBR translation shim |
| `rman_fallback.usda` | RenderMan PxrSurface best-effort + fallback |
| `missing_texture.usda` | Magenta-fallback texture, no crash |
| `unsupported_node.usda` | Unknown MaterialX node logged + constant-default fallback |
| `udim_textures.usda` | UDIM resolver + UV-tile arithmetic |
| `dome_light_envmap.usda` | Dome light + lat-long EXR importance sampling |
| `distant_plus_rect.usda` | Distant + rect light NEE+MIS |
| `emissive_mesh.usda` | Emissive-triangle alias-table sampling |
| `large_mesh_chunked.usda` | Mesh > stagingRing/4 oversize one-shot upload path |
| `aov_all_seven.usda` | Every supported AOV (`color`, `depth`, `normal`, `albedo`, `motionVector`, `materialId`, `instanceId` — §18.4 `AovFlag`) and format negotiation path (§25.I.1). The Python harness sweeps `HdAovDescriptor::format` across `{HdFormatFloat32Vec4, HdFormatFloat16Vec4, HdFormatFloat32, HdFormatFloat16Vec2, HdFormatInt32}` and asserts the renderer either accepts the request or returns `ErrorKind::AovFormatUnsupported` — never silently substitutes a wider format. v1 USD is single-frame (no animation, §42); the motionVector slice is exercised by the harness calling `RenderFrame` twice with a deliberate camera pan between calls and asserting (a) frame-0 motion vectors are all-zero (no prev camera — §18.6) and (b) frame-1 motion vectors match the pan magnitude within tolerance. |
| `bistro/` | Full Amazon Lumberyard Bistro USD — v1 hero scene, nightly regression seed (M8a) |

A PR adding a new public-API verb or a new MaterialX coverage path **must** add or
update a fixture above; reviewers check this before approval.

### CI / nightly
- CI: builds + unit tests + tiny-scene regressions, target wall-clock ≤ 10 minutes.
- Nightly (separate pipeline, not gating CI): Bistro regressions. Production-class
  scenes (Moana, ALab, etc.) are local/manual; gated by env var `PYXIS_BISTRO_DATASET_PATH`
  (and per-scene env vars for post-v1 datasets). Missing dataset → skip with clear message.
- Performance regression tracking: harness records per-test
  `frame.firstFrame`, `frame.cpu`, **`frame.timeToFirstImage`** (wall-clock from
  `pyxis.exe` invocation to first pixel written — the customer-perceived metric),
  peak GPU memory, per-pass GPU times into a CSV that the
  CI/dashboard tooling can plot.

---

## 36. Test Infrastructure (Concrete Files)


This section is the single source of truth for the *files* §35 / §30 / §37 keep
referencing. CI jobs that don't find one of these fail loudly.

### 36.1 `.clang-format`

Repo-root `.clang-format`, hand-written for this codebase. v1 baseline:

```yaml
BasedOnStyle: Google
ColumnLimit: 100
IndentWidth: 2
TabWidth: 2
UseTab: Never
DerivePointerAlignment: false
PointerAlignment: Left
AccessModifierOffset: -1
AllowShortFunctionsOnASingleLine: Inline
AllowShortIfStatementsOnASingleLine: Never
BreakBeforeBraces: Attach
NamespaceIndentation: None
SortIncludes: CaseInsensitive
IncludeBlocks: Regroup
IncludeCategories:
  - Regex: '^"'                        # same-target private headers first
    Priority: 1
  - Regex: '^<Pyxis/'                  # Pyxis public headers
    Priority: 2
  - Regex: '^<nvrhi/'                  # NVRHI / Vulkan
    Priority: 3
  - Regex: '^<vulkan/'
    Priority: 3
  - Regex: '^<.*/.*'                   # third-party
    Priority: 4
  - Regex: '^<[^/]+>'                  # standard library
    Priority: 5
```

CI (`_pipelines/pyxis_ci.yml`) runs `_tools/run_clang_format.py --check`.

### 36.2 `.clang-tidy`

Repo-root `.clang-tidy`. Curated check set (§37 made concrete):

```yaml
Checks: >
  bugprone-*,
  -bugprone-easily-swappable-parameters,
  performance-*,
  modernize-use-nullptr,
  modernize-use-override,
  modernize-use-default-member-init,
  modernize-use-using,
  modernize-make-unique,
  modernize-use-noexcept,
  cppcoreguidelines-pro-type-cstyle-cast,
  cppcoreguidelines-pro-type-member-init,
  cppcoreguidelines-slicing,
  hicpp-exception-baseclass,
  misc-const-correctness,
  misc-unused-parameters,
  readability-identifier-naming,
WarningsAsErrors: '*'
HeaderFilterRegex: 'sources/pyxis_.*'
CheckOptions:
  readability-identifier-naming.ClassCase: CamelCase
  readability-identifier-naming.StructCase: CamelCase
  readability-identifier-naming.FunctionCase: CamelCase
  readability-identifier-naming.PrivateMemberPrefix: '_'
  readability-identifier-naming.PrivateMemberCase: camelBack
  readability-identifier-naming.GlobalConstantCase: CamelCase
  readability-identifier-naming.GlobalConstantPrefix: 'k'
  readability-identifier-naming.MacroDefinitionCase: UPPER_CASE
  readability-identifier-naming.MacroDefinitionPrefix: 'PYXIS_'
```

Per §30.11, `Private/Scene/Components/` files are exempt from
`readability-identifier-naming.PrivateMemberPrefix` (POD components have public
fields without `_`). The `.clang-tidy` in that folder overrides accordingly.

### 36.3 Sanitizers

Three CI jobs in addition to the standard Debug + Release matrix:

- **ASan/UBSan job** — clang-cl `-fsanitize=address,undefined`, runs every
  unit test plus the tiny-scene regression. Required on every PR.
- **MSan job** — Linux-cross-compile (Release pipeline only, post-v1; clang-cl
  doesn't support MSan on Windows). Tracks uninit reads. Optional v1.
- **TSan job** — Linux-cross-compile, Release pipeline only. Tracks data races
  on `SceneWorld` mutation queue. Required at M11 onward.
- The `pyxis_renderer/Private/Scene/` Flecs systems carry an explicit
  `static_assert(!ASAN_ENABLED || PYXIS_ALLOW_ASAN_OVERHEAD, ...)` guard so
  developers don't ship ASan-instrumented Release accidentally.

### 36.4 Fuzzing

`_tools/fuzz_parameters_json.cpp` builds a libFuzzer harness against the
`Configuration::parse` entry point. Required because `parameters.json` is the
only attacker-controlled input.

- Corpus: every `tests/fixtures/*.json` plus a handful of malformed seeds
  (truncated JSON, deeply-nested arrays, invalid UTF-8, integer overflow,
  unknown keys).
- Run: a 5-minute fuzz pass on every PR (CI-time-bounded), and a 4-hour pass
  in the nightly pipeline. Crashes / hangs are S1 incidents (§45.3).
- Coverage target: ≥ 90 % line coverage of `parameters` parsing code by M11.

### 36.5 GPU determinism scope

§33.7's "byte-identical EXR" claim is **scoped explicitly** to:

- One GPU model: **NVIDIA RTX 4080** (the lab reference).
- One driver version range: the lab-snapshot range pinned in
  `_tools/regression_matrix.json` at the v1 cut date (a contiguous NVIDIA
  Game Ready Driver range, e.g. `xxx.xx–yyy.yy`). The exact range is bumped
  per release in that JSON, not in this plan.
- One Vulkan SDK: matched to `_cmake/Thirdparty.cmake`'s pin.
- One OS: Windows 11 23H2 or 24H2.

Outside this matrix, regression tolerances widen to per-test floats
(`tolerance.rmse`, `tolerance.maxAbsoluteDelta`) declared in the fixture's
`parameters.json`. Cross-driver / cross-vendor determinism is **not promised**;
attempting to enforce it would prevent landing any GPU vendor's path-tracing
optimisation.

`_tools/run_regression.py` reads the host's GPU + driver, compares against the
pinned matrix, and either runs in *strict* mode (byte-identity required) or
*tolerant* mode (per-test floats), printing which mode it picked.

### 36.6 Performance regression detection

§35's CSV is consumed by `_tools/perf_compare.py`:

- Reads per-test KPIs (`frame.firstFrame`, `frame.cpu`,
  `frame.timeToFirstImage`, peak GPU memory, per-pass GPU times).
- Compares against the previous nightly's CSV via a 3-day rolling median.
- **Alerting thresholds**: > 10 % regression on any KPI for two consecutive
  nights triggers an S2 incident (§45.3).
- Output: a markdown report posted as an artifact on every nightly run, plus
  a compact one-liner summary in the pipeline log.
- A bisect helper `_tools/perf_bisect.py` wraps `git bisect` for the user;
  the bisect script invokes the regression harness and decides good/bad based
  on the same threshold.
- Dashboard: a static HTML page generated by `_tools/perf_dashboard.py` from
  the rolling CSV; published as a CI artifact each nightly run. No external
  dashboard service v1.

### 36.7 MaterialX coverage matrix

`tests/fixtures/materialx/coverage/` ships one fixture per supported nodedef
in the OpenPBR `open_pbr_surface` (`OPS_*`) and `standard_surface` (`SS_*`)
node sets. The harness asserts:

1. Every authored input value round-trips into the corresponding
   `OpenPBRMaterialDesc` field (RMSE = 0 inside the per-field tolerance).
2. Every unsupported nodedef is logged exactly once into
   `unsupported_features.json`.
3. The matrix CSV (`tests/fixtures/materialx/coverage/coverage.csv`) is
   diffed against the fixture set; missing combinations fail the harness.

PRs that add a new conversion path **must** add a matrix entry; reviewers
check this against the §45.1 PR checklist.

### 36.8 Hostile / dirty USD fixtures

`tests/fixtures/dirty_usd/` exists specifically to prove *the renderer never
hangs and never silently mis-renders* under bad input. Required fixtures:

| Fixture | Failure expected |
|---|---|
| `empty_stage.usda` | Renders a blank `color` AOV, exits 0; logs once. |
| `truncated.usda` | `ErrorKind::UsdStageOpenFailed`; exit code 3; no minidump. |
| `unresolved_reference.usda` | One `TF_WARN`-equivalent spdlog entry; the unresolved subtree is omitted; rest renders. |
| `cyclic_layer_stack.usda` | OpenUSD detects the cycle; we propagate `UsdStageOpenFailed`. |
| `extreme_nesting.usda` (200 nested instancers) | Flatten cap (§17) hits; `FrameStats::degraded = true`; one log line; renders what fits. |
| `infinite_extent.usda` | Camera framing degenerate; `RenderTargets::color` filled with the missing-texture color; exit 0. |
| `nan_positions.usda` | Triangulation drops NaN tris; `FrameStats::degraded = true`; remaining tris render. |
| `mixed_endianness.usdz` | Detected by USD; we propagate the error cleanly. |
| `gigantic_texture.usda` (16 K × 16 K EXR) | Clamped by `textures.maxResolution`; logged once. |

Every fixture has a **5-minute hard timeout** in the harness; exceeding it is
an S1 incident.

---

## 37. CI / Quality Gates


| Stage | Tooling | Fails build on |
|---|---|---|
| Build | clang-cl, CMake, Ninja | Any `/W4` warning, link error, missing dep |
| Static analysis | clang-tidy (curated checks: bugprone-*, performance-*, modernize-* selectively) | Any new diagnostic |
| Format | clang-format (repo-root `.clang-format`) | Any unformatted file |
| Unit tests | gtest | Any failing case |
| Tiny image regression | Python harness | RMSE > tolerance |
| NVRHI validation (Debug) | Vulkan validation layer | Any error or perf warning |
| Memory leak (Debug) | VMA `vmaCalculateStatistics` at process exit (intra-process GPU allocation tracking; `VkPhysicalDeviceMemoryBudget` reports system-wide pressure and cannot detect leaks) + `BudgetTracker` post-run delta | Non-zero leak |
| Nightly | Subset-Moana headless | RMSE > tolerance, peak GPU > +10% baseline |

CI lives under `_pipelines/pyxis_ci.yml`.
Build matrix: Debug + Release; both use Vulkan validation in Debug only.

---

# Part VIII — Roadmap

## 38. Minimal Vertical Slice Milestones


| # | Milestone | Done when |
|---|---|---|
| M0 | Skeleton | `pyxis.exe` opens NVRHI/Vulkan device, prints adapter, exits; `flecs::world` initialised with all components + phase pipeline registered; `SceneWorldInit` unit test green |
| M1 | Viewer triangle | hard-coded triangle, RenderGraph, ImGui setup, profiler scopes |
| M2 | Headless triangle | `--headless --config` writes EXR; same render core |
| M3 | Slang path-trace box | one cube, one camera, BLAS+TLAS, raygen/closesthit/miss, accum + tonemap |
| M3.5 | Default startup scene | `pyxis_app/Resources/scenes/default.usd` ships in the build tree; `pyxis.exe` with no args resolves through the §29.4.a chain and renders the three-spheres-on-ground composition; bundled `default_sky.exr` loads correctly; resolved-source spdlog line emitted |
| M4 | Hydra delegate stub | `HdPyxisRenderDelegate` registered; usdview can pick it; renders one mesh |
| M5 | UsdPreviewSurface→OpenPBR | textured cube via UsdPreviewSurface, OpenPBR shader |
| M6 | Native instancing | instanced rocks, BLAS sharing, instance/material AOVs |
| M7 | Lighting | dome + distant + rect lights; importance sampling |
| M8a | Bistro render | full Bistro USD (interior + exterior) loads + renders headless and viewer; visually plausible; nightly regression seed |
| M8b | Bistro performance pass | Bistro hero camera meets §34.3 KPIs (`pass.PathTrace < 12 ms`, `frame.cpu.commitResources < 2 ms`, `timeToFirstImage < 15 s`) on RTX 4080 reference; per-frame profile written |
| M9 | Bistro visually correct | dome+sun alignment, normals/tangents fallbacks, emissive sampling, MaterialX coverage gaps closed for Bistro hero assets — hero camera converges to a recognizable, color-correct image |
| M10 | Bistro headless + regression | nightly Bistro regression test green; per-test KPIs CSV emitted |
| M11 | Profiling/reporting polish | full spdlog summary, ImGui profiler panel, JSON/CSV reports |

---

## 39. Step-by-Step Implementation Order


1. CMake scaffolding (`pyxis_platform`, `pyxis_renderer`, `pyxis_material_translation`,
   `pyxis_hydra`, `pyxis_usd_ingest`, `pyxis`, shaders),
   thirdparty fetch (vcpkg or git submodules), clang-cl `/W4 /WX` toolchain (CMake `CMAKE_CXX_COMPILER=clang-cl`).
2. Logging (spdlog), Tracy, json, configuration loader, CLI parser, schema validation,
   effective-config writer.
3. `VkDeviceManager` + `VkDeviceManagerHeadless` (see §5.c).
4. NVRHI bindless layout, descriptor table manager.
5. RenderGraph + `IRenderPass` interface, automatic GPU timestamps.
6. **Flecs setup**: vcpkg port pinned, `SceneWorld` wrapper, component registration in
   `Private/Scene/Components/`, custom phase pipeline (`Pipeline.cpp`), no-op systems
   stubbed in their phases, `HandleBimap`, observer skeleton, Flecs Explorer gated
   behind `PYXIS_DEBUG_TOOLS`. Verified by a unit test that creates a world, registers
   all components, runs `world.progress()` once, and tears down cleanly.
7. Slang compile pipeline; ShaderInterop header shared C++/Slang.
8. ImGui integration (viewer mode only, gated by `PYXIS_DEBUG_TOOLS`).
9. `Profiler`, `ProfilerData`, `FrameProfile`, backend interfaces.
10. `GpuScene` skeleton: handles, MeshTable, MaterialTable, TextureTable, InstanceTable,
    BlasCache, TlasBuilder, UploadQueue, DeletionQueue, BudgetTracker.
11. Hard-coded triangle / cube path-trace (no Hydra) → M3.
12. OpenPBR shader (single hit group), accumulation, tonemap, AOV resolve.
13. Hydra plugin scaffolding: plugInfo.json, `HdPyxisRendererPlugin`,
    `HdPyxisRenderDelegate`, render-pass stub.
14. `HdPyxisMesh`, `HdPyxisInstancer`, `HdPyxisCamera`, `HdPyxisRenderBuffer`.
15. UsdPreviewSurface → OpenPBR adapter; texture cache; first textured Hydra render → M5.
16. Lights (distant, dome, rect) → M7.
17. Moana stage open via `UsdImagingStageSceneIndex` (Hydra 2.0); attach flatten,
    prototype-propagating, material-binding scene-index filters; subsets, instancers,
    large texture handling → M8a/M8b/M9.
18. Headless mode polish; deterministic seeding; EXR writer; exit codes.
19. Regression harness in Python; tiny-scene fixtures; CI pipeline.
20. Profiling reports (JSON/CSV/ImGui panel) → M11.
21. MaterialX + RenderMan fallback adapters (after M5 baseline working).
22. **`pyxis_usd_ingest` walker**: implemented in parallel with the Hydra adapter from
    M4 onward. Same `MeshDesc`/`InstanceDesc`/`OpenPBRMaterialDesc` outputs; shares
    `pyxis_material_translation`. The walker emits prims in **`SdfPath`-sorted order**
    so instance IDs match the Hydra adapter byte-for-byte. M8a is gated on **both**
    adapters loading the Moana subset and producing RMSE-zero regression images.

---

## 40. Phased Delivery of the Final Architecture


The architecture documented in §1, §2, §8 *is* the final design **and what v1 ships**.
This section clarifies what that means, and what the very small set of post-v1 items
actually is.

### 40.1 What is "final" already on day 0

- The four-layer stack and the dependency direction (Application → ingest adapter →
  renderer → platform/NVRHI).
- The renderer's public API surface (§18). No method is added later; if a future
  feature needs new state, it goes through the existing verbs.
- The `SceneWorld` *implementation*: a real `flecs::world` with all components,
  systems, queries, observers, and the custom phase pipeline (§8). There is no shim,
  no v1 stand-in, no scheduled refactor. Once a system is registered in M0/M3, its
  schedule slot is final.
- **Both ingest adapters** (`pyxis_hydra` and `pyxis_usd_ingest`) ship from M0 and
  are at parity: each Moana regression image is rendered *twice* in CI (once per
  adapter) and they must match byte-for-byte.
- The folder layout (§2), all CMake targets (§3), all third-party deps (§4), the
  threading model (§31), the error contract (§18.6).

### 40.2 What is genuinely post-v1

This is the entire post-v1 list. Anything not on it is in v1.

1. **Hydra retirement** *(only once usdview parity is no longer needed)* — drop
   `pyxis_hydra` from default builds; keep it behind a CMake option
   `-DPYXIS_BUILD_HYDRA_DELEGATE=ON` for tools that need a render delegate.
   `pyxis_usd_ingest` becomes the default. **Zero renderer changes.**
2. **Animation / time-varying USD** — wire `UsdNotice` / time-sample sampling into
   `pyxis_usd_ingest::Sync/`, add `Dirty<Transform>` driving on a per-frame tick.
   Existing Flecs systems already handle the per-frame component updates; this is
   adapter work, not renderer work.
3. **OpenPBR per-material specialisation** (multi-hitgroup) once the generic path is
   profiled.
4. **Texture compression**, **subdivision**, **volumes**, **curves**, **D3D12/Linux**
   backends — see §42 deferred-features list.

There is **no** "Flecs swap" milestone. There is **no** "USD-direct ingest later"
milestone. Both are day-0 reality.

### 40.3 Why we build Flecs in v1 (instead of a plain-tables shim)

The plain-tables-shim option was considered and rejected:

- Hand-rolled tables that mimic an ECS schedule are not actually smaller than the
  Flecs equivalent — they reproduce roughly the same archetype/dirty-flag bookkeeping
  by hand, plus we then carry a planned swap milestone forever.
- A "swap to Flecs later" milestone is the kind of rework that historically slips.
  Building Flecs day 0 means the system pipeline is real from the first frame and
  every subsequent feature lands as components/systems, not as a future refactor.
- Flecs's CMake/vcpkg integration is one line. Flecs Explorer pays for itself on the
  first regression-image investigation.
- **Honest update on the "cost is zero" framing.** The earlier draft claimed the
  user-visible cost of choosing Flecs day 0 was zero because the public API
  (§18) doesn't leak any Flecs header. That is *still* true in the strict
  ABI sense — `pyxis_renderer` links `flecs` PRIVATE and `Public/` is
  Flecs-free — but several v1-shipped subsystems have grown affordances
  whose shape *assumes* an ECS underneath:
  - **Per-system profiler scopes (§34.2)** auto-name themselves from the
    Flecs system identifier; the Performance panel's CPU rows are
    one-per-system.
  - **Dirty-flag tracking** for TLAS / accumulation / material rebind
    (§24, §25.D) is implemented as Flecs tag components plus
    `query.changed()`; replacing the ECS would mean reimplementing
    those tags as bitsets and `query.changed()` as manual epoch
    counters.
  - **Commit phases** (`OnLoad → PostLoad → PreUpdate → … → OnStore`)
    used by `GpuScene::CommitResources` and the upload pipeline are
    Flecs phase entities; the ordering contract `pyxis_hydra` /
    `pyxis_usd_ingest` rely on (§31) is expressed in those phases.
  - **The Inspector / Material Report / Texture Cache panels (§29.3)**
    are read-only ECS queries; without a query layer they would each
    need a hand-rolled iterator with the same archetype filtering.

  None of these surface a Flecs type to the host, but together they mean
  "swap to a different scheduler later" is no longer a one-week refactor
  even internally. Building Flecs in v1 is therefore not just convenient —
  it is now load-bearing for the systems above. The user-visible cost is
  still zero (no Flecs in `Public/`); the engineering cost of a future swap
  is no longer near-zero, and we have decided to absorb that lock-in
  rather than litigate it again later.

### 40.4 Other future items

- **OpenPBR specialization**: per-material hit groups once the generic path is profiled.
- **Texture compression**: optional offline BC7/BC5 pre-bake tool, or runtime compute
  encoder, once memory pressure justifies the engineering cost.
- **Subdivision**: adaptive `pxOsd` evaluation or offline pre-baked subdivided meshes,
  once silhouette quality becomes a priority.
- **D3D12 / Linux**: NVRHI already abstracts D3D12; backend swap is a build-system concern.
- **Animation / time-varying USD**: `pyxis_usd_ingest`'s change-listener wires
  `UsdNotice` callbacks into `Dirty<Transform>` / `Dirty<Visibility>` writes. The
  existing Flecs systems then drive per-frame TLAS rebuilds, material updates, and
  texture re-uploads with no schedule changes.

### 40.5 Post-v1 scene & asset loading roadmap

This subsection is the consolidated checklist of *everything* the renderer might
have to do at load time or during incremental scene updates beyond v1. Each item
lists the v1 hook it builds on (so we can tell that we are not painting ourselves
into a corner) and the rough shape of the post-v1 work. None of these are in
v1; the point of writing them down is to keep the v1 architecture honest about
what it must not foreclose.

**Time, animation, and incremental change**

- **Time-varying USD samples.** Sample attributes at `UsdTimeCode currentTime`
  rather than `Default`. v1 hook: `IngestSnapshot` already carries one sampled
  value per attribute; post-v1 wraps the snapshot loop in a per-frame
  `currentTime` and adds a Flecs `Dirty<Transform>` / `Dirty<Skinning>` /
  `Dirty<Visibility>` tag write. No public API change.
- **`UsdNotice` listener.** Replace the one-shot importer with a long-lived
  `TfNotice` subscription on the `UsdStage`. Batches notices per frame
  (microtask drain at the top of `OnLoad`), translates each to the smallest
  possible `Dirty<…>` set. v1 hook: `pyxis_usd_ingest::Snapshot/` directory is
  already named to leave room for a `pyxis_usd_ingest::Live/` sibling.
- **Skeletal animation (UsdSkel).** Adds `Dirty<SkinPose>`, a per-instance
  blendshape buffer, and a compute pre-pass that writes skinned vertex
  positions into a transient buffer the BLAS rebuild reads. v1 hook:
  `MeshDesc::flags` reserves a `Skinned` bit; `GpuInstance` reserves
  `skeletonHandle`; `BLAS_BUILD_FLAG_ALLOW_UPDATE` is already a known flag.
- **Blend-shape / corrective targets.** Same compute pre-pass as skinning, with
  a per-target weight buffer. Reuses the skinned-vertex transient buffer.
- **Velocity / motion-vector accuracy from USD.** When time-varying transforms
  exist, populate `worldFromLocalPrev` from `UsdTimeCode(t - 1)` instead of the
  identity-matrix v1 default (§43.2). Closes the loop between USD time samples
  and the §18.4 motion-vector AOV.
- **Hot-swap with continuity.** Reload the scene without resetting accumulation
  on the *unchanged subset* of pixels. v1 ships the brute-force version
  (`GpuScene::Clear()` then full reload, accumulation reset; §29.4); post-v1
  diffs the snapshot and only invalidates pixels whose primary-ray instance
  changed (requires the instance-ID AOV to be available the frame before the
  swap, which it already is in v1).

**USD composition surface area**

- **Payloads & deferred loading.** Honour `UsdStage::Load` / `Unload` so
  payloaded prims are loaded on demand (proxy → render switch, kind
  filtering). v1 always loads everything; post-v1 lets the Inspector panel
  drive a per-prim load button and a memory-budget-aware preloader.
- **Variants and variant sets.** Expose `UsdVariantSet::SetVariantSelection`
  through the Inspector; trigger an incremental re-import of the affected
  subtree. v1 honours whatever variant selection the stage was authored with.
- **Purpose filtering.** Honour `UsdGeomImageable::ComputePurpose` so
  `proxy` / `guide` / `render` / `default` can be toggled live (the standard
  Hydra purpose set). v1 renders `default` + `render` only; the toggle UI is
  post-v1.
- **Draw modes.** `model:drawMode` (`bounds`, `cards`, `origin`, `inherited`)
  for asset-level LOD substitution. Lets a shot stage swap heavy assets for
  proxy cards above a memory threshold. Post-v1; v1 ignores draw modes.
- **Activation / visibility edits.** `UsdPrim::SetActive(false)` and
  `UsdGeomImageable::MakeInvisible` propagate through the change listener.
  v1 honours the *initial* activation/visibility; live edits are post-v1.
- **Sublayers, references, and edit targets.** "Save Scene As USD" (§29.7)
  ships v1 writing one overlay sublayer; post-v1 lets the user pick the edit
  target (session layer, root layer, named sublayer) and exposes layer-stack
  composition in a side panel.
- **Asset resolver context (Ar 2.0).** Allow callers to push a
  `ArResolverContext` for studio resolvers (URI schemes, version pinning, ACL).
  v1 uses the default file-system resolver; post-v1 plumbs a context through
  `pyxis::Configuration` and into both ingest adapters.
- **`UsdGeomXformCache` reuse.** v1 recomputes world transforms per ingest;
  post-v1 caches the `UsdGeomXformCache` across notices and only invalidates
  the dirty subtree.

**Geometry beyond v1's polymesh world**

- **Subdivision surfaces (`pxOsd`).** Adaptive limit-surface refinement at the
  required tessellation factor (view-dependent or fixed). v1 hook:
  `MeshDesc::GeometryKind::Subdiv` is in the public POD (§43.5); v1 rejects
  with `ErrorKind::GeometryKindUnsupported`.
- **Curves (`UsdGeomBasisCurves`, hair).** Linear / cubic catmull-rom /
  bezier curves with width and curve-type per primitive. Likely needs an
  intersection program in the closesthit shader and a separate BLAS build
  path (`VK_GEOMETRY_TYPE_AABBS_KHR` or hardware curves on Ada+).
  v1 hook: `GeometryKind::Curves`.
- **Points (`UsdGeomPoints`).** Sphere or disc primitives with per-point
  radius. Same intersection-program path as curves, simpler shader.
- **Volumes (`UsdVolVolume` + OpenVDB).** Heterogeneous media path tracer
  with delta tracking / ratio tracking, NanoVDB on the GPU. Adds
  `Volume` to `MaterialFlag` and a separate any-hit path. v1 hook:
  `GeometryKind::Volume`.
- **Displacement.** Either offline pre-displaced meshes (treat as plain
  polymesh) or runtime tessellation + displacement compute pass before
  BLAS build. The latter is its own RFC.
- **Point instancers (`UsdGeomPointInstancer`).** Already representable in
  v1 as N `GpuInstance` records pointing to the same `MeshHandle`, but the
  ingest path that flattens a million-instance instancer needs the dedicated
  fast path: per-instance buffer pre-allocated, single bulk
  `RegisterInstanceBatch(span<InstanceDesc>)` call (post-v1; v1 issues
  per-instance calls and pays an O(N) handle-table cost).
- **Native instancing (`UsdGeomImageable::IsInstance`).** Same flattening
  story but driven by USD's native instance prim mechanism.
- **Multi-segment motion blur.** More than two transform samples per shutter.
  Reserved by `worldFromLocalPrev` becoming `worldFromLocal[kMotionSteps]`
  in a future MINOR (§43.2 already lists motion blur).

**Materials, textures, and shading data**

- **Full MaterialX node graphs.** v1 only handles `open_pbr_surface` and a
  `standard_surface` translation shim, logging anything else. Post-v1 either
  bakes arbitrary node graphs into the OpenPBR closesthit via Slang code-gen
  or grows a separate generic-shader path.
- **UDIM / tile textures (`<UDIM>`).** Resolve `<UDIM>` patterns to a tile
  set; either pack into a single texture-array slice per tile and index by
  uv quadrant in the shader, or use a sparse residency mapping. v1
  rejects UDIM with a clear error; post-v1 wires the tile resolver into
  `TextureCache`.
- **Texture compression (BC7 / BC5 / ASTC).** Offline pre-bake tool plus a
  `.pyxis_tex` cache sidecar (hash → BCn payload, content-addressed). v1
  uploads textures in their native format and clamps with
  `textures.maxResolution`.
- **Mip streaming / partial residency.** Sparse-binding for huge texture
  sets (Moana hero foliage). Bind only the mip pyramid above the current
  screen-size estimate; promote/demote on visibility change. Requires
  `VK_KHR_sparse_*` and a residency-budget controller.
- **OpenColorIO 2 colour management.** Per-texture input colour space (USD
  `colorSpace` metadata), display transform pipeline, view/look chain. v1
  is sRGB-in / scene-linear-out / Reinhard or ACES-out and ignores
  `colorSpace` beyond `sRGB` vs `raw`. v1 hook: tone-mapping is already a
  separate pass with its own knobs (§17).
- **3D-LUT tone-map.** Already covered as a hook by §43.4; the reserved
  `RenderSettings::ToneMap::Lut3D` enum value is in place.
- **Material bindings driven by collections (`UsdShade::CollectionAPI`).**
  v1 honours direct `material:binding`; collection-based binding (a
  collection of prims gets the same material) needs a flatten pass at
  ingest. Post-v1 expands collections during snapshot construction.
- **Material network instancing.** Two prims pointing at the same
  `Material` prim share one `MaterialHandle` in v1. Inheritance of partial
  network parameter overrides (`UsdShade::Connection` deltas) is post-v1.
- **Geometric primvars beyond UV0 + tangent + normal.** Vertex colour,
  per-instance scalars, multi-UV sets. v1 ingests UV0 + normals + tangents
  only; post-v1 grows `MeshDesc::primvars` into a small named-attribute
  table the shader can index by name hash.
- **Skinning + morph + displacement stacking.** The compute pre-pass order
  (`skin → morph → displace`) and the transient-vertex-buffer ownership are
  RFC topics; v1 ships none of them.

**Lighting and cameras**

- **Light linking and shadow linking.** Field is reserved (§43.1) at
  64 light groups; the closesthit-shader evaluation is post-v1. Going
  beyond 64 groups needs a bindless mask table — separate RFC.
- **`UsdLuxCylinderLight` / `DiskLight` / `SphereLight` / `MeshLight`.**
  v1 ships `Distant`, `Dome`, `Rect` only (`LightDesc::Kind`). The enum
  is additive; new kinds are a MINOR bump.
- **IES profiles.** Reserved as `LightDesc::iesProfile : TextureHandle`
  (1D/2D angular intensity); v1 ignores it.
- **Per-light AOVs.** Light Path Expressions (LPEs) so the user can
  isolate the contribution of one light or one bounce depth. Adds a
  per-LPE accumulation buffer and an LPE-state byte to the path-throughput
  payload. Major shader work; post-v1.
- **Camera lens shaders** (distortion, polynomial radial, fisheye beyond
  pinhole). v1 ships pinhole + apertureFStop reservation (§43.3); other
  lens models are additive `CameraDesc::Lens` enum entries.
- **Stereo / multi-view rendering.** Two cameras sharing the same scene
  state; trivial to add by running `RenderFrame()` twice with different
  `RenderTargets`. Post-v1 once VR is in scope.

**Asset I/O, streaming, and memory**

- **Asynchronous, prioritised asset loading.** A worker pool that
  decompresses textures and parses meshes off the render thread, ordered
  by current screen importance. v1 loads synchronously during M3.5's
  default-scene path and during `Open USD…`; the staged-load progress
  bar is the v1 floor.
- **Progressive scene reveal.** Stream meshes/materials/textures into a
  partially-rendered view rather than blocking on full ingest. Builds on
  the same async loader; needs the renderer to tolerate "instance present
  but mesh not yet uploaded" (skip in TLAS, render fallback box).
- **GPU LRU eviction.** Texture and BLAS caches grow until a configurable
  budget is hit; LRU-evict on fault. Requires sparse residency for
  textures and `VK_KHR_acceleration_structure_*` rebuild on demand for
  geometry. v1 simply errors out at the budget ceiling.
- **On-disk geometry cache.** `pyxis_cache/` directory containing
  pre-deduplicated, pre-tangent-generated mesh blobs keyed by
  `MeshHandle` content hash. Skips MikkTSpace and quantisation on
  re-load. v1 always re-derives.
- **On-disk shader / SPIR-V cache.** Slang already supports compile
  caching; v1 ships with the cache off (deterministic from-source build);
  post-v1 turns on the disk cache for iteration speed only.
- **Multi-stage / multi-file scene composition** (camera in one USD,
  lighting in another, geometry in a third). v1 already handles this via
  USD's normal sublayer/reference composition; post-v1 adds a UI to
  open multiple roots and merge them with a chosen edit target.
- **Background scene preloading.** Preload N scenes from a recents list
  in the background so `Open USD…` is instant. Post-v1 viewer-mode-only.
- **Network / cloud assets.** Plug an HTTPS-backed `ArResolver` for
  cloud-stored stages; cache locally. Strictly studio-pipeline territory;
  post-v1.

**Editing, undo, and round-tripping**

- **Undo / redo.** A bounded edit stack on the host side; each edit is a
  `(prim, attribute, oldValue, newValue)` tuple. Drives both `GpuScene`
  re-uploads and the eventual "Save Scene As USD" flush. v1 has no undo;
  the Material Report editor (§29.3) writes through immediately.
- **Differential / partial USD save.** v1's "Save Scene As USD" (§29.7)
  writes the entire `RenderSettings` block + every modified material.
  Post-v1 narrows that to "only diffs since open" by tracking edits in
  the undo stack.
- **Multiple edit targets** (session layer vs root layer vs named
  sublayer). Lets the user keep experimental tweaks in the session layer
  and only promote to disk on demand. Hooks into `UsdEditTarget`.
- **Round-trip-stable Pyxis namespace.** v1's `customLayerData["pyxis:..."]`
  is the forward-compat surface (§29.7); post-v1 versions the schema with
  `pyxis:savedFromVersion` and refuses to load forward-incompatible
  overlays.

**Diagnostics, profiling, and validation specific to loading**

- **Per-asset load profile attribution.** v1 ships per-system CPU scopes
  and a load-time Gantt (§34.1). Post-v1 attributes each timeline span to
  the originating prim path so the LoadTimeline panel can answer "which
  prim cost 2 s of M3.5?" by clicking a bar.
- **Asset-level memory accounting.** Per-prim GPU and CPU watermark
  (which `MaterialHandle` / `MeshHandle` / `TextureHandle` cost what).
  v1 has aggregate watermarks (§34.4); post-v1 attributes them.
- **USD validator integration.** Run `usdchecker` on Save and surface
  warnings in the Console panel. Post-v1.
- **Asset hot-reload.** Watch the source `.usda` / `.usdc` and trigger a
  re-ingest on change. Same machinery as the post-v1 `UsdNotice` listener
  applied to file-system events. Strictly viewer/debug-tools.

This list is normative for "what the post-v1 architecture must remain
compatible with." Anything not on it (and not in the rest of §40.4 or
§42 / §43) is by default *not* a post-v1 commitment — it would need its
own RFC under §44 before any reservation in the v1 API.

---

## Architectural Reference Patterns

Pyxis intentionally borrows shapes that have been validated in adjacent NVRHI /
OpenUSD codebases (Aurora, donut samples, USD reference renderers). The patterns
below are *shapes*, not source code — described here so future readers do not
have to reverse-engineer them from the rest of the document.

- **Single-binary, two-mode application.** One executable carries an
  `Application` core that owns a `Profiler&`, a `Configuration`, an
  `IDeviceManager*`, and an active mode (viewer / headless). `main.cpp`
  parses CLI, constructs the right device manager (§5.c), constructs the
  mode object, and runs its loop. ImGui code is reachable only from the
  viewer mode and gated behind `PYXIS_DEBUG_TOOLS` for compile-time
  exclusion in shipping builds.

- **Headless device manager (§5.c).** A second `IDeviceManager`
  implementation that links no GLFW, requests no `VK_KHR_swapchain`, and
  whose `EndFrame()` issues an optional readback instead of a present.
  Pinned to `framesInFlight = 3` for byte-identical EXR output.

- **GpuScene as the single bindless database.** One class owns the handle
  maps (mesh / material / texture / instance), the GPU resource buffers,
  the dirty sets, and the descriptor table. All ingest adapters
  (`pyxis_hydra`, `pyxis_usd_ingest`) speak the same `GpuScene` verbs;
  the renderer reads from the descriptor table and never sees the
  adapter. See §8 / §18.

- **Pass orchestration via the RenderGraph.** Concrete passes implement
  `IRenderPass` (§9.2). At `PyxisRenderer::Init`, every pass is registered
  with `RenderGraph::AddPass(std::unique_ptr<IRenderPass>)`; the graph
  takes ownership and stores each pass alongside the texture / buffer
  refs it declared in `Declare(Builder&)`. Execution order is **derived**
  by `RenderGraph::Compile` from those declared producer / consumer
  edges (not from a hand-maintained list inside an orchestrator). At
  `Execute` time the graph walks compiled passes in topological order,
  feeding each one a `PassContext` (§9.2) plus the bindless descriptor
  table; passes never reach into `GpuScene` internals.

- **The graph is re-derived per frame from the active feature mask.**
  `RenderSettings::features` (§29.5) drives which passes participate. The
  Features panel mutates that mask at runtime through ImGui; on the next
  frame `RenderGraph::Compile` picks up the new mask and either retrieves
  the matching topological order from the compile cache (instant) or
  computes a new one (§9.2 cache rules). Passes that the new mask
  excludes are skipped in `Execute`; their NVRHI handles stay alive but
  idle, so flipping a toggle back is free. Invalid masks (a consumer
  whose producer was disabled) fail at `Compile` with
  `Error::RenderGraphMissingProducer` rather than silently miscompiling;
  the panel greys out toggles whose dependencies would break.

- **Shader hot-reload (viewer / `PYXIS_DEBUG_TOOLS` only).** A
  *Reload Shaders* button in the debug overlay (§10) spawns a detached
  `std::jthread` that recompiles every `.slang` entry point through
  Slang's session API off the render thread, then atomically swaps the
  SPIR-V blobs and PSOs at the next frame boundary under a single
  `ShaderLibrary` mutex. On any compile failure the diagnostic goes to
  spdlog and the ImGui status bar; the **previous PSOs stay live**, so a
  bad edit never interrupts the running frame. Accumulation is reset on
  successful swap. Headless mode does not expose this path — it is
  strictly an iteration aid.

- **Shader variant manifest.** A plain text file (`shaders.cfg`) lists
  one shader entry per build line: source file, entry point, profile,
  defines. ShaderMake reads this manifest and produces SPIR-V. Each
  variant compiles to a deterministic output filename consumed by
  `RenderPassBase::LoadShader(...)`. See §10.

- **Shared C++/Slang constants via `PYXIS_INTEROP_STRUCT`.** A single
  `ShaderInterop.slang` file declares every cross-language POD; both
  Slang (`#ifdef __cplusplus` toggles `hlslpp` aliases on the C++ side)
  and clang-cl compile it. See §23.

- **POD editing & serialisation pattern.** Every editable POD
  (`RenderSettings`, the public material descs, frame-pacing knobs) carries
  one `NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(...)` macro line (JSON
  load / save / default-on-missing in one statement) and a free function
  `DrawImGui(T&)` declared in `pyxis_app/UI/`. ImGui code stays plain —
  grouping headers, tooltips, dependent enable / disable, custom
  validators are all written explicitly per panel. No reflection POD, no
  template metaprogramming, no DSL. See §21.

These shapes are the only things assumed about prior NVRHI experience;
none of them is load-bearing for the architecture, but each one is a
familiar landmark for engineers who have shipped a similar renderer
before.

---

## Verification

Each milestone has a corresponding verification step:

1. Unit tests for OpenPBR conversion: feed UsdPreviewSurface inputs (programmatic),
   assert resulting `OpenPBRMaterialDesc` field-by-field.
2. Unit tests for `MeshTable` / `MaterialTable` deduplication and handle stability.
3. RenderGraph integration test: synthetic passes write known patterns, verify GPU
   timestamps appear, verify barriers don't crash NVRHI validation.
4. Tiny `.usda` fixtures rendered headless → image diff vs in-tree baseline EXR
   (RMSE < per-test tolerance).
5. Moana subset test (nightly): renders, diff vs baseline, profiles within ±10% of
   reference timings.
6. spdlog summary at end of each headless run is parsed and asserted to contain all
   required fields (no silent missing scopes).
7. RenderDoc / Nsight Graphics: manual visual inspection at M3 and M9.
8. NVRHI validation layer enabled in Debug — zero validation errors required for CI to pass.

---

## Decisions

- **Two ingest adapters day 0** — `pyxis_hydra` (Hydra 2.0 delegate) and
  `pyxis_usd_ingest` (direct USD-to-renderer importer); both ship in v1
  and produce byte-identical EXRs against the same scene (§O.3
  determinism contract).
- **OpenPBR is the canonical material**, all inputs converted on the CPU.
- **One generic OpenPBR closest-hit shader** in v1; specialization deferred.
- **Hydra ingestion**: Hydra 2.0 / Scene Indices (`UsdImagingStageSceneIndex` +
  filters). The legacy `UsdImagingDelegate` is **not** used.
- **`pyxis_usd_ingest` is a one-shot importer**: walks the stage once,
  emits an `IngestSnapshot`, releases the stage. No `UsdNotice` listener
  in v1 (§O.2). Live USD-driven updates outside Hydra are post-v1
  (§40.4 / §42).
- **One executable, two modes**, headless built on `VkDeviceManagerHeadless`.
- **Linear RenderGraph** in v1 (no automatic resource alias / culling).
- **Bindless single-table layout** — `RawBuffer_SRV(s=1)`, `Texture_SRV(s=2)` (the standard NVRHI bindless layout).
- **BLAS by MeshHandle** (strict prototype sharing); compaction default-on.
- **Profiling is first-class infra**; ImGui/spdlog/JSON/CSV are output backends.
- **Image is the only regression artifact** v1.
- **Subdivision, volumes, curves, displacement deferred** — Moana will be approximated.
- **Texture compression deferred** — native formats only v1; rely on resolution clamp.
- **MaterialX in scope, scoped** to `open_pbr_surface` (canonical 1:1) and
  `standard_surface` (translation shim); arbitrary node graphs logged-and-skipped.
- **Tracy enabled in Debug**; never required for the pipeline.
- **License: Apache 2.0** for all Pyxis source code (matches OpenUSD, MaterialX, Aurora;
  GPL-incompatible deps are excluded). A top-level `LICENSE` file carries the Apache 2.0
  text, and a `NOTICE` file aggregates third-party attributions (OpenUSD, MaterialX,
  NVRHI, Slang, ShaderMake, Tracy, spdlog, GLFW, ImGui, stb, tinyexr, MikkTSpace,
  hlslpp, nlohmann/json, gtest \u2014 all MIT/Apache-2/BSD/Zlib/public-domain, all
  Apache-2.0-compatible).

---

## Further Considerations

All four open questions from the previous draft have been resolved:

1. **Subdivision in v1?** → Deferred. Render polymesh hulls; revisit after M11.
2. **Texture compression?** → Deferred. Upload textures in their native format; rely on
   `textures.maxResolution` to cap GPU footprint. Revisit if Moana exceeds memory budget.
3. **MaterialX coverage v1?** → In scope, scoped to `open_pbr_surface` (canonical 1:1 mapping)
   plus `standard_surface` as a translation shim into OpenPBR. Arbitrary node graphs are
   logged-and-skipped with constant-default fallback, not baked.
4. **Scene Indices vs `UsdImagingDelegate`?** → Hydra 2.0 / Scene Indices
   (`UsdImagingStageSceneIndex` + filters) is the only path. Legacy delegate not used.

---

## 41. Per-Phase Detailed Implementation Plan


Each milestone (M0..M11) is broken into concrete deliverables, exit criteria, and the
files that must exist at the end. "Owner" is left blank to be filled per assignment.

### Phase M0 — Skeleton
**Goal**: build system green, single binary opens a Vulkan device.

- Files: `CMakeLists.txt`, `_cmake/Compiler.cmake`, `_cmake/Vulkan.cmake`,
  `sources/pyxis_platform/{Public,Private}/...` (Device + Logging + FileSystem only),
  `sources/pyxis_app/Main.cpp`.
- Deliverables:
  - clang-cl toolchain, `/W4 /WX`, vcpkg or fetch-content for thirdparty.
  - `VkDeviceManager::create()` returns `std::expected<...>`; logs adapter, driver, VRAM.
  - `pyxis.exe` exits 0 on success; exit codes: `0` ok, `2` device init fail, `3` config fail.
  - **Flecs vendored** via vcpkg (pinned baseline). `SceneWorld::Init` constructs a
    `flecs::world`, registers every component declared in
    `Private/Scene/Components/`, registers the `PYXIS_PHASE_*` custom pipeline with
    no-op systems, and tears down cleanly. Flecs Explorer launches in Debug
    (`http://localhost:27750`) and is verified by a unit test (`SceneWorldInit`).
- Exit: `pyxis.exe` runs and prints adapter on the lab machine; CI build green on Windows;
  `SceneWorldInit` unit test green; Flecs Explorer reachable in a Debug run.

### Phase M1 — Viewer triangle
- Add: GLFW window, NVRHI swapchain, `RenderGraph` + `IRenderPass`, hard-coded
  `TrianglePass`, ImGui (gated), `Profiler` skeleton, `CommandListMarker`.
- Exit: window draws a triangle at 60+ FPS; ImGui dockable panel with FPS shows.

### Phase M2 — Headless triangle
- Add: `VkDeviceManagerHeadless`, `HeadlessMode`, `parameters.json` loader, EXR writer
  (tinyexr), CLI parser.
- Exit: `pyxis --headless --config tests/fixtures/headless_triangle.json` writes a
  deterministic EXR; rerun gives byte-identical output.

### Phase M3 — Slang path-trace box
- Add: `Slang.cmake`+ShaderMake driver, `ShaderInterop.slang` with `PYXIS_INTEROP_STRUCT`,
  raygen/closesthit/miss for one cube, `BlasCache`, `TlasBuilder`, accumulation +
  tonemap passes.
- Wire **first non-trivial Flecs systems**: `System_UploadDirtyMaterials`,
  `System_BuildDirtyBlas`, `System_RebuildTlas` running in their phases inside
  `CommitResources`. The hardcoded cube is created via `GpuScene::CreateMesh` /
  `AppendInstance` so it goes through the real ECS pipeline.
- Hardcoded scene: one cube, one camera, one distant light. No Hydra.
- Exit: viewer + headless render the cube; accumulation converges; row-major matrix
  unit test green.

### Phase M4 — Hydra delegate stub + USD-direct stub
- Add: `pyxis_hydra` shared lib, `plugInfo.json`, `HdPyxisRendererPlugin`,
  `HdPyxisRenderDelegate` stub (mesh + camera + renderBuffer types only),
  `HdPyxisRenderPass` + `HdPyxisRenderTask` calling `PyxisRenderer::RenderFrame`.
- Add: `pyxis_usd_ingest` shared lib, `StageWalker` + `Geom/Mesh` + `Camera` minimal
  paths producing the same `MeshDesc`/`CameraDesc` for the same tiny `.usda`.
- Add: `pyxis_material_translation` static lib, used by both adapters from M4.
- Application gains `HydraEngine` and `UsdDirectEngine`; selection via
  `app.ingest = "hydra" | "usd_direct"`.
- Exit: `usdview` can pick the delegate; the tiny USD renders identically in standalone
  Pyxis (both adapters), and `usdview`. Regression image diff Hydra-vs-USD-direct = 0.

**M4 stub note**: at the M4 milestone Pyxis ships
`HydraEngine::Load(path, scene)` as a thin wrapper around the same
`pyxis_usd_ingest::StageWalker` the USD-direct path uses. This
guarantees the §25.O.3 byte-equal P0 invariant trivially for the
M4-tier scenes (mesh + camera, no shader-driven divergence between
adapters yet) and is verified by `M4.AdapterParityByteEqualEXR` in
the CTest suite. The pyxis_hydra delegate's real
`HdPyxisMesh::Sync` + `HdPyxisCamera::Sync` impls are wired so
`usdview` can drive Pyxis through the Hd plugin registry.

The full `UsdImagingStageSceneIndex → HdRenderIndex → HdEngine →
HdRenderTask` plumbing inside HydraEngine lands at **M5** alongside
the OpenPBR shader, when material-driven differences make the
Hydra dirty-bit dispatch + sync-cycle behaviour observable in the
EXR output. Until then the StageWalker shortcut is the documented
Pyxis-side adapter behaviour; usdview sees the per-prim Sync impls
since usdview drives via its own HdEngine pipeline outside Pyxis.

**Build artefacts shipped at M4**:
- `<bin>/pyxis_hydra.dll` (the loadable Hydra plugin)
- `<bin>/Resources/usd/hdPyxis/resources/plugInfo.json` (so Hd hosts
  discover the delegate via `PXR_PLUGINPATH_NAME` /
  `<host-dir>/usd/`)
- `<bin>/usd/` — vcpkg's USD plugin tree (44 plugins, ~7 MB) +
  aggregator `plugInfo.json` (`{"Includes": ["*/resources/"]}`) so
  USD's PlugRegistry resolves UsdGeom / UsdLux / UsdShade prim type
  IDs at startup. The aggregator is essential — without it
  `UsdStage::Open` blocks waiting for schemas to register and the
  app appears to hang. The pyxis_app `POST_BUILD` step writes both
  the tree copy and the aggregator.

**CI vcpkg cache (M4 only)**: `usd[imaging,materialx]` adds a
~30-90 min cold compile to first `cmake configure`. The
`x-gha,readwrite` binary-cache backend wired in
`.github/workflows/build.yml` + `CMakePresets.json`'s `ci` preset
caps that to seconds on every subsequent CI run. USD is also pinned
to `26.3` via `vcpkg.json` `overrides` so unrelated baseline rolls
don't invalidate the cache key. sccache (`mozilla-actions/sccache-
action`) layers on top for per-TU object caching.

### Phase M5 — UsdPreviewSurface → OpenPBR
- Add: `UsdPreviewSurfaceToOpenPBR` adapter, `MaterialTable` deduplication, `TextureCache`
  with stb/tinyexr decode, GPU mip generation pass, OpenPBR shader (single hit group,
  branchless on `flags`).
- Exit: textured cube via UsdPreviewSurface renders correctly headless and viewer; unit
  test for OpenPBR conversion field-by-field green.

### Phase M6 — Native instancing
- Add: `HdPyxisInstancer`, instance flattening (nested instancers), BLAS sharing rule,
  `instanceId` / `materialId` AOVs.
- Exit: a 10k-instance scene renders at interactive rates on the lab GPU; AOVs match
  expected mapping.

### Phase M7 — Lighting
- Add: distant + dome (lat-long EXR + alias table) + rect lights; importance sampling
  in raygen; NEE; multiple-importance sampling with BRDF.
- Exit: a Cornell-box-equivalent scene with all three light types converges to a known
  reference within RMSE tolerance.

### Phase M8a — Bistro render
- Add: full Amazon Lumberyard Bistro USD (interior + exterior) shipped via
  `tests/fixtures/bistro/` (or fetched via `PYXIS_BISTRO_DATASET_PATH`).
- Wire `UsdImagingStageSceneIndex` ingestion against the full Bistro stage; scene-index
  filters (flatten + prototype-propagating + material-binding), native instancing,
  subsets, large-texture handling exercised on real-shipped game-asset content.
- Exit: Bistro renders headless and viewer; recognizable visuals; load profile written.
  Used as the nightly regression seed (§35).

### Phase M8b — Bistro performance pass
- Tighten: lazy texture decode at scale, large-mesh chunked staging where needed, progress
  logging, `unsupported_features.json` writer, budget tracker hard caps active. No new
  features beyond M8a; pure perf + observability work driven by §34 profiles.
- Exit: Bistro hero camera meets §34.3 KPIs on RTX 4080 reference (`pass.PathTrace < 12 ms`,
  `pass.Accumulation+ToneMap+AovResolve < 2 ms`, `frame.cpu.commitResources < 2 ms`,
  `timeToFirstImage < 15 s`, p99/p50 < 1.4); per-frame profile written; load profile
  written.

### Phase M9 — Bistro visually correct
- Polish: dome+sun alignment, normals/tangents fallbacks, double-sided, emissive
  triangles, MaterialX coverage gaps closed for Bistro hero assets, small synthetic
  UDIM fixture green (UDIM path validated even though Bistro itself doesn't use UDIM).
- Exit: visually recognizable, color-correct Bistro frame on the hero camera; AOV outputs
  valid; accumulation converges.

### Phase M10 — Bistro headless + regression
- Add: Python regression harness, fixtures, CI pipeline (build + unit + tiny regressions),
  nightly Bistro job.
- Exit: nightly green; per-test KPIs CSV emitted.

### Phase M11 — Profiling polish
- Polish: full spdlog summary tables, ImGui profiler panels, JSON/CSV reports, Tracy.
- Exit: dashboards readable; profiling overhead < 1% in Release.

---

## 42. Strict "Do Not Build Yet" List


This list is the v1 floor. The full post-v1 scene-loading roadmap (animation,
incremental USD updates, point instancers, payloads, variants, purpose,
draw modes, UDIM, streaming, LRU eviction, OCIO, undo/redo, asset resolvers,
on-disk caches, …) lives in §40.5 and is not duplicated here.

- Subdivision surface limit-surface refinement.
- Curves, points, NURBS, volumes.
- True volumetric path tracing.
- Real-time denoiser (OIDN/OptiX).
- DLSS / frame generation.
- Multi-GPU / async-compute optimization.
- D3D12 backend, macOS, Linux.
- Per-material shader specialization (multi-hitgroup).
- Hot-swappable scene reload, animation, time-varying USD. (The bare
  resource-reset primitive `GpuScene::Clear()` ships v1 — see §19.4 — but
  the *animated / time-varying* reload path that drives `UsdNotice` and
  per-frame `Dirty<Transform>` updates is post-v1.)
- OpenColorIO full color management.
- USD lighting beyond distant/dome/rect.
- Material editor, node graph UI.
- Streaming-from-disk geometry / texture LRU eviction on the GPU.
- Custom asset resolvers.
- Network-distributed rendering, render farm features.
- Picking outside instance-ID AOV.

If any of these become tempting, log the temptation and keep going.

---

## 43. Domain Hooks Reserved for Post-v1


These are not implemented v1 but the architecture must not foreclose them.
Each item lists the reserved field / API space so adding the feature is a
MINOR bump (§22.3) rather than a MAJOR rework.

### 43.1 Light linking & shadow linking

- Reserved field on `LightDesc`: `uint64_t lightLinkSet = ~0ull;` (every bit
  set ⇒ "lights everything", the v1 default). Reserved field on
  `InstanceDesc`: `uint64_t lightLinkMask = ~0ull;`. Closesthit shader
  evaluates `(light.lightLinkSet & instance.lightLinkMask) != 0` before
  contributing the light's NEE term.
- 64 bits ⇒ 64 distinct light groups. Past 64 we go to a bindless mask
  table (RFC).
- v1 ships the fields zero-cost (`-1` bits skip the AND on common branch);
  the *evaluation* is enabled in M9+.

### 43.2 Motion blur

- Reserved fields on `RenderSettings`:
  `float shutterOpen = 0.0f, shutterClose = 0.0f;` (closed shutter ⇒ no blur,
  v1 default). On `InstanceDesc`:
  `hlslpp::float4x4 worldFromLocalPrev{};` (defaulted to identity ⇒
  prev == curr ⇒ zero motion vectors, matching §18.6's first-frame
  contract).
- The motion-vector AOV (§18.4 `MotionVector`) already uses the prev/curr
  matrix pair, so adding shutter integration is a closesthit-shader change
  and a TLAS-build flag (`PREFER_FAST_BUILD | MOTION`), not an API break.
- v1 closes the shutter unconditionally; M11 RFC re-enables.

### 43.3 Depth of field

- `CameraDesc::apertureFStop` is already in the API (`= 0.0f` ⇒ pinhole).
- v1 closesthit ignores it; the M9+ shader path samples a disk on the
  aperture (Halton dimensions reserved at §12.2: "lens → dim 2–3").
- No new public field needed; the conversion of `apertureFStop` ⇒ aperture
  radius is a shader-side function tied to `focalLengthMm`.

### 43.4 Tone-map LUT

- Reserved field on `RenderSettings`: `TextureHandle toneMapLut3D =
  TextureHandle::Invalid;`. Sampler: `RenderSettings::ToneMap::Lut3D`
  added to the enum *at the end* (additive — §22.1 MINOR).
- LUT format: 33³ RGB16F `.cube` decoded by `pyxis_app/Resources/cube_decoder.cpp`
  into a `Texture3D` uploaded through the standard `TextureCache`.
- Bypasses the operator switch in `ToneMap.cs.slang`; same pass otherwise.

### 43.5 Volumes / curves / displacement

- `pyxis_renderer/Public/Pyxis/Renderer/Descs/MeshDesc.h` ships an
  `enum class GeometryKind : uint8_t { Polymesh = 0, Volume = 1, Curves = 2,
  Subdiv = 3 };` field with a `Polymesh` default. v1 rejects anything other
  than `Polymesh` with `ErrorKind::GeometryKindUnsupported`.
- The kind byte is part of the public POD so a future `pyxis_volumes`
  ingest path can populate it without an ABI break.

---

# Part IX — Governance & Operations

## 44. RFC Process


Non-trivial design and process changes are proposed via short markdown documents
under `_documentation/rfcs/` before any code lands. The process is the gate
mentioned throughout the plan (§42, §40.1, §30.11, §18, §45, …).

### 44.1 What needs an RFC

- Anything that changes the public API surface (§18) in a non-trivial way:
  new method, new public POD, new handle, new error kind, new feature flag.
- Anything that changes the `_renderer/Private/Scene/` Flecs conventions (§30.11):
  new phase, reordered phases, new component category, observer policy change.
- Adding back any item from the §42 deferred-features list.
- Bumping a third-party dependency to a new MAJOR (e.g. OpenUSD v25 → v26).
- Changing the BLAS / TLAS flag policy (§16) or the bindless capacity (§5).
- Anything that removes or weakens a normative rule in §30.

### 44.2 What doesn't need an RFC

- Bug fixes that don't change behaviour outside the per-test RMSE tolerance.
- Internal refactors strictly inside one `Private/` folder.
- New `Private/` files, new internal helpers, new fixtures.
- New `Profiler` scopes, new spdlog categories.
- New CI checks that strictly tighten existing rules.

### 44.3 Template

`_documentation/rfcs/0000-template.md`:

```markdown
# RFC NNNN: <short title>

- Status: Draft | Review | Accepted | Rejected | Superseded by NNNN
- Author(s):
- Created:
- Last updated:
- Implementation PRs:

## Summary
One paragraph.

## Motivation
What problem, why now, who's blocked.

## Detailed design
The actual change. Code sketches. POD diffs. Phase-pipeline diffs.
Cross-reference plan.md sections.

## Alternatives considered
At least two; explain why rejected.

## Drawbacks / risks
Honest list, including ABI / regression-image impact.

## Migration & impact
Who must do what, and on what timeline. List affected milestones (§38/§41).

## Open questions
Questions blocking acceptance.
```

### 44.4 Lifecycle

1. PR opens against `_documentation/rfcs/NNNN-<slug>.md` with `Status: Draft`.
2. **Minimum 7 calendar days** between PR open and merge (gives reviewers time;
   shortened to 24 h only for security fixes by code-owner consensus).
3. **At least one code-owner approval** for the area being changed (§45).
   For RFCs touching the public API, **two** approvals are required.
4. Status transitions to `Accepted` and the RFC merges *before* any implementation
   PR. The implementation PR's description links the RFC number.
5. Rejected RFCs merge with `Status: Rejected` plus a "Why rejected" section. They
   are searchable so the same idea isn't re-proposed.
6. Superseded RFCs link forward (`Superseded by NNNN`).

### 44.5 Decision log

`_documentation/rfcs/README.md` lists every RFC with one-line status. A merged
plan.md change that resolves an RFC links the RFC in its commit message.

---

## 45. Repository Governance


### 45.1 `CONTRIBUTING.md` (shipped at repo root)

Required sections:

1. **Build from source** — pointer to `_documentation/getting_started.md` (§50).
2. **How to run the regression suite** — `_tools/run_regression.py` invocation.
3. **Code style** — link to `.clang-format`, `.clang-tidy`, §30.
4. **Commit-message convention** — Conventional Commits 1.0.0:
   `<type>(<scope>): <subject>` where `type ∈ {feat, fix, perf, refactor, test,
   docs, build, ci, chore, revert}` and `scope ∈ {platform, renderer, hydra,
   usd_ingest, material, app, shaders, build, ci, docs, rfc}`. Body is optional;
   breaking-change footers (`BREAKING CHANGE:`) are required for any §22.1 MAJOR
   diff. CI lints with `_tools/check_commit_messages.py`.
5. **Branch policy** — `main` is protected; PRs only; squash-merge default.
6. **PR checklist** (mirror in `.github/pull_request_template.md`):
   - [ ] `clang-format` clean.
   - [ ] `clang-tidy` no new warnings.
   - [ ] Unit tests added/updated where relevant.
   - [ ] Regression fixtures updated where the PR touches §35's rule.
   - [ ] If the PR touches `Public/`, `version.txt` bumped per §22.
   - [ ] If the PR adds a new third-party dep, both `vcpkg.json` baseline and
         `_cmake/Thirdparty.cmake` SHA updated.
   - [ ] If the PR violates a §30 rule, link the RFC that approved it.
   - [ ] No `[[deprecated]]` removals before §22.3 window expires.
7. **Reviewer checklist** — what reviewers must check (this is the artefact the
   plan keeps referencing as "reviewers reject"). Lives at
   `.github/REVIEW_CHECKLIST.md`. Includes:
   - §30.3 header-discipline scan (no `pxr/` in renderer; no STL containers in
     `Public/`).
   - §30.11 Flecs scan (no per-frame query construction; no observer abuse).
   - §18.9 ABI scan (no STL container in any new Desc; offsets reviewed).
   - Profiler scopes present on every new pass / hot CPU function (§34).
   - Tracy/spdlog category prefix follows §31 convention.
   - Error catalogue (§20) used; no raw `bool` returns for failure paths.

### 45.2 `CODEOWNERS`

Shipped at repo root in GitHub-flavoured CODEOWNERS syntax. Initial mapping:

```
# Default fallback
*                                                       @pyxis-maintainers

# Per-module ownership
/sources/pyxis_platform/                                @pyxis-platform-team
/sources/pyxis_renderer/                                @pyxis-renderer-team
/sources/pyxis_renderer/Public/                         @pyxis-renderer-team @pyxis-maintainers
/sources/pyxis_renderer/Private/Scene/                  @pyxis-ecs-team @pyxis-renderer-team
/sources/pyxis_renderer/Private/Passes/                 @pyxis-renderer-team
/sources/pyxis_renderer/Private/Shaders/                @pyxis-shading-team
/sources/pyxis_hydra/                                   @pyxis-ingest-team
/sources/pyxis_usd_ingest/                              @pyxis-ingest-team
/sources/pyxis_material_translation/                    @pyxis-shading-team @pyxis-ingest-team
/sources/pyxis_app/                                     @pyxis-app-team
/resources/shaders/                                     @pyxis-shading-team
/_cmake/                                                @pyxis-build-team
/_pipelines/                                            @pyxis-build-team
/_documentation/rfcs/                                   @pyxis-maintainers
/plan.md                                                @pyxis-maintainers
/version.txt                                            @pyxis-maintainers
```

Public-API changes (§18) require two approvals because **`/sources/pyxis_renderer/Public/`**
and `/plan.md` are dual-owned by `@pyxis-renderer-team` *and* `@pyxis-maintainers`.

### 45.3 Triage & on-call

- **Nightly regression failures**: a rotating one-week on-call slot among
  `@pyxis-maintainers`. The on-call's only commitment is to **acknowledge a red
  nightly within 24 h** (label, assign, and either revert the offending PR or
  open a tracking issue).
- **Severity classes**:
  - **S1** — main branch fails to build, or the headless smoke-test crashes.
    Revert-or-fix within 24 h. Block all merges to main until green.
  - **S2** — nightly subset-Moana RMSE regression > 2× tolerance, or peak GPU
    > +20 % baseline. Tracking issue + assignment within 48 h.
  - **S3** — flaky test, perf jitter inside ±10 % budget. Logged; addressed in
    the next maintenance sprint.
- **No silent skips.** A failing test is *never* `@disabled` without a tracking
  issue + a target re-enable date in the issue.
- **Postmortems** for any S1 incident: short markdown under
  `_documentation/postmortems/YYYY-MM-DD-<slug>.md`, no blame, focus on the
  detection-and-recovery loop.

---

## 46. Build Supply Chain


### 46.1 vcpkg binary cache

Without a binary cache, every clean CI machine and every fresh dev clone rebuilds
OpenUSD + MaterialX from source (~20 min). v1 ships with **two** cache backends
configured, fronted by vcpkg's standard env vars:

- **CI**: a project NuGet feed (`VCPKG_BINARY_SOURCES=clear;nuget,<url>,readwrite`).
  CI build agents write; PR build agents read.
- **Dev**: `VCPKG_DEFAULT_BINARY_CACHE=%LOCALAPPDATA%/Pyxis/vcpkg-cache` (the vcpkg
  default location on Windows). No upstream sharing — purely a per-developer cache
  to avoid repeat work between branches.

CI is required to keep wall-clock ≤ 10 minutes for the build+unit+tiny-regression
stage. A monthly job purges binary-cache entries older than 90 days.

### 46.2 Reproducible builds

Pyxis aims for **byte-identical Release binaries** given the same source tree, the
same vcpkg baseline, and the same toolchain pin. Concretely:

- `__DATE__` / `__TIME__` are **forbidden**: a `clang-tidy` rule
  (`bugprone-reserved-identifier` extended) catches them, and the CI compiler flag
  `-Werror=date-time` (clang-cl) is set on every target.
- `__FILE__` is allowed in `PYXIS_ERROR(...)` macros but normalised to a
  repo-relative path via the clang-cl `-fmacro-prefix-map=<srcroot>=.` flag, so PDB
  paths and error messages don't leak absolute developer paths.
- PDBs are stripped of `/SOURCE_LINK` absolute paths via
  `link.exe /PDBALTPATH:%_PDB%`.
- Build hosts pin the Windows SDK version explicitly in the toolchain file.
- v1 commitment is *bit-reproducible Release `pyxis_renderer.dll`* in CI.
  PDB byte-identity and PE timestamp normalisation (`/Brepro`) are stretch goals.

### 46.3 SBOM (deferred to v1.1, scaffolding ready)

- v1 ships `NOTICE` (third-party attributions) plus `LICENSE`. SBOM generation
  (CycloneDX or SPDX) is **not required for v1**.
- v1.1 wires `_tools/generate_sbom.py` into the install step, driven by
  `vcpkg list --x-installed` plus `_cmake/Thirdparty.cmake`'s pinned SHAs. Output:
  `pyxis-<version>-sbom.cdx.json`. CI artifact alongside the installer.

### 46.4 Code signing (Authenticode)

- v1 distributes unsigned binaries; the README documents this and the SHA-256
  hashes published per release.
- Once a code-signing certificate is procured (post-v1, project-internal decision):
  - All shipped DLLs and `pyxis.exe` are signed via `signtool` invoked by the
    `_pipelines/release.yml` pipeline.
  - The pipeline secret references the cert; never checked in.
  - SHA-256 hashes plus signing timestamp are published in the GitHub release
    notes.

### 46.5 First-clone cache-warm

For developers, fresh-clone build time is dominated by OpenUSD. v1 ships:

- `_tools/cache_warm.cmd` — runs `cmake --preset dev` configure (which
  populates the vcpkg binary cache from the public NuGet feed if the dev opted in)
  + a no-op build of `pyxis_platform` (forces FetchContent for OpenUSD's
  pinned SHA into the local `_deps/` folder). After running, a fresh build of
  `pyxis.exe` is < 5 min.
- `_documentation/getting_started.md` (§50) walks through both the cache-warm
  path and the slow-path (no cache) so new developers see realistic times either
  way.

---

## 47. Crash Reporting, Telemetry, Update Policy


### 47.1 Crash reporter (minidump)

Aftermath (§33.9) covers GPU device-lost. CPU crashes need a separate path.

- A top-level SEH filter is installed in `pyxis_app/Main.cpp` via
  `SetUnhandledExceptionFilter(&pyxis::CrashHandler)`. The handler writes a
  minidump via `MiniDumpWriteDump(MiniDumpWithDataSegs | MiniDumpWithIndirectlyReferencedMemory)`
  to `%LOCALAPPDATA%/Pyxis/Crashes/<timestamp>/pyxis-<version>-<gitSha>.dmp`.
- The handler also flushes spdlog and Tracy before re-raising the exception, and
  copies the last 4 KiB of the spdlog ringbuffer alongside the dump as
  `spdlog-tail.log`. This is the same dump folder Aftermath writes into (§33.9), so
  GPU and CPU crashes share one inspection path.
- A symbol server is **not** part of v1; PDBs are shipped alongside the installer
  in a sibling `pyxis-<version>-pdbs.zip`. PDB upload to a public symbol server
  (Microsoft public symbol server format) is RFC'd post-v1.
- No third-party crash uploader (Sentry, Backtrace, Crashpad) is integrated v1;
  the `_documentation/troubleshooting.md` (§50) explains how to attach the dump
  folder to a GitHub issue.

### 47.2 Log rotation

`pyxis::Logging::Get()` (§33.10) is configured with a **rotating sink**:

- File: `%LOCALAPPDATA%/Pyxis/Logs/pyxis-<pid>-YYYYMMDD.log`.
- Rotation: 64 MiB per file, 10 files retained per `<pid>` ⇒ ≤ 640 MiB per
  process. Sized to absorb a Moana ingest run with chatty unsupported-feature
  warnings without truncating the tail.
- Process-exit cleanup: log files older than 30 days are deleted on startup
  (best-effort; failures swallowed).
- Headless `--profile` mode adds an extra unrotated sink at the path requested
  by `output.effectiveConfig`'s sibling slot, capped at 100 MiB; if exceeded the
  run aborts with `ErrorKind::FileQuotaExceeded` (added to §20) rather than
  filling the disk.

### 47.3 Telemetry

**Pyxis ships zero telemetry in v1.** No phone-home, no anonymous usage stats,
no automatic crash-dump upload. This is normative:

- No code in any module opens an outbound network socket. CI runs the
  `_tools/check_no_network.py` script that greps for `WSAStartup`, `socket`,
  `connect`, `WinHttp*`, `curl_easy_*`, `cpr::`, etc., and fails on any hit
  outside an `// allow-network: <reason>` annotation.
- If telemetry is added post-v1, it must be **opt-in only**, default off, and
  governed by a `parameters.json` field `app.telemetry.enabled = false` plus
  a UI toggle in viewer mode. The first opt-in version bumps MAJOR per §22.

### 47.4 Update / version-check policy

- `pyxis.exe` does **not** check for updates over the network. There is no
  `pyxis update` subcommand v1.
- `pyxis --version` prints `PYXIS_VERSION_STRING` + git SHA + ingest adapter
  list + Vulkan device used (no network).
- A future "check for updates" feature would require an opt-in toggle and an
  explicit RFC.

---

## 48. Compliance & Distribution Stance


### 48.1 Privacy / GDPR

- Pyxis processes **no personal data**. No telemetry (§47.3), no user
  identifiers, no scene file uploads.
- Configuration files (`parameters.json`) and per-run reports
  (`profile.json`, `unsupported_features.json`) live entirely on the local
  machine under `%LOCALAPPDATA%/Pyxis/` or the explicit `output.*` paths.
- The SEH minidump (§47.1) and Aftermath dump (§33.9) are written locally
  and never uploaded; the user is responsible for sharing them on a GitHub
  issue at their discretion.
- This stance is documented in `_documentation/privacy.md` and reflected
  in the in-app "About" panel.

### 48.2 Export control

- Pyxis links Vulkan with ray-tracing extensions, NVIDIA Aftermath (optional),
  and Slang (an open-source compiler). None of these are subject to export
  control beyond the underlying GPU vendor's own SDK distribution terms.
- The project distributes only source code and pre-built Windows binaries;
  consumers in regions where NVIDIA SDK redistribution is restricted are
  responsible for sourcing the SDK themselves and rebuilding.
- An export-control / EULA stub lives at `_documentation/distribution.md`;
  legal review is required before the first public release tag.

### 48.3 License audit

- The Apache-2.0 + dependency-mix audit (OpenUSD + MaterialX + NVRHI +
  Slang + Tracy + spdlog + GLFW + ImGui + stb + tinyexr + MikkTSpace +
  hlslpp + nlohmann/json + gtest + Flecs + moodycamel-concurrentqueue) is
  a `_tools/license_audit.py` script that walks `vcpkg list` plus
  `_cmake/Thirdparty.cmake` and emits `NOTICE.generated`. The shipped
  `NOTICE` is asserted byte-equal to `NOTICE.generated` in CI; drift fails
  the build.
- Adding a dep with a non-compatible licence (GPL, AGPL, SSPL) fails the
  same CI step.


# Part X — Reference Appendices

## 49. CMake Architecture Detail


- Top-level `CMakeLists.txt` enforces:
  - `set(CMAKE_CXX_STANDARD 23)`, `CMAKE_CXX_EXTENSIONS OFF`, `CMAKE_CXX_STANDARD_REQUIRED ON`.
  - `set(CMAKE_CXX_COMPILER clang-cl)` via toolchain file `_cmake/clang-cl.toolchain.cmake`.
  - `add_compile_options(/W4 /WX /permissive- /Zc:__cplusplus /Zc:preprocessor /utf-8)`.
- Per-target options:
  - `pyxis_platform`, `pyxis_renderer`: `/GR-`, `/EHs-c-`, `PYXIS_NO_EXCEPTIONS`.
  - `pyxis_hydra`, `pyxis_usd_ingest`, `pyxis_material_translation`: `/GR`, `/EHsc`
    (USD requires both).
  - `pyxis_renderer` links `flecs::flecs_static` PRIVATE; the Flecs include directory is
    not propagated to dependents (no `target_include_directories(... PUBLIC ...)`).
- `_cmake/Compiler.cmake` provides `pyxis_set_target_compile_options(target)`,
  `pyxis_treat_thirdparty_as_external(target)`, `pyxis_define_module(name)`.
- `_cmake/Slang.cmake`/`_cmake/ShaderMake.cmake` defines `pyxis_add_shader_target(...)`.
  Inputs: list of `.slang` files, manifest, output dir. Output: a custom target that
  produces `.spv` + a generated `ShaderRegistry.cpp` mapping permutation IDs to file paths.
- vcpkg manifest mode (`vcpkg.json`) for spdlog, glfw, gtest, nlohmann-json, tinyexr,
  stb, mikktspace, hlslpp, tracy, moodycamel-concurrentqueue, **flecs** (with the
  `rest` feature in Debug builds only, gated by `PYXIS_DEBUG_TOOLS`). **spdlog and
  Tracy are linked SHARED** (§33.10): the project ships a custom triplet
  `_cmake/triplets/x64-windows-pyxis.cmake` that sets
  `VCPKG_LIBRARY_LINKAGE = dynamic` for those two ports while keeping the rest of
  the manifest at the default static linkage. CI verifies via
  `dumpbin /exports pyxis_platform.dll | findstr spdlog` (must succeed) and
  `dumpbin /exports pyxis_renderer.dll | findstr spdlog` (must be empty). NVRHI /
  Slang / ShaderMake / OpenUSD / MaterialX are built from source via `FetchContent`
  (pinned commits) so we control configuration. **Single source of truth for versions:**
  - `vcpkg.json` carries a pinned `builtin-baseline` (commit SHA of the vcpkg registry)
    plus per-port `version>=` lower bounds; the baseline is what reproduces a build.
  - `_cmake/Thirdparty.cmake` declares each `FetchContent_Declare` with an explicit
    `GIT_TAG <full-40-char-SHA>` (never a branch name, never a moving tag). A CI step
    `_tools/check_pins.py` greps the file and fails if any tag is shorter than 40 chars
    or matches `master|main|HEAD`.
  - Both files are reviewed together; PRs that bump one without the other are rejected.
- Install: `pyxis.exe`, `pyxis_renderer.dll`, `pyxis_platform.dll`, `pyxis_hydra.dll`,
  `pyxis_usd_ingest.dll`, `plugInfo.json`, `resources/shaders/*.spv`, USD plugins folder
  layout, **`pyxis_app/Resources/parameters.schema.json`** (the JSON Schema
  draft-2020-12 document validated against in CI — see §27). `LICENSE` (Apache 2.0)
  and `NOTICE` (third-party attributions, including Flecs's MIT notice) are installed
  at the install-tree root. Headless-friendly install tree.
- `pyxis_renderer.dll` is built with `/Zc:throwingNew` (clang-cl), which keeps
  `operator new`'s standard contract (it may signal failure by throwing
  `std::bad_alloc`) — even though `/EHs-c-` disables exception handling. Under
  `/EHs-c-` an unhandled throw goes straight to `std::terminate`; we install a
  `std::set_terminate(&pyxis::FatalTerminate)` handler in `pyxis_platform`'s
  startup that flushes spdlog + Tracy and routes through `PYXIS_FATAL` for a
  clean exit-with-diagnostics instead of OS-level UB. (`/Zc:throwingNew` is
  accepted by clang-cl; verified in CI on the toolchain pinned in
  `_cmake/clang-cl.toolchain.cmake`.)

---

## 50. Documentation Deliverables


Every section below has a target file and an owner.

| File | Purpose | Owner |
|---|---|---|
| `_documentation/getting_started.md` | First-run walkthrough: clone → build → run viewer triangle → run headless triangle. | `@pyxis-build-team` |
| `_documentation/troubleshooting.md` | Symptom → cause matrix. Required entries: validation-layer error, device-lost, OOM, missing texture, missing material, slow first frame, TLAS instance-limit hit, ingest adapter mismatch. | `@pyxis-maintainers` |
| `_documentation/openpbr.md` | Full conversion table mentioned in §11. | `@pyxis-shading-team` |
| `_documentation/parameters.md` | Schema walkthrough + four canonical configs from §26. | `@pyxis-app-team` |
| `_documentation/profiling.md` | Tracy / spdlog / JSON / CSV reading guide. | `@pyxis-maintainers` |
| `_documentation/colorblind_palettes.md` | AOV-debug palette reference (§29.2). | `@pyxis-app-team` |
| `_documentation/accessibility.md` | Accessibility statement + shortcut table (§29.2). | `@pyxis-app-team` |
| `_documentation/postmortems/` | One file per S1 incident (§45.3). | rotating |
| `_documentation/rfcs/` | RFC archive (§44). | rotating |
| `tests/fixtures/sample_usd/welcome.usda` | A small "drop your USD here" hello-Pyxis scene shipped in the installer. | `@pyxis-ingest-team` |

Reviewers check the §45.1 PR checklist for "documentation updated where the
PR changes user-visible behaviour".

---

