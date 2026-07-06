// Pyxis renderer — RenderSettings POD (M3 subset).
//
// Plan §18.4 / §21. The M3 surface ships render resolution, the
// backbuffer clear colour, and the §29.5 feature-mask seed. The full
// path-trace knob set (samplesPerFrame, maxBounces, RR, firefly
// clamp, low-discrepancy sampling, tone-map operator, debug view,
// AOV mask) wires in progressively from M5 onward as each subsystem
// lands.

#pragma once

#include <cstdint>

namespace pyxis {

// DLSS Stage 1 (rtx-realtime-alignment-design.md, "DLSS — corrected
// stance" + "DLSS scope includes upscaling", owner 2026-07-05) —
// RenderSettings::RealTimeQuality::denoiser encoding. Named here (rather
// than an `enum class`, matching the raw-uint32-plus-doc-comment
// convention `tonemapOperator` already uses in this file) so Configuration
// / EditorPanel / PyxisRenderer share one source of truth instead of
// duplicating the 0/1/2 literals.
inline constexpr uint32_t DENOISER_DLSS = 0u;      // optional Streamline/NGX runtime (DEFAULT)
inline constexpr uint32_t DENOISER_BUILTIN = 1u;   // ReLAX/SIGMA chain + TAA (today's pipeline)
inline constexpr uint32_t DENOISER_OFF = 2u;       // raw noisy signals, no denoise, no TAA

// RenderSettings::RealTimeQuality::dlssExecMode encoding — mirrors
// omni:rtx:post:dlss:execMode 1:1 (ovrtx default = Auto).
inline constexpr uint32_t DLSS_EXEC_MODE_AUTO = 0u;
inline constexpr uint32_t DLSS_EXEC_MODE_QUALITY = 1u;
inline constexpr uint32_t DLSS_EXEC_MODE_BALANCED = 2u;
inline constexpr uint32_t DLSS_EXEC_MODE_PERFORMANCE = 3u;
inline constexpr uint32_t DLSS_EXEC_MODE_DLAA = 4u;

struct RenderSettings {
  uint32_t width = 1920;
  uint32_t height = 1080;

  // Backbuffer clear before any pass. Linear sRGB (§25.I.2 color
  // pipeline). M1 uses this to verify the swapchain present path
  // before any draw lands; a known non-black colour also makes
  // "did the swapchain even resize?" obvious in the viewer.
  float clearColor[4] = {0.05f, 0.05f, 0.06f, 1.0f};

  // §29.5 feature mask seed. M3 ships RaytracedLightingPass as the only
  // mandatory pass plus the ImGui overlay; the rest of the §29.5
  // toggle table (`accumulation`, `nee`, `mis`, `denoise`,
  // `toneMap`, `taa`, `motionVectors`, `aovs.*`) fills in at M5+.
  struct Features {
    bool imguiOverlay = true;  // PYXIS_DEBUG_TOOLS-gated
  } features;

  // M7 follow-up — viewer-driven AOV inspector + pixel picker. The
  // raygen reads these out of CameraUniforms each frame. Mirrors the
  // DEBUG_VIEW_* constants in resources/shaders/ShaderInterop.slang.
  // Headless mode leaves these at defaults (Color, no mouse hover)
  // so byte-equal regression artefacts stay stable.
  enum class DebugView : uint32_t {
    Color       = 0,   // post-tonemap radiance
    Normal      = 1,   // (n*0.5+0.5)
    Depth       = 2,   // 1/depth grayscale
    PrimId      = 3,   // hashed colour per slot — Hydra's HdAovTokens->primId
    MaterialId  = 4,   // hashed colour per material
    BaseColor   = 5,   // raw OpenPBR baseColor (pre-shading albedo)
    WorldPos    = 6,   // 10-unit-period fract of world hit position
    // Tier 1 Hydra-canonical AOVs — exposed as inspector views too so
    // the editor can sanity-check what Hydra delegates pull.
    Alpha       = 7,   // 1.0 on hit, 0.0 on miss (binary today)
    ElementId   = 8,   // hashed colour per face within a BLAS
    NormalEye   = 9,   // eye-space normal as (n*0.5+0.5)
    WorldPosEye = 10,  // sin-encoded eye-space position
  };
  DebugView debugView = DebugView::Color;

