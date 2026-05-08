---
name: flecs-conventions-audit
description: Audit Pyxis ECS code under sources/pyxis_renderer/Private/Scene/ against §30.11 Flecs conventions and §31 single-writer threading. Invoke when a component, system, query, observer, or phase is added/changed, when reviewing PRs that touch Scene/, or when investigating per-frame allocations in the ECS path. Reports PR-blocking violations with plan section citations.
---

# flecs-conventions-audit

§30.11 codifies Flecs usage; reviewers reject any violation. Flecs lives strictly inside `sources/pyxis_renderer/Private/Scene/` — not a single Flecs symbol may leak into `Public/` or any other module.

## Targets

- `sources/pyxis_renderer/Private/Scene/Components/` — POD components, one per header
- `sources/pyxis_renderer/Private/Scene/Systems/` — free functions named `System_VerbObject`
- `sources/pyxis_renderer/Private/Scene/Queries/QueryCache.h` — every cached `flecs::query_t*`
- `sources/pyxis_renderer/Private/Scene/Observers/`
- `sources/pyxis_renderer/Private/Scene/Phases.h` — `PYXIS_PHASE_*` pipeline registration
- `sources/pyxis_renderer/Private/Scene/World.h`, `Pipeline.cpp`, `HandleBimap.h`

## PR-blocking patterns to grep

Run these from the repo root with Grep:

| Pattern | Glob | Rule |
|---|---|---|
| `flecs` (any include or symbol) | `sources/pyxis_renderer/Public/**` | §30.11 — no Flecs in `Public/` |
| `flecs` | `sources/pyxis_hydra/**`, `sources/pyxis_usd_ingest/**`, `sources/pyxis_app/**`, `sources/pyxis_platform/**` | Flecs is renderer-private |
| `query_builder<` | `sources/pyxis_renderer/Private/Scene/Systems/**` | §30.11 — per-frame query construction is PR-blocking; queries must be cached at registration in `QueryCache.h` |
| `world\.query<` or `\.query<` inside system body | `Systems/**` | same as above |
| `std::vector`, `std::string`, `std::map`, `std::unordered_*` | `Components/**` | §30.11 — components are POD, no STL containers; variable-length data lives in `Private/GpuScene/` tables, referenced by handle |
| `virtual ` | `Components/**` | §30.11 — POD only |
| `flecs::OnUpdate`, `flecs::PostUpdate`, `flecs::PreUpdate`, `flecs::OnLoad`, `flecs::PostLoad` | anywhere | §30.11 — built-in pipeline phases are not used; only `PYXIS_PHASE_*` |
| `world.entity(`, `\.set<`, `\.destruct\(\)` | `sources/pyxis_hydra/**`, `sources/pyxis_usd_ingest/**`, `sources/pyxis_renderer/Private/Scene/Systems/**` callsites that aren't on the render thread | §30.11 + §31 — single-writer; ingest threads must enqueue `MutationCommand` records, only the render thread mutates the world |
| `\.observer<` | `Systems/**` | §30.11 — observers are reserved for cross-component invariants (refcount → deletion, BLAS release). Don't use observers as a substitute for systems. Confirm any new observer lives under `Observers/` and the use case is documented. |

## Naming

- Components: PascalCase POD struct in `Components/<Name>.h`, one type per header.
- Systems: free functions `System_VerbObject` in `Systems/<Verb>.cpp` (e.g. `System_BuildDirtyBlas`, `System_RebuildTlas`).
- `Dirty<T>` is a zero-size tag component. After per-phase work commits, `System_ClearDirtyFlags` must remove it.
- Prefer pair relationships `(Instance, MaterialOf, materialEntity)` over entity-field components.

## Phase pipeline (§30.11)

Reordering or inserting between `PYXIS_PHASE_*` phases requires an RFC (§44). Flag any diff that adds a `world.system<...>().kind(...)` with a non-`PYXIS_PHASE_*` kind, or any change to `Phases.h` that reorders existing phases — and prompt the user to point to the RFC.

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
