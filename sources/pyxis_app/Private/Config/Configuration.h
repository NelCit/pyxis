// Pyxis app — Configuration POD + loader.
//
// Plan §26 / §27. Mirrors the parameters.json schema 1:1; all fields
// the application code consumes flow through this struct. M2 ships the
// minimum subset needed by HeadlessMode (render dims + seed, output
// paths, diagnostics, limits.framesInFlight); M3+ grows the
// scene / textures / geometry / hydra / profiling sections in lockstep
// with the corresponding subsystems.
//
// The loader is exception-free: nlohmann::json's allow_exceptions=false
// parse mode keeps us compatible with /EHs-c- across pyxis_app.

#pragma once

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>

namespace pyxis::app {

struct CliArgs;  // CliArgs.h forward — the loader applies overrides.

// ----- §27.render --------------------------------------------------------
struct RenderConfig {
  uint32_t width = 1920;
  uint32_t height = 1080;
  uint32_t samplesPerFrame = 1;
  // RNG seed (§12 PCG32 + §33.7 determinism). Required to be non-zero
  // for headless EXR — a zero seed defeats the byte-identical contract
  // and is rejected at validation time.
  uint32_t seed = 1;
  // Photographic exposure in STOPS, applied on TOP of any
  // UsdGeomCamera.exposure the scene authors (added, then 2^stops in the
  // TonemapPass before ACES). USD/UsdLux author lights in physical units
  // (nits) paired with negative camera exposure to compress to display
  // range (M8a); scenes that author neither a camera nor exposure — e.g.
  // the OpenPBR Playground, whose area lights run 400..1500 — clip correct
  // albedos to white at the default 0. This headless knob supplies the
  // compensation the viewer's Exposure slider gives interactively. Default
  // 0 = no change (byte-equal for existing goldens, which all author 0).
  float exposure = 0.0f;
  // Auto-exposure (AutoExposurePass). When true, the renderer derives the
  // exposure from the frame's geometric-mean luminance so a scene with hot
  // lights + no authored camera exposure (the OpenPBR Playground) displays
  // without clipping to white; `exposure` above then rides on top as a bias.
  // OFF by default so existing goldens stay byte-equal. `autoExposureKey` is
  // the target middle-grey (0.18 = the photographic 18% card).
  bool  autoExposure = false;
  // The linear level the brightest pixel maps to before ACES (1.0 = max→white).
  float autoExposureKey = 1.0f;
  // RTX-alignment design (rtx-realtime-alignment-design.md), Phase C —
  // sub-mode when `autoExposure` above is true: false (default, schema
  // default) = the pre-Phase-C legacy clamped-max reduction; true = the
  // ovrtx-parity 64-bucket histogram (median filter). Kept as a separate
  // JSON key ("render.autoExposureHistogram") rather than widening
  // `autoExposure` itself into a 3-way enum, so existing
  // `"autoExposure": true/false` configs stay valid unchanged.
  // HeadlessMode / ViewerMode fold {autoExposure, autoExposureHistogram}
  // into the single RenderSettings::autoExposure uint32 (0=off / 1=on-legacy
  // / 2=on-histogram).
  bool  autoExposureHistogram = false;
  // RTX-alignment design, Phase B follow-up — headless temporal
  // convergence: render this many frames back-to-back in one renderer
  // session before writing the EXR (temporal denoiser + TAA history
  // accumulate across them; the RNG decorrelates per frameIndex). The
  // comparison harness renders NVIDIA's ovrtx reference at 128+ stepped
  // frames — this is the Pyxis-side equivalent. 1 (default) = the
  // pre-existing single-frame behaviour, byte-equal for all goldens.
  uint32_t accumulationFrames = 1;

  // RTX-alignment design (rtx-realtime-alignment-design.md), Phase C —
  // mirrors pyxis::RenderSettings::exposureMode / exposureResponsivity 1:1
  // (both live at the RenderSettings TOP level, not nested under
  // RealTimeQuality — see that struct's doc comment for why the physical-
  // camera exposure block ended up split across two §22.3 reserved tails).
  // 0 = Legacy (pre-Phase-C manual/auto exposure, byte-identical default),
  // 1 = PhysicalCamera (OmniRtxCameraExposureAPI parity — see
  // RealTimeQualityConfig::physicalCamera* below for the other four
  // exposure-model inputs).
  uint32_t exposureMode = 0u;
  float exposureResponsivity = 0.8821367311933349f;

