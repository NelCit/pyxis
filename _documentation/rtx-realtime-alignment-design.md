# Design: Align the Pyxis pipeline with Omniverse "RTX – Real-Time"

Owner-directed engineering design note (2026-07-05). Not an RFC — the owner
has waived the §44 process for this work ("no need for RFC go directly").
Builds on `passes-split-design.md` (the RaytracedGBuffer/RaytracedLighting
split) and extends it into the NVIDIA Omniverse RTX Real-Time pass
architecture. Owner constraints: **each CPU/GPU pass = one shader**; verify
against reference renders produced by NVIDIA's own `ovrtx` renderer on the
same USD scenes; stay within quota limits by routing subagent work to
cheaper models.

## What "RTX – Real-Time" actually is (research summary)

Two generations exist (docs.omniverse.nvidia.com, materials-and-rendering):

- **RTX Real-Time (Legacy)** — the classic hybrid: primary visibility, then
  *separate ray-traced passes per lighting term* — direct lighting with
  ray-traced shadows, ray-traced AO, ray-traced indirect diffuse GI,
  ray-traced reflections, ray-traced translucency/SSS — **each pass
  independently denoised** (NRD family: ReLAX/ReBLUR for radiance, SIGMA for
  shadows), then composited, then AA/upscale (DLSS/DLAA/TAA/FXAA), then post
  (auto-exposure → tonemap → color-correction → grading → FFT bloom → CA).
- **RTX Real-Time 2.0** (current default) — a reduced-cost *path tracer*
  (maxBounces=3, radiance cache, Sampled Direct Lighting always on) that
  leans on DLSS Ray Reconstruction (proprietary AI denoise+upscale) instead
  of per-signal denoisers.

**Decision: Pyxis targets the Legacy-hybrid pass decomposition with RT 2.0's
sampling upgrades.** Rationale: (a) it is the architecture NVIDIA actually
documents at pass granularity, (b) Pyxis already has the visibility-buffer +
deferred-lighting split it requires, (c) RT 2.0's look depends on DLSS-RR,
which is closed and license-incompatible (below); the Legacy decomposition
with an NRD-class denoiser reimplementation is the closest reachable point,
and it is exactly the "one signal = one pass = one shader" shape the owner
asked for.

### Licensing (verified 2026-07-05)

NRD, RTXDI, and DLSS are governed by the proprietary **NVIDIA RTX SDKs
License** — §4(e) forbids use "in any manner that would cause it to become
subject to an open source software license". Pyxis is Apache-2.0, therefore:

- **Never vendor or copy** NRD / RTXDI sources or binaries into the
  repo. Use them as *algorithmic references only*; reimplement in our own
  Slang from the published papers (ReSTIR: Bitterli et al. 2020; SVGF:
  Schied et al. 2017; A-SVGF/ReLAX design notes) and public docs.
- **DLSS — corrected stance (owner, 2026-07-05)**: an open-source app MAY
  use DLSS as an *optional proprietary runtime integration*; what's
  forbidden is committing/relicensing SDK materials under Apache-2.0 or
  shipping the SDK standalone. The compliant shape: keep the repo clean of
  proprietary bits, integrate via **Streamline** (NVIDIA's MIT-licensed
  integration layer — headers/interposer are open source), runtime-load
  the proprietary NGX plugin DLLs (`sl.interposer.dll` + `nvngx_dlss*`)
  from outside the repo (user/packager-provided, or a setup script that
  downloads NVIDIA's release under NVIDIA's terms), and always provide the
  built-in fallback (our ReLAX/SIGMA chain + TAA). Renderer policy:
  `RealTimeQuality.denoiser = Dlss` (DEFAULT) `| Builtin | Off`; at
  startup a capability probe (NVIDIA GPU + loadable Streamline + DLSS-RR
  feature available) downgrades Dlss → Builtin with a logged reason and a
  visible indicator. Headless/golden configs pin Builtin (DLSS output is
  not byte-deterministic across drivers; §33.7 stays on the built-in
  path).
- **DLSS scope includes upscaling** (owner, 2026-07-05): Pyxis currently
  has no sub-native rendering (TAA is AA-only, SSAA super-samples). The
  DLSS package introduces the two-resolution pipeline: G-buffer + signal
  passes + guides + composite at `renderResolution`, DLSS-RR reconstructs
  to `displayResolution`, post (AE/tonemap/blit) at display res.
  `dlssExecMode = Auto | Quality | Balanced | Performance | DLAA`
  mirroring `omni:rtx:post:dlss:execMode` (default Auto, like ovrtx).
  Fallback when DLSS is unavailable: native-resolution rendering with the
  builtin chain + TAA (today's pipeline, unchanged — no hand-rolled TAAU
  in v1 of this package). Side benefit: internal res shrinks every
  per-signal RGBA16F target — relieves the 8 GB VRAM budget.
- **RTXGI-DDGI v1.x and Donut are MIT** — safe to port/adapt directly if we
  ever add probe GI (post-phase-D option, not in scope here).
- DLSS is architecturally out; **TAA** is the documented fallback AA mode in
  Omniverse's own settings enum and is what we implement.

### DLSS (optional) — implementation status (2026-07-06)

Implements the two bullets above ("DLSS — corrected stance" /
"DLSS scope includes upscaling") in two stages:

- **Stage 1 (shipped)** — everything that works WITHOUT the Streamline SDK
  physically present: `RenderSettings::RealTimeQuality` grows `denoiser`
  (`DENOISER_DLSS` default / `DENOISER_BUILTIN` / `DENOISER_OFF`) and
  `dlssExecMode` (`Auto`/`Quality`/`Balanced`/`Performance`/`DLAA`, mirrors
  `omni:rtx:post:dlss:execMode`) — a MAJOR version bump (RealTimeQuality's
  own §22.3 reserved tail was already spent by Phase C). `Private/Dlss/
  DlssProvider.{h,cpp}` is the capability-probe scaffold: DLL discovery
  (`PYXIS_DLSS_PATH` env override, else `<exe-dir>/sl.interposer.dll`) +
  resolving the Streamline C entry points Stage 2 will call
  (`slInit`/`slSetFeatureLoaded`/`slIsFeatureSupported`), with NO Streamline
  headers vendored and NO `slInit` call — that needs device interop (Stage
  2). `usable` is therefore unconditionally `false` in Stage 1, even when
  discovery + symbol resolution both succeed ("Stage 2 integration
  pending"). `PyxisRenderer::RenderFrame` resolves `{requested, effective}`
  denoiser every frame, logs the resolution once per change
  ("`denoiser: requested=Dlss effective=Builtin (reason: ...)`"), and masks
  `PASS_MASK_DENOISE`/`PASS_MASK_TAA` out of a local `RenderSettings` copy
  threaded through `PassContext` whenever the effective denoiser isn't
  Builtin (`Off` = explicit; a hypothetical usable `Dlss` = Streamline's own
  reconstruction replaces both) — the existing self-gates in
  `DenoiseShadowPass`/`DenoiseTemporalPass`/`DenoiseHistoryFixPass`/
  `DenoiseAtrousPass`/`TaaPass`/`CompositePass` do the rest, unmodified.
  `_tools/setup_dlss.py` downloads NVIDIA's Streamline release (behind
  `--accept-nvidia-license`) into the untracked `_local/dlss/` directory and
  prints the `PYXIS_DLSS_PATH` / exe-dir staging instructions; the repo
  itself never vendors the binaries (`.gitignore`'s `_local/` entry).
  Headless/golden configs don't author `denoiser`, so they keep resolving
  Dlss → Builtin exactly like every other machine without the SDK staged —
  byte-identical to pre-Stage-1 output, since `passMask`'s denoise/TAA bits
  already default OFF regardless.
- **Stage 2 (not started)** — the actual Streamline hookup: `slInit` against
  the live NVRHI/Vulkan device, the two-resolution pipeline this section's
  "DLSS scope includes upscaling" bullet describes (signal passes +
  composite at `renderResolution`, DLSS-RR reconstructs to
  `displayResolution`, post at display res), and flipping `DlssProvider::
  Availability::usable` to `true` once Streamline reports the feature
  supported on the running GPU/driver. Until Stage 2 lands, `effective=Dlss`
  is unreachable no matter how correctly the Stage 1 DLLs are staged.

## Target pass graph (one pass = one shader file)

Every pass owns exactly one `.slang` file named after it (RT passes carry
all their entry points — raygen/closesthit/miss/anyhit — inside that one
file; iterated passes re-dispatch the same shader). Linear graph (§9 stays
linear):

```
RaytracedGBufferPass        raytraced_gbuffer.slang       (extend: motion vectors, viewZ, normal+roughness for denoisers)
DirectLightingPass          direct_lighting.slang         (RIS sampled lighting, shadow rays, demodulated diff+spec out)
IndirectDiffusePass         indirect_diffuse.slang        (1 spp cosine bounce, maxBounces 2, dome IBL)
AmbientOcclusionPass        ambient_occlusion.slang       (RTAO, rayLength 35, composited into indirect diffuse)
ReflectionsPass             reflections.slang             (GGX 1 spp, maxRoughness 0.3 rough-fallback, 1 bounce)
TranslucencyPass            translucency.slang            (refraction ≤6 bounces, reflect-at-all-bounce)
DenoiseShadowPass           denoise_shadow.slang          (SIGMA-class: spatial-dominant penumbra filter)
DenoiseTemporalPass         denoise_temporal.slang        (ReLAX-class: reproject + accumulate diff+spec, 30-frame history)
DenoiseHistoryFixPass       denoise_history_fix.slang     (disocclusion fix, 3-frame threshold)
DenoiseAtrousPass           denoise_atrous.slang          (edge-stopping à-trous, 5 iterations ping-pong)
CompositePass               composite.slang               (albedo×diffuse + Fenv×spec + emissive + translucency)
TaaPass                     taa.slang                     (jittered history, neighborhood clamp; SSAA remains headless-determinism path)
AutoExposurePass            auto_exposure.slang           (histogram median metering, EV100 clamp, adaptation speed)
TonemapPass                 tonemap.slang                 (operator enum: Clamp/Linear/Reinhard/ModReinhard/HejiHableAlu/HableUC2/ACES/IrayRef)
BloomPass                   bloom.slang                   (phase D, off by default)
BlitToSrgbPass              blit_to_srgb.slang            (unchanged)
```

`DebugView` / AOV-resolve behavior is preserved where it lives today.

### Full shader-rewrite mandate (owner note, 2026-07-05)

The owner has authorized: **rewrite all shaders, reorder all bindings, pack
structured buffers**. No legacy-slot compatibility is owed. The rewrite
adopts a clean two-set layout instead of today's flat 0–34 single-set table:

- **Set 0 — SceneBindings** (one immutable NVRHI layout, built once, shared
  verbatim by every RT pass): camera cbuffer, TLAS, materials, instance
  info, mesh info, lights, pooled geometry (indices / UVs / vertex attribs /
  face normals), dome env map + samplers, bindless texture array.
- **Set 1 — per-pass I/O** (small, pass-owned): that pass's input SRVs
  (visibility, G-buffer guides, prior signals) and output UAVs (its signal).

Packing candidates taken with the mandate:
- `VisibilityGpu` 32 B → 16 B: `{float hitT; uint baryPacked(2×f16);
  uint instanceId; uint primitiveIndex}` — halves visibility-buffer
  bandwidth/VRAM; bary at f16 is a sub-golden-tolerance change absorbed by
  the Phase A rebake.
- G-buffer guides as textures (2D locality for denoisers), not structured
  buffers: `gNormalRoughness` RGBA16F (world normal + linear roughness),
  `gViewZ` R32F, `gMotionVector` RG16F, `gAlbedo`/`gSpecularF0` RGBA16F,
  `gEmissive` RGBA16F (material G-buffer — evaluated once in the G-buffer
  pass, consumed by every signal pass instead of re-evaluating materials
  per pass).
- Every pass = exactly one `.slang` file carrying all its entry points
  (raygen/closesthit/miss/anyhit for RT passes; single compute entry for
  compute passes); the existing per-stage file split
  (`raytraced_lighting_{raygen,closesthit,...}.slang`) is retired during
  the Phase A rewrite.

## Authoritative defaults — extracted from ovrtx 0.3.0's own USD schema

`ovrtx/bin/usd_plugins/rtx_settings/generatedSchema.usda` (the renderer's
applied-API-schema definitions) settles every default the web docs left
open. These are the values NVIDIA's shipping renderer actually uses:

- **Tonemap operator default = `acesApproximation`** ("the old RTX 'ACES'
  tonemapper, an approximation of the ACES RRT") — Pyxis's existing
  Narkowicz ACES fit is already the right default operator. Full enum:
  `raw, none, reinhard, modifiedReinhard, hejiHableAlu, hableUc2,
  acesApproximation, iray`. Iray sub-params: burnHighlights 0.7 (per
  component), crushBlacks 0.5, saturation 1.
- **Camera exposure model** (OmniRtxCameraExposureAPI): fStop 5, iso 100,
  time 1 (legacy carb default 0.02), responsivity 1 — with the note "RTX
  needs 0.8821367311933349 to match the USD result". This photographic
  divisor is why radiometrically-authored Pyxis fixtures render nearly
  black in ovrtx (measured: m7 fixture, distant intensity 3 → HDR max
  ≈0.003). **Matching Omniverse's brightness = adopting this imaging
  equation**, not tweaking our exposure default.
- **Auto exposure** (OmniRtxCameraAutoExposureAPI): disabled by default;
  when on: histogram-based, filter = `median` (vs average), adaptation
  speed (tau) 3.5, histogram luminance clamp [50, 100000], white point
  scale 10.
- **RT mode** (OmniRtxSettingsRtAPI/RtAdvancedAPI): directLighting
  samples=2, maxRayUnexposedIntensity=6400 (exposure-scaled), sampled
  lighting replaces an **LTC (linearly-transformed-cosine) analytic
  area-light approximation** (that is the non-sampled fallback — not a
  naive light loop); ris:meshLights=false; AO enabled, rayLength=35 **cm**;
  indirectDiffuse enabled, samples=1, maxBounces=2, intensity=1,
  clamp=6400; reflections enabled, samples=1, maxBounces=1,
  roughnessCacheThreshold=0.3, clamp=19200; refractions enabled,
  maxBounces=6, clamp=19200, depthCorrection=true, motionCorrection=true,
  reflectionInRefraction=false, roughnessSampling=false; sss disabled
  (samples=32 when on); ambientLight color (0,0,0) intensity 0; ecoMode
  enabled, maxFramesWithoutChange=500; fractionalOpacity=false; DLSS
  execMode="auto", frameGeneration=false; caustics disabled (photon pass,
  filterIterations=5).

## Technique specs and stock-look defaults (from NVIDIA docs/SDK headers)

| Area | Spec | Default (Omniverse stock) |
|---|---|---|
| Sampled direct lighting | RIS candidate resampling over the light list (ReSTIR DI lineage); 1 shadow ray to the winning candidate's sampled point | samplesPerPixel=2 (Omniverse) / 4 candidates (Remix analog); maxRayIntensity=6400 |
| Shadows | Ray-traced visibility to a sampled point on the light's surface — soft shadows fall out of area sampling; no shadow maps | shadowBias=0.001; SIGMA-class denoise |
| Reflections | GGX importance-sampled, 1 spp; surfaces rougher than threshold fall back to approximated/cached term | maxRoughness=0.3; maxReflectionBounces=1; clamp=19200 |
| Translucency | True refraction; reflection evaluated at every refraction bounce | maxRefractionBounces=6; reflectAtAllBounce=true |
| Indirect diffuse | 1 spp/frame ray-traced bounce, temporally amortized | enabled; fetchSampleCount=1; maxBounces=2; scalingFactor=1.0 |
| RTAO | Long-radius hemispherical occlusion (not contact-only) | enabled; rayLength=35 scene units; adaptive 3–9 spp |
| Ambient fill | Present but zero by default | ambientLightIntensity=0.0, color (0.1,0.1,0.1) |
| Denoisers (reimplemented) | ReLAX-class for diff+spec: temporal accumulation + history fix + 5× à-trous; SIGMA-class for shadows: spatial-dominant | NRD defaults as targets: maxAccumulatedFrameNum=30, fast history 6, historyFixFrameNum=3, atrousIterationNum=5, depthThreshold=0.003, diffPhiLuminance=2.0, specPhiLuminance=1.0 |
| AA | TAA (DLSS/DLAA slots reserved in enum, unimplemented) | Omniverse default = DLSS; ours = TAA (closest reachable) |
| Auto exposure | Log-luminance histogram, median filter, EV100 clamp, temporal adaptation | adaptation speed 5.0, EV100 range [-2, 5] (Remix analog; Omniverse numbers unpublished — calibrate against ovrtx captures) |
| Tonemap | Operator set above; factory default undocumented (evidence: simple operator, Iray/ACES are opt-in upgrades) | **Calibrate default against ovrtx captures**; keep current ACES as fallback |
| Firefly clamp | Per-signal maxRayIntensity clamps (not one global) | direct 6400, reflections/translucency 19200 |
| Eco mode | Stop re-accumulating after N unchanged frames | ~500 frames (viewer only) |

## Buffer contract (denoiser guides — NRD-shaped, our own packing)

New G-buffer outputs from `RaytracedGBufferPass` (all fp precision chosen per
determinism section):

- `gMotionVectors` (RG16F) — screen-space; camera motion now, instance motion
  via the §43.2 reserved `worldFromLocalPrev` when animation lands.
- `gViewZ` (R32F) — view-space depth of primary hit.
- `gNormalRoughness` (RGB10A2 or RGBA16F) — world normal + linear roughness.
- Signals are **demodulated** before denoising (radiance / albedo) and
  re-modulated in `CompositePass` — the standard NRD discipline, required
  for texture detail to survive filtering.

## WP2 implementation contract (Phase A signal split — frozen before fan-out)

**Pass files and entry points** (one file per pass, all entries inside):

| Pass (class, `Private/Passes/`) | Shader file | Entries | Set-1 outputs |
|---|---|---|---|
| RaytracedGBufferPass (extended) | `raytraced_gbuffer.slang` | RayGen/ClosestHit/Miss/AnyHit | visibility (16 B packed), gAlbedo, gNormalRoughness, gEmissive, gViewZ, gMotionVector, id/pick AOVs (picker moves here) |
| DirectLightingPass | `direct_lighting.slang` | RayGen, ShadowMiss | gDirectDiffuse, gDirectSpecular (demodulated) |
| IndirectDiffusePass | `indirect_diffuse.slang` | RayGen, ClosestHit, Miss | gIndirectDiffuse (demodulated; Phase A = dome-mip ambient, Phase B = 1 spp cosine bounce) |
| AmbientOcclusionPass | `ambient_occlusion.slang` | RayGen, Miss | gAo (R16F) |
| ReflectionsPass | `reflections.slang` | RayGen, ClosestHit, Miss | gSpecularIndirect + hitT (RTX-RT semantics: 1 bounce, direct+env at hit, roughness>0.3 → env fallback) |
| TranslucencyPass | `translucency.slang` | RayGen, ClosestHit, Miss, AnyHit | gTranslucency + coverage (ports the segment walk) |
| CompositePass | `composite.slang` | CS | gLinearColor = albedo·(dirDiff + indirDiff·ao) + dirSpec + specIndirect + emissive, translucency blended over |

The `raytraced_lighting_*` megakernel family is **retired**. Signal targets
are RGBA16F (VRAM matters on 8 GB); the one-time golden rebake absorbs the
quantization. Reflections intentionally change look (single-bounce
direct+env at hit — matches RTX-RT maxBounces=1) rather than replicating
today's recursive mirror.

**Binding sets**: Set 0 = SceneBindings, one immutable layout shared by all
RT passes, owned by a new `Private/Passes/SceneBindings.{h,cpp}` helper —
`0` CameraUniforms (grown: adds prevClipFromWorld for MV; rewrite mandate
covers the size change), `1` TLAS, `2` materials, `3` instance info,
`4` mesh info, `5` lights, `6` index pool, `7` UVs, `8` vertex attribs,
`9` face normals, `10` dome env, `11` dome sampler, `12` material sampler,
`13` bindless textures[4096]. Set 1 = per-pass I/O only.

**RenderSettings**: grows a nested `RealTimeQuality` POD mirroring
`omni:rtx:rt:*` 1:1 (directSamples 2, indirectSamples 1 / bounces 2,
reflectionSamples 1 / maxRoughness 0.3, refractionBounces 6, aoRayLengthCm
35, per-signal maxRayIntensity 6400/6400/19200, passMask all-on, ecoMode
500, tonemapOperator = AcesApproximation, auto-exposure block) + a fresh
`_reserved` tail. Public-POD growth beyond the 2 remaining reserved slots ⇒
version.txt 2.0.0 → 3.0.0 (§22 MAJOR; owner-approved).

## Phase B implementation spec (frozen for fan-out)

All stochastic sampling uses the §12 PCG32 streams seeded from
`render.seed` + frameIndex + pixel — deterministic for fixed seed+frame.

- **Sampled direct lighting** (`direct_lighting.slang` estimator swap): per
  pixel draw `directSamples` (default 2) RIS candidates — uniform light pick
  × unshadowed contribution weight (Talbot RIS), one shadow ray to the
  winner's *sampled surface point* (rect: uniform area; sphere: solid-angle;
  distant: cone of angular size; dome: excluded, stays in indirect). Soft
  shadows fall out of area sampling. Clamp per-sample contribution to
  `maxRayIntensityDirect` (6400, exposure-scaled). LTC fallback is post-v1;
  below-threshold pixels keep the analytic centroid evaluation.
- **Indirect diffuse** (estimator swap): 1 cosine-hemisphere bounce ray;
  at hit evaluate direct light (1 RIS sample) + emissive; miss → dome.
  maxBounces 2 = one extra recursive bounce at half weight. Clamp 6400.
  Demodulated output. Replaces the dome-mip trick.
- **Reflections** (estimator swap): GGX VNDF sample of the specular lobe
  (roughness < reflectionMaxRoughness), else keep env-mip fallback.
  Clamp 19200. Output .a = hitT (NRD-style normalized later).
- **Denoiser chain** (new passes, one shader each; NRD defaults as targets):
  1. `denoise_shadow.slang` — SIGMA-class: penumbra-width-aware spatial
     blur of the direct-light shadow term guided by occluder hitT, 2 passes.
  2. `denoise_temporal.slang` — ReLAX-class temporal: reproject prev
     accumulation via gMotionVector, disocclusion test (viewZ plane-distance
     0.003 + normal dot 0.5), exponential accumulation capped at 30 frames
     (fast history 6), luminance clamp σ≈2 vs fast history. Diffuse +
     specular in one dispatch (two texture pairs).
  3. `denoise_history_fix.slang` — for pixels with history < 3 frames,
     widen spatial support (stride 14→1) to fill disocclusions.
  4. `denoise_atrous.slang` — 5 à-trous iterations (ping-pong, same
     shader), edge-stopping: luminance φ 2.0 (diffuse)/1.0 (spec) scaled by
     variance, normal power ~8-32, viewZ plane-distance 0.003. Roughness-
     aware kernel for specular.
  Denoisers run between the signal passes and Composite; passMask bit 5
  (denoise) gates them; headless default ON with fixed frame budget.
- **TAA** (`taa.slang`, after Tonemap? No — before Tonemap on linear color,
  matching NRD guidance): camera jitter from the §12.4 sequence (wire
  jitter into CameraUniforms + G-buffer ray gen), history reproject via
  gMotionVector, 3×3 neighborhood min/max clamp, blend α=0.1. SSAA remains
  the headless-determinism alternative; `taaEnabled` in RealTimeQuality
  (viewer default ON, headless default OFF until Phase C calibration).
- **Eco mode**: stop re-rendering after `ecoModeFrames` (500) unchanged
  frames (viewer only; camera/scene change resets).

## Noise-floor + vegetation spec (frozen; launches after RR lands)

Owner reports (2026-07-06): vegetation reads as having no self-shadowing;
final render still too noisy. Diagnosis: the occlusion SIGNALS are present
(AO AOV shows dark plant interiors; opaque leaves occlude shadow rays; no
backface culling on occlusion rays) — the presentation chain loses them:
all foliage shares ONE material so the denoisers' materialId edge-stop
gives no intra-plant protection, normals/viewZ are chaotic at blade scale,
so history-fix/à-trous flatten plant interiors to their mean while still
leaving speckle (the known 40%-vs-25% floor). Work items, builtin chain:
1. Denoise AO (currently raw) — cheapest occlusion-variant filter.
2. ReLAX dual history (fast+slow) instead of the single accumulator;
   luminance-variance-guided à-trous φ (currently fixed).
3. Adaptive TAA blend (α from history confidence, not fixed 0.1).
4. Thin-geometry handling: when temporal reprojection rejects on
   depth/normal at sub-pixel geometry, prefer fast-history over the
   history-fix wide blur (stride-14 taps are what flatten plants).
5. Evaluate RR's effect on vegetation specifically (neural denoise should
   preserve blade-scale shading) — RR may make items 2-4 viewer-optional.
Measure: planter crops (raw signal vs denoised vs NVIDIA), converged RMSE
(baseline 0.2150), and the noise proxy (single-frame ratio target ≤25%).

## KEY FINDING (2026-07-06): no true accumulation buffer — top lever for <0.05

Denoiser diagnostic proved: `accumulationFrames` does NOT accumulate the
path trace — denoise-OFF is flat (0% noise reduction) across N=1..64. The
ONLY temporal averaging is the denoiser's EMA history, capped at
maxAccumFrames=30(slow)/6(fast)/32(AO). So the "converged 96f" render
plateaus at the denoiser EMA floor (~frame 30); our 0.0303 noise proxy is
that CAP, not a real convergence (ovrtx neural floor 0.0153). Failure mode
is pure high-freq SPECKLE (under-denoise, 1-spp NEE fireflies), NOT
over-blur — detail intact, NRD constants already match published defaults.

**Fix = add a true progressive-accumulation buffer** (plan §9 AccumulationPass,
deferred in the rewrite): headless/quality mode averages the RAW (pre-denoise)
composite radiance across accumulationFrames → unbiased MC estimate converging
to the true path-traced image. Effects: (1) removes the noise component of
RMSE (currently conflated with structural error); (2) de-confounds all other
fix measurements (noise currently pollutes every material/GI number); (3)
enables "closer than them" — unbounded convergence goes below ovrtx's
real-time neural-denoiser floor. Sits after Composite (raw path), before
AutoExposure; reset on camera/settings change; static-camera+fixed-seed
headless is deterministic. Touches a new AccumulationPass + PyxisRenderer
registration (sequence to avoid the in-flight reflection/floor agents on
PyxisRenderer.cpp). Denoiser real-time sub-levers (SIGMA temporal state,
foliage coherence, adaptive à-trous phi, firefly clamp) are SECONDARY —
they help the real-time few-frame viewer, not the converged 0.05 metric.

