// Pyxis renderer — shared exposure-scale math (RTX-alignment design,
// rtx-realtime-alignment-design.md, "unexposed intensity" clamp fix).
//
// ovrtx's OmniRtxSettingsRtAPI/RtAdvancedAPI documents its per-signal firefly
// clamps (maxRayUnexposedIntensity: direct/indirect 6400, reflections/
// refractions 19200) as "automatically scaled with the exposure" — they cap
// the EXPOSED intensity (radiance x exposureScale), so the equivalent cap in
// Pyxis's own UNEXPOSED radiance space (where every RT pass's gQuality clamp
// actually fires — see direct_lighting.slang / indirect_diffuse.slang /
// reflections.slang) is `setting / exposureScale`. This header is the ONE
// place that formula lives, shared by TonemapPass::Execute (the display
// path's own exposure derivation, which this mirrors) and
// SceneBindings::Update (which descales RenderSettings::RealTimeQuality's
// maxRayIntensity* fields before uploading RealTimeQualityUniforms) — see
// each caller for its own citation. Never duplicate this formula a third
// time; add a caller instead.

#pragma once

#include <Pyxis/Renderer/Descs/CameraDesc.h>
#include <Pyxis/Renderer/Descs/RenderSettings.h>

#include <cmath>

namespace pyxis {

// The physical-camera "exposureScale" multiplier (OmniRtxCameraExposureAPI /
// UsdGeomCamera::ComputeLinearExposureScale parity) — 1.0 (no-op) in Legacy
// mode (RenderSettings::exposureMode == 0). Identical to the expression
// TonemapPass::Execute has always inlined for TonemapUniforms::exposureScale;
// factored out here so the display path and any other CPU-side consumer
// (below) share ONE definition instead of drifting copies. The `pow(2,
// exposure)` term of USD's own formula is intentionally NOT included here —
// see ComputeEffectiveExposureScale, which folds it in separately exactly as
// tonemap.slang's `exposureGain = exposureScale * exp2(effectiveExposure)`
// does.
[[nodiscard]] inline float ComputePhysicalCameraExposureScale(
    const RenderSettings& settings) noexcept {
  if (settings.exposureMode == 0u)
    return 1.0f;
  const RenderSettings::RealTimeQuality& rtq = settings.realTimeQuality;
  const float fStop = (rtq.physicalCameraFStop > 0.0f) ? rtq.physicalCameraFStop : 1.0f;
  return settings.exposureResponsivity * rtq.physicalCameraExposureTimeSeconds
       * (rtq.physicalCameraIso / 100.0f) / (fStop * fStop);
}

// The full PRE-RENDER-KNOWABLE exposure gain TonemapPass's COLOR path applies
// to unexposed linear radiance: `exposureScale * 2^cameraExposure` — the same
// `exposureGain` tonemap.slang computes when auto-exposure is OFF.
// `camera.exposure` already carries BOTH the USD camera's authored
// `exposure` attribute AND config.render.exposure stops added on top
// (HeadlessMode.cpp / ViewerMode.cpp fold them into ONE CameraDesc field
// before GpuScene ever sees it), so no separate "config stops" term is
// needed here.
//
// NOT auto-exposure-aware: when RenderSettings::autoExposure is enabled,
// TonemapPass's ACTUAL runtime exposureGain also folds in a histogram/
// highlights-derived `effectiveExposure` term computed from the frame's own
// rendered radiance — a value that does not exist yet when the earlier RT/
// lighting passes run (they need their firefly-clamp uniforms BEFORE the
// frame's radiance is known). Callers using this to descale a pre-render
// clamp (SceneBindings::Update) are therefore exact only with auto-exposure
// off; with it on, the clamp descales by the manual/legacy exposure term
// alone — a documented limitation (RTX-alignment design FIX 1), not a bug.
[[nodiscard]] inline float ComputeEffectiveExposureScale(const RenderSettings& settings,
                                                          const CameraDesc& camera) noexcept {
  return ComputePhysicalCameraExposureScale(settings) * std::exp2(camera.exposure);
}

}  // namespace pyxis
