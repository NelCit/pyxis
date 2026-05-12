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
| **B. Real-time raytracer** | Three components per pixel: direct lighting, ray-traced shadows, 1-bounce ray-traced specular reflections. That's it — no RTAO, no light tree, no MIS, no denoiser, no TAA. Roughness-blended fallback to dome envmap on rough surfaces keeps reflections clean without ML denoising. Real-time on RTX 4070+ at 1080p. | Multi-bounce indirect, RTAO, denoisers, TAA, light tree (all v3+) |

**Order**: Pillar A first (M12..M28), Pillar B second (M29..M31). Loading completeness is a prerequisite — the real-time pipeline still consumes Pyxis's `SceneWorld` view, so any prim type missing from ingest is also missing from real-time rendering.

**On "completeness"**: this plan enumerates the **common production cases** under named milestones (M12..M27). The plan is not the proof of completeness — USD's schema set is enormous and corners exist that no enumeration catches. **M28** is the formal audit milestone: an automated walk of every `pxr/usd*/` schema header that compares against Pyxis's handler registry and reports unhandled prim types / applied APIs / attribute spec gaps. M28's exit gate is the audit-script signal, not a fixture-by-fixture check.

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

### V2.A.14 Texture compression + format coverage (M21)

**Why it matters**: v1 uploads raw RGBA8/16F/32F. Lobby has 169 textures totalling ~1.4 GB on disk; uncompressed in VRAM that's ~4-6 GB. With BCn-compressed uploads, the same set fits in ~250-400 MB — critical for the streaming budget (V2.A.12) and for 8 GB-class consumer GPUs.

**Plan**:
- **Online BCn encoding**: new vcpkg dep `nvtt` (NVIDIA Texture Tools) or `Compressonator`. Encode at decode time before GPU upload. CPU-bound; runs on the asset I/O pool.
  - BC1 for opaque RGB diffuse (4 bpp).
  - BC3 for RGBA with alpha (8 bpp).
  - BC5 for two-channel data (normal X/Y, metallic-roughness).
  - BC6H for HDR floats (env-map dome, emissive textures with >1.0 values).
  - BC7 for high-quality opaque RGB / RGBA where 8 bpp is OK.
- **Per-role compression policy**:

| Role | Default format | Rationale |
|---|---|---|
| `BaseColor` (`SRGB`) | BC7 with sRGB flag | High quality; alpha when needed via BC3 |
| `NormalMap` (`Linear`) | BC5 | Two-channel encoded; reconstruct Z in shader |
| `RoughnessMetallic` (`Linear`) | BC5 | Two channels packed |
| `Emission` (HDR) | BC6H | HDR signed float |
| `Opacity` (`Linear`) | BC4 | Single channel |
| `EnvLatLong` (`Linear`, HDR) | BC6H | Dome HDRI |

- **Pre-compressed input**: ingest DDS / KTX2 directly when authored. Omniverse + NVIDIA-pipeline scenes often ship `.dds` with mip chain baked. Add `gli` or `tinyddsloader` vcpkg port for DDS read; KTX2 via `libktx`.
- **Additional formats**: TIFF (`libtiff`), Radiance HDR (already in stb), KTX2.
- **Per-texture format choice**: `TextureKey::compressionPolicy = {Auto, ForceBCn, ForceUncompressed}` field (claims a TextureKey reserved slot).
- **Premultiplied vs straight alpha**: detect via `<UsdUVTexture>::sourceColorSpace` + alpha histogram; convert to straight-alpha at decode unless authored premultiplied.
- **Mipmap policy**: respect authored mips in DDS/KTX2; regenerate via GPU mip pass for PNG/JPG/EXR/TIFF.

**Loaded but unused-on-GPU**:
- The full mip chain is loaded; streaming (V2.A.12) may drop the finest mips under VRAM pressure.

**Exit**: lobby texture VRAM footprint drops from ~1.4 GB to < 300 MB. Per-texture decode pipeline reports compression ratio in the §34.1 load summary.

### V2.A.15 USD composition + load modes (M21)

**Plan**:
- **`UsdStage::Open(path, loadMask)`** — new CLI `--load-mode {all,none,explicit}` overriding the default LoadAll.
- **`UsdStagePopulationMask`** — `--population-mask "/path/to/subtree"` for partial loads. Critical for opening a slice of a 100 GB scene.
- **Sublayers** (`SdfLayer::subLayers`): verify our `UsdStage::Open` honours sublayer composition (it does, but we never tested with a 5-sublayer asset).
- **`payload` vs `references`** distinction: `payload` is lazy by default. With `--load-mode none`, the user iteratively `LoadAndUnload` payloads to walk a huge scene.
- **`UsdStage::Reload()`** for hot-reload — pairs with M16's `UsdNotice` listener.

**Loaded but unused-on-GPU**:
- Unloaded payload paths recorded so the inspector can show "this prim is unloaded; click to load".

**Exit**: a 100 GB synthetic stage with population mask loads in < 1 second to "stage opened, root subtree only".

### V2.A.16 Stage + layer metadata (M14)

USD's stage and layer carry metadata that v1 ignores:

**Plan**:
- **Stage metadata**: `defaultPrim`, `startTimeCode`, `endTimeCode`, `timeCodesPerSecond`, `framesPerSecond` — animation gating. `upAxis` + `metersPerUnit` already in `StageContext`. `documentation` (free-form text), `customLayerData` (DCC pipeline metadata Omniverse + Maya-USD author).
- **Per-layer metadata**: `SdfLayer::GetComment()`, `SdfLayer::GetOwner()`, version metadata.
- **Per-prim metadata**: `customData` dictionary (round-trip-only), `assetInfo` (DCC-pipeline asset tracking — Maya / Houdini-USD use this for asset versions), `documentation`.
- **`UsdPrim::GetMetadata` for arbitrary keys** — store as a per-prim metadata map in the Pyxis scene state.