  // RTX-alignment design (rtx-realtime-alignment-design.md), WP2-final —
  // mirrors pyxis::RenderSettings::RealTimeQuality 1:1 (JSON section
  // "render.realTimeQuality"); HeadlessMode / ViewerMode copy this
  // field-for-field into RenderSettings::realTimeQuality each run. See
  // that struct's doc comment (Public/Pyxis/Renderer/Descs/RenderSettings.h)
  // for which fields are live vs. Phase-B-reserved no-ops today.
  struct RealTimeQualityConfig {
    uint32_t passMask = 0x1Fu;
    uint32_t directSamples = 2u;
    uint32_t indirectSamples = 1u;
    uint32_t indirectMaxBounces = 2u;
    uint32_t reflectionSamples = 1u;
    float reflectionMaxRoughness = 0.3f;
    uint32_t refractionMaxBounces = 6u;
    // RTX-alignment design, Phase C — 0.35 m (35 ovrtx stage units == 35 cm
    // for World Lobby's metersPerUnit=0.01); see RenderSettings.h's
    // aoRayLength doc comment for the full unit-reconciliation trace.
    float aoRayLength = 0.35f;
    float maxRayIntensityDirect = 6400.0f;
    float maxRayIntensityIndirect = 6400.0f;
    float maxRayIntensityReflections = 19200.0f;
    // RTX-alignment design (rtx-realtime-alignment-design.md), Phase C —
    // mirrors pyxis::RenderSettings::RealTimeQuality's own Phase C fields.
    // tonemapOperator: TONEMAP_OPERATOR_* index (ShaderInterop.slang);
    // default 6 = AcesApproximation (byte-identical to pre-Phase-C).
    uint32_t tonemapOperator = 6u;
    // physicalCamera{FStop,Iso,ExposureTimeSeconds}: active only when
    // RenderConfig::exposureMode above is PhysicalCamera (1). Defaults
    // mirror ovrtx's OmniRtxCameraExposureAPI schema (fStop 5, iso 100,
    // time 1).
    float physicalCameraFStop = 5.0f;
    float physicalCameraIso = 100.0f;
    float physicalCameraExposureTimeSeconds = 1.0f;

    // DLSS Stage 1 (rtx-realtime-alignment-design.md, "DLSS — corrected
    // stance" + "DLSS scope includes upscaling") — mirrors
    // pyxis::RenderSettings::RealTimeQuality::{denoiser,dlssExecMode} 1:1
    // (DENOISER_*/DLSS_EXEC_MODE_* constants, RenderSettings.h). JSON
    // accepts either the string form ("dlss"/"builtin"/"off",
    // "auto"/"quality"/"balanced"/"performance"/"dlaa") or the raw integer
    // — see Configuration.cpp's ReadDenoiserField / ReadDlssExecModeField.
    // denoiser defaults to Dlss (matches RenderSettings' own default);
    // headless/golden configs don't author this key, so the probe's
    // Dlss->Builtin downgrade is the only outcome they ever see (§33.7 —
    // byte-identical regardless, since passMask's own denoise/TAA bits
    // already default OFF).
    uint32_t denoiser = 0u;        // 0=dlss, 1=builtin, 2=off
    uint32_t dlssExecMode = 4u;    // 0=auto,1=quality,2=balanced,3=performance,4=dlaa
                                   // (default DLAA — owner 2026-07-11; mirrors
                                   // RenderSettings::RealTimeQuality::dlssExecMode)

    // RTX-alignment 2026-07-10 ("image not smooth") — mirrors
    // pyxis::RenderSettings::RealTimeQuality::postSoftenSigma 1:1: optional
    // post-tonemap display-space Gaussian sigma in PIXELS. 0 (default)
    // disables the pass entirely (byte-identical output); the
    // ovrtx-alignment profile sets 0.5 (measured — see RenderSettings.h).
    float postSoftenSigma = 0.0f;

    // RTX-alignment 2026-07-11 ("window borders shadowed") — mirrors
    // pyxis::RenderSettings::RealTimeQuality::postBloomGain 1:1: optional
    // post-tonemap veiling-bloom gain (threshold/sigma are fixed measured
    // constants in post_bloom.slang). 0 (default) disables the pass
    // entirely; the ovrtx-alignment profile sets 0.10 (measured — see
    // RenderSettings.h).
    float postBloomGain = 0.0f;

    // RTX-alignment 2026-07-11 (item 1, "windows different") — mirrors
    // pyxis::RenderSettings::RealTimeQuality::maxExposedLuminance 1:1:
    // frame-wide composed-luminance cap in EXPOSED units. 0 (default)
    // disables it; the ovrtx-alignment profile sets 3.6 (their measured
    // pre-tonemap bound — see RenderSettings.h).
    float maxExposedLuminance = 0.0f;
  } realTimeQuality;

