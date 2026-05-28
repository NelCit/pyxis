---
name: flecs-conventions-audit
description: Audit Pyxis ECS code under sources/pyxis_renderer/Private/Scene/ against §30.11 Flecs conventions and §31 single-writer threading. Invoke when a component, system, query, observer, or phase is added/changed, when reviewing PRs that touch Scene/, or when investigating per-frame allocations in the ECS path. Reports PR-blocking violations with plan section citations.
---

# flecs-conventions-audit

§30.11 codifies Flecs usage; reviewers reject any violation. Flecs is renderer-PRIVATE — not a single Flecs symbol may leak into `Public/` or any other module.

Since RFC 0009 the `flecs::world` is owned by `GpuScene::Impl`, so the ECS now lives across **two** Private folders: `Private/GpuScene/` (the real entities, components, systems, cached queries) and `Private/Scene/` (the shared primitives that survived the facade retirement). Audit both.

## Targets

- `sources/pyxis_renderer/Private/GpuScene/Internal.h` — the per-entity POD components (`GpuLightComponent`, `GpuMaterialComponent`, `GpuTextureComponent`, `GpuMeshComponent`, `GpuInstanceComponent`, `GpuVolumeComponent`) + the `MeshOf`/`MaterialOf` relationship tags
- `sources/pyxis_renderer/Private/GpuScene/Commit.cpp` — `RegisterCommitPipeline()` registers the real commit systems (`Sys_*` run-callbacks) on the §30.11 phases, driven by `CommitResources → world.progress()`; cached queries (e.g. `lightQuery`) are built once in the `GpuScene` ctor
- `sources/pyxis_renderer/Private/GpuScene/GpuSlotMap.h` — the Flecs-backed handle table every entity type uses (slot == GPU buffer index, §19.7)
- `sources/pyxis_renderer/Private/Scene/Phases.h` — the `PhaseUpload*`/`PhaseExtractMeshes`/… custom pipeline tags + `RegisterPhasePipeline`
- `sources/pyxis_renderer/Private/Scene/HandleBimap.h` — the light handle table
- `sources/pyxis_renderer/Private/Scene/Components/Dirty.h` — the `Dirty<T>` zero-size tags (`DirtyTopology`/`DirtyTransform`/`DirtyTexture`/`DirtyMaterial`/`DirtyVisibility`/`DirtyLight`)

## PR-blocking patterns to grep

Run these from the repo root with Grep:

| Pattern | Glob | Rule |
|---|---|---|
| `flecs` (any include or symbol) | `sources/pyxis_renderer/Public/**` | §30.11 — no Flecs in `Public/` |
| `flecs` | `sources/pyxis_hydra/**`, `sources/pyxis_usd_ingest/**`, `sources/pyxis_app/**`, `sources/pyxis_platform/**` | Flecs is renderer-private |
| `query_builder<`, `world\.query<`, `\.query<` inside a `Sys_*` run-callback | `sources/pyxis_renderer/Private/GpuScene/**` | §30.11 — per-frame query construction is PR-blocking; queries must be cached once (e.g. the `GpuScene` ctor builds `lightQuery`) |
| `std::vector`, `std::string`, `std::map`, `std::unordered_*`, `virtual ` inside a component **struct** (`Gpu*Component`, `MeshOf`/`MaterialOf`, `Dirty.h` tags) | `Private/GpuScene/Internal.h`, `Private/Scene/Components/**` | §30.11 — the per-entity component structs are POD; variable-length data lives in the slot-indexed side tables on `GpuScene::Impl` (those legitimately use `std::vector` — don't flag the Impl side tables, only the component structs) |
| `flecs::OnUpdate`, `flecs::PostUpdate`, `flecs::PreUpdate`, `flecs::OnLoad`, `flecs::PostLoad` | anywhere | §30.11 — built-in pipeline phases are not used; only the custom `Phase*` tags from `Scene/Phases.h` |
| `world.entity(`, `\.set<`, `\.destruct\(\)` | `sources/pyxis_hydra/**`, `sources/pyxis_usd_ingest/**` (or any non-render-thread callsite) | §30.11 + §31 — single-writer; ingest threads must enqueue `MutationCommand` records, only the render thread mutates the world (the `GpuScene` verbs run on the render thread, which is allowed) |
| `\.observer<` | `sources/pyxis_renderer/Private/GpuScene/**` | §30.11 — observers are reserved for cross-component invariants (refcount → deletion, BLAS release). Don't use observers as a substitute for systems; confirm the use case is documented. |

## Naming

- Components: PascalCase POD struct (e.g. `GpuMeshComponent` in `GpuScene/Internal.h`); the `Dirty<T>` tags live in `Scene/Components/Dirty.h`, one type per declaration.
- Systems: registered in `GpuScene/Commit.cpp::RegisterCommitPipeline()` as `Sys_VerbObject` run-callbacks bound to a phase (e.g. `Sys_BuildBlas`, `Sys_RebuildTlas`), each forwarding to an `Impl::Upload*`/`Build*` method.
- `Dirty<T>` is a zero-size tag, cleared two ways: FINE-grained tags whose system processes only the tagged entities clear the tag themselves (`DirtyTopology` once the BLAS is built; `DirtyTexture` after upload); COARSE "buffer dirty?" tags (`DirtyTransform`/`DirtyVisibility`/`DirtyMaterial`/`DirtyLight`) are cleared by `Sys_ClearDirty` on `PhaseClearDirty`. A removal (which can't tag the dead entity) tags the persistent `removalSentinel` instead.
- Prefer pair relationships `(Instance, MaterialOf, materialEntity)` over entity-field components.

## Phase pipeline (§30.11)

Reordering or inserting between the `Scene/Phases.h` phase tags requires an RFC (§44). Flag any diff that adds a `world.system(...).kind<...>()` with a kind that isn't one of the `Scene/Phases.h` `Phase*` tags, or any change to `Phases.h`/`RegisterPhasePipeline` that reorders existing phases — and prompt the user to point to the RFC.

## Single-writer mutation (§30.11 + §31)

The render thread is the only thread permitted to call `world.entity(...)`, `e.set<T>(...)`, or `e.destruct()`. Ingest threads (`pyxis_hydra`, `pyxis_usd_ingest`) push `MutationCommand` records onto the `moodycamel::ConcurrentQueue`, drained at the start of `CommitResources`.

Confirm any new ingest-side code uses the queue, not direct world mutation.

## Debug Explorer

Flecs Explorer (REST UI on `http://localhost:27750`) is gated behind `PYXIS_DEBUG_TOOLS`. New Explorer-related code must respect that gate.

## Output

```
## PR-blocking (§30.11)
- sources/pyxis_renderer/Private/Scene/Systems/Materials.cpp:73 — query_builder<MaterialDirty> built per frame; move to QueryCache.h

## Threading (§31)
- sources/pyxis_hydra/Private/MeshAdapter.cpp:128 — direct world.set<>(); must enqueue MutationCommand instead

## Naming / structure
- ...

## OK
- (n components, m systems, k queries audited; no new violations)
```

Cite the plan section for every finding. Don't auto-fix — report only.