**Loaded but unused-on-GPU**: All of the above. Rendered as inspector entries; preserved on save-as-USD.

### V2.A.17 Prim state — active / specifier / kind / hidden (M14)

**Plan**:
- **`UsdPrim::IsActive`** — `active = false` should drop the prim + all descendants. Today we don't check.
- **`UsdPrim::GetSpecifier`** — `def` / `over` / `class`. `over`s with no `def` ancestor are abstract; `class`es are pure abstract. We treat all as `def`.
- **`UsdPrim::GetKind`** — `assembly / group / component / subcomponent / model / etc.` — traversal pruning hints. Not strictly required for rendering but used by inspectors + tools.
- **`hidden` vs `invisible`** — `hidden = true` is a UI hint (collapse in scene browser); `visibility = invisible` actually hides from render. v1 conflates them.

**Loaded but unused-on-GPU**:
- `kind` / `hidden` / `specifier` preserved in per-prim metadata; not consumed by the renderer.

### V2.A.18 Material binding details (M18)

**Plan**:
- **Binding strength**: `UsdShadeMaterialBindingAPI::ComputeBoundMaterial(purpose, materialPurpose)` with `bindingStrength` resolution. `weakerThanDescendants` lets a descendant override; `strongerThanDescendants` doesn't. v1 picks "whatever ComputeBoundMaterial returns first" without checking strength.
- **Per-render-context material binding**: a single prim can author `material:binding`, `material:binding:full`, `material:binding:preview`, etc. We use the default. Production pipelines author `:full` (final-quality MaterialX) and `:preview` (UsdPreviewSurface fast-path) separately.
- **`UsdShadeMaterial::ComputeSurfaceSource(renderContexts)`** — multi-context resolution. Pyxis should attempt `mdl → MaterialX → UsdPreviewSurface` in priority order, falling back to whichever exists.

**Loaded but unused-on-GPU**:
- Non-selected material variants — preserved for save-as-USD round-trip.

### V2.A.19 Light extras — collections + filters + shadow API (M14)

**Plan**:
- **`UsdLuxCollectionAPI`** for light linking — the canonical USD way to declare "this light affects these prims" via `collection:lights:includes/excludes`. Resolves to the `lightLinkSet/Mask` reserved fields v1 already has on `LightDesc` / `InstanceDesc` (§43.1).
  - At ingest: walk every light's collection, resolve `includes`/`excludes` to a set of `InstanceHandle`s, populate the v1-reserved mask fields.
  - Closesthit `(light.lightLinkSet & instance.lightLinkMask) != 0` gate fires during Pillar B's per-light evaluation.
- **`UsdLuxShadowAPI`** — `inputs:shadow:enable`, `inputs:shadow:color`, `inputs:shadow:falloff`, `inputs:shadow:falloffGamma`, `inputs:shadow:distance`. New fields on `LightDesc`.
- **`UsdLuxLightFilter`** + **`UsdLuxLightFilter::FilterLinkAPI`** — light cookies / gobos (project a texture through a light). Rare; loaded into per-light filter list, ignored on GPU until v3.
- **`UsdLuxShapingAPI::shaping:ies:file`** — IES profile (identified earlier as unused in lobby, deferred). v2 picks up: parser + GPU 1D texture + closesthit cone modulation.

**Loaded but unused-on-GPU**:
- Light filters / cookies (v3).
- `shadow:falloff` curves (v2.0 ships boolean shadow enable + color; falloff curves v2.1+).

### V2.A.20 Camera extras (M14)

**Plan**:
- **Orthographic projection** — `UsdGeomCamera::projection = orthographic`. Different raygen ray-direction math (uniform parallel rays vs perspective fan).
- **`clippingPlanes`** array — beyond near/far. NVRHI doesn't directly support arbitrary clipping planes for RT; we'd need closesthit-side discard.
- **`shutter:open/close`** — already loaded for §43.2 motion blur reservation.
- **`exposure`** — already loaded (v1).
- **`focalLength` / `horizontalAperture` / `verticalAperture`** in mm + `focusDistance` + `fStop` — physically-based camera intrinsics. v1 reads focal length; the rest are loaded but unused.
- **Stereo cameras** — `UsdGeomCamera::stereoRole = mono|left|right`. Out of scope for rendering, loaded into camera metadata.

**Loaded but unused-on-GPU**:
- DoF until §43.3 lands in Pillar B.
- Stereo cameras (rendered mono for v2).

### V2.A.21 Analytic geometric primitives (M13)

USD ships several analytic geom prims that v1 doesn't ingest:

| Prim type | v2 approach |
|---|---|
| `UsdGeomSphere` | Tessellate at ingest (icosphere) OR add intersection-shader BLAS primitive (NVRHI custom-primitive AS). Tessellate v2.0; intersection-shader v2.1+. |
| `UsdGeomCube` | Tessellate to 12 triangles. |
| `UsdGeomCylinder` | Tessellate with `radius` + `height` + `axis`. |
| `UsdGeomCone` | Tessellate with `radius` + `height` + `axis`. |
| `UsdGeomCapsule` | Tessellate (cylinder + 2 hemispheres). |