  // Mouse pixel for the picker. 0xFFFFFFFF on either axis = "no
  // hover"; raygen short-circuits the pick write.
  static constexpr uint32_t MOUSE_PIXEL_NONE = 0xFFFFFFFFu;
  uint32_t mousePixelX = MOUSE_PIXEL_NONE;
  uint32_t mousePixelY = MOUSE_PIXEL_NONE;

  // WorldPos AOV display period (scene units, typically meters).
  // The display transform in tonemap.slang encodes worldPos via
  // sin(p * 2pi / worldPosPeriod) so a smaller value gives finer
  // bands. 10 m is a sensible default for human-scale scenes;
  // crank to ~50 m for World Lobby-scale (or down to ~0.1 m for a unit
  // cube). 0 falls through to TonemapPass's default of 10.
  float worldPosPeriod = 10.0f;

  // Deterministic supersampling factor (SSAA). When > 1, the caller
  // renders the whole frame at `ssaaFactor`× the {width, height} per
  // axis (allocating super-res AOVs) and binds an output-resolution
  // `RenderTargets::colorResolved`; the renderer's SsaaResolvePass
  // box-downsamples the super-res color into it. 1 = off (no resolve;
  // the caller uses RenderTargets::color directly). Noise-free AA —
  // no jitter, no accumulation. Clamp to a sane ceiling (4) at the
  // call site; the renderer honours whatever it's given.
  uint32_t ssaaFactor = 1;

  // Q3 OpenPBR-complete (openpbr-complete-design.md "Control
  // surface") — bitmask gating the OpenPBR closure blocks of
  // openpbr_material.slang. Each bit selects a specialization-constant
  // pipeline variant (built lazily, cached); a bit that is OFF
  // produces EXACTLY the same image as that feature's weight being 0
  // on every material (gate-OFF == weight-0 semantics). Bits:
  //   bit 0 (0x01) — coat layer (GGX + absorption + darkening +
  //                  roughening)
  //   bit 1 (0x02) — fuzz (Zeltner LTC sheen)
  //   bit 2 (0x04) — transmission (translucent base)
  //   bit 3 (0x08) — subsurface (SSS-as-diffuse fallback)
  //   bit 4 (0x10) — anisotropy (anisotropic GGX)
  //   bit 5 (0x20) — energy-preserving (EON) diffuse; OFF = Lambert
  // Default 0x3F = all six ON — image-identical to the pre-toggle
  // renderer, so headless EXR goldens and §25.O.3 adapter parity stay
  // stable. The value is a literal because this public header cannot
  // include ShaderInterop.slang (§18.9 — the renderer is the only
  // consumer of the interop file); a static_assert in
  // PyxisRenderer.cpp pins it to shaderinterop::OPENPBR_FEATURES_ALL
  // so the two cannot drift.
  uint32_t openPbrFeatureMask = 0x3Fu;

  // Auto-exposure (AutoExposurePass) — consumes two §22.3 reserved tail slots
  // (no sizeof growth). When non-zero, the renderer reduces the frame's HDR
  // radiance to a geometric-mean luminance and TonemapPass derives an exposure
  // that lands that average on `autoExposureKey` (middle grey); the camera's
  // manual exposure then rides on top as a bias. Lets a scene with hot UsdLux
  // lights and no authored camera exposure (the OpenPBR Playground) display
  // without clipping to white. 0 (OFF) by default — headless EXR goldens and
  // §25.O.3 adapter parity stay byte-equal (and the reduction is integer, so
  // even ON is deterministic). Exposed via the viewer's ImGui editor + the
  // Omniverse Render Settings `pyxis:autoExposure` toggle. uint32 (not bool)
  // keeps the frozen-tail layout POD-clean.
  uint32_t autoExposure = 0u;
  // Auto-exposure target: the linear level the BRIGHTEST pixel maps to before
  // ACES (auto-exposure exposes for the highlights — autoEV = log2(target) −
  // maxLog2Lum — so nothing clips). 1.0 (max → white, ACES rolls the top off)
  // is a safe default; lower darkens, higher brightens. Ignored when
  // autoExposure is 0.
  float autoExposureKey = 1.0f;

