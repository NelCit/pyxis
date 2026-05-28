# RFC 0009: Wire the Flecs SceneWorld as the real scene representation

- Status: Accepted (P1–P6 implemented + green; see "Implementation status")
- Author(s): Pyxis team
- Created: 2026-05-28
- Last updated: 2026-05-28
- Implementation PRs: (wire-flecs-scene branch — phased, P1–P6)
- Amends: §8 (SceneWorld), §30.11 (Flecs conventions)

## Summary

Make the §8 Flecs `SceneWorld` the actual scene representation. Today the renderer
has **two parallel, disconnected scene stores**: the real one is `GpuScene::Impl`
(Private/GpuScene/) — six `std::vector<*Entry>` tables with free-slot recycling,
dedup hash maps, and GPU handles, fed by the public `GpuScene` API; the Flecs
`SceneWorld`/`SceneWorldFacade` (Private/Scene/) is a separate world the app
constructs and `Tick()`s, whose systems are all `M0 no-op` stubs and which never
touches `GpuScene`. This RFC unifies them: `GpuScene` owns the `SceneWorld`, the
mutation verbs create Flecs entities with POD components (the per-entity bookkeeping
moves *out* of `Impl`, so there is no duplication), and `CommitResources` reads the
scene from Flecs. The GPU-resource layer (buffer pools, RTXMU BLAS, bindless table,
GPU buffers, variable-length CPU backing) stays in `GpuScene`, referenced by handle.

## Motivation

- The codebase **claims** the §8 ECS architecture (CLAUDE.md, §30.11, a
  `flecs-conventions-audit` skill, hundreds of `// Plan §8.1` comments) but
  **implements** a god-object `Impl`. The docs are misleading; new contributors read
  `Scene/Systems/RebuildTlas.cpp`, find a 14-line stub, and are lost.
- The two biggest perf findings from the repo audit — the quadratic mesh side-table
  re-upload (one *global* dirty bool re-packs every mesh on each add) and the TLAS
  that always rebuilds and never refits — are exactly the problems per-entity
  `Dirty<T>` tags + cached queries exist to solve. Wiring Flecs is the natural
  vehicle for that work rather than bolting per-entity flags onto the god-object.
- Scaling toward the §16.5 millions-of-instances / animation roadmap wants archetype
  storage + queries, not parallel `std::vector`s + maps.

## Detailed design

### Ownership & bridge
`GpuScene::Impl` owns a `pyxis::scene::SceneWorld` (its `flecs::world` + the §8.2
`HandleBimap` per entity type + a `QueryCache`). The public `GpuScene` verbs forward
into `Impl`, which now mutates Flecs entities instead of the `*Entry` vectors.

Systems that must touch GPU state (upload buffers, build BLAS) cannot run as bare
`world.system()` callbacks — a `flecs::iter` has no handle to the `nvrhi::IDevice` /
per-frame `ICommandList` / GPU buffers. The bridge is a **singleton `FrameContext`
component** (`{ IDevice*, ICommandList*, GpuResources* }`) set on the world at the
top of `CommitResources`; systems read it via the world context. (Introduced when the
first GPU-touching system moves to `world.progress()`; see Phasing — early phases
call the pack/upload directly from `CommitResources` reading a cached Flecs query,
which is already "Flecs is the source of truth" without the singleton.)

### Component model
One entity per logical object; components are POD (§30.11). Where a public POD
already carries exactly the per-entity data (e.g. `LightDesc`), it is registered as
the component directly — no intermediate mirror struct — so packing calls the
existing `Pack*Gpu` unchanged and stays byte-identical. Dedup hash maps are retained
as **indices** (hash → entity), which is lookup acceleration, not duplicated data.
Dirty state uses the existing `Dirty*` zero-size tags.

### Determinism (the hard constraint)
Byte-identical EXR (§33.7) + ~430 goldens must stay green. Two rules:
1. **Packing order:** Flecs query iteration is archetype order, not slot order.
   Every pack/upload collects `(slotIndex, …)` and **sorts by slotIndex** before
   writing the GPU buffer, reproducing today's vector-order output. Slot index comes
   from `HandleBimap` (stored on the entity as a component for the sort key).