**Plan**: a small `pyxis_usd_ingest/Private/AnalyticPrimMesh.cpp` helper that, given the prim type + its parameters, returns a triangle mesh suitable for `GpuScene::CreateMesh`. Configurable tessellation level per type.

**Loaded but unused-on-GPU**:
- The authored `radius / height` etc. preserved on the Pyxis entity; renderer reads only the resulting mesh.

### V2.A.22 `UsdRender` schema (M19)

USD has a first-class **render schema** for declaring render outputs in USD itself:

**Plan**:
- **`UsdRenderProduct`** — declares one output image (path, AOVs, camera, resolution). Multiple per stage common.
- **`UsdRenderVar`** — declares an AOV by name + format + sourceType.
- **`UsdRenderSettings`** — global render config (camera target, sample count, resolution). Authored on the stage; should override `Configuration` JSON when present.

**Plan**:
- New ingest pass after pass 1 (materials): collect every `UsdRenderProduct/Var/Settings` prim.
- New `Pyxis::RenderSettingsAuthored` struct holding the parsed values; `Configuration` overlay precedence: USD-authored > JSON > CLI > defaults.
- `--render-product <name>` CLI flag picks which RenderProduct to use when the stage authors multiple (default: first by SdfPath).

**Loaded but unused-on-GPU**:
- AOV declarations beyond what we already write (Cryptomatte, motion vector debug, etc.) — loaded for compatibility; renderer doesn't fill them yet.

### V2.A.23 MDL materials (M18)

Omniverse / NVIDIA pipelines author **MDL** (Material Definition Language) shaders alongside MaterialX. v1 plan §11 deliberately deferred; v2 picks up because Omniverse-pipeline scenes (including future hero scenes) frequently author MDL.

**Plan**:
- Detect `info:mdl:sourceAsset` + `info:mdl:sourceAsset:subIdentifier` on `UsdShadeShader` prims.
- Translate the supported MDL OmniPBR / OmniGlass / OmniSurface materials to `OpenPBRMaterialDesc` — these are the three Omniverse ships by default for arch viz.
- Unsupported MDL networks: log once into `unsupported_features.json` + fall back to the closest UsdPreviewSurface authored on the same material (production scenes typically author both).
- Full MDL runtime evaluation (the proper way) is out of v2 scope — it requires the NVIDIA MDL SDK, which is a large dep with its own licensing. We translate known patterns to OpenPBR instead.

**Loaded but unused-on-GPU**:
- MDL networks beyond OmniPBR/Glass/Surface — logged + skipped + the fallback path used.

### V2.A.24 Texture / shader edge cases (M18)

**Plan**:
- **`UsdUVTexture` wrap modes**: `wrapS` / `wrapT` / `wrapR` — `repeat` / `mirror` / `clamp` / `useMetadata`. Per-texture sampler addressing mode in NVRHI. New `TextureSampler` struct + per-binding sampler resolution.
- **`UsdUVTexture::sourceColorSpace`** — `raw` / `sRGB` / `auto`. Today we map `BaseColor` role to sRGB EOTF on decode; explicit per-shader override should win.
- **Filter modes** — `minFilter` / `magFilter` authored on shaders — `nearest` / `linear` / `cubic`. v1 always linear-trilinear.
- **`UsdTransform2d`** — 2D texcoord transform shaders (scale/rotate/translate UVs). Currently the closesthit reads raw `uv0`; with `UsdTransform2d` in the network, we should apply the transform at material conversion time (CPU-side affine bake into the texture sampling matrix).
- **`UsdPrimvarReader_*`** typed shaders — `_float`, `_float2`, `_float3`, `_int`, `_string`. Route to closesthit primvar reads.

**Loaded but unused-on-GPU**:
- Filter modes beyond linear (cubic upsample is GPU-expensive; v2 ignores and always linear).

### V2.A.25 Primvar interpolation edge cases (M14)

v1 handles `vertex` + `faceVarying` + `constant` + `uniform`. Missing:

**Plan**:
- **`varying` interpolation** — per-vertex but interpolated per-pixel slightly differently than `vertex` (USD spec calls this "linearly interpolated"). Not commonly used; treat as `vertex` for now and log if encountered.
- **Primvar inheritance** (`UsdGeomPrimvarsAPI::FindPrimvarsWithInheritance`) — primvars authored on ancestor prims inherit down. We today read primvars on the leaf prim only.
- **Indexed primvars beyond ComputeFlattened**: we `ComputeFlattened` for UVs; verify other primvars (normals, tangents, colors) flatten correctly when indexed.

**Loaded but unused-on-GPU**:
- Inherited primvars resolved at ingest into the leaf primvar set.

### V2.A.26 Geometry mesh attribute coverage (M12 / M16)

**Plan**:
- **`UsdGeomMesh::orientation`** — `rightHanded` (default) vs `leftHanded`. If `leftHanded`, flip face winding at ingest so normals point out.
- **`UsdGeomMesh::holeIndices`** — face holes; drop those faces at triangulation.
- **`UsdGeomMesh::triangleSubdivisionRule`** — affects subdiv stencil computation (M12).
- **`UsdGeomMesh::cornerSharpnesses` / `creaseSharpnesses` with +inf** — infinite-sharp creases (CAD-derived assets). M12's OpenSubdiv path honours this when the C++ float is +infinity.
- **`UsdGeomMesh::interpolateBoundary` modes** (`none`/`edgeOnly`/`edgeAndCorner`) — M12 must respect.
- **`UsdGeomMesh::faceVaryingLinearInterpolation` modes** — FVAR primvar interpolation for subdiv. M12 must respect.
- **`UsdGeomMesh::geomBindTransform`** — used by `UsdSkel` skinning (M16); also useful for skinned-mesh transforms.