  // RTX-alignment design (rtx-realtime-alignment-design.md), WP2-final —
  // mirrors ovrtx 0.3.0's own omni:rtx:rt:* applied-API-schema defaults
  // 1:1 (generatedSchema.usda), so Pyxis's Real-Time Quality knobs match
  // Omniverse RTX Real-Time's stock look byte-for-byte in intent. This is
  // a nested POD rather than spending RenderSettings' own two remaining
  // `_reserved` slots (below) — it doesn't fit in 8 bytes — hence the
  // owner-approved MAJOR version bump (version.txt 2.0.0 -> 3.0.0, §22).
  //
  // Live today: `passMask` gates whether PyxisRenderer::RenderFrame even
  // dispatches a given signal pass this frame (Execute() simply isn't
  // called when its bit is clear — the signal texture then holds
  // whatever it held last frame / at creation). `aoRayLength` and
  // `reflectionMaxRoughness` are uploaded into the shared SceneBindings
  // gQuality cbuffer (binding 15) and read by AmbientOcclusionPass /
  // ReflectionsPass respectively every frame. Every other field is
  // plumbed into that SAME cbuffer (so no further RenderSettings growth
  // is needed when Phase B lands) but read by no shader body yet — a
  // documented no-op, not a bug.
  struct RealTimeQuality {
    // Bit 0 direct, bit 1 indirect, bit 2 ao, bit 3 reflections, bit 4
    // translucency. All-on (0x1F) is image-identical to "always run
    // every signal pass" — the v1 default and the only combination the
    // headless goldens / §25.O.3 adapter parity exercise.
    uint32_t passMask = 0x1Fu;

    // Phase B (RIS sampled direct lighting) — no-op until the
    // DirectLightingPass estimator goes stochastic.
    uint32_t directSamples = 2u;
    // Phase B (cosine-weighted indirect bounce) — no-op until
    // IndirectDiffusePass traces real bounce rays.
    uint32_t indirectSamples = 1u;
    uint32_t indirectMaxBounces = 2u;
    // Phase B (GGX-importance-sampled reflections) — no-op until
    // ReflectionsPass goes multi-sample.
    uint32_t reflectionSamples = 1u;
    // LIVE — ReflectionsPass's roughness cutoff between a sharp
    // mirror-direction TraceRay and the roughness-proportional dome-mip
    // "rough fallback" (ovrtx's roughnessCacheThreshold default 0.3).
    float reflectionMaxRoughness = 0.3f;
    // Phase B (true refraction bounce budget) — no-op until
    // TranslucencyPass's segment loop reads this instead of its
    // compile-time MAX_TRANSPARENT_SEGMENTS spec constant (16).
    uint32_t refractionMaxBounces = 6u;
    // LIVE — AmbientOcclusionPass's hemispherical visibility-ray TMax,
    // in Pyxis's own METERS convention (StageWalker / HdPyxisRenderDelegate
    // bake each stage's metersPerUnit into the stage-to-world transform at
    // ingest, so all in-engine geometry — and therefore all ray lengths —
    // live in real-world meters regardless of how the source USD was
    // authored). ovrtx's OmniRtxSettingsRtAPI:rayLength default is 35 —
    // Phase C calibration (rtx-realtime-alignment-design.md) traced this
    // number: Kit's viewport UI (and the renderer setting itself) operates
    // in the CURRENT STAGE's own native units, not physical meters; for
    // World Lobby (metersPerUnit = 0.01, i.e. 1 stage unit = 1 cm — see the
    // stage's root layer metadata), "35" is 35 stage units = 35 cm = 0.35
    // real-world meters. The pre-Phase-C default (35.0) took the number
    // AS-AUTHORED and applied it directly as METERS — 100x too long for
    // this scene (an AO ray that reaches across most of the lobby instead
    // of a human-scale 35 cm contact/hemispherical occlusion test).
    // 0.35 is therefore the unit-correct default for World Lobby, Pyxis's
    // primary v1 target scene (plan §1); it is NOT scene-agnostic — a
    // differently-scaled stage would need a different constant, and v1 has
    // no per-scene metersPerUnit-aware settings rescaling (RenderSettings is
    // resolved once, before StageWalker reports the loaded stage's unit
    // scale back). Tracked as debt: a proper fix threads the ingested
    // scene's metersPerUnit into settings resolution so this (and every
    // other meters-authored knob) rescales per scene.
    float aoRayLength = 0.35f;
    // Phase B per-signal firefly clamps — no-op until each pass's
    // estimator can actually produce outlier fireflies to clamp
    // (today's Phase A estimators are each a single deterministic ray).
    float maxRayIntensityDirect = 6400.0f;
    float maxRayIntensityIndirect = 6400.0f;
    float maxRayIntensityReflections = 19200.0f;

