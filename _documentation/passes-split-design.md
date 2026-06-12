# Design: Split `pass.PathTrace` into RaytracedGBuffer + RaytracedLighting + Tonemap; specialization constants; GPU buffer packing

Owner-directed engineering design note (2026-06-12). Not an RFC — the owner
has waived the §44 process for this work. Supersedes plan_for_v2.md V2.B.1's
"single-pass throughout" and carries out RFC 0003's accepted SceneResources
cleanup along the way.

## Summary

Restructure the single ray-tracing megakernel (`pass.PathTrace`) into three
passes — **RaytracedGBufferPass** (visibility buffer), **RaytracedLightingPass**
(deferred first-hit shading + all secondary rays), and **TonemapPass** (the
display/color-space transform extracted from raygen's inline branch) — while
keeping the render graph linear. Introduce **Vulkan specialization constants**
(NVRHI `createShaderSpecialization`) for dispatch-uniform values currently read
from cbuffers or duplicated as literals. Consolidate GPU buffers: merge the
two per-instance and five per-mesh offset side tables into packed
`InstanceInfoGpu` / `MeshInfoGpu` structs, merge the per-vertex normal+tangent
streams, and pool the per-mesh vertex/index buffers (§14.5 as planned but never
built) — implementing RFC 0003's `SceneResources` internal view to retire the
16 public `GpuScene::Get*` NVRHI getters.

## Motivation

- plan_for_v2.md:618 explicitly deferred this: "If a future v3 adds RTAO or
  multi-bounce or denoising, the closesthit becomes too divergent and we'd
  refactor to a split-pass architecture then." The closesthit is now 1163
  lines with an 80-byte spilled payload; first-hit shading (textures, normal
  pipeline, light loop, reflection inner loop) runs in the most
  divergence-hostile stage. The owner has directed the restructure now.
- raygen.slang:328 carries the standing M9 TODO ("ToneMapPass takeover") —
  the display transform was always meant to leave raygen.
- At World Lobby scale the renderer holds ~5,000 nvrhi objects, of which
  ~2,000 are per-mesh vertex/index buffers used only once as BLAS build
  inputs, plus a flat duplicate of all index data (~12 MB). Five parallel
  per-mesh offset tables cost five buffer loads per hit where one cache line
  would do.

## Detailed design

### D1. Pass split (visibility-buffer architecture)

Linear graph (§9 stays linear; no DAG, no aliasing):

```
RaytracedGBufferPass -> RaytracedLightingPass -> TonemapPass -> SsaaResolvePass -> BlitToSrgbPass
```

**RaytracedGBufferPass** (`pass.RaytracedGBuffer`) — RT pipeline A.
- raygen: camera ray construction (verbatim shared module `camera_ray.slang`),
  ONE TraceRay (no transparency loop), writes
  `RWStructuredBuffer<VisibilityGpu>` (width*height):
  `{ float hitT; float baryX; float baryY; uint instanceId; uint primitiveIndex; uint _pad0/_1/_2 }`
  (32 B, fp32-exact — no quantization anywhere).
- closesthit (thin): payload = `{hitT, bary, InstanceID(), PrimitiveIndex()}`
  (20 B vs today's 80 B).
- anyhit: the SAME alpha-test stub (ALPHA_TESTED -> IgnoreHit) so visibility
  semantics are bit-identical. miss: hitT = -1.

**RaytracedLightingPass** (`pass.RaytracedLighting`) — RT pipeline B.
- raygen: reads VisibilityGpu. Miss pixels: background via the shared
  dome-sample function (same math as miss.slang). Hit pixels: reconstructs
  every closesthit input from `{instanceId, primitiveIndex, bary}`:
  - object->world 3x4 from the new per-instance transform (in
    `InstanceInfoGpu`) — replaces `ObjectToWorld3x4()`;
  - `worldHit = origin + hitT*dir` with the identical expression;
  - UV/LOD/texture/normal pipeline + light loop + AO + shadow rays +
    deterministic mirror reflection — extracted VERBATIM from
    closesthit.slang into a shared `shading.slang` module, consumed by both
    this raygen and the megakernel closesthit.
  - Transparency: continues today's front-to-back loop from segment 2 using
    the retained megakernel closesthit at payload.recursionDepth=0 — identical
    ray topology, identical gates. Reflection rays launch with
    recursionDepth=1, exactly matching today's inner-loop depth bookkeeping.
- Writes: fp32 `linearColor` (new RGBA32F scratch), all 11 AOVs, pick buffer
  (full precision — picker stays here).
- The megakernel closesthit survives as this pipeline's hit shader for all
  secondary rays (transparent continuation + reflections); only its
  primary-hit role is replaced.

**Shader file naming (owner-directed)**: shader files are named after the pass
that owns them — `raytraced_gbuffer_{raygen,closesthit,miss}.slang`,
`raytraced_lighting_{raygen,closesthit,miss,shadow_miss}.slang` (the lighting
closesthit is today's megakernel, retained for secondary rays),
`raytraced_anyhit.slang` (one module bound into both RT pipelines),
`tonemap.slang`, `ssaa_resolve.slang` (renamed from ssaa_downsample),
`blit_to_srgb.slang`; shared modules `camera_ray.slang` / `shading.slang` /
`dome_sample.slang` / `ShaderInterop.slang`. CMake registrations and
AssetLocator/ReloadShaders path strings move in the same phase as each rename
(P3: tonemap + ssaa_resolve; P4: the RT families).

**TonemapPass** (`pass.Tonemap`) — compute.
- Input: `linearColor` (fp32 — NOT the RGBA16F colorHdr AOV, to avoid
  quantizing the display path), AOVs for the 10 debug views, gCamera
  (exposure), gFrameUi (debugViewMode stays a runtime cbuffer value — it is
  viewer-interactive).
- Output: gOutput (BGRA8) — the exact display branch from raygen.slang:330-442
  (exposure exp2 + Narkowicz ACES on COLOR; the 10 debug encodings verbatim).

Caveat: `ReloadShaders` is per-pass old-on-failure but not transactional
across passes — a partial reload (one pass's .spv unreadable) can leave the
two RT pipelines built from different generations of the shared
`camera_ray.slang` / `shading.slang` modules; the RenderGraph logs an error
when this happens, and the remedy is to fix the failing shader and reload
again.

### D2. Specialization constants (NVRHI `ShaderSpecialization`, 32-bit, explicit `[[vk::constant_id(N)]]`)

Per §34.3 these ship with profile evidence (before/after in the PR).

| Constant | id | Shader | Today | Policy |
|---|---|---|---|---|
| `SSAA_FACTOR` | 0 | ssaa_downsample | volatile cbuffer field, runtime loop | pipeline per factor (2/3/4), created outside Execute |
| `PROJECTION_MODE` | 0 | both raygens | cbuffer branch | variant per camera mode, rebuilt outside Execute on change |
| `MAX_RAY_RECURSION` | 1 | lighting raygen + closesthit | literal 3 duplicated shader+C++ | C++ single source of truth |
| `MAX_TRANSPARENT_SEGMENTS` / `REFL_MAX_SEGMENTS` | 2/3 | lighting raygen / closesthit | literals 16 | same values, C++-fed |

debugViewMode stays runtime (interactive). MaterialFlag branches stay runtime
(§11 one-generic-closesthit is not relitigated).

### D3. Buffer packing (implements RFC 0003)

New interop structs (ShaderInterop.slang, scalar-only, 16-byte rows,
static_asserts):

- `InstanceInfoGpu` (64 B): `float3x4 objectToWorld` (owner-directed: matrix
  type, not 12 scalars — hlslpp::float3x4 is 3 SIMD rows = 48 B, matching
  Slang row-major; add the `using float3x4` alias to the interop C++ block;
  replaces the hit-stage-only `ObjectToWorld3x4()` for the lighting raygen);
  row 3 = `{uint materialSlot; uint meshSlot; uint _r0; uint _r1}`.
  Likewise `VertexAttribGpu` uses `float4 normal; float4 tangent;` (not 8
  scalars). static_asserts (64/32) stay as layout tripwires.
  Replaces bindings 4 (gInstanceMaterial) + 6 (gInstanceMesh). Written by the
  existing UploadInstanceSideTables walk + the TLAS instance-desc transform
  (identical values).
- `MeshInfoGpu` (32 B): `{uint faceOffset; uint uvOffset; uint indexOffset;
  uint vertexAttribOffset; uint vertexCount; uint _r0; uint _r1; uint _r2}`.
  Replaces bindings 8/25/27/30/32 (five parallel uint offset tables). One
  cache line per hit instead of five buffer loads.
- `VertexAttribGpu` (32 B): `{float4 normal; float4 tangent}` — merges the two
  per-vertex padded streams (bindings 29 + 31); they already share element
  counts and offsets by construction. Zero-magnitude sentinels preserved
  independently per field.
- **Pooled geometry pages (§14.5)**: per-mesh vertex/index buffers are
  replaced by two grow-only pooled buffers (BLAS build inputs take
  buffer+offset). The index pool doubles as `gMeshIndices` (binding 26)
  via structured view — deleting the flat duplicate (~12 MB at Lobby scale)
  and its separate upload path. Pool ranges from destroyed meshes leak until
  `Clear()` (matches the multicycle VRAM test's whole-scene lifecycle).
  Leak observability is a Debug-level log line in `DestroyMesh` only — the
  public `FrameStats` POD has no trailing `_reserved` slot to consume
  (§22.3), so a leaked-bytes counter would be a layout break; demoted to
  debug-log-only. `LastFrameStats` consequently reports live-range bytes,
  not the pool's actual allocation (which also carries up to ~2x growth
  slack).
- Fallback consolidation: PathTracePass's 14 one-element fallback buffers
  collapse to one per distinct stride.
- `SceneResources` (RFC 0003): internal view in `Private/Scene/`, populated
  per frame, threaded via PassContext; the 16 public `GpuScene::Get*` NVRHI
  getters are removed (consumers: PathTracePass + one unit test — both
  in-repo). version.txt: 1.0.0 -> 2.0.0 (§22 MAJOR: public symbol removal).

Final RT binding table — lighting pipeline B (set 0, as shipped in the
`raytraced_lighting_*` / `shading` / `dome_sample` / `camera_ray` .slang
`[[vk::binding]]` declarations): 0 gCamera, 1 gTlas, 2 gLinearColor (RGBA32F
UAV — reuses the slot gOutput vacated), 3 gMaterials, 4 gInstanceInfo,
5 gLights, 6 gMeshInfo, 7 gMeshFaceNormals, 9 gDomeEnvMap,
10 gBindlessSampler, 11-18 AOVs/pick, 19 gFrameUi, 20-23 Tier-1 AOVs,
24 gMeshUvs, 26 gMeshIndices (pool view), 28 gBindlessTextures[4096],
29 gMeshVertexAttribs, 33 gDomeSampler, 34 gVisibility (SRV).
GBuffer pipeline A has its own slim layout: 0 gCamera, 1 gTlas,
2 gVisibility (UAV), 3 gMaterials, 4 gInstanceInfo (anyhit alpha test).
(gOutput moves to TonemapPass's compute layout at binding 12;
8/25/27/30/31/32 retired.)

## Determinism (the hard constraint)

The blast radius is 121 byte-equal PNG goldens (gOutput BGRA8 -> fixed
sRGB LUT -> PNG, zero tolerance), the M2 byte-equal EXR, and the tolerant
World Lobby EXR (rmse 0.02 / mae 0.01 / maxAbs 0.50).

- Values and indexing are preserved exactly: packed structs carry identical
  numbers; pool offsets feed identical index values; the visibility buffer is
  fp32 (no quantization); the display path reads fp32 linearColor (never the
  RGBA16F colorHdr); ray topology (count, order, flags, miss indices,
  recursion gates, loop caps, epsilons) is reproduced 1:1.
- The residual risk is FP contraction: the driver may FMA-contract the same
  expressions differently when they move between kernels. Mitigation:
  byte-compare the golden suite after every phase; if flips occur they are
  epsilon-level (±1/255 post-quantization), are quantified in the PR, and the
  baselines are rebaked in a dedicated, clearly-labeled commit (precedent:
  the #64 World Lobby rebake). A pure-packing phase (P2) must be EXACTLY
  byte-equal — any drift there is a bug, not contraction.

## Phasing (each phase is a complete, green checkpoint)

- P0: baseline capture — goldens green, unit tests green, perf profile
  (`--bench-frames`) recorded for default scene + heaviest golden fixture.
- P1: tripwires — `static_assert(sizeof(LightGpu) == 96)` (currently missing),
  stale size comments fixed.
- P2: SceneResources + packing (megakernel untouched semantically).
  Gate: goldens BYTE-EQUAL, all unit tests, incremental-upload test updated.
- P3: TonemapPass extraction + fp32 linearColor. Gate: goldens byte-compare
  (rebake decision if contraction flips).
- P4: visibility-buffer split (GBuffer + Lighting passes; megakernel CH
  retained for secondary rays). Gate: goldens byte-compare + perf delta.
- P5: specialization constants. Gate: goldens + before/after numbers.
- P6: docs, version bump, KPI scope-name updates (perf tooling reads
  pass.RaytracedGBuffer / pass.RaytracedLighting / pass.Tonemap).

## Alternatives considered

1. **Keep single-pass (status quo per V2.B.1)** — rejected by owner
   directive; the closesthit has outgrown the shape v2 predicted it would.
2. **Full G-buffer (store shaded attributes)** — rejected: RGBA16F G-buffer
   quantizes shading inputs (breaks goldens); fp32 MRT G-buffer costs
   ~96 B/px vs the 32 B/px visibility buffer, and re-fetching attributes is
   cache-friendly with the packed MeshInfo/VertexAttrib tables.
3. **Compute-shader lighting with RayQuery** — rejected for v2.5: changes
   shadow/AO/reflection traversal semantics (no anyhit stub parity) and
   forks the megakernel; revisit post-denoiser.
4. **Per-material pipeline specialization for MaterialFlag branches** —
   explicitly out (§11/§42; would need its own RFC).

## Drawbacks / risks

- Golden rebake risk from FP contraction (mitigated/owned above).
- VRAM: +48 B/px of new scratch (32 B/px visibility + 16 B/px linearColor),
  sized off the display target: ~99.5 MB at 1920x1080 and ~398 MB at viewer
  SSAA 2x (3840x2160 dispatch dims). A real cost on 8 GB-class targets —
  acceptable at plain 1080p against the §17 budgets, but SSAA 2x spends a
  noticeable slice of the budget on it, and SSAA 4x on 8 GB (already
  memory-hostile pre-split) gets ~1.6 GB worse.
- Perf-history discontinuity: pass.PathTrace disappears from nightly CSVs;
  perf_compare baselines restart (documented in the PR).
- Public-surface removal (RFC 0003) is a MAJOR version bump.
- Pooled-geometry leak bytes (destroyed meshes' ranges, held until `Clear()`)
  are NOT surfaced in `FrameStats` — the POD has no `_reserved` slot left
  (§22.3) — so within a destroy-heavy scene lifecycle actual pool VRAM
  exceeds the reported vertex/index bytes; the only signal is the
  Debug-level `DestroyMesh` log line.
- Two RT pipelines + SBTs (GBuffer/Lighting) roughly double pipeline-create
  time at startup (one-time, small).

## Migration & impact

- PathTracePass.cpp is refactored into RaytracedLightingPass (inherits binding
  machinery, fallbacks, picker) + new slim RaytracedGBufferPass + TonemapPass.
- tests/unit/GpuSceneIncrementalUpload.cpp moves to SceneResources accessors.
- m9 docs referencing Get*Buffer getters annotated.
- No ingest-adapter or app-mode changes; headless/viewer/Kit paths unchanged
  (they consume gOutput/colorHdr exactly as before).

## Open questions

- None blocking; rebake decision (if contraction flips pixels) is taken
  per-phase with the owner.
