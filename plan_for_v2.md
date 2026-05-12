# Pyxis v2 plan — full-fidelity USD loading + real-time raytracer

**Status**: draft, post-v1.0 (v1.0 shipped at commit 552a929).

**Source of truth lineage**: [`plan_final.md`](plan_final.md) is the v1 plan and remains the §-citation source for everything carried forward. This file extends it. Section IDs here are `V2.A.x` (loading completeness) and `V2.B.x` (real-time raytracer) so they don't collide.

**Reference target**: https://docs.omniverse.nvidia.com/usd/latest/usd_content_samples/res_lobby.html — the same lobby asset Pyxis v1 already ships as its hero scene. v2's exit gate is "render this lobby in real time, visually comparable to OVRTX, without claiming pixel parity."

---

## V2.0 — Two pillars

v2 has two non-overlapping bodies of work:

| Pillar | Goal | Out-of-scope |
|---|---|---|
| **A. Loading completeness** | Faithfully load every prim type, primvar, transform mode, time sample, variant, and relationship USD authors. The Pyxis-side scene state mirrors USD's intent. Renderer may *ignore* loaded data initially; loading it is non-negotiable. | New rendering algorithms |
| **B. Real-time raytracer** | Replace v1's path-traced integrator with a **hybrid raytracer**: rasterized G-buffer feel + ray-traced reflections + ray-traced shadows + RTAO + denoiser. Real-time on RTX 4070+ at 1080p. Approximation over correctness — "OVRTX-equivalent at a glance", not "physically converged". | Full path tracing (v1 remains the offline path) |

**Order**: Pillar A first (M12..M20), Pillar B second (M21..M25). Loading completeness is a prerequisite — the real-time pipeline still consumes Pyxis's `SceneWorld` view, so any prim type missing from ingest is also missing from real-time rendering.

---

## Part 1 — Pillar A: loading completeness

### Philosophy

> *"The Pyxis scene representation is a strict superset of what the renderer consumes. If USD authored it, we load it. If we can't render it yet, we mark it inert. We never silently drop authored information."*

Practical consequences:

- Every USD prim type maps to a Pyxis ECS archetype, even types the closesthit doesn't yet sample (curves with no Marschner BSDF still ingest as `CurvesGeom + InactiveRender`).
- Every authored primvar lands in a Pyxis-side primvar map per geom, even those the shader doesn't read. Round-trip integrity: `USD → Pyxis → save-as → USD` preserves the primvar set.
- Every `xformOpOrder` variant resolves at ingest into the canonical 4×4 matrix the renderer wants, but the *authored* op chain is preserved for round-trip + inspector display.
- Every USD relationship target (`material:binding`, `proxyPrim`, light-linking collections, …) lands as a typed handle. Renderer may ignore; data isn't lost.
- Every time sample (animation) is loaded into a `TimeSampleSet` POD per attribute. Pillar B's real-time renderer reads frame-by-frame; v1 used `UsdTimeCode::Default()` only.

### V2.A.1 Subdivision surfaces (M12)

**The 83-mesh lobby bug.** `pxr::UsdGeomMesh::GetSubdivisionSchemeAttr()` returns `catmullClark` or `loop` on 8.8% of lobby meshes — we currently render those as the unrefined cage, producing faceted output where smooth surfaces were authored.

**Plan**:
- New vcpkg dep: `opensubdiv` (`Tracy::TracyClient`-style import target).
- New ingest path: when `subdivisionScheme != "none"`, refine the cage via OpenSubdiv `Far::PatchTable` + `Osd::CpuPatchEvaluator` at ingest. Refinement level capped at 2 by default (configurable via `RenderSettings::subdivLevelCap`, `_reserved1` field on RenderSettings repurposed per §22 MINOR-additive rules).
- Honour authored crease (`creaseIndices` + `creaseSharpnesses`), boundary (`interpolateBoundary`), corner (`cornerIndices` + `cornerSharpnesses`), and FVAR-interpolation (`faceVaryingLinearInterpolation`) attributes.
- Refined limit-surface mesh becomes the BLAS source; the cage is dropped post-refinement.
- Per-subdiv-mesh ingest cost: ~50ms for an average lobby mesh at level 2. The M10 parallel-prep pool handles this in parallel.
- Loaded but unused-on-GPU: `interpolateBoundary` modes beyond `edgeAndCorner` (rare; we always refine with `edgeAndCorner`).

**Exit**: 83 lobby meshes refine to smooth limit surface. M10.WorldLobbyRegression's baseline rebakes once. Visible faceting on the marble columns, the curved bench, and the spiral staircase resolved.

### V2.A.2 Visibility + purpose + variants (M12)

**Bug today**: `UsdGeomImageable::ComputeVisibility()` is only checked for lights (StageWalker.cpp:526). The 4 invisible lobby prims render anyway. `UsdGeomImageable::GetPurpose()` is not checked at all — `proxy` and `guide` purpose prims render alongside `render`.