    // RTX-alignment design (rtx-realtime-alignment-design.md), Phase C —
    // consumes this nested POD's own §22.3 reserved tail (was
    // `uint32_t _reserved[4]`; same total size, no sizeof growth). Tonemap
    // operator selector + the "core" physical-camera exposure knobs (mode /
    // fStop / iso / exposureTime); `exposureResponsivity` lives on the
    // OUTER RenderSettings::_reserved tail below (this nested POD's own
    // reserved budget — 16 bytes / 4 slots — was one field short of the
    // full 5-field physical-camera block; see that field's own comment for
    // the cross-reference). TonemapPass::Execute reads both halves and
    // assembles them into its own (freely-growable, private)
    // shaderinterop::TonemapUniforms cbuffer — these are NOT uploaded via
    // the shared SceneBindings gQuality cbuffer (RealTimeQualityUniforms),
    // which stays Phase B's/RT-signal-passes' domain.
    //
    // tonemapOperator mirrors ovrtx's OmniRtxTonemapAPI enum 1:1 (see
    // TONEMAP_OPERATOR_* in ShaderInterop.slang): raw=0, none=1,
    // reinhard=2, modifiedReinhard=3, hejiHableAlu=4, hableUc2=5,
    // acesApproximation=6 (default — byte-identical to the pre-Phase-C
    // Narkowicz ACES fit), iray=7.
    uint32_t tonemapOperator = 6u;
    // Physical-camera exposure (OmniRtxCameraExposureAPI parity) — active
    // only when RenderSettings::exposureMode (below) is PhysicalCamera.
    // fStop/iso mirror ovrtx's schema (generatedSchema.usda). exposureTime
    // is 0.02 s — the schema AUTHORS 1.0 but its own docstring records
    // "deprecated carb default value: 0.02", and ovrtx's measured output
    // level matches 0.02, verified against World Lobby captures: best-fit
    // display exposure 2^-10.5 ≈ 6.9e-4 vs the imaging equation
    // 0.8821·0.02·(100/100)/5² ≈ 7.06e-4. With 1.0, PhysicalCamera renders
    // ~50× brighter than Omniverse. See UsdGeomCamera's documented
    // ComputeLinearExposureScale for the formula these feed
    // (TonemapPass::Execute; cited there).
    float physicalCameraFStop = 5.0f;
    float physicalCameraIso = 100.0f;
    float physicalCameraExposureTimeSeconds = 0.02f;

    // DLSS Stage 1 (rtx-realtime-alignment-design.md, "DLSS — corrected
    // stance" + "DLSS scope includes upscaling", owner 2026-07-05) — this
    // nested POD's §22.3 reserved tail was fully consumed by the Phase C
    // fields above; these two fields grow sizeof(RealTimeQuality) (and
    // therefore sizeof(RenderSettings)) — version.txt bumped MAJOR
    // alongside this change, same rule `seed`'s addition used above.
    //
    // `denoiser` selects the whole denoise+TAA chain: DENOISER_DLSS
    // (DEFAULT, per owner) is an optional proprietary Streamline/NGX
    // runtime integration — PyxisRenderer's startup capability probe
    // (Private/Dlss/DlssProvider.h) downgrades it to DENOISER_BUILTIN with
    // a logged reason whenever the SDK isn't staged (the only case v1 ever
    // exercises — see that header). DENOISER_BUILTIN is today's ReLAX/
    // SIGMA denoiser chain + TaaPass (passMask bits 5/6 above, unchanged).
    // DENOISER_OFF forces both bits off regardless of passMask's own
    // authored value — raw per-signal noise straight into CompositePass.
    // Headless/golden configs don't author this field, so it stays at the
    // DEFAULT; since the DLSS SDK is never staged in CI the probe always
    // fails and effective-denoiser resolves to Builtin — byte-identical to
    // the pre-Stage-1 behaviour (§33.7), because passMask's own denoise/TAA
    // bits already default OFF (0x1F, see above) regardless.
    uint32_t denoiser = DENOISER_DLSS;
    // DLSS execution-mode preset — mirrors omni:rtx:post:dlss:execMode 1:1
    // (see DLSS_EXEC_MODE_* above). Stage 1: parsed, plumbed, logged;
    // consumed by nothing until Stage 2's two-resolution pipeline
    // (Streamline hookup) lands.
    uint32_t dlssExecMode = DLSS_EXEC_MODE_AUTO;

