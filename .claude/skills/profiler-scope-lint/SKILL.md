---
name: profiler-scope-lint
description: Verify Pyxis render passes, hot CPU paths, and ingest entry points carry the right Profiler/Tracy scopes with §31 dotted prefixes (pass.*, render.*, frame.*, assets.*, ingest.hydra.*, ingest.usd.*, ingest.shared.*, app.*). Invoke when a new IRenderPass is added, when a hot function is created or renamed, or before a PR that introduces a new render-graph pass. Reports missing/misnamed scopes per §34.
---

# profiler-scope-lint

§45.1 reviewer checklist requires: *"Profiler scopes present on every new pass / hot CPU function (§34). Tracy/spdlog category prefix follows §31 convention."* Missing or wrongly-prefixed scopes break the load-time and per-frame KPI dashboards.

## Macros / scopes (recap)

- `ProfilingScope` — CPU RAII scope, feeds `ProfilerData`
- `ProfilingPass` — GPU RAII scope around a render-graph pass, feeds `GpuTimestampPool` → `FrameProfile`
- Both flow into spdlog summary, ImGui panel, JSON/CSV, and Tracy

## Required dotted prefixes (§31)

| Prefix | Owner thread | Examples |
|---|---|---|
| `pass.*` | render thread (GPU) | `pass.PathTrace`, `pass.Accumulation`, `pass.ToneMap`, `pass.AovResolve`, `pass.DebugView`, `pass.CopyToHydraBuffer`, `pass.PresentBlit` |
| `accel.*` | render thread (GPU) | `accel.TlasBuild`, `accel.BlasBuild` |
| `texture.*` | render thread (GPU) | `texture.UploadCopy` |
| `frame.cpu.*` | render thread (CPU) | `frame.cpu`, `frame.cpu.flecsTick`, `frame.cpu.commitResources`, `frame.cpu.renderGraph.compile`, `frame.cpu.renderGraph.execute`, `frame.cpu.recordCommandLists`, `frame.cpu.imguiBuild`, `frame.cpu.present.wait` |
| `render.*` | render thread (CPU, load-time) | `render.commitResources`, `render.frame.firstFrame`, `render.frame.timeToFirstImage`, `render.geometry.upload`, `render.texture.upload`, `render.blas.build`, `render.tlas.build` |
| `assets.*` | I/O pool | `assets.texture.pathResolve`, `assets.texture.decode`, `assets.mesh.extraction`, `assets.mesh.triangulation`, `assets.primvar.extraction`, `assets.mesh.mikktSpace` |
| `ingest.hydra.*` | ingest thread (Hydra adapter) | `ingest.hydra.init`, `ingest.hydra.primDiscovery`, `ingest.hydra.firstSync`, `ingest.hydra.sync` |
| `ingest.usd.*` | ingest thread (USD-direct adapter) | `ingest.usd.stageOpen`, `ingest.usd.stageWalk`, `ingest.usd.populationMask`, `ingest.usd.composition` |
| `ingest.shared.*` | shared (`pyxis_material_translation`) | `ingest.shared.material.network.extraction`, `ingest.shared.material.openpbr.conversion` |
| `app.*` | application | `app.imgui`, `app.config.load`, `app.report.write` |

A scope must run on the thread its prefix names — never re-tag work in another zone (§31).

## What to check

1. **Every new `IRenderPass` subclass** under `sources/pyxis_renderer/Private/Passes/` or `sources/pyxis_renderer/Private/RenderGraph/`:
   - `Execute(ICommandList*)` opens a `ProfilingPass` named `pass.<PascalCaseName>` matching the pass class.
   - Resolution / sample-count / dispatch-size / output-format are recorded for the pass (§34.2).
2. **Every new commit-resources phase** in `sources/pyxis_renderer/Private/Scene/` or `Private/GpuScene/`: opens a `ProfilingScope` under the correct `frame.cpu.*` or `render.*` name.
3. **Every new ingest entry point** under `sources/pyxis_hydra/`, `sources/pyxis_usd_ingest/`, or `sources/pyxis_material_translation/`: opens a `ProfilingScope` under `ingest.hydra.*`, `ingest.usd.*`, or `ingest.shared.*` matching the work.
4. **Every new asset-pool job** under `Private/Assets/` or equivalent: `assets.*`.
5. **Auto-named Flecs system scopes**: every system that runs in the render-frame schedule must be auto-named from the system identifier (so adding a system surfaces a scope automatically). Confirm any new system goes through the registration helper that wires this.

## Anti-patterns to flag

- Profiler scope without a dotted prefix (`ProfilingScope("DoWork")`).
- Wrong prefix for the owning thread (e.g. `pass.*` on a CPU-only function, `assets.*` on the render thread).
- Ad-hoc strings like `"misc"`, `"todo"`, `"part1"`, `"foo"` — names must be stable and dashboard-readable.
- A pass with no `ProfilingPass` at all.
- A renamed scope without an entry in the §34 named-scopes list (load-time scopes have a fixed must-exist set; deviations are §34.1 violations).
- New shader specialisation constants, new bindless capacity > 80k, or hand-vectorised hot loops added without a Tracy/`FrameProfile` entry justifying the change (§34.3 anti-patterns "forbidden without profile evidence").

## KPIs to re-check after a change

If the diff touches a hot path, remind the user of the per-frame KPIs (1080p hero camera, RTX 4080, post-warm):

- `pass.PathTrace` < 12 ms
- `pass.Accumulation + pass.ToneMap + pass.AovResolve` < 2 ms combined
- `frame.cpu.commitResources` < 2 ms steady state
- p99 / p50 frame ratio < 1.4

Load-time:

- `render.frame.timeToFirstImage` for Moana subset (M8a) < 30 s on the lab machine
- `assets.texture.decode` parallelism ≥ 6 of 8 worker threads sustained
- `render.blas.build` ≥ 4 builds in flight at peak

§34.3: **PRs without profile evidence are rejected.** A regression > 10% on any pinned KPI is reverted by default.

## Output

```
## Missing scopes (PR-blocking)
- sources/pyxis_renderer/Private/Passes/DenoisePass.cpp:Execute():14 — no ProfilingPass; expected `pass.Denoise`

## Misnamed / wrong prefix
- sources/pyxis_hydra/Private/Sync.cpp:88 — ProfilingScope("hydraSync"); expected `ingest.hydra.sync`

## Ad-hoc / dashboard-unfriendly
- sources/pyxis_renderer/Private/Foo.cpp:33 — ProfilingScope("part1"); rename to a stable §31-prefixed name

## KPI reminder
- This PR touches PathTracePass; rerun pass.PathTrace KPI on the 1080p hero camera before merge
```

Don't auto-fix — report only.