### V2.A.27 USD composition fine points (M19)

**Plan**:
- **`inherits` and `specializes` arc resolution** — pxr handles internally; v2 adds regression fixtures asserting correct composition order.
- **`UsdEditTarget`** for runtime edits — wire into M16's live-USD pipeline.
- **`SdfChangeBlock`** for batching — used internally by the live-update pipeline for atomic multi-prim edits.
- **Layer offsets + scale** for time-remapped sublayers — animation retiming.

### V2.A.29 OpenColorIO color management (M18)

v1 hardcodes sRGB / Linear EOTFs based on `TextureKey::Role`. Production pipelines author textures in **ACEScg** or **Rec.709** or studio-custom color spaces.

**Plan**:
- New vcpkg dep: `opencolorio` (OCIO). Apache-2.0.
- `UsdUVTexture::sourceColorSpace` resolves through an OCIO config when one is supplied via `Configuration::ocio.configPath` (new field on the config JSON, not the public renderer API).
- Default OCIO config: built-in ACES 1.3 (OCIO ships a baseline). Studio override via env var `OCIO=path/to/config.ocio`.
- Closesthit-side: linear-light path stays unchanged; OCIO is a decode-time transform on the CPU.
- Display-side: tone-map pass optionally consumes an OCIO display+view transform instead of ACES-fixed.

**Loaded but unused-on-GPU**:
- Full OCIO graph metadata preserved per-texture (for round-trip USD-save).

**Exit**: a fixture with ACEScg-authored textures + OCIO config renders correctly; the lobby unchanged (sRGB default still works).

### V2.A.30 Material network outputs beyond surface (M18)

USD `UsdShadeMaterial` exposes four output triplets: `surface`, `displacement`, `volume`, `light`. v1 reads only `surface`.

**Plan**:
- **`displacement`** output: load the network even though we don't render displacement (v1.x doesn't have a displacement pass; OMM/DMM micromaps are out of v2). Round-trip preservation.
- **`volume`** output: when present alongside V2.A.5's Volumes, route to the volume integrator's material parameters.
- **`light`** output: filterable shader networks for `UsdLuxLightFilter` (cookies / gobos). Load but ignore on GPU.

**Loaded but unused-on-GPU**:
- Displacement networks — preserved; no renderer consumer until v3.
- Light shader networks — preserved.

### V2.A.31 Bounding-box hints (M14)

**Plan**:
- **`UsdGeomBoundable::ComputeExtent` + `extentsHint`** — read at ingest, populate per-instance AABBs for the TLAS. Speeds up TLAS rebuilds (no recomputation) and lets Pillar B's primary ray do early-out frustum culling efficiently.
- **`extent`** vs **`extentsHint`** distinction: `extent` is authored locally on the prim; `extentsHint` is pre-cached on a higher prim covering multiple descendants. Use whichever is authored.

**Loaded but unused-on-GPU**:
- Currently NVRHI/RTXMU recomputes AABBs from triangle data — the hint is loaded into Pyxis state but the BLAS builder doesn't consume it yet. Plumbing for "use authored AABB" lands when needed.

### V2.A.32 Codeless schemas + custom prim types + UsdProc (M28)

USD supports plugin-registered prim types beyond the built-in schema set:

**Plan**:
- **`UsdSchemaRegistry::FindConcretePrimDefinition`** — query at ingest to know whether a type is a known schema (handled) vs a plugin-defined type (codeless).
- **Codeless schemas** (`_codelessSchemaTypeNames` in `plugInfo.json`) — log once with the type name + the plugin source. Don't crash.
- **`UsdProcGenerativeProcedural`** — runtime procedural geometry. v2 logs + skips; v3 may execute via a procedural-evaluation framework.

**Strategy**: M28's audit script picks these up automatically by walking against `UsdSchemaRegistry`. Unknown types → "explicit skip with log line" category, not "unhandled".

### V2.A.33 displayColor + displayOpacity fallback (M14)

**Plan**:
- When a mesh has **no material binding**, fall back to `primvars:displayColor` (per-vertex or per-face RGB) + `primvars:displayOpacity` (alpha) as a Lambertian colour. v1 currently uses a hardcoded grey.
- When BOTH are absent, v1's grey fallback remains.
- Time-varying `displayColor` honoured via V2.A.13.

**Loaded but unused-on-GPU**:
- `displayColor` always loaded (even when a material is bound); the renderer ignores it if a material is bound but the data is preserved for inspector / USD round-trip.

### V2.A.28 Loaded-everything audit (M28 — formal completeness gate)

**Strengthened from V2.A.11**: this milestone's exit gate is the **automated schema audit**, not fixture-by-fixture testing.

**Plan**:
- New tool `_tools/usd_loading_audit.py`:
  1. Enumerates every C++ schema header under `pxr/usd/usd*` + `pxr/usd/usdGeom*` + `pxr/usd/usdLux*` + `pxr/usd/usdShade*` + `pxr/usd/usdSkel*` + `pxr/usd/usdVol*` + `pxr/usd/usdRender*` + `pxr/usd/usdRi*` + `pxr/usd/usdProc*` + `pxr/usd/usdUI*`.
  2. Extracts every concrete `UsdSchemaBase` subclass + every `*API` schema applied via `UsdAPISchemaBase`.
  3. Cross-references against Pyxis's handler registry (a new `StageWalker` static registry table mapping schema-name → handler-function).
  4. Reports as Markdown:
     - **Handled**: schema → handler function name + the milestone that added it.
     - **Explicitly skipped**: schema → log-line emitted when encountered + reason.
     - **Unhandled**: schema → silently dropped at ingest. **These are the v2 ship-blockers.**
