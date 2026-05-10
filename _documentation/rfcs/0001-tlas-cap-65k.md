# RFC 0001: TLAS instance-cap of 65 536 for v1

- Status: Accepted
- Author(s): Pyxis renderer team
- Created: 2026-05-10
- Last updated: 2026-05-10
- Implementation PRs: milestone/m8a-world-lobby (`Internal.h:TLAS_MAX_INSTANCES`)

## Summary

Pin the TLAS top-level instance cap at **65 536** for v1. The original
M3 stub used 256 (default scene needed ~5); v1 production-class scenes
need a meaningful uplift. This RFC documents the choice + the upgrade
path to plan §16.5's sharded TLAS for the 16M+ scenes after v1.

## Motivation

Plan §16.5 specifies sharded-TLAS for >16M instances and treats the
single-TLAS path as the v1 default. It does NOT pin a specific
instance cap on the single-TLAS path — only the upper bound. The cap
needs to be set somewhere, and `TLAS_MAX_INSTANCES = 65536` is the
choice that has shipped on the M8a branch.

Sizing rationale:
- v1 §41 milestones target Bistro (Amazon Lumberyard, ~50K instances).
- Production-class architectural scenes (lobby, hotel, restaurant) typically run 1K–10K instances.
- 24-bit `instanceCustomIndex` (§19.7 HANDLE_SLOT_BITS) caps the addressable range at 16M; 65K leaves the §16.5 sharding strategy ample room.
- Vulkan TLAS scratch cost per instance is ~128 B; 65K × 128 B = ~8 MB up front. Cheap on every v1 GPU class (8 GB VRAM minimum per CLAUDE.md).

## Detailed design

```cpp
// sources/pyxis_renderer/Private/GpuScene/Internal.h
constexpr std::size_t TLAS_MAX_INSTANCES = 65536u;
```

`GpuScene::Impl::RebuildTlasIfDirty` allocates a TLAS sized to this
cap on first need; subsequent rebuilds reuse it. Per-frame TLAS
rebuild + refit cost scales linearly with active instance count, not
with cap.

When `instanceDescs.size() > TLAS_MAX_INSTANCES`, GpuScene returns
`ErrorKind::TlasInstanceLimitExceeded` from `CommitResources`. The
renderer surfaces this via `FrameStats::degraded` and a one-shot
spdlog entry — the scene continues to render with the instances
that fit.

## Alternatives considered

1. **Match §19.7 HANDLE_SLOT_BITS cap (16M)** — would consume ~2 GB
   TLAS scratch on a fresh device; unacceptable on 8 GB VRAM machines.
   Rejected.
2. **Make it configurable via `GpuSceneCreateDesc::tlasMaxInstances`**
   — adds an ABI field to a frozen POD with no concrete v1 caller
   needing it. Rejected (revisit when M11+ scene-loading polish lands;
   could use one of the §22.3 reserved slots).
3. **Use 4 096 to match the other v1 caps (BINDLESS_TEXTURES_CAP, RFC
   0002)** — would block Bistro at ingest. Rejected.

## Drawbacks / risks

- A scene with >65 536 instances drops into the
  `TlasInstanceLimitExceeded` degraded path. The lobby (~1K) + Bistro
  (~50K) both fit comfortably; Moana (~28M) does not — that scene
  needs §16.5 sharded TLAS regardless and is post-v1 (§42).
- The 8 MB upfront cost is paid even on small scenes. Acceptable given
  the §17 v1 memory budget.

## Migration & impact

- No public API change; internal constant only.
- Affected milestones: M8a (lobby — within budget), M8b (Bistro — within
  budget), M9-M10 (no change), M11 (when the §16.5 sharded-TLAS path
  lands, this constant becomes the per-shard cap and a new constant
  governs the shard count).
- No regression-image impact (cap doesn't affect determinism).

## Open questions

- Should the cap be raised to 262 144 (~32 MB scratch) before the
  §16.5 sharded path lands, to give a smoother on-ramp for ALAB-class
  scenes? Defer to M11 sweep.