2. **Slot allocation:** `HandleBimap::Allocate` reuses freed slots by a **forward
   scan (lowest-free-first)**; the legacy `Impl` used a **LIFO stack**. These are
   identical for static scenes (no frees → sequential append), so the golden/parity
   suite is unaffected. They differ only under dynamic remove+add; this RFC adopts
   the `HandleBimap` (§8.2 canonical) policy. `HandleBimap::Encode` offsets slot by
   +1, so handle 0 stays `Invalid` with no reserved sentinel slot.

### System pipeline
The §30.11 phases (`PhaseUploadTextures → … → PhaseClearDirty`) and the
`System_*` registrations already exist as stubs. They become real, reading the
`FrameContext` singleton and operating over cached queries, replacing the
hand-sequenced work inside `CommitResources::CommitResources`.

## Phasing (each phase is a complete, green checkpoint)

- **P1 — foundation + lights.** `GpuScene` owns the `SceneWorld`; lights become
  entities (`LightDesc` component + slot + `Live`/`DirtyLight` tags) via `HandleBimap`;
  the `LightEntry`/`lights`/`freeLightSlots` store is deleted; `UploadLightBuffer`
  reads a cached Flecs query (sorted by slot). Smallest golden surface (`light_*`).
- **P2 — materials + textures** (dedup-sensitive; hash maps become hash→entity).
- **P3 — meshes + BLAS** (mesh side-tables; per-mesh `DirtyTopology` fixes the
  quadratic re-upload).
- **P4 — instances + TLAS** (most determinism-critical; `DirtyTransform` enables
  refit per §16; SdfPath sort preserved).
- **P5 — volumes; promote the pack/upload work into `world.progress()` systems via
  `FrameContext`; delete the dead `Impl` vectors; remove the parallel
  `SceneWorldFacade` world or fold it onto `GpuScene`'s; flip the docs.**

## Alternatives considered

- **Mirror `Impl` into Flecs (keep both).** Rejected: that is the exact data
  duplication this RFC removes, and doubles the mutation cost.
- **Retire `Scene/` entirely (delete the ECS, keep the god-object).** A legitimate
  option for a v1 that only renders the World Lobby; rejected here because the team
  chose to make the documented architecture real and to use the ECS as the home for
  the per-entity dirty-tracking perf work. (If the roadmap narrows, this RFC can be
  superseded by a "retire Scene/" RFC.)
- **One atomic rewrite.** Rejected: six subsystems × byte-identical parity across
  ~430 goldens is too much regression surface for a single change.

## Drawbacks / risks

- Large, multi-phase refactor of the renderer's core; each phase carries golden /
  byte-identical-EXR regression risk, mitigated by the determinism rules above and a
  green checkpoint per phase.
- Slot-reuse policy change (LIFO → lowest-free) is a behavioural difference for
  dynamic scenes (none in the current test matrix).
- Flecs now sits in the `GpuScene` hot path (Private only; no Public/ ABI change).

## Migration & impact

No public API (§18) change — `GpuScene` / `PyxisRenderer` signatures and the
`*Desc` PODs are untouched; Flecs stays out of every `Public/` header. Affects the
internal commit path only. Milestones: unblocks the M8b perf items (per-entity dirty
tracking, TLAS refit); no milestone exit-criteria change.

## Implementation status

**Done (P1–P6, all green — 291/291 unit + 433/433 golden/integration, byte-identical
incl. M2 byte-equal EXR + M10 World Lobby):** all six entity types now live in the
`GpuScene`-owned Flecs `sceneWorld`. `GpuScene::Impl` holds **no** `std::vector<*Entry>`
— each type is a Flecs entity + a `GpuSlotMap` handle table (gpuscene_detail encoding,
slot == GPU/buffer index, LIFO free-list) + slot-indexed side tables for non-POD
GPU/CPU data (§30.11), with the dedup hash maps retained as indices.

| Type | Allocator | Component | Dirty / relationships |
|---|---|---|---|
| Lights | `HandleBimap` | `GpuLightComponent` (LightDesc) | — |
| Materials | `materialSlots` | `GpuMaterialComponent` | dedup index |
| Textures | `textureSlots` | `GpuTextureComponent` | **`DirtyTexture`** |
| Meshes | `meshSlots` | `GpuMeshComponent` | **`DirtyTopology`** + BLAS release on destruct |
| Instances | `instanceSlots` | `GpuInstanceComponent` | **`DirtyTransform`** + **`MeshOf`/`MaterialOf`** pairs |
| Volumes | `volumeSlots` | `GpuVolumeComponent` | — |