  // M3+ extensions: maxBounces, enableAccumulation, toneMap,
  // debugView, accumulationFrameLimit, russianRouletteStartBounce,
  // fireflyClampLuminance, lowDiscrepancySampling, aovs, ...
};

// ----- §27.output --------------------------------------------------------
struct OutputConfig {
  std::string image;            // EXR path (required when headless).
  std::string ldr;              // Optional PNG path; empty = skip.
  std::string effectiveConfig;  // Resolved-config JSON dump path.
};

// ----- §27.diagnostics ---------------------------------------------------
struct DiagnosticsConfig {
  bool validationLayer = false;
  bool aftermath = false;
};

// ----- §27.limits --------------------------------------------------------
struct LimitsConfig {
  // §33.1 cap is 3; the headless EXR path raises this from M2's default
  // back to 3 per §33.7 (byte-identical pinning).
  uint32_t framesInFlight = 1;
};

// ----- §27.paths ---------------------------------------------------------
// Filesystem inputs the app honours. M3.5 wires `scene` (the
// §29.4.a default-startup-scene chain reads this); M5+ adds
// `allowedRoots` for the §29.7 "Save Scene As USD" sandbox; M11+
// adds `pipelineCache` etc.
struct PathsConfig {
  // Empty = defer to the §29.4.a fallback chain (CLI > config >
  // recent_scenes > bundled default). Non-empty = explicit user
  // override; SceneResolver picks this if --scene wasn't passed.
  std::string scene;
};

// ----- §27.scene -----------------------------------------------------------
// RTX-alignment design (rtx-realtime-alignment-design.md), WP2-final. Scene-
// content selectors that aren't filesystem paths (those live in
// PathsConfig above).
struct SceneConfig {
  // Empty = auto (StageWalker's existing boundCamera-hint + first-in-
  // SdfPath-order fallback). Non-empty = the exact SdfPath of the
  // UsdGeomCamera to make active; --camera <sdfPath> / JSON
  // "scene.camera" both feed this. StageWalker warns + falls back to
  // auto when the path doesn't match any camera the stage authors.
  std::string camera;
};

// ----- §27.app -----------------------------------------------------------
// Application-wide knobs that don't fit the render / output / paths
// buckets. M4 adds `ingest`; M5+ may add `theme`, `language`, etc.
struct AppConfig {
  // §3 / §25.O. Selects which ingest adapter IngestUsd() drives at
  // startup:
  //   - "hydra"      → Hydra adapter (UsdImagingStageSceneIndex +
  //                    HdRenderIndex + HdPyxisRenderDelegate at M5+;
  //                    M4 stub wraps StageWalker for byte-equal
  //                    parity). Default — matches what DCCs (usdview,
  //                    Solaris, Maya-USD) drive when they pick Pyxis
  //                    through the Hd plugin registry.
  //   - "usd_direct" → Direct one-shot StageWalker, no Hydra dep.
  //                    Lighter for headless farm workers and CI.
  // Both adapters MUST produce byte-identical EXR output for the
  // same .usd input (§25.O.3 P0 invariant).
  std::string ingest = "hydra";
};

// ----- The whole tree ----------------------------------------------------
struct Configuration {
  RenderConfig render;
  OutputConfig output;
  DiagnosticsConfig diagnostics;
  LimitsConfig limits;
  PathsConfig paths;
  SceneConfig scene;
  AppConfig app;
  // M5+ sections (textures, geometry, hydra, profiling) land
  // alongside the systems that consume them.
};

// Overlay a parameters.json document onto an existing Configuration.
// Missing keys keep the previous value; present keys replace it
// (§29.1 "Each overlay is a key-by-key merge"). Returns the
// unexpected branch with a human-readable message on JSON / type
// errors.
[[nodiscard]] std::expected<void, std::string> OverlayConfiguration(
    Configuration& target, std::string_view jsonText) noexcept;

// Convenience: parse a parameters.json document into a fresh
// Configuration. Equivalent to `Configuration{}; OverlayConfiguration(...)`.
// Useful for tests that want a one-shot parse without the overlay
// chain.
[[nodiscard]] std::expected<Configuration, std::string> ParseConfiguration(
    std::string_view jsonText) noexcept;

// Resolve the full overlay chain (§29.1): C++ defaults (the
// Configuration{} field initialisers) -> <exe-dir>/Resources/
// parameters.default.json -> %LOCALAPPDATA%/Pyxis/parameters.json ->
// --config <path>, then ApplyCliOverrides. Returns the unexpected
// branch with a human-readable message if --config is supplied but
// can't be loaded; missing default / user files are silently skipped.
[[nodiscard]] std::expected<Configuration, std::string> ResolveConfiguration(
    const CliArgs& cli) noexcept;

// Overlay CLI args onto a parsed Configuration. Applied AFTER all
// JSON overlays, so CLI values take precedence (§27 "applied after
// parse, before validate"). Mutates `config` in place.
void ApplyCliOverrides(Configuration& config, const CliArgs& cli) noexcept;

// Validate a fully-resolved Configuration (after CLI overrides). Headless
// invocations must carry a non-zero seed and a non-empty output.image.
[[nodiscard]] std::expected<void, std::string> ValidateForHeadless(
    const Configuration& config) noexcept;

// Write the resolved Configuration back to disk at
// output.effectiveConfig. The parent directory is created first.
// Returns the unexpected branch with a human-readable reason on
// failure (call sites typically log it and continue — the EXR is the
// primary artefact, the effective-config dump is a sidecar).
[[nodiscard]] std::expected<void, std::string> WriteEffectiveConfig(
    const Configuration& config) noexcept;

}  // namespace pyxis::app
