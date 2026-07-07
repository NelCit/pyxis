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

**Conclusion:** SHARC works (runs, engages, converges, mean-preserving) but does
NOT move the honest metric — empirically confirming the gap characterisation
above. Why: (1) the dominant residuals (glass floor, satin tables, windows) are
SPECULAR/reflection surfaces; SHARC feeds only the indirect-DIFFUSE pass. (2) In a
dome-lit interior the indirect-diffuse term is a small energy fraction and the
infinite-bounce (3rd+) increment over the existing 2-bounce estimate is tiny. (3)
The residual is reflection dynamic-range + high-frequency content, which no
diffuse-irradiance cache touches. Shipped as an OPTIONAL mode (off by default);
the reflection-fidelity lever above remains the path toward 0.05.