- **Exit gate**: zero unhandled rows in the report. Every USD schema either has a handler OR an explicit-skip log. Silent drops fail the milestone.
- **§36.7 / §36.8 fixtures** all green: every fixture renders without unexpected error + the dirty-USD fixtures produce typed-error exits.

**Round-trip integrity**: a separate test `tests/integration/m28_round_trip.cmake` opens the lobby, exports via `gpuScene.SaveAsUsd(out.usd)`, opens `out.usd`, compares — `usddiff` must return empty.

**Loaded but unused-on-GPU**:
- Schema types we explicitly skip (e.g. `UsdGeomTetMesh`, `UsdGeomHermiteCurves`) are documented in `_documentation/v2-skipped-schemas.md` with rationale. Future v3 work picks from this list.

---

## Part 2 — Pillar B: real-time raytracer

### Philosophy

> *"For each pixel: direct lighting, shadows, specular reflections. That's almost everything."*

v1's closesthit returned the direct contribution from N lights and stopped — a degenerate single-bounce path tracer. v2's real-time renderer is a **classic hybrid ray tracer**: per-pixel direct lighting + shadow rays (v1 already does these) plus one new addition: **per-pixel specular reflection rays**.

Deliberately **out of scope**:

- No path tracing (no recursive multi-bounce indirect).
- No light tree, no MIS — v1's straight per-light enumeration runs fine for the ~30 lights the lobby ships and similar arch-viz scenes. Plain.
- No RTAO. Ambient is a flat tunable.
- No denoiser. With direct + 1 reflection ray + simple roughness-blend-with-envmap, the per-pixel result is clean enough without ML / spatial filtering machinery.
- No TAA / temporal reprojection. Each frame is independent.

This is the "RTX-On, 2018 vintage" feature set — exactly what the user asked for. Visually: hard direct lighting, hard ray-traced shadows, ray-traced reflections on glossy surfaces. No fake hacks (no SSR, no static envmap-only reflections, no screen-space shadow approximation). Just three ray-traced components, composited.

| Component | v1 path tracer | v2 real-time |
|---|---|---|
| Primary visibility | TraceRay closest-hit | TraceRay closest-hit (unchanged) |
| Direct lighting | Per-light shadow ray + Lambert+GGX | Per-light shadow ray + Lambert+GGX (unchanged) |
| Indirect specular | (none) | **1-bounce reflection TraceRay** |
| Indirect diffuse | (none) | Flat ambient × dome-light tint |
| Anti-aliasing | Halton jitter + accumulation | Halton jitter + accumulation (unchanged) |
| Tone-map | ACES | ACES (unchanged) |

### V2.B.1 Pipeline architecture (M29) — single-pass

The `RenderGraph` shape is **unchanged from v1**. No new render pass is added. The entire Pillar B addition lives inside the existing `pass.PathTrace`'s closesthit body.

```
PathTrace → Accumulation → ToneMap → AovResolve → DebugView →
            CopyToHydraBuffer → Present
```

The closesthit body gains one inline `TraceRay` for the reflection branch:

```hlsl
[shader("closesthit")]
void main(inout HitInfo payload, in BuiltInTriangleIntersectionAttributes attribs) {
    // (existing v1 prologue: read material, interp UV+normal+tangent, sample baseColor, etc.)

    // -------- Direct + shadows (already v1, body unchanged) --------
    for (uint i = 0; i < lightCount; ++i) {
        // Lambert + GGX, per-light shadow ray, light/shadow linking gate (M14)
        // accumulates into directDiffuse + directSpecular
    }

    // -------- New: per-pixel RT specular reflection (M30) --------
    // Gated on: still under recursion budget + material is reflective enough
    // to be worth the ray. Rough surfaces hit the envmap fallback below.
    float3 reflected = float3(0, 0, 0);
    const bool wantsReflection =
        payload.recursionDepth < MAX_REFLECTION_DEPTH
        && (mat.metalness > 0.01 || mat.specular > 0.01)
        && roughness < gRenderSettings.reflectionMaxRoughness;

    if (wantsReflection) {
        const float3 refDir = reflect(WorldRayDirection(), nWorld);
        RayDesc reflRay;
        reflRay.Origin    = hitWorld + nWorld * 1e-3;
        reflRay.Direction = refDir;
        reflRay.TMin      = 1e-3;
        reflRay.TMax      = 1e30;
        HitInfo reflPayload;
        reflPayload.color           = float3(0,0,0);
        reflPayload.recursionDepth  = payload.recursionDepth + 1;  // increment
        TraceRay(gTlas,
                 RAY_FLAG_NONE,
                 0xFFu,
                 0, 0, 0,
                 reflRay,
                 reflPayload);
        reflected = reflPayload.color;
    } else if (mat.metalness > 0.01 || mat.specular > 0.01) {
        // Rough surface — sample the dome envmap at the reflection direction.
        // No ray cost; smooth result because the envmap is pre-filtered.
        reflected = SampleEnvmap(reflect(WorldRayDirection(), nWorld));
    }

    // -------- Composite --------
    const float3 specularWeight = SchlickF0(baseColor, mat.metalness, mat.specularIor);
    payload.color = directDiffuse + directSpecular + emission +
                    reflected * specularWeight;
}
```

Key points:

- **One `TraceRay` dispatch from raygen**. The reflection is a recursive `TraceRay` *inside* the closesthit, not a new pass.
- **`HitInfo::recursionDepth` field added** to the shared payload — increments per bounce. The closesthit checks it to prevent further reflection on a reflection's hit (one bounce only).
- **`maxRecursionDepth = 3` on the RT pipeline**: primary closesthit + reflection ray's closesthit + reflection's shadow rays. Already where v1's pipeline is (we set 2 for shadow rays in M9-fidelity; v2 bumps to 3).
- **No new pass class, no new binding-set wiring, no new render-graph node.** Just an extension of the existing closesthit body.
- **No intermediate AOV** — reflection radiance is composited inline into `payload.color`, never round-trips through a render target.

**Trade-off acknowledged**: this design **doesn't scale** when Pillar B grows beyond direct + shadows + 1 reflection. If a future v3 adds RTAO or multi-bounce or denoising, the closesthit becomes too divergent and we'd refactor to a split-pass architecture then. For v2's tight scope, single-pass is the right shape.

### V2.B.2 Per-pixel direct lighting (no change from v1)

Already shipped. v2 keeps the same body:

- Loop every authored light
- Compute Lambert diffuse + Schlick-Fresnel GGX specular
- Per-light shadow ray (RAY_FLAG_SKIP_CLOSEST_HIT_SHADER | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH)
- Light/shadow linking via the v1-reserved `lightLinkSet`/`lightLinkMask` fields, populated by V2.A.19's collection resolution

**No light tree, no MIS** — for scenes with ≤ 50 lights (the v2 target lobby + typical arch viz), per-light enumeration is < 1 ms on RTX 4070 and doesn't need the complexity of stochastic sampling.

### V2.B.3 Per-pixel RT specular reflections (M30)

The single new feature in Pillar B. Implemented as an **inline `TraceRay` inside the existing closesthit** — no new pass class.

**Plan**:

- `HitInfo` payload (shared C++/Slang struct in `ShaderInterop.slang`) gains a `uint8_t recursionDepth` field. Initialised to 0 by raygen; incremented on the reflection ray; capped at `MAX_REFLECTION_DEPTH = 1` (one bounce only).
- Pipeline `maxRecursionDepth` bumped from 2 (v1 shadow rays) to 3 (primary + reflection + reflection's shadow rays).
- Closesthit's reflection block:
  1. Read material `metalness` + `specular` + `roughness` (already there)
  2. Compute reflection direction: `reflect(WorldRayDirection(), nWorld)`. For v2.0: pure mirror direction (perfect specular). GGX-importance-sampled direction is a v2.1 polish if roughness < threshold produces visible artifacts.
  3. **Roughness-aware branching**:
     - `roughness < gRenderSettings.reflectionMaxRoughness` AND material is reflective → fire inline `TraceRay`, get reflection radiance from the secondary closesthit invocation (which runs the same shader recursively, but its own reflection branch is gated by `recursionDepth >= MAX`).
     - `roughness ≥ reflectionMaxRoughness` AND material is reflective → sample dome envmap at the reflection direction. No ray cost; smooth result.
     - Not reflective (`metalness < 0.01 && specular < 0.01`) → skip entirely.
  4. Composite: `payload.color = directDiffuse + directSpecular + emission + reflected * F0`
- **Default `reflectionMaxRoughness = 0.3`** — surfaces rougher than this fall back to the envmap. Tunable per scene.

**Why a hard threshold instead of stochastic GGX sampling at all roughnesses**:

GGX-importance-sampled rays are noisy at high roughness without temporal accumulation or a denoiser. We're avoiding both. So we cut over to envmap-only at the roughness where GGX would start looking grainy. The result: marble + chrome reflect via rays; matte plaster / brushed metal reflect via envmap. Both look smooth.

**Per-pixel cost**: ~2.5-3 ms on lobby at 1080p RTX 4070 (the reflection ray's closesthit also fires per-light shadow rays — that's the dominant cost in the reflected path).

**No new public API beyond `RenderSettings::reflectionMaxRoughness`** (claims the `_reserved3` slot reserved in v1's `RenderSettings`).

**Exit**:
- Lobby's marble floor reflects the ceiling lights + dome via ray-tracing
- Glossy chrome surfaces reflect the room via ray-tracing
- Rough surfaces (plaster, brushed wood) take the envmap fallback — smooth, no noise

### V2.B.4 Performance gate + KPI (M31)

**Hero scene**: World Lobby, 1080p, RTX 4070 Laptop. Visual target: per-pixel direct + shadows + glossy reflections, OVRTX-like at a glance.

**KPI budget**:

| Pass | Budget |
|---|---|
| `pass.PathTrace` (primary + per-light shadow rays + inline reflection ray + reflected hit's shadow rays) | 10 ms |
| `pass.Accumulation + ToneMap + AovResolve` | 1 ms |
| Headroom | 5 ms |
| **Total per frame** | **16 ms = 60 FPS** |

§34's "1080p hero camera" KPI gate is updated for v2: `pass.PathTrace + pass.Accumulation + pass.ToneMap + pass.AovResolve < 16 ms p99` on RTX 4070 Laptop.

The single `pass.PathTrace` covers everything (primary visibility + direct + shadows + inline reflection + reflection's direct + reflection's shadows). No separate reflection pass to budget for.

**Profiling overhead** (M11 gate): < 1% Release. Held over from v1.

**No denoiser, no TAA, no light tree.** The single-frame output is the final image at 1-spp (with Halton jitter + accumulation when the camera is stationary, as v1 already supports).

---

## Part 3 — Milestones

### Loading completeness (Pillar A)

| | Name | Plan ref | Exit |
|---|---|---|---|
| **M12** | Subdivision + visibility + purpose + mesh attr coverage | V2.A.1 + V2.A.2 + V2.A.26 + V2.A.33 | 83 lobby subdiv meshes refined; 4 invisible prims hidden; purpose filter active; orientation + holes + creases honoured; displayColor fallback |
| **M13** | Curves + points + analytic primitives | V2.A.3 + V2.A.21 | `BasisCurves` + `Points` + Sphere/Cube/Cylinder/Cone/Capsule ingest |
| **M14** | xformOps + variants + UDIM + time-varying primvars + PointInstancer extras + stage metadata + prim state + camera extras + light extras + primvar edge cases + bounding-box hints | V2.A.6 + A.2 + A.7 + A.9 + A.13 + A.16 + A.17 + A.19 + A.20 + A.25 + A.31 | Production-pipeline correctness pass (broad batch of small, low-risk additions) |
| **M15** | Volumes (OpenVDB) | V2.A.5 | Smoke fixture renders with absorption + scattering |
| **M16** | NURBS + Skel + time-varying USD | V2.A.4 | Animated character renders, advancing per-frame |
| **M17** | Texture streaming + LRU | V2.A.12 | Oversized-lobby × 10 renders at 8 GB VRAM with degraded mips |
| **M18** | MaterialX coverage matrix + material binding details + texture/shader edge cases + MDL translation + OCIO + material network outputs | V2.A.8 + V2.A.18 + V2.A.24 + V2.A.23 + V2.A.29 + V2.A.30 | Every OPS / StdSurface nodedef fixture-tested; binding strength honoured; wrap/filter modes; MDL OmniPBR/Glass/Surface → OpenPBR; OCIO color management; displacement/volume/light networks loaded |
| **M19** | Asset resolvers + USDZ stress + UsdRender schema + composition fine points | V2.A.10 + V2.A.22 + V2.A.27 | USDZ + custom ArResolver fixtures pass; UsdRenderProduct/Var/Settings honoured |
| **M21** | Texture compression + format coverage + USD composition load modes | V2.A.14 + V2.A.15 | BCn-encoded uploads; DDS / KTX2 / TIFF input; --load-mode + --population-mask |
| **M28** | Loaded-everything audit (schema-walk) + round-trip + dirty fixtures + codeless-schema log | V2.A.28 + V2.A.32 | `_tools/usd_loading_audit.py` returns zero unhandled rows; lobby USD round-trips byte-identical via `usddiff` |

### Real-time raytracer (Pillar B)

Two milestones — scope is tight enough that the pipeline restructure isn't needed. Single-pass throughout.

| | Name | Plan ref | Exit |
|---|---|---|---|
| **M29** | RT specular reflections (inline in closesthit) | V2.B.1 + V2.B.2 + V2.B.3 | Closesthit gains inline `TraceRay` for reflection branch; `HitInfo` carries `recursionDepth`; pipeline `maxRecursionDepth = 3`; new `RenderSettings::reflectionMaxRoughness`. Lobby's marble + chrome ray-trace; rough surfaces envmap-fallback |
| **M30** | Lobby real-time gate | V2.B.4 | Lobby at 60 FPS p99 at 1080p RTX 4070 Laptop with direct + shadows + reflections |

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
- `RenderSettings::reflectionMaxRoughness` (claims `_reserved3`) — above this roughness, blend to envmap instead of tracing
- `RenderSettings::timeCode` (claims `_reserved4`)
- `RenderSettings::ambientStrength` (claims `_reserved5`) — flat ambient term for diffuse indirect approximation
- `OpenPBRMaterialDesc::volumeAbsorption / volumeScattering / volumeAnisotropy` (claims trailing `_reserved` slots)
- New POD: `CurvesDesc`, `PointsDesc`, `VolumeDesc` — entirely new files in `Public/Pyxis/Renderer/Descs/`
- New POD: `TimeSampleSet` (`Public/Pyxis/Renderer/Descs/TimeSampleSet.h`) — generic time-sample container loaded for any animated attribute
- New POD: `SkeletalAnimation`, `JointTransform` for skel
- New Profiler scopes: `pass.ReflectionRays` (only new pass in Pillar B; existing `pass.PathTrace` keeps its name)

All additive. Version bump per v2 cut: 1.0.0 → 2.0.0 only when the **rendering algorithm** changes (M29 lands). M12-M28 are loading-correctness — each bumps minor (1.1.0, 1.2.0, …, 1.x.0). M29+ bumps to 2.0.0.

### V2.4.2 Regression infrastructure

M10 already shipped the harness. v2 adds:

- Per-milestone baseline EXR captures (some scenes will reshape; some will look identical post-subdiv refinement, etc.).
- Per-milestone KPI CSV rows.
- `_tools/perf_compare.py` rolling median window updated to 7-day for the longer v2 milestone cadence.

### V2.4.3 Build matrix

Same Windows / clang-cl / `/W4 /WX` baseline from v1. New vcpkg deps:

- `opensubdiv` (M12)
- `openvdb` (M15)
- `opencolorio` (M18)
- `nvtt` or `Compressonator` (M21 — texture compression)
- `gli` / `tinyddsloader` (M21 — DDS input)
- `libktx` (M21 — KTX2 input)
- `libtiff` (M21 — TIFF input)

No denoiser dep — v2's simplified Pillar B doesn't ship one. (Re-evaluate for v3 if RTAO / multi-bounce lands.)

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

1. **M12 first.** The 83 lobby subdiv meshes are a real silent bug we ship today. Highest leverage per effort. Also bundles mesh attribute coverage (orientation / holes / creases) since those need M12's OpenSubdiv path.
2. **M13 second.** Curves + points + analytic primitives. Common production geometry beyond meshes.
3. **M14 third.** Wide low-risk batch: xformOp coverage + variants + UDIM + time-varying primvars + PointInstancer extras + stage metadata + prim state + camera extras + light extras + primvar edge cases. Each section is small; bundling keeps the milestone shape consistent.
4. **M15 + M16** can run in parallel if there's bandwidth — Volumes (M15) is a new subsystem; NURBS+Skel+TimeVarying (M16) is a different new subsystem. Both touch §22-reserved API slots without colliding.
5. **M17.** Texture streaming. Foundational for any large-scene work; depends on M12-M16 completing so the streaming budget is calibrated against the right working sets.
6. **M18.** Materials wide pass — MaterialX matrix + binding strength + texture/shader edge cases + MDL translation.
7. **M19.** Asset resolvers + USDZ + UsdRender schema + composition fine points. Production-pipeline integration.
8. **M21.** Texture compression. Reads as out-of-order vs the natural M17-M18 sequence, but slotted here because:
   - Compression infrastructure is independent of streaming logic (M17) — they compose but don't depend.
   - Putting compression after M18-M19 means the material translator already routes textures via roles, which is needed to pick the right BCn format per role.
   - Holds the broader Pillar A timeline more cleanly than slotting between M17 and M18.
9. **M28.** Loaded-everything audit + round-trip. The formal completeness gate. Runs after M12-M21 land so the schema-walk script reports against the final handler registry.
10. **Cut 1.x → 2.0.** Tag the loading-completeness milestone. Pillar A done.
11. **M29.** RT specular reflections. The whole Pillar B feature in one milestone: extend the existing closesthit with an inline `TraceRay` for reflection rays, bump pipeline `maxRecursionDepth` 2→3, add `RenderSettings::reflectionMaxRoughness`. Single-pass throughout — no new RenderGraph node, no new AOV.
12. **M30.** Lobby real-time gate at 60 FPS p99. The v2 ship signal.

Estimated calendar: M12-M28 is ~9 months at one milestone per 2-3 weeks (some bundled into M14/M18/M19). M29-M30 is ~3 weeks (Pillar B is one inline closesthit extension + a perf-tuning pass). Total v2: ~10 months from v1.0.0.

---

## Part 6 — What v2 explicitly does NOT do

A reminder, because the §42 list is long and tempting:

| Deferred to v3+ | Why |
|---|---|
| **Multi-bounce indirect diffuse** (path tracing) | v2 has ambient × dome only; indirect bounce is offline / v3 |
| **RTAO** | Out of v2 scope per the simplified Pillar B; flat ambient covers it |
| **Light tree + MIS** | Per-light enumeration runs fine for ≤ 50 lights (lobby + typical arch viz); revisit when scenes need it |
| **Denoiser (OIDN / OptiX)** | v2 produces clean output without it via roughness-blended reflections |
| **TAA / temporal reprojection** | Per-frame independent rendering; v2 doesn't accumulate motion-blurred history |
| **DDGI** / probe systems | v3 |
| **DLSS / DLSS-RR / DLSS3 / Reflex** | NV SDK gates; ship as opt-in v3 maybe |
| **Spectral rendering** | Niche; almost no production USD authors spectral data |
| **OMM (Opacity Micro Maps)** | RTXMU v1 didn't support; revisit if RTXMU adds the feature |
| **DMM (Displacement Micro Maps)** | Same as OMM |
| **Bidirectional path tracing / VCM** | Offline-only |
| **D3D12 backend** | Windows-only via Vulkan in v2 |
| **macOS / Linux ports** | Still §42 |
| **Network / render-farm features** | Never v1/v2 scope |
| **Hair Marschner BSDF** | v2 ships Lambert per-strand; Marschner deferred |
| **Material editor UI** | Separate tool, not in pyxis_app |

If any of these become tempting, file an RFC under §44 first. Don't slip them in mid-milestone.

---

## Part 7 — Success metrics

v2 is done when:

1. **Loading completeness**: `_tools/usd_loading_audit.py` walks a corpus of production USD scenes (lobby + a curated set of public Omniverse / NVIDIA samples) and reports zero unhandled prim types / applied APIs / unhandled-attribute warnings. Codeless-schema types log explicitly; nothing is silently dropped.
2. **Round-trip integrity**: `gpuScene.SaveAsUsd(path)` on the lobby produces a USD whose `usddiff` against the source is empty (modulo metadata reorderings USD's serializer can't avoid).
3. **Real-time KPI**: lobby renders at 60 FPS p99 at 1080p on RTX 4070 Laptop, with: per-pixel direct lighting + per-light shadow rays + per-pixel 1-bounce RT specular reflections.
4. **Visual reference**: side-by-side screenshot vs OVRTX-rendered lobby (https://docs.omniverse.nvidia.com/usd/latest/usd_content_samples/res_lobby.html) reads as "same scene, same lighting intent, different (simpler) renderer" — not "different scene" or "obviously broken". v2 deliberately ships a coarser-quality approximation; the absence of multi-bounce indirect / RTAO / proper denoising is visible at close inspection but the at-a-glance read is similar.
5. **Profiler overhead**: still < 1% Release per M11 / §34 gate.
6. **106/106 + new** tests pass.

That's the v2 ship gate. v1.0.0 closed; v2.0.0 awaits.