**Deferred follow-ups** (do not block this RFC; each is an optimization or cleanup on
top of the now-Flecs-resident data, to be done as separate green checkpoints):

- Promote the pack/upload work from `CommitResources`-called functions into
  `world.progress()` systems via a `FrameContext` singleton (the §30.11 phase
  pipeline executes for real). The data + `Dirty<T>` tags are in place; this is the
  execution-model change.
- **TLAS refit** using `DirtyTransform` (per §16) instead of always-rebuild — the tag
  is set; the refit path is the remaining work.
- ~~**Incremental side-table upload** for meshes using `DirtyTopology` (the audit's
  quadratic-load fix).~~ **Done.** `UploadMeshSideTable` (Internal.h) appends only the
  new tail slots when every dirty mesh is a new tail (`LowestDirtyMeshSlot()
  >= packedSlots`) and capacity allows, with geometric buffer growth on the
  full-rebuild fallback — a run of one-at-a-time commits amortises to O(total) rather
  than O(geometry·meshCount). Byte-identical to the single full pack; the golden suite
  only does single bulk commits, so `GpuSceneIncrementalUpload` (device-backed
  white-box) pins incremental==bulk over the 10 mesh buffers across many commits.
  NB: no current ingest path commits incrementally (both adapters do one bulk commit);
  this future-proofs progressive-load / live-edit and removes the latent quadratic.
- **Refcount-on-destroy / orphan detection** consuming the `MeshOf`/`MaterialOf`
  relationships (release a shared BLAS when its last instance goes away).
- ~~Remove the now-redundant app-side `SceneWorldFacade` world (its no-op systems /
  QueryCache stub).~~ **Done.** Deleted `SceneWorldFacade` (public header + impl), the
  `SceneWorld`/`World` wrapper, the 7 no-op `System_*` stubs + `Systems/Pipeline`, the
  `QueryCache` stub, and the unused component mirrors (`Geom`/`Transform`/`Visibility`/
  `MaterialRef`/`MeshRef`/`BlasRef`/`MaterialGpu`/`TextureGpu`/`LightParams`). `Private/
  Scene/` now holds only what `GpuScene` actually uses: `Phases.*` (the §30.11 pipeline),
  `HandleBimap.*` (light handles), `Components/Dirty.h`. The app no longer constructs/
  `Tick()`s a parallel world (HeadlessMode/ViewerMode); `SceneWorldInit` is repointed to
  pin `RegisterPhasePipeline` (registration + declared phase order) instead of the facade
  lifecycle. Docs flipped (CLAUDE.md §2 map, plan_final.md §8).

**All deferred follow-ups complete** — RFC 0009 is fully realised.

### Post-review hardening + the real dirty-tag model

A max-effort code review of the migration surfaced fixes (per-frame dome-texture
alloc, a 5× per-commit mesh-table rescan, a texture-component round-trip, a TLAS
refit count-mismatch guard, a dangling material `sourcePrim` view, an intra-phase
ordering dependency, an unchecked `get<>()`), all addressed; `Internal.h` was then
split into `Components.h` (the component PODs) + `Detail.h` (the `gpuscene_detail`
helpers).

The §8.1/§30.11 **dirty-tag model is now real** (it had been half-wired: `PhaseClearDirty`
was empty and `DirtyTransform` was set-but-never-read/cleared). `Sys_ClearDirty` runs on
`PhaseClearDirty`; the verbs tag `DirtyMaterial`/`DirtyLight`/`DirtyVisibility`/`DirtyTransform`
(removals tag a persistent `removalSentinel`); and the upload/build gates read the tags
(`count<Dirty*>()` for the coarse buffers + the TLAS; `LowestDirtyMeshSlot`/`DirtyTopology`
for meshes) instead of the old `*NeedUpload`/`tlas*` bools, which are deleted. Fine-grained
tags (`DirtyTopology`/`DirtyTexture`) are cleared by their consuming system; coarse tags by
`Sys_ClearDirty`. The dead `DirtyTextureGpu` tag was removed; `PhaseUpdateBindless` stays a
documented reserved no-op (the bindless table is bound by PathTracePass, not GpuScene). The
instance side-table keeps one bool (`instanceMaterialNeedsUpload`) because its
`UpdateInstanceMaterial` trigger is side-table-only and must not rebuild the TLAS, so it maps
to no TLAS tag. Byte-identical throughout (300 unit + 442 golden/integration, incl. M2
byte-equal EXR + M10 World Lobby).