**Plan**:
- Per-mesh / per-instance: call `ComputeVisibility(UsdTimeCode::Default())` at ingest. `invisible` → skip emit. Inherited hierarchy walk handled by USD.
- Per-mesh: `GetPurpose()`. Filter: `default` + `render` emit, `proxy` + `guide` skip (configurable via `RenderSettings::purposeFilter` bitmask).
- `UsdVariantSet`: respect the active variant selection authored on the stage. Today we use stage-default; if a scene authored `variantSelection`, USD already honours it transitively through `UsdStage::Open`. Verify + add a regression fixture.
- Per-`UsdGeomImageable::ComputeProxyPrim()`: skip `proxy:proxyPrim` overrides (purpose-related but a separate prim relationship).

**Exit**: 4 invisible lobby prims hidden. Purpose-filtered fixture renders only `render` + `default` prims.

### V2.A.3 Curves + points (M13)

**Plan**:
- `UsdGeomBasisCurves` ingest: cubic + linear, periodic + nonperiodic, basis = `bezier|catmullRom|bspline`. Widths primvar (constant / varying / vertex / faceVarying). curveVertexCounts per-curve.
- Triangulation strategy: **camera-facing ribbon strips**. Each curve segment → 2 triangles (quad), normal points away from camera. Width-aware (curve thickness from `widths` primvar). Re-triangulated when camera moves (v1 ingests once; for real-time the ribbons must orient per-frame — handled in Pillar B's primary pass via a vertex-shader-style step in the closesthit, OR pre-triangulated at the dominant view direction).
- `UsdGeomPoints`: each point → camera-facing quad billboard. Width from `widths` primvar (per-point or uniform).
- Closesthit: keep Lambert per-strand for v2.0. Marschner BSDF added in v2.1 if needed.
- New geom kinds in Pyxis: `CurvesHandle`, `PointsHandle`, parallel to `MeshHandle`. BLAS per curves prim (one prim's worth of segments).
- Loaded but unused-on-GPU: hair-specific attributes (`type=hair`), `normals` on curves (rare).

**Exit**: any USD scene with curves/points renders. Lobby unchanged (0 curves/points authored). Vegetation scenes (`UsdLuxKitchenSet` test fixture, post-v2 hero candidates) light up.

### V2.A.4 NURBS + skel + time-varying USD (M14)

**NURBS**:
- `UsdGeomNurbsPatch` → tessellate via OpenSubdiv's NURBS path (already a vcpkg dep from A.1) or a small custom tessellator. Render as triangles.
- `UsdGeomNurbsCurves` → same as `UsdGeomBasisCurves` post-tessellation.

**Skel**:
- `UsdSkelRoot` discovery walk: find every `UsdSkelSkeleton` + `UsdSkelBlendShape` + `UsdSkelSkinningQuery`.
- Bind pose: world transform per joint at the bind frame.
- Skinning: GPU-side vertex transform via skeletal matrices. New buffer at binding 34 (`StructuredBuffer<float4x4> gJointMatrices`) per skinned mesh.
- Per-vertex `jointIndices` + `jointWeights` primvars resolved at ingest.
- Animated joints: `UsdSkelAnimQuery` exposes per-frame joint transforms. Pillar A loads the time sample set; Pillar B advances per-frame.

**Time-varying USD**:
- `UsdNotice::ObjectsChanged` listener on the stage. Drains into Pyxis's mutation queue (§31 single-writer model).
- Per-frame `Dirty<Transform>` tags when xforms changed; `Dirty<Mesh>` when geometry changed.
- TLAS **refit** (not rebuild) when only transforms changed — NVRHI's `PerformanceFlags::AllowUpdate` on TLAS build.
- Frame-time advancement: `RenderSettings::timeCode` (per §22.3 MINOR-additive — new field). Headless `--frame N` CLI flag.

**Loaded but unused-on-GPU**:
- `UsdSkelInbetweenShape` (intermediate blendshape positions; v2 takes only endpoints).
- `UsdSkelPackedJointAnimation` (packed encoding; we expand to per-joint matrices at ingest).
- Time samples on materials (v2 picks default; production scenes rarely animate materials anyway).

**Exit**: animated character fixture renders, advancing through frames. UsdNotice live-reload works in the viewer.

### V2.A.5 Volumes (M15)

**Plan**:
- New vcpkg dep: `openvdb` (Apache-2.0).
- `UsdVolVolume` ingest: discover OpenVDB asset paths via `UsdVolOpenVDBAsset` and Field3D asset via `UsdVolField3DAsset` (latter rarely used in v2-target scenes; we'll log + skip Field3D until requested).
- VDB → GPU: dense 3D texture for low-resolution grids; sparse representation (brick map) for high-res.
- New BLAS shape: **AABB primitives** (NVRHI supports). One AABB per active VDB brick. Custom intersection shader marches through bricks.
- New `Volume` material category in OpenPBR (absorption + scattering coefficients, anisotropy `g`, emission). New `OpenPBRMaterialDesc::volumeAbsorption`, `volumeScattering`, etc.
- Volume integrator: a separate `VolumePass` after the main closesthit, ray-marching through any volume AABBs the primary ray + shadow rays intersected.

**Loaded but unused-on-GPU**:
- Multi-resolution VDB grids (we pick the finest level that fits the budget).
- Animated VDB (time samples on volume asset paths); load metadata, decode lazily.

**Exit**: a smoke fixture renders with proper extinction + emission. Lobby unaffected (0 volumes).

### V2.A.6 Full `xformOpOrder` coverage (M14)

Today we handle `xformOp:translate` + `xformOp:rotate{X,Y,Z,XYZ}` + `xformOp:scale`. Production USD authors more:

- `xformOp:transform` — full 4×4 matrix authored directly.
- Maya-style ordered rotates: `rotateXYZ`, `rotateXZY`, `rotateYXZ`, `rotateYZX`, `rotateZXY`, `rotateZYX`.
- `xformOp:orient` — quaternion rotate.
- `xformOpOrder` arbitrary chains: e.g. `["xformOp:translate:pivot", "xformOp:scale", "xformOp:translate:!invert!:pivot"]` for scale-about-pivot.
- `!invert!` suffix on any op.
- Per-axis decomposed translates with suffix: `xformOp:translate:pivot`, `xformOp:translate:foo`.

**Plan**: replace our hand-rolled `ComposeWorldFromLocal` with `UsdGeomXformable::GetLocalTransformation()` (pxr's canonical method). Verify against a fixture matrix multiplied against authored ops.

**Loaded but unused-on-GPU**:
- Authored op chain string preserved on the Pyxis transform component for round-trip USD-save.
- Per-frame xform time samples loaded into a `TransformAnimationSet` POD; Pillar B advances per-frame.

**Exit**: a fixture with every xformOp variant + `!invert!` round-trips through both adapters byte-identically.

### V2.A.7 UDIM + texture coverage (M14)

**Plan**:
- `ArResolver`-resolve UDIM patterns: `path/<UDIM>.exr` → enumerate 1001…1100 tiles that exist.
- Per-UDIM-tile texture upload. New `Texture2DArray` binding for UDIM atlases (or a per-tile bindless slot — TBD by perf measurement).
- Closesthit UV-tile arithmetic per §13: `tile = floor(uv); local_uv = frac(uv); tile_index = base + (tile.y * 10 + tile.x); sample(textures[tile_index], local_uv)`.
- Falls back to a single-tile path when no UDIM is detected (current behaviour preserved).

**Loaded but unused-on-GPU**:
- `<UDIM>` patterns that resolve to a single tile (lobby has none; common in studio pipelines).

**Exit**: a UDIM fixture (multi-tile texture across one mesh) renders correctly. M10 regression baseline captured.

### V2.A.8 MaterialX coverage matrix (M18)

Plan §36.7 already mandated the matrix; v1 only shipped `open_pbr_surface` + `standard_surface` shims.

**Plan**:
- `tests/fixtures/materialx/coverage/` per §36.7 — one fixture per supported nodedef.
- Audit the OpenPBR `OPS_*` + StdSurface `SS_*` node sets:
  - **Math**: `add`, `subtract`, `multiply`, `divide`, `power`, `sin/cos/tan`, `mix`, …
  - **Color**: `convert`, `extract`, `combine{2,3,4}`, `srgb_to_linear`, `linear_to_srgb`, …
  - **Image**: `image`, `tiledimage`, `triplanarimage`.
  - **Geom**: `position`, `normal`, `tangent`, `bitangent`, `texcoord`, `geomcolor`, `geompropvalue`.
  - **Procedural**: `noise2d/3d`, `cellnoise2d/3d`, `worleynoise2d/3d`, `checkerboard`, `ramp4`.
  - **Shading lobes**: `oren_nayar_diffuse_bsdf`, `dielectric_bsdf`, `generalized_schlick_bsdf`, `conductor_bsdf`, `subsurface_bsdf`, `sheen_bsdf`, `thin_film_bsdf`, `translucent_bsdf`.
- For each unsupported nodedef: emit a single log line into `unsupported_features.json` at ingest end (§35).
- Closesthit changes only when a nodedef is wired to the OpenPBR closesthit's existing fields; otherwise translator-side only.

**Loaded but unused-on-GPU**:
- Entire MaterialX networks beyond the closesthit's input set load into a per-material network graph (small flat representation). Renderer ignores; tools can introspect.

**Exit**: every nodedef in the OPS + StdSurface sets has a fixture + a matrix CSV row + a measured RMSE against an authoring tool reference. Missing nodedefs logged once into `unsupported_features.json`.

### V2.A.9 PointInstancer invisibleIds + velocities + accelerations (M14)

§25.O.1 already lists these as "defers to M7+". v2 picks them up:

- `UsdGeomPointInstancer::invisibleIds`: per-prototype-instance hide. Honoured at ingest (drop the instance) or at TLAS-build (zero `instanceMask`).
- `UsdGeomPointInstancer::inactiveIds`: similar semantics; treat as `invisibleIds` for v2.0.
- `velocities` / `accelerations` primvars: load into per-instance state; not used by v1's static TLAS but consumed by Pillar B's motion blur reservation.

**Loaded but unused-on-GPU**:
- `protoIndices` ranges that exceed the live prototype count (USD spec allows; we clamp to last valid prototype).

### V2.A.10 Asset resolvers + USDZ (M19)

**Plan**:
- Test the existing default `ArDefaultResolver` against:
  - **USDZ** packages (`.usdz` zipped USD). vcpkg USD builds with USDZ support; verify against a curated fixture set.
  - Network-mounted paths (Omniverse-style `omniverse://server/asset.usd` — requires Omniverse client SDK; gated as an optional v2.5 feature).
- Studio-custom `ArResolver` plugins: documented integration path in `_documentation/asset-resolvers.md`. Pyxis itself ships only the default; production users plug in their own.

**Loaded but unused-on-GPU**:
- ArResolverContext bindings (per-stage context); not exposed in API yet.

### V2.A.11 Loaded-everything audit + dirty USD fixtures (M20)

Final loading-completeness milestone — the audit pass.

**Plan**:
- Walk every USD authoring spec: enumerate every prim type in `pxr/usd/usd*` + `pxr/usd/usdGeom*` + `pxr/usd/usdLux*` + `pxr/usd/usdShade*` + `pxr/usd/usdSkel*` + `pxr/usd/usdVol*` + `pxr/usd/usdRender*` + `pxr/usd/usdRi*`. Check each against StageWalker dispatch.
- Per primvar: every `UsdGeomPrimvar` interpolation mode (`constant`, `uniform`, `varying`, `vertex`, `faceVarying`) handled or explicitly skipped with a log line.
- Per attribute: time samples loaded even if v2.0 renders only default frame.
- Per-relationship: `material:binding`, `proxyPrim`, light-linking `collection:lights:includes/excludes`, render-product targets — all loaded into typed handles.

**The "Pyxis scene state" assertion**: at end of ingest, `gpuScene.LastFrameStats()` reports every count >0 for prim types in the source USD. A regression test asserts the count.

**§36.8 dirty fixtures rebroadened**:
- `truncated.usda` (existing)
- `unresolved_reference.usda` (existing)
- New for v2: `cyclic_layer_stack.usda`, `extreme_nesting.usda`, `infinite_extent.usda`, `nan_positions.usda`, `gigantic_texture.usda` (16K×16K EXR), `mixed_endianness.usdz`.
- Every fixture has a **5-minute hard timeout**. Exceeding → S1 incident per §45.3.

**Exit**: any production USD package opens cleanly (or fails loudly with a typed error). Production-pipeline assertion: round-trip USD → Pyxis scene state → save back to USD preserves every authored property. Plumbing for round-trip is `gpuScene.SaveAsUsd(path)` (§29.7 reserved hook activated).

### V2.A.12 Texture streaming + LRU eviction (M17)

§42 explicit defer. Bumped in v2 because Pillar A's "load everything" can blow VRAM on big scenes if every texture decodes eagerly.

**Plan**:
- Budget tracker (already plumbed via `BudgetTracker` in §17) extended with per-texture-class budgets.
- Texture LRU: `OnTextureMissedThisFrame()` increments a per-texture LRU counter; the `BudgetCommitPhase` evicts (CPU + GPU) the coldest textures when over budget.
- Async upload via asset I/O pool (already exists for ingest). New `PendingUpload` → `Pending Eviction` lifecycle.
- Mip-aware streaming: load mip 0 (coarse) first, refine to higher mips on demand. New `Texture2D` `ResourceStates::ShaderResource | StreamingPartial`.

**Loaded but unused-on-GPU**:
- Per-texture mip metadata loaded; only the budget-fitting subset uploaded.

**Exit**: an "oversized lobby" stress test (lobby × 10) ingests + renders without OOM at 8 GB VRAM, with degraded mip levels logged.

### V2.A.13 Time-varying primvars (M14)

Time samples on primvars (`UsdGeomPrimvar` over time).

**Plan**:
- Loaded by `UsdGeomPrimvar::ComputeFlattened(time)` per frame.
- Per-frame `Dirty<Primvar>` system rebuilds the affected mesh's vertex buffer + BLAS refit if positions changed.
- Heavy-handed but correct: any animated primvar triggers a per-frame upload of just that primvar slice.

**Loaded but unused-on-GPU**:
- Static primvars (vast majority) stay in their v1 paths.

---

## Part 2 — Pillar B: real-time raytracer

### Philosophy

> *"Real-time rendering is the art of skipping the right physics."*

v1's closesthit was a forward-Euler step toward a real path tracer — single bounce, NEE direct, no MIS. Production real-time renderers (OVRTX, Lumen, Frostbite) **never** path-trace; they decompose the rendering equation into **components** and use the cheapest technique that approximates each:

| Component | v1 path tracer | v2 real-time |
|---|---|---|
| Primary visibility | TraceRay closest-hit | TraceRay closest-hit (unchanged) |
| Direct lighting | Per-light shadow ray + Lambert+GGX | Per-light shadow ray + Lambert+GGX + **light tree sampling** + **MIS** |
| Indirect specular | (none) | **1-bounce reflection TraceRay** + spatial-temporal denoise |
| Indirect diffuse | (none) | **RTAO**: 1-spp AO ray + denoise (NOT proper indirect diffuse; cheap approximation) |
| Indirect via probes | (none) | (deferred to v3 — DDGI / probe-based irradiance) |
| Anti-aliasing | Halton jitter + accumulation | **TAA**: temporal reprojection + history buffer |
| Tone-map | Reinhard / ACES | ACES (unchanged) + OCIO output (v2.A.10's optional addition) |

The omission of full indirect diffuse is deliberate. OVRTX's lobby render uses irradiance probes (DDGI) for indirect diffuse; we approximate via RTAO + a tunable ambient term. The result reads as "OVRTX-like at a glance" without the multi-bounce cost.

### V2.B.1 Pipeline architecture (M21)

New `RenderGraph` shape, replacing v1's `PathTrace → Accumulation → ToneMap → AovResolve → DebugView → CopyToHydraBuffer → Present`:

```
PrimaryHit → ShadowRays → ReflectionRays → RTAO →
              Denoise (per component) →
              TemporalReproject (TAA + history) →
              Composite →
              ToneMap →
              AovResolve →
              DebugView →
              CopyToHydraBuffer →
              Present
```

Concretely:

- **PrimaryHit**: TraceRay closest-hit. Writes `albedo`, `normal`, `materialId`, `worldPos`, `motionVector`, `roughness`, `metallic` AOVs (G-buffer-like layout, but ray-traced not rasterized).
- **ShadowRays**: per-direct-light TraceRay with `RAY_FLAG_SKIP_CLOSEST_HIT_SHADER | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH` (v1 already does this). Writes per-light `shadowMask` AOV. **Light tree sampling**: 64 lights/frame max via §40.5's stochastic light tree.
- **ReflectionRays**: 1 TraceRay per pixel, GGX-importance-sampled direction. Hit's direct contribution evaluated inline. Writes `reflectionRadiance` AOV.
- **RTAO**: per pixel, hemisphere-sampled AO ray. World-space radius configurable (`RenderSettings::aoRadius`). Writes `aoMask` AOV.
- **Denoise** (per AOV): OIDN or OptiX denoiser, separately on `shadowMask`, `reflectionRadiance`, `aoMask`. Auxiliary inputs: `albedo` + `normal`. Each denoiser pass: ~1-3ms on 4070.
- **TemporalReproject**: previous-frame motion vectors + history buffer for each component. Stabilizes 1-spp output.
- **Composite**: combine `directRadiance + reflectionRadiance + emissive + (albedo × ambient × aoMask)`. Linear-light, pre-tone-map.
- **ToneMap**: ACES (unchanged from v1).

New passes that didn't exist in v1:
- `pass.ShadowRays` (split out from PathTrace's per-light loop)
- `pass.ReflectionRays`
- `pass.RTAO`
- `pass.DenoiseShadow`, `pass.DenoiseReflection`, `pass.DenoiseAO`
- `pass.TemporalReproject`
- `pass.Composite`

### V2.B.2 Direct lighting + light tree (M21)

v1's closesthit iterates every light, every pixel. Doesn't scale past ~50 lights.

**Plan**:
- **Light tree**: hierarchical BVH over lights, traversed stochastically. ~10 lights sampled per pixel for a 200-light scene (typical for an interior arch viz).
- Built CPU-side at scene load, refit on `Dirty<Light>` events.
- Stochastic descent: each level picks a child weighted by `(power × importance)`, importance derived from light's distance to the pixel's world-pos + cosine to surface normal.
- MIS: weight between the light tree's sample direction and a BSDF-sampled direction. Critical for glossy surfaces lit by area lights.
- Per-light shadow rays only fired for the sampled lights, not the full set.
- Falls back to the v1 enumeration path when light count ≤ 16.

**Reservation honoured**: §43.1 light/shadow linking already has `lightLinkSet` / `lightLinkMask` fields on `LightDesc` / `InstanceDesc`. The light-tree traversal AND-checks these masks before contributing.

**Exit**: 200-light synthetic test scene renders at the same per-frame cost as a 16-light scene (light-tree decorrelation working). Lobby's 30 lights run on the light tree path (over the 16-light threshold).

### V2.B.3 RT reflections (M22)

**Plan**:
- 1 TraceRay per pixel, direction = GGX-importance-sampled reflection.
- Roughness-aware: very rough surfaces (`roughness > 0.7`) skip the reflection ray and fall back to ambient-only — reflection is invisible anyway, save the ray.
- 1-bounce only (no recursion). 2nd-hit closesthit evaluates direct + emissive, no further reflection.
- New `ReflectionRayPayload` (smaller than the primary HitInfo — only RGB radiance + hitT).
- maxRecursionDepth = 3 (primary + reflection + reflection's shadow ray).

**Denoising**: SVGF-style spatial-temporal filter on `reflectionRadiance`. Auxiliary inputs: world-space normal, depth, roughness, history.

**Exit**: lobby's marble floor reflects the ceiling lights + dome. Glossy chrome reflects the room.

### V2.B.4 RTAO (M22)

**Plan**:
- Per pixel, hemisphere cosine-weighted ray, world-radius configurable (default 2.0 scene-space units).
- 1 ray per pixel; denoised spatially + temporally.
- Returns `1.0` (unoccluded) or `1.0 - hitFraction` (partial occlusion).
- Modulates `ambient × albedo` in the composite pass.

**Denoising**: shared SVGF infrastructure with reflections.

**Exit**: lobby contact shadows under furniture; ambient darkening in corners.

### V2.B.5 Denoiser integration (M23)

**Plan**:
- **OIDN** primary (Intel Open Image Denoise, Apache-2.0, vcpkg-packaged). Runs CUDA on NV / HIP on AMD / CPU fallback.
- **OptiX Denoiser** optional (NV SDK download; better-quality temporal mode but NV-only).
- Per-AOV denoise: `shadowMask`, `reflectionRadiance`, `aoMask` each get their own pass. **Not** combined into one denoiser invocation — each channel's noise profile is different.
- Auxiliary inputs: `albedo` + `normal` AOVs (already written by v1).
- History buffer (motion-vector reprojected previous frame) for temporal stability.
- New `RenderSettings::denoiser = {Off, OIDN_Cpu, OIDN_Cuda, OptiX}` field.

**Exit**: 1-spp single-pass output reads clean at 1080p.

### V2.B.6 Temporal reprojection + TAA (M24)

**Plan**:
- Per-frame motion vectors: `worldFromLocalPrev` (already a §43.2-reserved field on `InstanceDesc`) feeds the closesthit's `motionVector` AOV.
- History buffer: previous-frame composite + per-component buffers.
- Reproject: read history at `worldPos.xy - motionVector.xy`. Clamp/clip via neighborhood color box.
- 8-sample jitter on raygen (Halton already implemented).

**Exit**: camera move under-1-spp budget reads stable; no shimmer.

### V2.B.7 Performance gate + KPI (M25)

**Hero scene**: World Lobby, 1080p, RTX 4070 Laptop. Visual target: OVRTX-equivalent at a glance.

**KPI budget**:

| Pass | Budget |
|---|---|
| `pass.PrimaryHit` | 4 ms |
| `pass.ShadowRays` (~10 lights via tree) | 3 ms |
| `pass.ReflectionRays` | 3 ms |
| `pass.RTAO` | 1 ms |
| `pass.DenoiseShadow` | 1 ms |
| `pass.DenoiseReflection` | 1 ms |
| `pass.DenoiseAO` | 1 ms |
| `pass.TemporalReproject` | 0.5 ms |
| `pass.Composite + ToneMap + AovResolve` | 1 ms |
| **Total per frame** | **15.5 ms = 64 FPS** |

§34's "1080p hero camera" KPI gate is updated: `pass.PrimaryHit + pass.Shadow + pass.Reflection + pass.RTAO + pass.Denoise* + pass.TAA + pass.Composite < 16 ms p99` on RTX 4070 Laptop.

**Profiling overhead** (M11 gate): < 1% Release. Held over from v1.

---

## Part 3 — Milestones

### Loading completeness (Pillar A)

| | Name | Plan ref | Exit |
|---|---|---|---|
| **M12** | Subdivision + visibility + purpose | V2.A.1 + V2.A.2 | 83 lobby subdiv meshes refined; 4 invisible prims hidden; purpose filter active |
| **M13** | Curves + points | V2.A.3 | `BasisCurves` + `Points` ingest + ribbon/billboard triangulation + Lambert per-strand |
| **M14** | xformOps + variants + UDIM + time-varying primvars + PointInstancer extras | V2.A.6 + A.2 + A.7 + A.9 + A.13 | Production-pipeline correctness pass |
| **M15** | Volumes (OpenVDB) | V2.A.5 | Smoke fixture renders with absorption + scattering |
| **M16** | NURBS + Skel + time-varying USD | V2.A.4 | Animated character renders, advancing per-frame |
| **M17** | Texture streaming + LRU | V2.A.12 | Oversized-lobby × 10 renders at 8 GB VRAM with degraded mips |
| **M18** | MaterialX coverage matrix | V2.A.8 | Every OPS / StdSurface nodedef fixture-tested |
| **M19** | Asset resolvers + USDZ stress | V2.A.10 | USDZ + custom ArResolver fixtures pass |
| **M20** | Loaded-everything audit + dirty fixtures | V2.A.11 | Round-trip USD→Pyxis→USD preserves every authored property |

### Real-time raytracer (Pillar B)

| | Name | Plan ref | Exit |
|---|---|---|---|
| **M21** | Pipeline pivot + light tree + MIS | V2.B.1 + V2.B.2 | New `PrimaryHit + ShadowRays` passes; light tree handles 200-light scenes; MIS for direct |
| **M22** | RT reflections + RTAO | V2.B.3 + V2.B.4 | Reflection + AO AOVs written, composite uses them |
| **M23** | Denoiser integration | V2.B.5 | OIDN passes denoise each component; visible quality unlock |
| **M24** | Temporal reprojection + TAA | V2.B.6 | Stable under-1-spp rendering during camera move |
| **M25** | Lobby real-time gate | V2.B.7 | Lobby renders at 60 FPS p99, visually comparable to OVRTX reference |

### Cuts to keep v2 from sprawling

Out of v2 scope (revisit for v3):

- **DDGI** / probe-based indirect diffuse (RTAO is the v2 approximation).
- **DLSS / DLSS-RR / DLSS3** (NV SDK licensing gate; out of v2 even if we ship it as opt-in later).
- **Multi-GPU**, **D3D12 backend**, **macOS / Linux ports** — same §42 deferral as v1.
- **Multi-bounce indirect specular** beyond 1 bounce.
- **Spectral rendering** for dispersion / iridescence.
- **Hair Marschner BSDF** (v2.1 if curves needed).
- **Material editor / node graph UI** — out of scope, separate tool.
- **Render farm / distributed rendering** — never v1/v2 scope.

---

## Part 4 — Cross-cutting changes

### V2.4.1 Public API additions (§22 MINOR-additive)

Per §22.3, every v2 public API addition uses the trailing `_reserved*` slots reserved in v1 PODs:

- `RenderSettings::subdivLevelCap` (claims `_reserved1`)
- `RenderSettings::purposeFilter` (claims `_reserved2`)
- `RenderSettings::denoiser` (claims `_reserved3`)
- `RenderSettings::aoRadius` (claims `_reserved4`)
- `RenderSettings::timeCode` (claims `_reserved5`)
- `OpenPBRMaterialDesc::volumeAbsorption / volumeScattering / volumeAnisotropy` (claims trailing `_reserved` slots)
- New POD: `CurvesDesc`, `PointsDesc`, `VolumeDesc` — entirely new files in `Public/Pyxis/Renderer/Descs/`
- New POD: `TimeSampleSet` (`Public/Pyxis/Renderer/Descs/TimeSampleSet.h`) — generic time-sample container loaded for any animated attribute
- New POD: `SkeletalAnimation`, `JointTransform` for skel
- New Profiler scopes: `pass.PrimaryHit`, `pass.ShadowRays`, `pass.ReflectionRays`, `pass.RTAO`, `pass.Denoise*`, `pass.TemporalReproject`, `pass.Composite`

All additive. Version bump per v2 cut: 1.0.0 → 2.0.0 only when the **rendering algorithm** changes (M21 lands). M12-M20 are loading-correctness — each bumps minor (1.1.0, 1.2.0, …, 1.9.0). M21+ bumps to 2.0.0.

### V2.4.2 Regression infrastructure

M10 already shipped the harness. v2 adds:

- Per-milestone baseline EXR captures (some scenes will reshape; some will look identical post-subdiv refinement, etc.).
- Per-milestone KPI CSV rows.
- `_tools/perf_compare.py` rolling median window updated to 7-day for the longer v2 milestone cadence.

### V2.4.3 Build matrix

Same Windows / clang-cl / `/W4 /WX` baseline from v1. New vcpkg deps:

- `opensubdiv` (M12)
- `openvdb` (M15)
- `oidn` (M23 — primary denoiser)

OptiX is an NV SDK download, not vcpkg-packaged. Optional dep with `find_package(OptiX QUIET)` gate.

### V2.4.4 Testing

Every milestone adds:

- Unit tests for new translator paths (subdiv refinement output, NURBS tessellation, time-sample interpolation).
- Integration fixtures (`tests/regression/<scene>/`) — at minimum one positive case per gap closed.
- Profile capture in headless bench mode.

### V2.4.5 Documentation

Each milestone updates `plan_for_v2.md` (this file) with deltas + actual numbers measured against KPI budgets.

A new `_documentation/v2-mxx-<short-name>.md` per milestone, same structure as `_documentation/m9-lobby-visual-correctness.md` / `_documentation/m11-profiling-overhead.md` (v1 precedent).

---

## Part 5 — Order of execution

1. **M12 first.** The 83 lobby subdiv meshes are a real silent bug we ship today. Highest leverage per effort.
2. **M13 second.** Curves + points are the next-most-common production geometry beyond meshes. Required for any outdoor / garden scene.
3. **M14 third.** xformOp coverage + variants + UDIM + time-varying primvars + PointInstancer extras. Lots of small wins, low risk per change.
4. **M15 + M16** can run in parallel if there's bandwidth — Volumes (M15) is a new subsystem; NURBS+Skel+TimeVarying (M16) is a different new subsystem. Both touch §22-reserved API slots without colliding.
5. **M17.** Texture streaming. Foundational for any large-scene work; depends on M12-M16 completing so the streaming budget is calibrated against the right working sets.
6. **M18-M20.** Materials, resolvers, audit. Mostly correctness polish; can interleave with later items if needed.
7. **Cut 1.x → 2.0**. Tag the loading-completeness milestone. Pillar A done.
8. **M21.** Pipeline pivot. The single biggest commit in v2. Replaces the closesthit. Likely a v2 standalone PR.
9. **M22-M24.** Real-time refinement passes. Sequenced because each depends on its predecessor (reflections need primary G-buffer; AO needs primary; denoisers need component AOVs; TAA needs motion vectors).
10. **M25.** Lobby real-time gate. The KPI hit is the v2 ship signal.

Estimated calendar: M12-M20 is ~6 months at one milestone per 2-3 weeks. M21-M25 is ~3 months. Total v2: ~9-12 months from v1.0.0.

---

## Part 6 — What v2 explicitly does NOT do

A reminder, because the §42 list is long and tempting:

| Deferred to v3+ | Why |
|---|---|
| **Full path-traced indirect diffuse** (multi-bounce) | RTAO is the v2 approximation; full indirect is offline-only |
| **DDGI** / probe systems | Out of v2 scope; complex enough to warrant its own milestone block |
| **DLSS / DLSS-RR / DLSS3 / Reflex** | NV SDK gates; ship as opt-in v3 maybe |
| **Spectral rendering** | Niche; almost no production USD authors spectral data |
| **OMM (Opacity Micro Maps)** | RTXMU v1 didn't support; revisit if RTXMU adds the feature |
| **DMM (Displacement Micro Maps)** | Same as OMM |
| **Bidirectional path tracing / VCM** | Offline-only; v2 is real-time |
| **D3D12 backend** | Windows-only via Vulkan in v2 |
| **macOS / Linux ports** | Still §42 |
| **Network / render-farm features** | Never v1/v2 scope |
| **Hair Marschner BSDF** | v2 ships Lambert per-strand; Marschner deferred |
| **Material editor UI** | Separate tool, not in pyxis_app |
| **Custom OCIO color management** | Optional v2.A.10 if community asks; default ACES sufficient |

If any of these become tempting, file an RFC under §44 first. Don't slip them in mid-milestone.

---

## Part 7 — Success metrics

v2 is done when:

1. **Loading completeness**: a `_tools/usd_loading_audit.py` script walks a corpus of production USD scenes (lobby + a curated set of public Omniverse / NVIDIA samples) and reports < 1% unsupported-prim-type warnings. The current lobby reports 0% but the bar widens with M13+ work.
2. **Round-trip integrity**: `gpuScene.SaveAsUsd(path)` on the lobby produces a USD whose `usddiff` against the source is empty (modulo metadata reorderings USD's serializer can't avoid).
3. **Real-time KPI**: lobby renders at 60 FPS p99 at 1080p on RTX 4070 Laptop, with all of: light-tree direct + RT shadows + RT reflections + RTAO + denoise + TAA enabled.
4. **Visual reference**: side-by-side screenshot vs OVRTX-rendered lobby (https://docs.omniverse.nvidia.com/usd/latest/usd_content_samples/res_lobby.html) reads as "same scene, same lighting intent, different renderer" — not "different scene" or "obviously broken".
5. **Profiler overhead**: still < 1% Release per M11 / §34 gate.
6. **106/106 + new** tests pass.

That's the v2 ship gate. v1.0.0 closed; v2.0.0 awaits.