**AccumulationPass shipped (2026-07-06).** New compute pass
(accumulate.slang, Private/Passes/AccumulationPass.{h,cpp}) with a
persistent RGBA32F running-mean buffer, registered between CompositePass
and DlssPass. Design choice per the recommendation above: accumulates the
RAW composite — when `RealTimeQuality::passMask` bit 7
(`PASS_MASK_ACCUMULATE`, ShaderInterop.slang, driven off the EXISTING
passMask config field — no RenderSettings POD change) is set,
`PyxisRenderer::RenderFrame` unconditionally force-clears the
DENOISE/TAA passMask bits on its local settings copy (same "local copy,
single gate" shape the DLSS force-masking already uses), so the
denoiser's own EMA and TAA's own history never double-average against
this pass. Bit clear (every existing config) → AccumulationPass is a
pure passthrough; verified byte-identical two ways: (a) code inspection
— `Execute()`'s first branch returns before touching any resource when
inactive; (b) same config rendered twice with the post-change binary
is bit-exact (max abs diff 0.0). `PyxisRenderer::ResetAccumulation()`
(a long-reserved §18.6 stub) now actually resets the running-mean
counter; also resets on a resolution change and an inactive→active
transition.

Measured (World Lobby, CamLobbyWide, 1920×1080, seed 42, denoiser=builtin
baseline reproduced at RMSE 0.177053 ≈ the 0.1771 scoreboard entry —
methodology validated):

| N | accum-mode RMSE vs ovrtx | accum-mode hf (noise proxy) |
|---|---|---|
| 1 | 0.3645 (= denoise-off single frame, unchanged) | 0.2283 |
| 16 | 0.2080 | 0.0928 |
| 32 | 0.1963 | 0.0719 |
| 64 | 0.1901 | 0.0573 |
| 96 | 0.1880 | 0.0510 |
| 200 | 0.1858 | 0.0427 |

Both RMSE and hf decrease monotonically with N — the core bug (denoise-OFF
flat across N, reconfirmed here as a control: hf 0.22835→0.22902 for
N=1→96 with the new bit NOT set) is fixed; accumulation now does
real work. Fitting hf ≈ a + b/√N (R² ≈ 1 across N=16/32/96/200) gives an
extrapolated N→∞ floor **a ≈ 0.023, well BELOW the denoiser's 0.038**
(same scene/res) — confirming the denoiser's EMA cap, not the scene
signal, is the noise floor being removed. However convergence is the
classic MC 1/√N rate: RMSE has NOT yet crossed below the denoiser-EMA
baseline (0.186 @ N=200 vs 0.177 baseline) — extrapolating the hf fit,
the crossover is ≈N=340 for the noise proxy alone, and the RMSE crossover
(bias+noise combined) needs low thousands of frames given the residual
noise-only component shrinks slowly. "Closer than them" via raw
accumulation is a real, working lever, but not a free win at
accumulationFrames=96 — it needs either many more frames (quality/offline
mode) or pairing with the denoiser sub-levers above for the interactive
range. Cost: one extra RGBA32F copy + lerp dispatch per frame when active
(~free relative to path-trace cost); zero cost when inactive.

## Determinism & regression strategy (the hard constraint)

Stochastic sampling + temporal passes change every golden image. Strategy:

- All new stochastic passes consume the existing deterministic PCG32 stream
  (§12) seeded by `render.seed` + frameIndex — headless renders remain
  bit-reproducible for a fixed frame count.
- Headless default: denoisers and TAA run with a **fixed frame budget**
  (accumulate N frames, then write) so EXRs are deterministic; SSAA path
  retained for byte-equal golden lineage.
- The 121 PNG goldens + EXR baselines are rebaked once per phase gate in
  clearly-labeled commits (precedent: #64 World Lobby rebake), with
  side-by-side grids in the PR.
- New nightly artifact: RMSE/SSIM comparison against pinned `ovrtx`
  reference captures for the shared scenes (default, m5/m6/m7 fixtures,
  World Lobby hero camera) — "closeness to NVIDIA" becomes a measured KPI,
  not an opinion.

## Phasing (each phase is a green checkpoint)

- **A — Signal split + guides**: G-buffer adds MV/viewZ/normal-roughness;
  lighting splits into DirectLighting / IndirectDiffuse / Reflections /
  Translucency passes with demodulated outputs; CompositePass recombines.
  Image ≈ unchanged (same estimators, new plumbing). Goldens rebaked once.
- **B — Sampled lighting + temporal**: RIS sampled direct lighting;
  per-signal firefly clamps; TAA; ReLAX-class denoiser chain (temporal +
  history fix + à-trous); SIGMA-class shadow filter; AO pass.
- **C — Look calibration**: tonemap operator set + histogram auto-exposure;
  defaults tuned by numeric comparison against ovrtx captures of the same
  scenes; World Lobby side-by-side within agreed RMSE.
- **D — Extras**: ReSTIR temporal/spatial reservoir reuse, FFT bloom,
  chromatic aberration, eco mode. Optional DDGI (MIT) if indirect quality
  needs it.

## Alternatives considered

1. **Chase RT 2.0 exactly** (unified reduced-cost PT + AI denoise) —
   rejected: its look is inseparable from DLSS Ray Reconstruction, which is
   closed-source, NGX-runtime-bound, and license-incompatible with an
   Apache-2.0 tree.
2. **Integrate NRD/RTXDI as binaries** — rejected: NVIDIA RTX SDKs License
   §4(e) (no OSS contamination) conflicts with Apache-2.0; also forces NRI
   or native-VK glue outside NVRHI.
3. **OIDN/OptiX denoiser** (per §42's original deferral) — rejected for the
   real-time path: both are offline-oriented (OptiX = CUDA interop; OIDN =
   CPU/SYCL) and neither matches the per-signal NRD look that defines RTX
   Real-Time.

## Drawbacks / risks

- Golden churn: every phase rebakes; mitigated by per-phase gates + ovrtx
  RMSE tracking.
- VRAM: +~40 B/px of new per-signal + history targets at render res —
  significant on the 8 GB target with SSAA; TAA replaces SSAA in viewer to
  claw back the 4× dispatch area.
- Temporal artifacts (ghosting/disocclusion) are a new failure class; the
  history-fix pass and validation debug views mitigate.
- A hand-rolled ReLAX/SIGMA will not equal NRD quality on day one; the NRD
  default constants are targets, and the ovrtx RMSE metric tells us how far
  we are.

## Scoreboard (World Lobby / CamLobbyWide vs ovrtx-RT, display RMSE)

| Milestone | RMSE | mean (ref 0.487) |
|---|---|---|
| Megakernel baseline (pre-effort) | 0.386 | 0.248 |
| Phase A composite (placeholder estimators) | 0.4715 | 0.173 |
| Phase B estimators + denoisers | 0.4806 | 0.178 |
| + dome-IBL direct + GI through glass (B.2) | 0.4243 | 0.237 |
| + texture-less dome = color×intensity (UsdLux/ovrtx parity) | 0.3969 | 0.395 |
| + temporal accumulation fixed (NVRHI maxVersions starvation), N=96 | 0.3884 | 0.392 |
| + exposure calibrated (imaging eq, time=0.02 ⇒ 2^-10.5) | 0.3664 | 0.482 |
| + MDL Albedo Adjustments (brightness/desat/add) + glass albedo fix — top-14 albedo damage −83% | 0.3696 @ fixed exposure / 0.3176 level-matched | 0.295 / 0.481 |
| + 2nd GI bounce (ovrtx default maxBounces=2) | 0.3636 | 0.304 |
| + unexposed-intensity clamp semantics (cap = setting/exposureScale — was truncating the 12000-radiance dome at 6400) + dome-NEE at bounce hits | 0.3161 | 0.428 |
| + **aspect-ratio conform** (owner-spotted FOV mismatch: ovrtx conforms horizontal-aperture-wins to 16:9, Pyxis rendered the full authored 1.37:1 vertical FOV; measured sy=1.30 sx=1.00, fixed in SceneBindings by scaling projection row 1) | **0.2476** | **0.471** |

| + halo fix: demodulation floor 0.04 (round-trip exact) + materialId edge-stop in shadow/history-fix/à-trous denoisers (history-fix had NONE at stride ≤14 — dominant leak via jitter-disocclusion at silhouettes) | **0.2318** | — |

| + materials round 2: ORM influence weights honored (metallic_texture_influence=0 was ignored — chrome tables), .mdl preset-file parsing in the classifier (131→152 Mdl, 0 fallback-grey), Clear_Glass transmissive | **0.2152** | 0.502 (first overshoot) |

Round-2 notes: OpenPBRMaterialDesc consumed 3 more reserved slots
(influence weights + aoToDiffuse), GPU row 15 (240→256 B). Foliage solid
leaves = authored data (no opacity maps exist in the scene), not a bug.
Debt: Plaster_Wall_Cracked (bespoke vMaterials df:: graph — 0.05 vs ovrtx
0.50, biggest single remaining material), Isabelle lamp Shade untextured,
metals/glass rows in the forensics table are a methodology artifact
(diffuse-only reference AOV vs our raw-albedo diagnostic).

| + DLSS Stage 2a (Streamline SR live: 1280×720 → 1080p, ~28% faster frame) | 0.2181 @ DLSS vs 0.2183 builtin (statistically equal) | — |
| + gradient-footprint clamp (64-texel) in SampleByGrad — legit hardening, no-op for continuous UVs; cleaned the vase's LEFT half only | **0.2152** (unchanged full-frame) | — |

| + **USD st V-flip** (StToImageUv at the bindless sample boundary): Pyxis sampled raw `st` against top-row-first image uploads — spec requires the bottom-left→image flip. Invisible on tileable content, catastrophic on bake atlases (islands land in dilation gutters = the Seahorn stripes). Verified: iso front+back clean, Lobby vase coherent, matches ovrtx | **0.2150** | 0.504 |

| + Plaster/UsdPreviewSurface-fallback fix (texture × disconnected 0.18 placeholder tint — general bug) | **0.2145** | — |
| + noise-floor + veg (AO denoise, dual-history, adaptive TAA) | 0.2135 | 0.502 |
| + **traced rough reflections** (ceiling 0.3→0.6: indoor glossy surfaces trace the real dark interior via GGX-VNDF instead of reflecting the bright sky-dome) | **0.1826** | — |
| + **occlusion-aware ambient at reflection ClosestHit** (re-enable EvaluateDomeAmbient in reflections.slang, gated by a short per-hit cosine-hemisphere AO ray; tables id40 mean 0.099→0.144 vs ovrtx 0.322, panel id7 mean 0.276→0.299 vs 0.561) | **0.1771** | 0.474 |

Traced-reflections notes (measured 2026-07-06): tables 2.1× too bright →
FIXED (now slightly too dark — reflected interior lacks ambient fill;
occlusion-aware ambient at reflection ClosestHit is the follow-up). Floor
UNCHANGED — its gap is material-translation (marble inlay), not reflection.
materialId 7 door panel minor regress (same too-dark cause). GPU flat
(~3 ms pass.Reflections). Fallback disable (1.01) bought only ~1% more, so
0.6 ships. The dome-ambient-in-reflection experiment that regressed earlier
is DIFFERENT from the wanted occlusion-aware term — blanket unoccluded
ambient over-brightens; the follow-up must gate on an AO ray.

Occlusion-aware-ambient notes (measured 2026-07-06, follow-up to the above):
reflections.slang ClosestHitMain now fires ONE short cosine-hemisphere AO
ray (TMax = gQuality.aoRayLength, transmissive-passthrough gate) from the
reflection hit and passes its visibility into EvaluateDomeAmbient — so
enclosed reflected surfaces get little fill and open ones get full, exactly
the term the earlier BLANKET (aoVisibility=1.0) version lacked (that one
regressed 0.2135→0.2167). Whole-frame 0.1826→0.1771 (−0.0055, no trade).
Per-material sweep: tables id40 local RMSE 0.314→0.282, door id7 0.311→0.291,
plus 8+ other reflective interior surfaces improved (id2 −0.026, id27 −0.015,
id24 −0.014). Three surfaces regressed slightly (id29 dark table + reflective
floor +0.028, id5 curtain-wall glass +0.022, id6 window frame +0.010) — all
reflect the now-brighter interior and are separately too-bright-vs-ovrtx
already (material-translation gaps, not this term); net frame still improves.
Only reflected surfaces changed (59% of pixels carry a reflection signal;
non-reflected materials byte-unchanged — this only touches the reflection
CH). GPU: pass.Reflections p50 6.32→6.6 ms (1080p, 1 reflection spp; one
extra AO ray per reflection hit; the earlier "~3 ms" figure predates the
GGX-VNDF spp path). Pipeline maxRecursionDepth 1→2 (RayGen reflection ray =
level 1, ClosestHit AO ray = level 2); new HitGroupAo/AoMissMain SBT records.
Noise clean at 96f (AO-gated ambient is low-variance — the dome term is a
smooth multiplier, not a high-freq NEE draw). DEBT: one-bounce GI at the
reflected hit not implemented (needs recursion level 3-4 for a second-order
effect on an already-dome-lit signal); dome-ambient×AO is the pragmatic term.

DLSS Stage 2b status (2026-07-06): Ray Reconstruction implemented
end-to-end (gSpecularAlbedo via EnvBRDFApprox2, EvaluateRR, graceful
RR→SR+builtin→native ladder) but ENVIRONMENT-BLOCKED here: loading
kFeatureDLSS_RR pre-device triggers an unbounded nvngx_update.exe retry
storm (NGX 0xbad00005 self-heal loop; no network/model cache). Feature
load safety-reverted + documented in DlssDeviceExtensions.cpp. Validate
RR on a machine with a warm NGX cache or newer Streamline. The builtin
chain is therefore the only user-facing denoiser on this machine.

Seahorn saga final record (three wrong theories, one real bug, owner
caught two over-claims): wrap modes (nil — UVs never leave [0,1]),
BCn/aliasing/mip (nil — bypass-insensitive), gradient footprint (partial —
the 64-texel clamp is kept as legit hardening), network selection (nil —
textures near-identical), ingest stream (nil — byte-exact vs pxr),
instance rotation (nil — ovrtx back view is clean). Root cause: the
missing st→image V flip, falsified/confirmed by a 1-line flip experiment
turning the striped back view into ovrtx's exact glaze. Fix lives in the
three SampleBy* helpers AFTER the MDL UV transforms (MDL's own transform
space is bottom-left); gradient V components sign-flip accordingly.
GOLDEN IMPACT: every textured fixture rebakes (tracked debt).

Post-halo residual (8×-diff attribution): denoiser speckle, tabletop
material (dark lacquer vs chrome-white — metals debt), floor gloss extent,
right-side pot texture (owner-reported, diagnosis in flight). Halo side
finds: albedo<1e-4 was force-zeroed (energy-loss bug, fixed — dark
fixtures brighten slightly even denoise-off); TAA raw-HDR clamp = ringing
debt at emissive edges; DenoiseTemporalPass still lacks materialId
edge-stop (empirically clean today).
Conform note: every golden fixture whose camera aperture aspect ≠ its render
aspect changes — rebake owed (the 256×256 suite renders 1.37:1 film backs at
1:1).

Materials chapter (2026-07-05): ovrtx `DiffuseAlbedoSD` is LINEAR u8
(calibrated against vendored .mdl formulas). Root causes were translation
math, not texture binding: OmniPBR's Albedo Adjustments stage
(`tex·brightness + add`, desaturate toward Rec.709 luma, then tint) was
dropped entirely (OakDark authors brightness 0.26, Modern-White 0.07);
Tinted_Glass (396 k px, the "dark floor inlay") fell through to the
0.18-grey albedo fallback. Fields consume 3 OpenPBRMaterialDesc reserved
slots + GPU row 14. The 0.3696-vs-0.3176 gap at fixed exposure is MISSING
INTERIOR GI ENERGY (~+1.5 stops), not exposure error — indirect diffuse
ships 1 bounce vs the RTX-RT default 2 (Phase B debt); the second bounce
is the next lever, exposure stays at the physically-verified constant.

vs ovrtx-PT ground truth at the same calibration: 0.3661. Level now matches;
the residual is structural: (1) MDL material-translation fidelity — wall wood
renders orange-brown vs beige, round tables chrome-white vs dark lacquer,
floor marble inlay missing (M9-class per-material work, next frontier);
(2) denoiser floor — hand-rolled chain converges to ~40% of single-frame
noise vs the ≤25% target (AO undenoised, TAA α fixed at 0.1 — spec change);
(3) reflection gloss quality. Sharp edge for new passes: NVRHI volatile CBs
silently rebind version 0 when maxVersions is exceeded by unthrottled
back-to-back frames — size maxVersions ≥ 512/writes-per-frame (bitten twice).

## Quantitative goal (owner: "be closer than them")

On World Lobby / CamLobbyWide, NVIDIA's RT mode measures **RMSE 0.0124**
(MAE 0.0073, display space) against NVIDIA's own converged PT ground truth.
Success metric for this effort, in order:
1. Pyxis vs ovrtx-RT RMSE: 0.386 today → target < 0.05 ("really close").
2. Pyxis vs ovrtx-PT-truth RMSE < 0.0124 → Pyxis is *closer to ground truth
   than NVIDIA's real-time mode* ("closer than them"), achievable because we
   can spend more samples/frames than their 60 Hz budget when quality mode
   is requested.

## Measured gap — World Lobby, matched camera (2026-07-05 baseline)

ovrtx 0.3.0 (RTX Real-Time defaults, 128 frames, `/World/Cameras/CamLobbyWide`
— the camera Pyxis's StageWalker auto-picks; note `--camera`/`scene.camera`
are parsed-but-ignored stubs today, implement the override in WP2-final)
vs current Pyxis: RMSE 0.386, display-space mean 0.487 vs 0.248. Visual
attribution: (1) missing indirect diffuse GI — NVIDIA's ceiling/walls are
bounce-lit, Pyxis's are near-black; (2) exposure model; (3) floor gloss —
roughness-cutoff glossy vs deterministic mirror; (4) emissive luminaires
render dimmer. Reference artifacts: scratchpad `ovrtx_ref/` (LdrColor PNG,
HdrColor npz, DepthSD/NormalSD/DiffuseAlbedoSD; RT-mode wide + reception
cameras, plus a 256-frame converged PT-mode capture of the wide camera —
NVIDIA's own ground-truth mode). Per-signal AOVs (DirectDiffuse/…/Pt*) are
NOT produced by ovrtx 0.3 regardless of the `omni:rtx:pt:*AOV` flags or
sourceName spelling (verified empirically, both RT and PT modes) — Phase B
calibrates against final images + the depth/normal/albedo guides instead.

## Verification

- `ovrtx_capture.py` (scratchpad → `_tools/` when stabilized) renders the
  same USD through NVIDIA's renderer (RTX Real-Time mode) and dumps
  LdrColor/HdrColor + per-signal AOVs (DirectDiffuse/DirectSpecular/
  IndirectDiffuse/Reflections/AO/MotionVectors) — per-signal ground truth
  for each new pass, not just final-image comparison.
- Per-pass profiler scopes (`pass.*`) extend the §34 KPI table; PathTrace
  budget re-splits across the new passes.

---

## Correctness Roadmap — 5-agent deep audit (2026-07-07)

State at audit: whole-frame RMSE vs ovrtx rt_wide = **0.17232** (HEAD 8443eb8);
after flip_tangent_v fix (abcf577) = 0.17220. Committed this session: GGX
stochastic default (04e03d1), dome double-count MIS (8443eb8), flip_tangent_v
(abcf577).

### CRITICAL META-FINDING: the metric is in the WRONG colour space, and the bugs cancel
- **Colour space (D2-calibration):** ExrWriter.cpp `Bgra8ToRgbaFloat` copies the
  post-Tonemap BGRA8 bytes as `byte/255` (NO sRGB OETF), while PngWriter applies
  the sRGB LUT. Verified empirically: `verify.png == sRGB(domemis.exr)` (RMSE
  0.001). So the `.exr` is ACES-linear, ovrtx PNG is sRGB(ACES); rank.py compares
  them RAW = mismatched spaces. It "works" (0.172) only because the USD camera
  `exposure = -10` is tuned ~1.3 stops bright, coincidentally cancelling the
  missing gamma. Fitting ovrtx's own HdrColor->LdrColor = sRGB(ACES_Narkowicz)
  at RMSE 0.0011 (its tonemap = Pyxis's ACES constants + sRGB).
- **The 0.172 is a fragile balance of ~10 correctness bugs that partially cancel.**
  Isolated "correct" fixes overshoot (pi-fix -> 0.208). Fix as COORDINATED SETS,
  after fixing the colour-space measurement + re-deriving exposure/clamps.

### Ranked correctness defects (file:line, both Fable audits + D-agents)
1. **pi units error** openpbr_material.slang:793 EvaluateOpenPBRAmbient returns
   rho*L/pi; L is a mip radiance average -> correct is rho*L. Reflection hits +
   translucency are pi(3.14x) too dark (id40 gap 3.25x~=pi). THE reflective-wall
   root cause. Entangled with #6 (fix together).
2. **Cross-signal dome double-count** ComputeDomeDirectDiffuse (gDirectDiffuse) +
   raygen first-bounce-miss dome (gIndirectDiffuse) both count primary dome, no
   MIS. ~2x. Cancelled by #3 on this scene.
3. **Firefly clamp bias** maxRayIntensity 6400 < dome 12000 -> every dome sample
   x0.53. Cancels #2. Must re-derive clamps as multiples of dome radiance.
4. **Bounce dome NEE missing albedo** indirect_diffuse.slang:382 domeContribution
   not x surf.baseColor (bright).
5. **Area lights double-area** StageWalker x=worldArea AND shader x area/distSq
   (rect too bright, small lights too dim); disk/sphere pdf wrong; sphere one-sided.
6. **VNDF drops G2/G1** reflections.slang:546 plain average, missing F*G2/G1;
   correct absorbed factor is env-BRDF (F0*A+B) not F(NdotV). Bright, cancels #1.
7. **Composite double-Fresnel** composite.slang:198 (1-specWeight) attenuates
   directSpecular (already Fresnel-weighted) + double-taxes direct diffuse.
8. **Metals: no specular IBL + fictitious diffuse fill** EvaluateOpenPBRAmbient
   x(1-metalness) (no specular env term) AND composite re-modulates dome/GI by
   raw baseColor with no (1-M) gate -> metals get impossible Lambertian dome.
9. **AO double-count** composite.slang:188 indirectDiffuse x ao (indirect already
   self-occludes). RTX-RT convention, but a dark bias.
10. **flip_tangent_v** FIXED (abcf577). **specular_level not read** (D4): dielectric
    grazing ~2x too strong (F(grazing)->1 vs ovrtx 0.5; map specularWeight=2*level).
    **metal F82 vs OmniPBR flat tint** (D7): gold too bright at grazing.
    **Concrete grainy** (D3): single-sample firefly -> enable AccumulationPass.
    **Vegetation dark** (D3): missing backlight/wrap (model gap).

### ovrtx reflection mechanism (D1): ovrtx RT == its own PT (brightness baked into
transport, not a real-time trick). So the reflective darkness is OUR bugs (#1,#6,#8),
not an unmatchable model gap.

### Coordinated fix plan (in order)
0. Colour pipeline: ExrWriter sRGB + re-tune USD exposure so sRGB output mean ~=
   ovrtx 0.487; make rank.py the correct space. Re-baseline.
1. Reflections set: #1 (pi) + #6 (G2/G1 or env-BRDF) + #7 (composite Fresnel) together.
2. Dome set: #2 + #3 (re-derive clamp) + #4 together.
3. Metals: #8 specular IBL + (1-M) gate + #7/D4/D7.
4. Area lights #5. Then #9 decision, vegetation, concrete (AccumulationPass).

---

## HONEST-METRIC BREAKTHROUGH (2026-07-07, cont.)

**The measurement was in the wrong colour space all along.** rank.py now applies
the sRGB OETF to the ACES-linear .exr (matching Pyxis's own .png and ovrtx's PNG
space) — the correct comparison. Old raw metric (0.172) was ACES-linear vs sRGB,
a flattering coincidence.

**In the honest sRGB metric, the entire frame is ~uniformly 2.14x TOO BRIGHT**
(mean 0.643 vs ovrtx 0.487; committed-state honest RMSE 0.231). NOT the exposure:
CamLobbyWide's photographic attrs are neutral (iso100/time1/fStop1/resp1 ->
scale 1.0), and Pyxis uses the same authored exposure=-10 as ovrtx. So Pyxis's
LIGHT TRANSPORT emits 2.14x more radiance than ovrtx's for the same scene/camera.
Root not yet pinned (dome double-count self-cancels ~1x via the firefly clamp;
likely a combination of the audit's brightening bugs #3/#6/metals, or a dome
radiance-units convention). 

**Global exposure calibration render.exposure = -1.1 (on top of -10) matches
ovrtx's mean EXACTLY (0.487) and gives honest RMSE 0.231 -> 0.15959** — the single
biggest lever. Stopgap for the 2.14x until the light-transport root is fixed.

**Honest-space per-material residuals at correct exposure (ev110, 0.15959):**
- id30 glass floor: localRMSE 0.306, MSEshare 0.01135 (44% of total!), mean +0.020
  (right) but STRUCTURAL variance 0.294 -> reflected content differs per-pixel
  (reflection ClosestHit shades hits with a simpler model than the full pipeline).
  THE dominant remaining residual; hard (reflection-accuracy).
- id7 iron door -0.208 (metal, needs specular IBL); id3 concrete +0.117 (firefly
  fro single-sample metalness -> AccumulationPass); id40 tables -0.071; id2 -0.062;
  id36 -0.082. Most other materials within +/-0.03.

Note: the raw-metric "reflective too dark" narrative was an artifact; in the honest
metric reflective surfaces are ~right-or-bright. Prior raw-space fix directions
(pi-brighten, GI-fill) were WRONG-signed. Honest metric is now the standard.

## ovrtx PIPELINE VERIFIED + GAP CHARACTERISED (2026-07-07)

**Q (owner): does ovrtx use a radiance cache? verify its pipeline, compare to ours.**

**A: YES — and it matches our architecture, but it is NOT our residual's lever.**
Authoritative NVIDIA docs (Omniverse "RTX - Real-Time" mode + RTXGI 2.0):
- ovrtx RT runs SEPARATE per-signal passes: RT-AO, direct+RT-shadows, RT indirect
  diffuse GI, RT reflections, translucency, SSS — *identical decomposition to Pyxis*.
- Its indirect-diffuse GI uses a world-space radiance cache (RTXGI 2.0 SHARC /
  DDGI / NRC family) giving "infinite-bounce" GI. Our reference capture
  (rendermode "(default)" = RT, "Indirect Diffuse GI" on since 105.0) uses it.
- So ovrtx's reflected interiors carry infinite-bounce cached radiance; Pyxis's
  reflection/indirect hits truncate at 2 bounces + dome-ambient. Real difference.

**BUT the honest-metric residual is NOT brightness-deficit, so a radiance cache
(which only ADDS light) is the wrong tool.** Evidence chain (all no-render analysis
on the committed 0.15952 baseline, `scratchpad/*.py`):
1. Per-material signs are MIXED: id30 +0.020, id3 +0.115, id37 +0.036 too BRIGHT;
   id40 -0.071, id2 -0.062, id7 -0.208 too dark. Adding light helps some, hurts others.
2. id30 (glass floor, 45% of MSE) is matched-MEAN (dLuma +0.020) with localRMSE
   0.306 — a spatial PATTERN mismatch, not a brightness gap. Isolated crops confirm
   both render a dark glossy reflective floor; the difference is reflection
   sharpness/detail/colour-temp (hard, cross-renderer).
3. **Transfer function Pyxis->ovrtx is near-IDENTITY** (best global tone LUT only
   0.15952->0.15499; best gamma = 1.00). NOT a tone/contrast bug.
4. **Best pixel shift is (0,0)** — geometry/camera are perfectly aligned. NOT a
   misalignment bug.
5. **Perfect per-material MEAN match floors at 0.15046** — only 0.009 of the RMSE is
   fixable by any per-material brightness/tint change.
6. **Error is LOW-frequency-dominated**: blur8 residual 0.135 vs high-freq-only
   0.072. Not noise/denoiser/texture-detail — broad within-material shading.

**The gap IS real and fixable.** ovrtx's own RT-vs-PT render of this exact
scene/camera = **RMSE 0.012** (PT-vs-PT2 = 0.005). A correct renderer reaches ~0.012
against the RT reference; Pyxis at 0.159 is 13x worse. So 0.05 is achievable — the
residual is genuine Pyxis rendering error, not cross-renderer noise floor.

**Localised nature (low-freq signed heatmap, `scratchpad/lowfreq_heat.png`):** the
dominant error is on the FLOOR — bottom-left -0.192 (Pyxis too dark), bottom-center
+0.111 (too bright). Pyxis's reflective floor has COMPRESSED dynamic range: bright
window reflections (grazing, near-field) aren't bright enough; dark glass isn't dark
enough. Bottom-left is 36% Tinted_Glass (id30) + 32% Paint_Satin tables (id40,
base=0.029) + 26% Terrazzo. Recurring theme: **dark low-albedo glossy dielectrics
(satin tables, tinted glass) are reflection-dominated (diffuse~=0) and Pyxis's
reflection on them is too weak/flat**, so they fall toward black where ovrtx shows
environment sheen. Prime physical suspect: grazing-angle Fresnel + specular-IBL/
environment reflection on dielectrics at reflection/ambient hits.

**Next lever (recommended):** raise reflected dynamic range on dark glossy dielectrics
— verify grazing Fresnel rise + that dielectrics receive specular-environment
reflection in the ambient/indirect term (not just the explicit reflection trace).
NOT a radiance cache; NOT per-material tint; NOT tonemap. This is per-signal
reflection-fidelity work, GPU-render-gated + iterative.

## SHARC radiance cache — IMPLEMENTED (optional) + MEASURED neutral (2026-07-07)

Owner directive: "do sharc as an optional render setting, either our current way,
either sharc." SHARC (Spatial Hash Radiance Cache) is now a real, toggleable GI
mode. **Clean-room** Slang implementation (`radiance_cache.slang`,
`sharc_resolve.slang`) written from the published algorithm (SHaRC Integration
guide) — NVIDIA's RTXGI SDK source was NOT copied: its NVIDIA-RTX-SDK license
§4(e) forbids the SDK becoming subject to an OSS license, incompatible with
Pyxis's Apache-2.0. We own every line.

**Architecture** (adapted to Pyxis's static-camera, N-frame-converged headless
render): a world-space multi-resolution spatial-hash cache (2^21 cells, ~75 MiB,
fp32; distance-based voxel LoD). Three byte-address buffers (hash checksum /
per-frame accum / cross-frame resolved) owned by a new `SharcResolvePass`
(compute) that runs BEFORE IndirectDiffusePass. IndirectDiffusePass, at its
depth-1 bounce vertex, UPDATEs the cache with `direct+dome+emission+albedo·[cached
deeper term]`; at the depth-2 vertex it QUERIEs the cache and early-terminates on
a hit (infinite-bounce feedback across pixels/frames). Gated on
`RealTimeQuality::passMask` bit 9 (`PASS_MASK_SHARC_GI` = 0x200) → `gQuality.giMode`
(a dynamically-uniform shader branch, no spec constant); clear (default) =
byte-identical builtin path.

**Result (World Lobby / CamLobbyWide, 96 frames, honest sRGB metric):**
- SHARC OFF (default, new binary): **0.15959** — unregressed vs the 0.15952 baseline.
- SHARC ON: **0.15998** — NEUTRAL (+0.0004 = run-to-run noise). Per-material
  breakdown is unchanged (id30 glass floor 0.306, id40 satin -0.071, id7 iron
  -0.209 all identical). Mean preserved (0.489 vs 0.487).

**First result (SHARC → IndirectDiffuse only): NEUTRAL** (0.15952 → 0.15998). Why:
the indirect-DIFFUSE pass already does 2-bounce GI, so the infinite-bounce
increment is tiny; and the dominant residuals are SPECULAR surfaces the diffuse
pass never touches. That pointed at the real hole:

**BREAKTHROUGH (SHARC → Reflections): 0.15952 → 0.15273** (−0.0068, ~4%). The
reflection ClosestHit shaded reflected hits with only `direct + dome-ambient +
emission` — NO indirect GI (its own "One-bounce GI at the reflection hit (NOT
implemented)" note). So the reflected WORLD was dimmer than the real world →
every glossy surface read too dark. Wiring the SHARC query into ReflectionsPass's
ClosestHit (glossy hits only, roughness > 0.3 so the sharp id30 mirror keeps its
traced sample) supplies that missing indirect term from the cache. Measured
per-material moves (96f, honest, ev re-tuned -1.1→-1.2 to re-match the mean the
added energy shifted):
- id40 Paint_Satin tables: **-0.071 → +0.028**, MSEshare 0.00313 → 0.00120 (off
  the top ranks — the biggest single win).
- id7 Iron_Brushed: **-0.209 → -0.156** (metal reflection less dim).
- id30 glass floor: unchanged (roughness gate protected the sharp mirror).
- SHARC OFF default: 0.15959, unregressed.

**Conclusion:** SHARC is the right tool, but for the REFLECTION pass (where the GI
was missing), not the indirect-diffuse pass (which already had it). Confirms the
gap = reflected-content dynamic range. Committed as the optional giMode.

**NO-GATE follow-up: 0.15273 → ~0.1258** (best in an ev sweep, ev -1.1→-1.3). The
roughness>0.3 gate was REMOVED after measuring: the glass FLOOR (id30, roughness 0,
~48% of frame MSE) is reflection-dominated and was DIM without cached GI; letting
it (and all traced reflections) query the cache dropped whole-frame RMSE
0.15273 → 0.12607 (id30 local 0.306 → 0.235, MSEshare 0.0113 → 0.0067), then the ev
re-tune to ~-1.3 (the added reflection energy shifted the mean) → ~0.1258. The
voxel blur costs less than the brightness match gains, and it matches ovrtx (whose
RT reflections query the radiance cache too); the distance-LoD keeps near-field
reflections finer. **Cumulative: 0.15959 (builtin) → ~0.1258 (SHARC on), -0.034 /
-21%.** Debt: a true mirror OBJECT would ideally still trace — a cone-width gate
(SHaRC GetVoxelSize) is the faithful refinement. Remaining top residuals: id30 (now
0.235, still #1), id2 windows (transmission, too dark), id3 concrete +0.12 (matte).

## Dome-albedo correctness fix — 0.12537 → 0.11746 (2026-07-07)

Audit defect "bounce dome missing albedo": indirect_diffuse.slang ClosestHit's
dome-NEE term (domeContribution) omitted the bounce hit's diffuse albedo, while
directContribution (BSDF) and secondBounceRadiance (× baseColor) both carried it.
For a cosine-sampled Lambertian lobe f·cos/pdf = rho (albedo), NOT 1 — so the dome
term was too bright for DARK bounce surfaces, inflating the SHARC cache and every
reflection reading from it. Fix: domeValue *= surf.baseColor. Helps BOTH paths
(the exposure re-tunes brighter since the indirect darkens):
- SHARC OFF (default): 0.15959 → 0.15644 (ev -0.85).
- SHARC ON: 0.12537 → 0.11746 (ev -0.70). id30 glass floor +0.072 → +0.009
  (RMSE 0.230 → 0.198) — the over-bright cache was the main id30 residual.
Side effect: the correct (darker) cache REVEALED that id40 satin's reflection is
itself a bit too dark (-0.076, was masked at +0.00 by the over-bright cache) — a
reflection-strength residual for later. Cumulative honest RMSE: 0.15959 → 0.11746
(-26%) since the SHARC-reflection + dome-albedo work.

## Reflection-through-glass — 0.11746 → 0.10260 (2026-07-07)

Heatmap at 0.117 showed the left terrazzo floor too dark (−0.11): it grazes toward
the floor-to-ceiling windows, but in reflection a Clear_Glass hit returned its ~black
diffuse instead of the bright exterior behind it. Fix (reflections.slang ClosestHit):
a transmissive reflection hit (transmissionWeight > 0.5) returns the dome radiance in
the ray direction × transmission_color (thin-pane ≈ straight-through), instead of the
dark glass. id30 glass floor halved (0.00475 → 0.00177 MSEshare); with the exposure
re-tuned for the added reflected-sky energy (−0.70 → −1.00) whole-frame 0.11746 →
0.10260. CUMULATIVE 0.15959 → 0.10260 (−36%). New residuals: id2 windows direct
transmission −0.046 (now #1), and glossy/metal surfaces reflecting the full bright
dome now slightly over-bright (id40 satin +0.098, id38 metal-feet +0.538 blown — the
approximation lacks the mullion breakup ovrtx's real reflected window has).
## Session 2026-07-08 — SHARC-complete, tonemap-is-perfect, grain root-cause, DLAA characterised

Baseline confirmed **0.09713** (honest sRGB, rank.py) at ev **−0.70**, passMask 895
(SHARC on), builtin denoiser, 96 frames. NOTE the correct baseline is ev **−0.70**, not
the −1.00 in the stale sharc_best.json — a 0.30-stop mismatch that alone reads as
0.125. Pinned as scratchpad/base.exr + base_config.json.

**SHARC "full" = tier-1 (trilinear + finer grid), already shipped (f5576b5).** Tier-2
(SH-L1 directional radiance encoding: accum/resolved widened to 32 B/cell, luma-weighted
direction moment, reconstruct L = DC·(1 + w·(shDir·d)/DC_luma)) was implemented end-to-end
and **measured 0.396 — catastrophically worse**, then reverted. ROOT CAUSE (a real design
truth, not a tuning miss): the SHARC cache stores **view-independent diffuse** outgoing
radiance, populated from the single primary-camera view direction. A directional moment
therefore captures the *population* direction, not a genuine radiance directional
variation; re-projecting it onto a *different* reflection query direction over-brightens
up to 2× (dcAvg·(1+w·1)). Directional SH is fundamentally inappropriate for a diffuse
radiance cache. **Do not retry SH on this cache.**

**THE TONEMAP IS PERFECT — the entire gap is HDR light-transport.** tonemap_fit/
shape_diagnostic.py: sRGB(ACES_Narkowicz(h_ovrtx)) reproduces ovrtx's own LDR reference at
**RMSE 0.00114** at gain=1.0, with per-tone bias ~0 across all 10 luma deciles. Pyxis's
operator 6 == Narkowicz ACES == exactly ovrtx's curve. So every remaining 0.097 of error is
in the per-pixel HDR radiance (GI/materials/reflections), NOT in exposure/tonemap shape.
Corollary: chasing tonemap operators or global exposure curves is wasted effort.

**Grain (user issue #1) root-caused = albedo TEXTURE detail, not MC noise, not normal-map,
not texture-LOD aniso.** In correct sRGB space the wall HF-speckle gap is modest (pyx bw_hf
0.098 vs ovrtx 0.084; lw_hf 0.160 vs 0.144) — the raw-ACES forensics crops exaggerated it
by comparing raw-ACES-pyx vs sRGB-ovrtx (mismatched spaces). The à-trous denoiser smooths
the *lighting*, but composite re-modulates by the **sharp albedo texture** afterward, so the
final grain is the albedo map's own high-frequency content, which ovrtx's DLSS softens. PROOF
IT'S FIXABLE-BY-SOFTENING: a σ≈0.6 gaussian on the final image drops RMSE 0.09713 → **0.09158
(−5.7%)** (σ sweep peak; σ>1.0 over-blurs and regresses). This is the DLSS-character gap the
user asked to close.

**Anisotropic sampler (maxAnisotropy 1→16 on bindlessSampler) — REVERTED.** Hypothesis was
that SampleGrad silently collapsed to isotropic. Measured: did NOT reduce the wall speckle
(bw_hf 0.098→0.103) and brightened the frame (mean 0.485→0.524, RMSE 0.097→0.104). The grain
is not grazing-aniso aliasing. Reverted.

**DLAA (user request) — RUNS headless (NOT a hang) and FIXES the grain, but auto-exposure
brightens 1.5×.** The prior "hang" was just slowness: full-res DLAA is ~seconds/frame, so 96
frames exceeded a 260 s timeout; full-res **8 frames completes fine**, low-res smoke completes.
At native ev−0.70 DLAA gives lw_hf 0.160→**0.088** (grain gone, below ovrtx) but mean
**0.641** vs 0.485 — a fixed ~1.5× linear brightening independent of frame count and of TAA
(passMask 831). Forcing a lower exposure (ev−1.5) rebalances the mean (0.470) but STARVES the
DLSS input and explodes structure (bw_hf 0.277, RMSE 0.44) — so exposure-compensation is the
wrong fix. ROOT CAUSE: DlssProviderFrame.cpp:224 sets `useAutoExposure = eTrue` (v1 never
tagged kBufferTypeExposure), so DLSS runs its own exposure estimate, diverging from pyx's
manual −0.70. CORRECT FIX (scoped, not yet done): tag a 1×1 kBufferTypeExposure texture with
pyx's exposure gain (ExposureMath.h) + set `useAutoExposure = eFalse` (+ maybe preExposure/
exposureScale, sl_dlss.h:81-83); re-measure at native exposure. This is real multi-file
integration, not a config tweak — deferred as the concrete next DLAA step.

**Multi-sample GI (indirectSamples/directSamples 1→4) — biased.** Reduced leftwall noise but
brightened the frame to mean 0.637 (RMSE 0.405). Dome-direct (÷sampleCount, line 151) and RIS
(line 375) look normalized, so the bias is subtler (RIS reservoir behaviour) — a real but
side-path bug; default 1-spp is the tuned path. Not pursued.

**No ambient-fill term.** Subagent-confirmed: pyx has no analog to ovrtx's
`rtx:sceneDb:ambientLightIntensity = 0.7`; the dome (intensity 12000, no texture) ingests
1:1 with no scale, and composite applies AO onto the indirect term only (a contrast
asymmetry). Untested lever for the too-dark interior materials.

**Net:** baseline 0.09713 unchanged this session (all experiments regressed or reverted).
Highest-confidence remaining levers, in order: (1) DLAA with the kBufferTypeExposure fix
(closes the grain/DLSS-softening gap the user wants, worth ≈−5%); (2) per-material calibration
of the mixed offsets (id40 satin +0.078, id3 concrete +0.069, id30 floor +0.044 too bright;
id7 −0.091, id2 windows −0.048, id29 −0.039 too dark) — genuinely per-material, no single
lever; (3) the ambient-fill / AO-on-direct asymmetry. Whole-frame <0.05 is unlikely for a
different engine — the tonemap-perfect result proves the pipeline is correct and the residual
is genuine cross-engine light-transport + DLSS-detail difference.

## Session 2026-07-08 (cont.) — DLAA exposure integration + per-material verdict (owner: "do 1 and 2")

**#1 DLAA — exposure buffer IMPLEMENTED + DLAA re-routed off RR (uncommitted working tree).**
Root cause of the ~1.5x over-brightness was two-fold: (a) with `denoiser=dlss` the pass took
the **Ray Reconstruction** path (`EvaluateRR`), not the SR/`Evaluate` path — RR is a distinct
denoiser-replacing feature, NOT DLAA. Gated `useRR=false` when `dlssExecMode==DLSS_EXEC_MODE_DLAA`
so DLAA (an SR-feature native-res mode) routes through `Evaluate` (DlssPass.cpp). (b) The SR path
set `useAutoExposure=eTrue`. Added a 1x1 `kBufferTypeExposure` buffer (DlssPass creates it in the
ctor, writes `ComputeEffectiveExposureScale` — the SAME gain TonemapPass uses — each frame via
writeTexture; FrameInputs carries it; DlssProviderFrame tags it + sets `useAutoExposure=eFalse`
when present). MEASURED (1920x1080, 24f, passMask 831): mean 0.641 (RR/auto-exp) → **0.591**
(SR + exposure buffer). The buffer WORKS but only WEAKLY counters the residual: a 1.5x multiplier
on the exposure value moved mean only 0.591→0.578, proving DLSS-SR in HDR mode does its OWN tone
handling on the World Lobby's 0-12000 linear range (out of its trained input domain) that the
exposure buffer can't fully undo. Kept the principled `exposureGain` (dropped the fudge factor).
VERDICT: DLAA now runs + routes correctly + auto-exposure is off, but is still ~0.10 mean too
bright and does NOT beat the converged builtin metric (RMSE ~0.40 vs 0.097) — as expected, DLSS
is a real-time/few-sample AA feature, not a converged-offline lever. TRUE fix (debt): feed DLSS
PRE-EXPOSED (display-range) color and move the TonemapPass exposure ahead of DLSS — a pipeline
reorder, out of scope, and no metric payoff. RR path (Auto/Quality modes) unchanged.

**#2 Per-material — analysed precisely in honest sRGB; NO clean low-risk lever (confirmed).**
Corrected a stale belief: the DOOR (id6 Iron_Brushed) ALREADY matches ovrtx (pyx RGB
0.461/0.459/0.444 vs ovrtx 0.417/0.451/0.403, R/B 1.039 vs 1.035) — the earlier "wrong door
colour" was the raw-ACES-vs-sRGB crop artifact, not a real gap. The real top MSE offsets are all
in the traced-reflection / transmission path: id2 windows −0.048 (non-blown glass edges dimmer;
blown-sky fraction already matches 76.8% vs 76.4%), id30 floor +0.044, id40 Paint_Satin +0.078
(NOT blown, uniform), id3 concrete +0.069. Since REFLECTION_TRACE_ROUGHNESS_CEILING was raised to
0.6, id40 (rough ~0.4-0.5) and id30 (rough 0) BOTH trace (no dome-mip fallback to cheaply
attenuate) — their over-brightness lives in the stochastic-GGX-VNDF + SHARC-reflection path the
last several commits deliberately tuned, so there is no decoupled fix and any change risks
regressing that work. id7 warm metal (0.482 neutral vs 0.582 warm) is a clean conductor-tint gap
but tiny MSE (0.00024) and metals split opposite-sign (id5 gold +0.222) so no shared Fresnel fix.
CONCLUSION (matches every prior session): the 0.097 residual is the entangled reflection
dynamic-range tail; no single clean lever remains. Whole-frame <0.05 is not reachable for a
distinct engine (tonemap-perfect proves the pipeline is correct). A real per-material campaign
would be delicate per-surface reflection-model recalibration with regression risk to the tuned
stochastic-GGX/SHARC state — recommend NOT doing it blind.

## Session 2026-07-08 (cont.) — REFLECTIONS PASS THROUGH TRANSLUCENT (owner-identified equation bug): 0.09713 -> 0.09346

Owner spotted the real root cause of the reflection artifacts (objects vanishing behind glass in
reflections): "reflections don't pass on all translucent". CONFIRMED — reflections.slang
ClosestHitMain, on ANY transmissive hit (transmissionWeight>0.5), returned
`SampleDomeBackground(WorldRayDirection()) * transmissionColor * (1-R)` — it ASSUMED the only thing
behind glass is the exterior sky. True for the curtain-wall windows, WRONG for any glass with
geometry behind it (a plant behind a glass rail, objects behind a pane): they were replaced by the
dome and disappeared in reflections. Compounding it, HitGroupDefault had NO any-hit at all, so
alpha-cutout foliage also read as SOLID QUADS in reflections.
FIX (per-material reflection campaign, the clean win): gave the primary reflection ray a
transmissive-passthrough + alpha-test any-hit (reflections.slang AnyHitMain, mirrors the GI
AnyHitMain): a reflection ray now IgnoreHit()s glass (accumulating tint x angle-dependent (1-R)
Fresnel into ReflectionPayload.throughput via the per-face normal — preserving the grazing (1-R)
dimming the old dome-shortcut had) and alpha-cutout texels, so the SAME TraceRay continues to the
REAL geometry behind (ClosestHitMain) or the sky (MissMain). RayGen multiplies the traced color by
throughput. Bounded by MAX_REFLECTION_TRANSMISSIVE_CONTINUATIONS=4; past-cap glass falls back to the
old dome approximation. NO recursion increase (any-hit passthrough, maxRecursionDepth stays 2) —
important on the 8 GB GPU. Files: reflections.slang (payload+throughput, AnyHitMain, RayGen apply,
ClosestHit block re-scoped to past-cap fallback), ReflectionsPass.{h,cpp} (load reflections_anyhit.spv
+ HitGroupDefault.setAnyHitShader), CMakeLists (compile AnyHitMain).
MEASURED: whole-frame 0.09713 -> **0.09346 (-3.8%)**, biggest single campaign win + physically
CORRECT. id2 windows -0.00062 (biggest improver), id40 satin/id29/id7/id6 also better. Regressions
(the reflection-brightness entanglement, now that reflections show real content): id30 floor
+0.00038, id3 concrete +0.00025, id23/id37/id59 small — net still clearly positive. These
too-bright-reflection surfaces (id30/id3/id40 cluster) remain the next campaign target; a blanket
reflection intensity scale is too blunt (helps id40 but regresses id23 walls, whose dark-interior
reflection is correct) and gTranslucency is NOT a clean glass gate (nonzero on opaque surfaces).

## Session 2026-07-10 — AO parity audit + wall-speckle root cause (0.09713 → 0.08684)

**Committed:** f578c65 reflections-through-translucent (0.09713→0.09346, ev retuned −0.70→−0.90
→ **0.08986**), 28b7cfa DLSS DLAA exposure, 8a56d66 atrous diffuse-phi (**0.08986→0.08684**,
−10.6% cumulative this session). Pinned baseline: scratchpad/base.exr + base_config.json
(passMask 895, builtin, 96f, ev −0.90) = **0.08684**.

**AO parity vs ovrtx RT (user question "same as NVIDIA?") — VERDICT: at semantic parity.**
Authoritative source: ovrtx 0.3.0's own generatedSchema.usda (ovrtx/bin/usd_plugins/rtx_settings/).
ovrtx AO: enabled=1, rayLength **35 cm** (displayName "Ray length (cm)"), minSamples 3 /
maxSamples 9, denoiserMode "aggressive", stratification on, NO falloff/opacity knob. Pyx:
0.35 m (world = meters after StageWalker metersPerUnit bake) = **35 cm exact**; 2-spp cosine,
binary, own AO denoiser (5x5 spatial + 32f temporal); applied to indirect-diffuse ONLY —
which IS the RTX-RT convention (NVIDIA doc structure nests AO+ambient under "Indirect
Diffuse Lighting"; dome/IBL is Direct Lighting and pyx's dome-direct is real-shadow-traced,
verified direct_lighting.slang:130-142). Measured net: aoRayLength 5mm (+0.00016) and 1.05m
(+0.00200) both WORSE → 35 cm is locally optimal. Remaining deltas vs NVIDIA: 2 vs 3-9 spp
(converges out over 96f) and their "aggressive" AO denoiser mode. **Ambient light red herring
RESOLVED**: current ovrtx defaults ambientLight color (0,0,0) intensity 0, and the RT 2.0 FAQ
says "RTX - Real-Time 2.0 mode does not have global ambient light settings" — the World
Lobby's authored ambientLightIntensity=0.7 is IGNORED by the RT 2.0 mode we capture against.
Pyx having no ambient term is CORRECT parity. passMask-off A/B caveat (agent-verified): signal
textures are allocated but unwritten when a bit is clear → zero-init reads; AO A/B must go
through aoRayLength, not bit 2.

**Wall/planter speckle (user crop "fix this part") — ROOT CAUSE + FIX (8a56d66).**
Isolation chain: survives AO-off, SHARC-off, reflections-off; VANISHES with indirect-off
(wall HF 0.114→0.099 ≈ ovrtx 0.096); raw denoise-off@256f HF 0.229 (the denoiser reduces but
its residual rectifies into deterministic speckle); albedo AOV vs ovrtx DiffuseAlbedoSD:
CLEAN (texture path exonerated). Mechanism: fixed diffuse phi luminance edge-stop is binary
against the indirect signal's huge dome-NEE spikes → spikes preserved as "edges" through all
iterations + accumulation. Fix: diffuse luminance stop OFF (phi=1e6 = ReLAX's high-variance
limit; normal/viewZ/matId stops keep real edges). phi 6.0 measured ZERO effect (spikes >> phi
either way) — do not retry mid-range phi. Spec channel MUST keep phi 1.0: on flat mirrors the
luminance stop is the only reflected-content edge preserver (spec-inf: RMSE-flat, blurs the
glass-floor mirror image, id30 luma +0.02 worse direction). Remaining HF gap (wall 0.104,
planter 0.040 vs ovrtx 0.096/0.021): direct/AO/spec residue + ovrtx's aggressive AO denoiser
+ DLSS softening — diminishing. Proper future fix if wanted: SVGF variance-guided phi (2nd
luminance moment in DenoiseTemporalPass), tracked as debt.

## Session 2026-07-10 (cont.) — "keep pushing": 0.08684 → 0.08615 + audits closed

**Committed 8d48701**: SHARC cell key + normal octant (NVIDIA SHaRC parity; kills
cross-face cache sharing). 0.08684 → **0.08615**. Cumulative today: 0.09713 → 0.08615 (−11.3%).
Baseline pinned (base.exr/base_config.json, ev −0.90, passMask 895).

**Albedo audit — methodology finding (do not repeat the mistake):** ovrtx's DiffuseAlbedoSD
AOV = baseColor × (1 − metallic) (verified: dielectrics ratio ≈1, metals ≈0, ORM materials
exactly (1−ORM.B mean): Concrete_Formed ORM.B=0.26 → 0.213×0.74=0.158≈their 0.147). The
apparent per-material albedo "bugs" (id3/id27/id59/id61) dissolve under the correct semantic.
Real texture-path is CLEAN. Only true albedo anomaly left: id60 bark3 (pyx (0.11,0.09,0.07)
vs ovrtx (0.25,0.19,0.07) — R,G ~2.1x brighter in ovrtx, B matches; tiny area, parked).
Material slot names via temp MATSLOT log (reverted): 3=Concrete_Formed, 27=Slate,
59=Meadowlark_flowers, 60=bark3, 61=SquareGardenPlanterLong.

**User's column crop (id3 Concrete_Formed, x311-432) — quantified:** pyx is uniformly
BRIGHTER than ovrtx there (lit +0.042, shadow side +0.117 — the visual "darker/dirtier"
read was a contrast illusion). ovrtx has MORE lit→shadow contrast (2.4x vs 1.9x). The
normal-octant fix only moved the shadow side to +0.113 → the over-fill is NOT cache leak;
remaining suspects: dome-direct at the interior-facing side, bounce-GI over-fill, or
ovrtx-side stronger occlusion of its GI (their AO "aggressive" denoise). Fireflies on the
column: only 186 px > +0.25 — negligible MSE. NEXT candidates by MSE: id2 windows 0.00176
(−0.042 dark; ovrtx bloom hypothesis untested), id30 floor 0.00153 (+0.034).

**id2 bloom hypothesis REFUTED → "window-adjacent under-lighting" discovered (NEXT LEVER).**
Distance-ring analysis vs ovrtx blown-sky mask: far-field control (>16px from any blown sky,
1.58M px) matches ovrtx to −0.001 luma — pyx's global transport is *calibrated*. But the
window-adjacent rings are under-lit and the deficit GROWS outward (2px +0.026, 4px +0.070,
8px +0.085, 16px +0.121 ovrtx-brighter) — anti-bloom-shaped (bloom decays outward), so NOT
post-FX. Pyx under-lights the zone within/beyond ~16px of bright sky: window frames (id2),
terrazzo floor near the curtain wall (id29 −0.039), mullion metals (id7 −0.10), likely id24.
Combined ≈0.0027 MSE — the single biggest remaining cluster. Attribution TBD: extend rings to
32/64px (growing→zone under-lit; peaking→halo), then bisect the near-window signal (dome-direct
grazing transmission? missing sky energy at glancing incidence through panes? indirect bounce
off the bright floor?). NOTE the visibility-ray convention passes glass UNATTENUATED (would
bias bright, not dark) — the deficit mechanism is not the obvious one; measure before fixing.

## Session 2026-07-10 (cont. 2) — window-strip root cause + translucency-SHARC (→ 0.08304)

**Committed 97e8e66**: SHARC query at the terminal through-glass segment. The "window strip"
deficit was RESOLVED to its mechanism chain: strips = exterior curtain-wall structure seen
THROUGH the glass at near-normal incidence (NdotV~0.91 — Fresnel ruled out; measured pyx 3.0%
of sky radiance vs ovrtx 13.7%); TranslucencyPass's ShadeSurfaceHit segment shade has no
indirect GI → exterior sky-averted surfaces went dark. Same substitution as the reflections
breakthrough. Measured 0.08615 → 0.08338; id30 glass floor luma bias +0.034 → +0.005 (through-
floor content now lit — the floor's residual is PATTERN, not level, now). ev re-tuned
−0.90 → **−0.85** → **0.08304**. Cumulative today 0.09713 → 0.08304 (−14.5%).
Strips 0.479 → 0.507 vs target 0.746 — remainder is SHARC coverage of rarely-visited exterior
surfaces (cache populates only where camera-driven bounce vertices land). Diminishing there.

**License audit (agent, verbatim quotes on file):** NRD / SHARC / RTXDI / RTXGI / OMM all
still ship the proprietary "NVIDIA RTX SDKs LICENSE" (no 2024-2025 relicense; §4(b) no
copy/derivative, §4(e) no-open-source-subjection) → NOT vendorable; fetch-at-build opt-in
target (source stays out of repo, separate non-Apache lib, default OFF) is the defensible
integration if ever wanted. NRC = prebuilt DLLs only (NGX-style runtime posture). Streamline
= MIT incl. sl.nis / sl.deepdvc / sl.dlss_d plugin glue (NGX DLLs runtime-loaded) — extending
our DlssProvider to NIS/DeepDVC/RR is licensing-clean. NVRHI/Donut/RTXMU MIT (unchanged).
Our clean-room SHARC implementation remains the correct approach (algorithm not licensable;
NVIDIA's header text is).

## Session 2026-07-10 (cont. 3) — gap table + two cheap levers closed (→ 0.08240)

**Committed 604f941**: SHARC GRID_DENSITY 0.5→0.25 (swept 0.35/0.25/0.18; knee at 0.25).
0.08304 → **0.08240**. Cumulative today 0.09713 → 0.08240 (−15.2%), 7 commits.

**Gap-audit results (full pass-by-pass table in the workflow output, agent-verified vs
generatedSchema):** pipeline is at ALGORITHM-CLASS parity everywhere: G-buffer/RIS-direct/
dome-IBL/SHARC-GI-at-diffuse+specular+translucency/GGX-VNDF-reflections/à-trous-chain all
match ovrtx's documented shapes; exact-match constants (aTrous 5, historyFix 14/3, phi spec
1.0, clamps 6400/19200, AO 35cm). Remaining NON-parity items ranked: (1) reflection cached-
term fidelity [= the pattern tail, being chipped by density tuning]; (2) translucency firefly
clamp missing [NO-OP here: descaled clamps ~millions vs dome 12000 — hygiene only];
(3) diffuse phi=inf is a compensating patch vs NRD's variance-guided phi=2 [debt: SVGF 2nd
moment in DenoiseTemporalPass]; (4) composite double-Fresnel on directSpecular [A/B'd THIS
session: +0.00002 = EMPIRICALLY RULED OUT, directSpecular is negligible in this dome-lit
scene; reverted]; (5) DLSS-RR environment-block [driver/SDK model mismatch, not code].
Also noted: ovrtx runs a SEPARATE gentler indirect-diffuse denoiser (4 iter, kernel 32,
history 100) vs our shared chain; ovrtx MIS(BSDF+light) in sampled direct [low-med, unbiased
either way at convergence]; LTC fallback [not applicable to converged offline comparison].

**Honest asymptote statement for the 0.01 target:** ovrtx's own RT-vs-PT same-renderer delta
is 0.012; a distinct engine cannot beat cross-renderer detail differences. At 0.0824 the top-2
contributors (id2 windows 0.00137, id30 floor 0.00127) are ~1/3 of total MSE and are DETAIL/
pattern-dominated (levels match to +0.01/-0.04). Realistic floor with current architecture:
~0.06-0.075 (per-material mean-match analysis). Reaching further = NRD-optional integration
(fetch-at-build, licensing path documented) + DLSS-RR unblock + SVGF variance — each closes
"character" not "level" gaps.

## Session 2026-07-10 (final) — asymptote reached at 0.08240; negative results logged

Post-0.08240 experiments, ALL measured and reverted (do not retry without new information):
- **(1−M) diffuse-energy fix** (dome-direct + indirect legs × (1−baseMetalness), gAlbedo.a
  carrying metalness): PHYSICALLY CORRECT and it fixed the ORM quarter-metals exactly as
  predicted (id3 concrete +0.087 → +0.037), but TRUE metals collapsed (id6 −0.179, id7
  −0.155): their brightness was living off the incorrect diffuse fill because the conductor
  SPECULAR response under-delivers. Net +0.00856 WORSE. The tuned state is a compensating
  local optimum; this fix requires a companion conductor-specular-energy fix (F82/env-BRDF
  at dome scale) to land together. Full edit set preserved in this doc's git history.
- **Temporal history 30→100** (ovrtx indirect-denoiser parity): +0.00015 worse — 96-frame
  accumulation already dominates; longer EMA just converges slower within the run.
- **SHARC probe 8→12 + accum-cap 64→96**: +0.00002 (wash) — no collision pressure at
  density 0.25; resolve cap immaterial at convergence.
- **Composite double-Fresnel** and **translucency firefly clamp**: ruled out earlier this
  session (±0.00002 / structurally can't fire).

**Where the remaining 0.0824 lives**: id2 windows 0.00137 + id30 floor 0.00127 ≈ 1/3 of MSE,
both DETAIL/pattern (levels match to ±0.01); the metal cluster (id6/id7/id5, needs the
conductor-specular work); the DLSS-softening character (~5%). Sanctioned-but-unstarted big
items, in EV order: (1) conductor-specular energy + (1−M) as a PAIRED fix [the one remaining
LEVEL error, ~0.0005-0.001]; (2) NRD fetch-at-build optional (default-OFF, licensing path
documented above); (3) SVGF variance moments; (4) DLSS-RR NGX unblock (needs driver/SDK with
model 703). Expected honest floor remains ~0.06-0.075.

## Session 2026-07-10 (cont. 4) — metal pairing measured, NRD plumbing landed, SHaRC all-vertex updates

**Metal pairing (both variants measured, reverted):** (1−M)-alone +0.00856 (metals collapse:
id6 −0.179 — they lived off the wrong diffuse fill). PAIRED with removal of the ad-hoc
(1−r')^(2M) metal damping (reflections.slang): +0.00203 — id6 mean lands near target
(−0.031) but its MSE DOUBLES: the old sky-facing over-brightness (the band the damping was
added for, pre-SHARC content) returns while interior-facing stays dark. CONCLUSION: the metal
error is ORIENTATION-DEPENDENT REFLECTED-CONTENT error, not a weight/energy scalar — no
global specWeight/diffuse knob can fix both bands; requires per-direction content work
(e.g. what the mullion reflection actually shows toward sky vs interior). Parked with data.

**NRD optional dependency SHIPPED (1822bcc):** PYXIS_WITH_NRD (default OFF) →
FetchContent NVIDIA-RTX/NRD v4.17.3 (submodules, SPIRV-embedded static lib, ShaderMake FXC
probe disabled), linked into pyxis_renderer + PYXIS_WITH_NRD define; verified live log
"NRD backend available (v4.17.3, static, SPIRV-embedded)". OFF stays byte-green. Runtime
NrdProvider skeleton (instance + pipeline/pool translation onto NVRHI) in progress via agent.

**In flight (this session):** SHaRC bootstrap-only updates from translucency TERMINAL
vertices (NVIDIA's reference updates at all path vertices; ours was depth-1-only —
gShAccum bound at translucency Set-1 binding 4; accumulate ONLY on query-miss so GI-less
bootstrap can seed empty exterior cells but never dilute resolved ones).

**SHaRC translucency-terminal bootstrap: measured WASH (+0.00001), reverted.** The gShAccum
bootstrap-on-query-miss seeds empty exterior cells — but the seeded value IS the GI-less
shade, so a later query-hit returns exactly what the fallback would have shaded: identical
pixels by construction. The strip residual (0.519 vs ovrtx 0.747) is CONTENT-limited: those
cells need real exterior multi-bounce (sky→facade→cap) that only reaches them if interior
bounce rays through the glass land there (rare) — ovrtx's cache gets it from its
all-vertex updates over exterior geometry lit by its own GI. A translucency-terminal shade
with real GI rays is the remaining (expensive, low-EV ~0.0006 MSE) option. Same reasoning
kills the planned reflections-fallback bootstrap (same physics). NrdProvider skeleton
committed (3555bdc): instance + pipelines-from-embedded-SPIRV + pools translate onto NVRHI,
both configs green; next stage = per-frame dispatch translation + graph wiring.

## NRD integration — full staging record (2026-07-10, commits 1822bcc→6f628aa)

Six commits land the complete OPTIONAL NRD backend skeleton→first-light:
1822bcc fetch plumbing (PYXIS_WITH_NRD off-default, v4.17.3, SPIRV-only, FXC probe off);
3555bdc NrdProvider stage 1 (instance RELAX_DIFFUSE_SPECULAR+SIGMA_SHADOW, 22 pipelines
from embedded SPIRV via spirvBindingOffsets flattening, samplers, pools); 7a56e9e stage 2
per-frame Evaluate (CommonSettings TRANSPOSED — NRD is column-major doc'd vs our row-major;
motionVectorScale {-1/w,-1/h,0} for their prev-minus-current normalized-UV vs our
current-minus-previous pixels; jitter passthrough verified; NRD_NORMAL_ENCODING=3
RGBA16_UNORM + nrd_pack.slang best-fit pack; RelaxSettings) ; 3058128 denoiser=nrd settings
surface v6.1.0 (honest ladder); 6f628aa stage 3 graph wiring (NrdDenoisePass →
PassContext.nrdDenoised* → CompositePass preference; builtin trio still runs alongside).

**FIRST LIGHT STATUS**: requested=Nrd effective=Nrd runs end-to-end (no crash/hang, 96f
1080p). Output ENERGY-LOSING: whole-frame 0.240 vs builtin 0.08240 — image = direct-only
signature ⇒ NRD OUT_DIFF/OUT_SPEC ≈ black ⇒ either the pack dispatch writes zeros into
IN_* or the per-dispatch binding-slot translation misroutes (dispatches DO run, no NVRHI
errors, no NaNs). NEXT DEBUG STEPS (in order): (1) temp-log dispatchDescsNum + dispatch
names frame 0 (expect ~15-25 for RELAX); (2) dump _packedDiffuseRadianceHitDist after
DispatchPack (add a --save hook or copy to an AOV) — if zeros, the pack binding set/slot
order vs nrd_pack.slang bindings is wrong; (3) if pack OK, verify per-dispatch slot
assignment: NRD resources must bind at slot = range baseRegisterIndex + intra-range offset
(in DECLARATION order of the pipeline's resourceRanges), NOT resources[] array order;
(4) check constantBufferData upload versioning. Known input gaps regardless: gViewZ=0 on
miss (NRD wants >= denoisingRange), gIndirectDiffuse.a=1 placeholder hitDist. Builtin
default verified unchanged (0.08240 delta +0.00000). SIGMA_SHADOW created but undispatched.

## NRD FIRST LIGHT — WORKING (441e910, 2026-07-10)

Root cause of the black output: NRD SPIRV keeps TWO descriptor sets (resources space 0;
samplers s0..s1 + CB b0 in space 1 → [Set 1, bindings 0/1/2]); our flattened single-set
layout was spec-invalid on 20/22 pipelines (VUID-...-07988, found by running the smoke with
validationLayer=true — THE debugging lesson: validation names the exact set/binding/variable).
Fix: per-pipeline Set-0 resource layout + ONE shared Set-1 (samplers+CB) layout/binding-set;
ComputeState.bindings = {set0, set1}. MEASURED: denoiser=nrd @ default RelaxSettings =
**0.08949** vs tuned builtin 0.08240 (id2 windows already BETTER −8%, id40 luma dead-on
ovrtx). Remaining validation nits: 2× VkBufferMemoryBarrier2 access-mask warnings (NVRHI-
internal, benign). NEXT: RelaxSettings tuning toward the ovrtx schema (their indirect chain:
4 iterations, kernelRadius 32, history 100; main ReLAX phi 2/1, history 31/8) — plausibly
closes or beats 0.08240 since NRD is ovrtx's own denoiser class. Also SIGMA_SHADOW wiring
(created, undispatched) for the direct channel.

## NRD block CLOSED at working-optional (c34e101, 2026-07-10)

Post-first-light round: viewZ miss-sentinel remap (pack shader, packed R32F -> IN_VIEWZ) +
RelaxSettings.atrousIterationNum=4 (ovrtx parity) — both MEASURED CONVERGENCE-NEUTRAL on the
static-camera 96f comparison (0.08949 unchanged; hashes differ => knobs live, accumulation
converges RELAX to a settings-insensitive fixed point). VERDICT: denoiser=nrd = 0.08949 vs
tuned builtin 0.08240; the delta is structural converged bias, not tunable. The optional
NVIDIA-NRD path is COMPLETE for its purpose: ovrtx's own denoiser class, licensing-clean
(fetch-not-vendor), opt-in, honest ladder, byte-neutral default. Remaining NRD follow-ups
(low priority): SIGMA_SHADOW wiring for the direct channel, diffuse hitDist signal, moving-
camera validation (where NRD's temporal machinery actually differentiates).

## METAL THREAD CLOSED — the "wrong diffuse fill" is the conductor ambient (2026-07-10)

Round 2 of the paired metal fix ((1−M) diffuse legs + damping removal) was re-applied and
measured FACING-RESOLVED (normal-octant classification per material vs ovrtx):

  id6 mullion  −X(interior) n=18.5k: base −0.039 ✓ → fix −0.190 ✗✗ (collapse)
               −Y(underside) n=2.2k: +0.190 both (untouched by the fix)
               +Z            n=21k : base +0.059 → fix +0.086
  id7 iron     −X n=3.4k: −0.245 → −0.263 (bad both)   −Z n=24.7k: −0.074 → −0.056
  id5 gold     +Z n=8.2k: +0.041 → +0.007 ✓ (the one clean win)

**DEFINITIVE UNDERSTANDING (upgrade from "mysterious compensation"):** for a CONDUCTOR,
the composite's `indirectDiffuse × gAlbedo.rgb` leg is NOT an incorrect diffuse term —
since gAlbedo.rgb = baseColor = F0 for metals, that product IS the canonical wide-lobe
conductor environment approximation (irradiance × F0 ≈ split-sum ambient for rough metal).
ovrtx's interior-facing mullions glow at luma 0.70 from exactly this term. Applying (1−M)
deletes real physics; three measured configurations (B alone +0.0086, A pair +0.0020,
round-2 confirmation) all net-regress. DO NOT re-attempt (1−M) on the composite without
REPLACING the term by an explicit EnvBRDF(F0,r,NdotV)×irradiance — which it already
approximates. Remaining true per-face tails are small (id6 −Y underside +0.19 over 2.2k px,
id7 −X −0.25 over 3.4k px ≈ ≤0.0005 MSE total) and orientation-content-specific (floor
reflection on undersides; interior reflection content on iron) — parked with data.

STATUS: builtin baseline stands at 0.08240 (64ac447), every identified lever measured.

## Session 2026-07-11 — PostSoftenPass: DLSS-character softening, NEW BEST 0.07958

Owner's #1 complaint after the 0.08240 review: "the biggest problem is that my image is not
smooth." Root cause is a CHARACTER gap, not an error gap: ovrtx's reference PNG is
DLSS-processed (band-limited); pyx's 96-frame converged output is raw-sharp at the
texture-detail frequency band (wall HF stat 0.1051 vs ovrtx 0.0959).

**Why not the real ovrtx softener (owner asked):** ovrtx has NO dedicated soften pass — the
softness IS DLSS. Our DLSS integration exists (Streamline + NGX, DlssPass) and was
characterised 2026-07-08: DLAA fixes the grain (lw_hf 0.160→0.088) but DLSS-SR in HDR mode
does its own tone-handling on the World Lobby's 0–12000 linear range → ~0.10 mean brightness
error, whole-frame RMSE ~0.40 vs builtin 0.097 — a real-time few-sample AA feature, not a
converged-offline lever. DLSS-RR (what ovrtx actually runs, and the variant that would
denoise+soften correctly) remains driver/SDK-blocked (needs model 703). So the honest
reachable stand-in is a measured display-space Gaussian with DLSS's band-limiting character.

**Implementation (v6.2.0, additive MINOR):** `RealTimeQuality::postSoftenSigma` (consumes a
_reserved slot, default 0.0 = pass fully disabled → byte-identical, goldens untouched).
`PostSoftenPass` between Tonemap and SsaaResolve: radius-2 Gaussian (CPU-normalized 1D
weights, w2 derived in-shader from the 5-tap identity), blur into an owned same-format temp,
copyTexture back over targets->color. One shader (post_soften.slang), single Set-0 layout
(SRV/UAV/volatile-CB), zero allocations in Execute (EnsureTemp on the CPU frame path).
Config: render.realTimeQuality.postSoftenSigma (Configuration → Headless + Viewer seeds; the
viewer re-applies after the ImGui override since there is no editor knob yet).

**The blur must run in sRGB-ENCODED space** (encode taps → average → decode): the tuning
measurement and the reference PNG live in the final sRGB image. Measured in-engine at σ0.5:
display-linear blur 0.08095 (id6 metal REGRESS +0.00011 — bright pixels over-weighted
through the encode); sRGB-space blur **0.07958** = the numpy prediction (0.07956) within
0.00002, and EVERY per-material row flat-or-better vs base.

**Result: 0.08240 → 0.07958 (−3.4%), new campaign best.** Wall HF lands at ~ovrtx's 0.0959.
Baseline artifacts repinned: base.exr := soften05.exr, base_config.json += postSoftenSigma
0.5. Campaign total: 0.15959 → 0.07958 (−50.1%).

## Session 2026-07-11 (cont.) — DLSS-RR UNBLOCKED; converged-RR verdict measured

Owner: "use all ovrtx pipeline/algorithms available; if DLSS-RR blocked, let's see why."

**WHY IT WAS BLOCKED (now resolved):** the 07-06 block was a snippet↔model version
mismatch, not a hard driver limitation. Driver 610.62 provisions NGX dlssd models for
snippet versions 310.2.1/310.3.0/310.5.2/310.6.0 (NGX cache dirs 20316673/20316928/
20317442/20317696 — hex-encoded versions; ALL contain 160_E658700.bin), but Streamline
2.12's nvngx_dlssd.dll is 310.7 and demands 160_E658703 (absent, OTA has nothing). The
2.11.1 SDK staged into bin/Release at the tail of the 07-06 session (never re-probed)
ships nvngx_dlssd.dll 310.6.0 = exact match for the cached model. Probe: slIsFeatureSupported
(kFeatureDLSS_RR) OK; "Created DLSSDContext feature (853,480)->(1280,720)"; RR evaluates
end-to-end, fully offline. GENERAL RULE FOR FUTURE BLOCKS: match the snippet DLL version
to a hex-decoded NGX models/dlssd/versions/ dir — the driver/OTA model set trails the
newest public SDK snippet.

**First TRUE-RR converged numbers** (all prior "RR" data was the SR+builtin ladder —
RR never actually ran on this box before today):
  RR quality + accumulation   0.09294   (broad +0.02..+0.10 luma inflation)
  RR quality, no accumulation 0.09365   (passMask 767 — double-temporal ruled out)
  RR quality + pre-exposure   0.09289   (wash)
vs builtin+postSoften 0.07958 / NRD 0.08949. **Scale-invariance proven:** pre-exposing
input by ComputeEffectiveExposureScale (~3.5e-4 at ev-0.85 — a ~3000x input change)
leaves the output within run-to-run noise → RR's internal normalization removes global
scale; its luma inflation is converged tonal CHARACTER, not an exposure-level error and
not fixable by any input scaling. VERDICT: RR = real-time few-sample viewer feature
(now fully working for that purpose); the converged-offline champion remains
builtin+postSoften. Pre-exposure (dlss_expose.slang, scale→evaluate→inverse, failure
undo) kept: guide-correct exposed-input convention, protects moving-camera use.

**ovrtx algorithm-adoption scoreboard (owner directive):** SHARC clean-room ✓ (in 3
signal paths), NRD optional ✓ (0.08949), DLSS SR ✓ / DLAA ✓ / RR ✓ (all live), TAA ✓,
RIS direct ✓, physical-camera exposure ✓, ACES-approx tonemap ✓ (proven exact vs ovrtx).
Not adopted, ranked by expected value: SVGF variance moments (replaces the diffuse
phi=inf patch — real candidate), RTXDI/ReSTIR-DI (large integration; our RIS direct is
the lite version; converged benefit doubtful — unbiased either way at convergence),
NRC (binary-DLL licensing OK but converged benefit doubtful vs SHARC), OMM (blocked:
RTXMU has no OMM support, §16).

**RR research addendum (web sweep, 2026-07-11):** the missing model 703 is not published
ANYWHERE — the public NGX OTA bucket (https://ngx.download.nvidia.com/, unauthenticated
S3 listing, verified live) contains ONLY 160_E658700.bin under dlssd across all channels
including NVIDIA's own dev-models staging tree. NVIDIA has announced the 2nd-generation
DLSS 4.5 Ray Reconstruction transformer for **August 2026** (via NVIDIA App DLSS
Override) — the SL 2.12 snippet (310.7) shipped ahead of its own model rollout. So the
310.6.0 pin is not a workaround; it is the only correct configuration until then.
Streamline 2.12.0 is still the latest (repo renamed NVIDIA-RTX/Streamline); driver
610.74 (2026-07-07) exists but the OTA check says it can't help. RE-CHECK METHOD when
August comes: re-browse the OTA bucket for dlssd/160_E658703.bin (or just retry
slIsFeatureSupported with the 2.12 snippet) — don't guess by driver version. Trivia
that explains the DLL-size cliff: snippets ≤310.3 embed model weights (~73 MB), 310.4+
load the external NGX-provisioned .bin (~28 MB).

## Session 2026-07-11 (cont. 2) — noise + window-border forensics (user reports)

User reports post-soften: (1) "scene too noisy vs ovrtx"; (2) "window borders uniform on
ovrtx, shadowed on pyx". Full elimination chain, all measured:

**(1) The "noise" is converged bump-lighting response with EQUAL energy but WRONG PHASE
COHERENCE — not MC noise, not fireflies, not a filter bug.** Elimination trail:
- Raw converged signal (denoise off, 96f): wall HF 0.1027, SEED-INDEPENDENT (r=+0.08
  between seed noise fields), config-independent (removing direct/indirect/refl/AO one
  at a time leaves it at ~0.14 @8f), clamp-immune (indirect clamp sweep 25/50/100 =
  byte-flat: the estimators are bounded, nothing to clamp), NOT albedo (our baseColor
  AOV is CLEANER than ovrtx's DiffuseAlbedoSD: wall gx 0.0203 vs 0.0371, bowl HF 0.0093
  vs 0.0167), NOT the normal G-buffer per se (both engines' normal AOVs equally bumpy:
  flat-wall HFdev 0.15-0.27 BOTH).
- à-trous diffuse normal edge-stop 32 -> 1 (with phi already inf = filter nearly wide
  open): metric 0.07958 -> 0.07956 (wash), wall/bowl visuals UNCHANGED -> the mottle is
  not in the indirect-diffuse signal the atrous filters. REVERTED to 32.
- Multi-scale band energies (1-2/2-6/6-16/16-32px) on wall AND bowl: pyx == ovrtx to
  ~1e-3 AT EVERY SCALE. The visible difference is PHASE: their detail is spatially
  coherent (albedo streaks, smooth shading gradients), ours is random-phase blotch from
  bumpy-normal shading response scattered through all signals. Energy-domain filters
  (any phi/power/sigma tuning, any clamp) CANNOT change phase coherence — this is
  exactly the class of gap a learned denoiser (DLSS-RR) closes and hand filters don't.
- SOURCE-side fix identified (not yet built): Toksvig/LEAN-style filtered normal
  shading — flatten the shading normal toward geometric with distance/footprint and
  compensate by widening roughness. Kills the incoherent bump response at the source
  (walls/planters shade smooth like ovrtx's final) instead of asking a filter to
  reconstruct coherence after the fact.

**(2) Window borders: ovrtx has a wide bright halo around blown sky that we lack.**
Ring profile (luma vs distance-from-blown-sky): deficit -0.008 @1-2px (both clipped),
growing to -0.055/-0.056 @13-24px, decaying by 32px — the signature of a wide veiling
glare/RR bleed + clipping, NOT the earlier "anti-bloom" reading (which forgot that
clipping hides the near-edge delta). numpy bloom prototype on base.exr: threshold 0.8
(display-linear), sigma 24px, gain 0.10 -> whole-frame 0.07958 -> 0.07877 (-0.0008) and
ring 13-16 deficit -0.055 -> -0.029; stronger gain fixes the ring fully but regresses
whole-frame (single-Gaussian model saturates). Mullions inside the halo zone get veiled
-> directly addresses the "shadowed borders" perception. Proposed: optional PostBloomPass
(threshold/sigma/gain knobs, default OFF), ovrtx profile 0.8/24/0.10.

## Session 2026-07-11 (cont. 3) — PostBloom shipped (0.07883); mottle attribution CLOSED: RR is the answer

**PostBloom SHIPPED (1b99b1d):** in-engine 0.07883 (numpy predicted 0.07877), windows id2
-0.00040, mullions-against-sky veiled like the reference. Baseline repinned: base.exr /
base_config.json = passMask 895 + builtin + 96f + ev-0.85 + postSoftenSigma 0.5 +
postBloomGain 0.10 = 0.07883. Campaign 0.15959 -> 0.07883 (-50.6%).

**Mottle attribution, final rounds (all measured):**
- Global bump flatten x0.5 (shading.slang): -0.00043 whole-frame (softer shading reads
  ovrtx-ish) but the planter/wall mottle UNCHANGED -> not bump-response. REVERTED
  (material-override debt, off-thesis); logged as a look-tuning data point in-code.
- SHARC OFF at 96f raw: wall HF 0.1028 vs 0.1027 ON -> identical; cache voxel error
  eliminated as the blob source.
- ACES-shoulder insight: the 8f->96f "convergence stall" (1.33x vs sqrt(12)=3.46x) is a
  DISPLAY-SPACE artifact — the tonemap shoulder compresses large linear noise
  nonlinearly; linear-space convergence is likely healthy. (Retracts the earlier
  "heavy-tail firefly" framing; the clamp-sweep no-op already said the estimators are
  bounded.)
- Seed-pair test on the FULL pipeline (seed 42 vs 43, 96f): blob-band (2-16px)
  correlation +0.97 wall / +0.91 bowl -> the mottle is DETERMINISTIC, not seeded-RNG
  residue. rngSeed wiring verified correct (SceneBindings.cpp:406). Remaining
  deterministic input: the seed-independent frameIndex-Halton jitter path + converged
  filter residue; no cheap knob tests it.
- **DECISIVE: DLSS-RR renders the SAME raw signals with the planter bowl SMOOTH WHITE
  and the wall panels CLEAN + streaky — visually matching ovrtx almost exactly where
  the user complained** (crop_planter_rr.png / crop_wall_rr.png: ovrtx | builtin | RR).
  The mottle is hand-filter (ReLAX-class) residue character; ovrtx's smoothness IS its
  neural denoiser, and ours reproduces it now that RR is unblocked. RR whole-frame
  metric stays 0.09293 (converged tone/level diverges elsewhere — the known structural
  bias), so: **builtin profile = the metric champion (0.07883); RR profile = the
  character/look match for the viewer.** There is no hand-filter tuning path to RR's
  phase-coherent smoothness — that thread is CLOSED; do not spend more renders on
  atrous knobs chasing it.

## Session 2026-07-11 (cont. 4) — blob forensics round 2 (user 3rd push); aniso filtering shipped

Method upgrade after the user's third noise report: full-pipeline SIGNAL-ISOLATION renders
(one passMask signal + denoise + accum + SHARC at a time) correlated per-region against
the full image's blob field (2-16px bandpass) — fixes round 1's flaw (raw renders, 1px
stats). Results:
- direct-only +0.89 / indirect+AO +0.89 / reflections +0.40 / translucency +0.32 (wall)
  -> every diffuse term carries the same pattern; common factors: x albedo (composite)
  and the SHARED filter chain + guides (residue lands in the same places).
- Our albedo AOV's own blob field correlates +0.79 with the final on the wall; final amp
  0.0444 vs albedo 0.0255 — but ovrtx amplifies its albedo the same way (0.0301 ->
  0.0458). NOT an anomaly.
- Demod round-trip: MIN_DEMODULATION_ALBEDO 0.04 -> 4.0 (constant demod) = byte-flat
  blob band. MEASURED NON-LEVER, annotated in shading.slang.
- **FINAL-image blob amplitude vs ovrtx: wall 0.0444/0.0458, bowl 0.0249/0.0248,
  ceiling 0.0633/0.0648 = PARITY. Column 0.0361/0.0288 = the one +25% excess**, and it
  proved LIGHTING-residue: sampler aniso 16 (null) and planar-projection elliptical
  gradients (null on column, +0.0006) both left it unchanged.
- Shipped f4f92bb anyway (correctness): material sampler maxAnisotropy 1 -> 16 (the
  elliptical SampleGrad footprints finally do hardware aniso) + the planarProj gradient
  branch's isotropic-circle proxy replaced with the true view-stretched ellipse
  projected onto the projection-plane axes. Metric-neutral; matches ovrtx filtering by
  construction. (The 2026-07-08 'aniso 16 brightened' note did not reproduce.)

BOTTOM LINE (three rounds, every hypothesis measured): our blob AMPLITUDE equals
ovrtx's everywhere that matters; the perceptual 'noisy/non-uniform' = PHASE COHERENCE
of hand-filter residue vs their neural denoiser — reproduced exactly by our DLSS-RR
path (planter/wall crops). The builtin chain has no remaining measured smoothness
lever; further tuning renders on it for THIS purpose is waste. Profile guidance:
builtin = metric/regression champion (0.07901); denoiser=dlss (RR) = the ovrtx look.

## Session 2026-07-11 (cont. 5) — owner 7-item diff review: outcomes

Item-by-item (owner: "tackle one by one"):
1. WINDOWS — DONE (ee23566): maxExposedLuminance knob (v6.5.0), ovrtx's measured
   pre-tonemap bound; pinned 2.5 (windows-optimal; lower trades windows for other
   highlights). Residual = interior/sky transport ratio (their panes sit BELOW their
   own cap) + post-bloom self-veil.
2. MIDDLE TABLE — DONE (fc5467d): brightness was reflected-fixture energy; fixed by
   emissiveScale 0.5 (id40 'better') on top of reflectionSamples 4. reflSamples 8
   measured -0.00007 = diminishing (profile stays 4).
3. TOP LIGHTS — DONE (fc5467d): emissiveScale (v6.6.0, LAST RealTimeQuality reserved
   slot; RealTimeQualityUniforms 48->64B): bulbs render warm ((0.85,0.79,0.71) at
   ovrtx's hottest-pixel mask vs their (0.93,0.90,0.83); was washed white). Applied at
   pbr.emission's single fill site => direct + reflections + GI + cache all scale.
4. BLACK MIDDLE (plaque-fold pocket) — DOCUMENTED LIMITATION (83eb879): SHARC pocket
   fixed-point under-convergence (crease 0.242 cache vs 0.392 builtin vs 0.585 ovrtx);
   accum-cap 128 = non-lever (crease flat, whole-frame +0.0006); AO/normals eliminated.
   Future fix designed: confidence-weighted cache/bootstrap blend (per-cell counts).
5. WINDOW PILLAR — AT PARITY after today's fixes (dRGB <= 0.02, blob band within
   +-15%): no action needed beyond items 1/3's improvements.
6. LEFT DOORS — PARTIAL/DOCUMENTED: two components: (a) ovrtx's glass GREEN EDGE TINT
   = MDL thickness-based absorption (Beer-Lambert), a real feature (queued design);
   (b) gold-frame saturation = the parked conductor-color tail (F82 per-face data on
   file).
7. LAMP FLOOR REFLECTION — ROOT-CAUSED, fix reverted as measured-worse: the ovrtx
   streak = glossy floor reflecting the brushed-chrome stem which itself mirrors the
   window = MULTI-BOUNCE GLOSSY specular. Tried single-level mirror-metal continuation
   in ReflectionsPass (raygen-traced, SHARC-skip for conductors): streak did NOT
   materialize (stem roughness > the 0.15 mirror gate) and chrome glints regressed
   whole-frame +0.00061 (id40). Honest fix = roughness-cone continuation (glossy
   multi-bounce) — queued as the same feature class as item 6a. The earlier
   deterministic reflection-hit AO (7ea8b85) remains item 7's shipped improvement.

Baseline after the campaign: base.exr/base_config = 895/builtin/96f/ev-0.85/
soften 0.5/bloom 0.10/reflSamples 4/cap 2.5/emissiveScale 0.5 = **0.07641**
(campaign 0.15959 -> 0.07641, -52.1%). Queued features from this review:
confidence-blend SHARC query; glass thickness absorption; glossy-cone reflection
continuation; conductor F82 color tail.

## Session 2026-07-11 (cont. 6) — SHARC confidence-blend: measured null, diagnosis corrected

Implemented the queued item-4 fix (SharcQuery confidence from resolved.w frameWeight,
lerp(bootstrap, cached, conf) at query sites) and measured THREE configurations:
- All three query sites: whole-frame +0.0126 CATASTROPHIC (id30 floor +0.064 luma) —
  the reflected/through-glass world DEPENDS on rarely-updated cells; the unconditional
  substitution at reflections/translucency is the tuned behavior. Do not blend there.
- Indirect-depth>=2 only: whole-frame +0.0004, crease core UNCHANGED (0.238 vs 0.242).
**DIAGNOSIS CORRECTED: the pocket cells are NOT starved** — every camera-visible crease
pixel updates them each frame, frameWeight caps out, confidence = 1, the blend never
engages. The pocket's dark cache value is a CONVERGED-BUT-BIASED fixed point: the
recursion (cell ~ direct + albedo x cells) loses energy in the pocket relative to true
transport. Remaining suspects for the bias: normal-octant keying blocking cross-octant
energy inside the fold (the facing curl surfaces live in OPPOSITE octants and cannot
borrow each other's light), and the octant-averaged trilinear neighbourhood. Reverted
in full; item 4 stays a documented limitation with this sharper root cause. Next
investigation angle when resumed: octant-blended query (sample both the surface octant
and its opposite, cosine-weighted) at pocket-like curvature.