    // §22.3 reserved tail — the NEXT growth here (Stage 2's two-resolution
    // pipeline knobs, if they fit in 16 bytes) spends this instead of
    // another MAJOR bump.
    // NOLINTNEXTLINE(readability-identifier-naming)
    uint32_t _reserved[4]{};
  } realTimeQuality;

  // RTX-alignment design (rtx-realtime-alignment-design.md), Phase C —
  // consumes RenderSettings' own §22.3 reserved tail (was
  // `uint32_t _reserved[2]`; same total size, no sizeof growth).
  //
  // exposureMode selects between Pyxis's pre-Phase-C manual/auto exposure
  // (Legacy, default — byte-identical output) and the physical-camera model
  // (PhysicalCamera): when PhysicalCamera, TonemapPass multiplies
  // linearColor by an exposureScale derived from
  // realTimeQuality.{physicalCameraFStop,physicalCameraIso,
  // physicalCameraExposureTimeSeconds} + exposureResponsivity (below)
  // BEFORE the existing manual-stops/auto-exposure gain, which still rides
  // on top unchanged. 0 = Legacy, 1 = PhysicalCamera.
  uint32_t exposureMode = 0u;
  // OmniRtxCameraExposureAPI's own note: "RTX needs 0.8821367311933349 to
  // match the USD result" — a per-camera/lens responsivity calibration
  // constant (UsdGeomCamera's exposure:responsivity), NOT a stops or
  // f-stop-like quantity, hence its own slot rather than folding into the
  // block above. See TonemapPass::Execute for the full
  // ComputeLinearExposureScale-derived formula.
  float exposureResponsivity = 0.8821367311933349f;

  // RTX-alignment design (rtx-realtime-alignment-design.md), Phase B —
  // "interior illumination" follow-up. The plan §12.1 PCG32 seed every
  // stochastic estimator (DirectLightingPass's RIS + the new dome-IBL
  // term, IndirectDiffusePass's cosine bounce, AmbientOcclusionPass,
  // ReflectionsPass) folds into its SeedPcg32(...) call — see rng.slang.
  // Mirrors Configuration::RenderConfig::seed (pyxis_app, private — §33.7
  // requires non-zero in headless) so the app's --seed / render.seed
  // config actually reaches the shaders instead of the fixed
  // SceneBindings.cpp DEFAULT_RNG_SEED every stochastic pass used until
  // now (previously tracked as debt in RtxSamplingUniforms's own doc
  // comment, ShaderInterop.slang). 1 (matches DEFAULT_RNG_SEED /
  // Configuration's own default) keeps every existing default-seed render
  // byte-identical.
  //
  // NOTE (§22 ABI): RenderSettings' reserved tail is fully consumed (see
  // exposureMode/exposureResponsivity above) — there is no trailing
  // `_reserved` slot left to grow this field into "for free". Adding it
  // grows sizeof(RenderSettings), which is a MAJOR break per §22's frozen-
  // POD rule; version.txt is bumped 3.1.0 -> 4.0.0 alongside this change.
  // The owner has waived the §44 RFC process for the whole RTX-alignment
  // design effort (see the design doc's header), but this specific ABI
  // growth is flagged here for a reviewer to confirm at PR time rather
  // than folded in silently.
  uint32_t seed = 1u;
};

}  // namespace pyxis
