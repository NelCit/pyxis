// Pyxis renderer — DlssProvider per-frame path (DLSS Stage 2a):
// GetOptimalRenderResolution / Evaluate / ReleaseResources. See
// DlssProvider.h for the class overview and DlssProviderImpl.h for why
// this is a second .cpp sharing the same class.

#include "Dlss/DlssProvider.h"

#include "Dlss/DlssProviderImpl.h"

#include <Pyxis/Platform/Logging/Log.h>
#include <Pyxis/Platform/Logging/LogCategories.h>

#include <Pyxis/Renderer/Descs/RenderSettings.h>  // DLSS_EXEC_MODE_* constants

#include <cmath>
#include <cstring>
#include <string>

namespace pyxis {

namespace {

// Maps RenderSettings::RealTimeQuality::dlssExecMode (RenderSettings.h)
// onto sl::DLSSMode. There is no native "Auto" DLSSMode enumerator —
// Omniverse's own omni:rtx:post:dlss:execMode="auto" is an
// application-level heuristic layered on top of Streamline, not a
// Streamline concept. This mirrors NVIDIA's publicly documented
// resolution -> mode guidance (4K -> Performance, 1440p -> Balanced,
// 1080p and below -> Quality) — a specific, documented integration
// choice, not a value taken from an internal Omniverse table this
// integration doesn't have access to.
sl::DLSSMode ResolveDlssMode(uint32_t dlssExecMode, uint32_t displayHeight) noexcept {
  switch (dlssExecMode)
  {
    case DLSS_EXEC_MODE_QUALITY: return sl::DLSSMode::eMaxQuality;
    case DLSS_EXEC_MODE_BALANCED: return sl::DLSSMode::eBalanced;
    case DLSS_EXEC_MODE_PERFORMANCE: return sl::DLSSMode::eMaxPerformance;
    case DLSS_EXEC_MODE_DLAA: return sl::DLSSMode::eDLAA;
    case DLSS_EXEC_MODE_AUTO:
    default:
      if (displayHeight >= 2160u)
        return sl::DLSSMode::eMaxPerformance;
      if (displayHeight >= 1440u)
        return sl::DLSSMode::eBalanced;
      return sl::DLSSMode::eMaxQuality;
  }
}

// sl::float4x4 is a plain `float row[4][4]`-shaped row-major matrix
// (sl_consts.h); `src` is a 16-value row-major flattening (hlslpp::store's
// own layout — see SceneBindings.cpp's identical convention), so this is
// a straight memcpy, no transpose.
sl::float4x4 ToSlMatrix(const float* src) noexcept {
  sl::float4x4 m{};
  if (src != nullptr)
    std::memcpy(&m.row[0], src, sizeof(float) * 16);
  return m;
}

sl::Resource ToSlResource(const DlssProvider::TaggedImage& image) noexcept {
  sl::Resource resource{sl::ResourceType::eTex2d, image.image, /*memory*/ nullptr, image.imageView,
                        image.layout};
  resource.width = image.width;
  resource.height = image.height;
  resource.nativeFormat = image.format;
  resource.mipLevels = 1;
  resource.arrayLayers = 1;
  return resource;
}

// ---- 6.0 PROVIDE COMMON CONSTANTS (ProgrammingGuideDLSS.md) -------------
// Shared verbatim by Evaluate() (SR, kFeatureDLSS) and EvaluateRR() (DLSS
// Stage 2b, kFeatureDLSS_RR) — both features consume the SAME per-frame
// sl::Constants shape via the SAME slSetConstants entry point
// (ProgrammingGuideDLSS_RR.md 6.0: "Various per frame camera related
// constants are required by ALL Streamline features"). Pulled out of
// Evaluate()'s body (DLSS Stage 2a) unchanged, parameterized on primitives
// instead of FrameInputs so EvaluateRR()'s own FrameInputsRR can share it
// without the two structs needing to be the same type.
sl::Constants BuildCommonConstants(uint32_t renderWidth, uint32_t renderHeight,
                                   const float* cameraViewToClip, const float* clipToCameraView,
                                   const float* clipToPrevClip, const float* prevClipToClip,
                                   float jitterX, float jitterY, const float cameraPos[3],
                                   const float cameraUp[3], const float cameraRight[3],
                                   const float cameraFwd[3], float cameraNear, float cameraFar,
                                   bool orthographic, bool reset) noexcept {
  sl::Constants constants{};
  constants.cameraViewToClip = ToSlMatrix(cameraViewToClip);
  constants.clipToCameraView = ToSlMatrix(clipToCameraView);
  constants.clipToPrevClip = ToSlMatrix(clipToPrevClip);
  constants.prevClipToClip = ToSlMatrix(prevClipToClip);
  constants.jitterOffset = {jitterX, jitterY};
  // gMotionVector is pixel-space (RenderTargets.h: "screen-space motion
  // vector, in PIXELS") — ProgrammingGuideDLSS.md 10.0's troubleshooting
  // section is explicit about this scale for pixel-space motion vectors.
  constants.mvecScale = {renderWidth > 0u ? 1.0f / static_cast<float>(renderWidth) : 0.0f,
                        renderHeight > 0u ? 1.0f / static_cast<float>(renderHeight) : 0.0f};
  constants.cameraPos = {cameraPos[0], cameraPos[1], cameraPos[2]};
  constants.cameraUp = {cameraUp[0], cameraUp[1], cameraUp[2]};
  constants.cameraRight = {cameraRight[0], cameraRight[1], cameraRight[2]};
  constants.cameraFwd = {cameraFwd[0], cameraFwd[1], cameraFwd[2]};
  constants.cameraNear = cameraNear;
  constants.cameraFar = cameraFar;
  // cameraFOV / cameraAspectRatio have NO "Optional -" prefix in
  // sl_consts.h (unlike cameraPinholeOffset / clipToLensClip / etc.) and
  // default to sl::INVALID_FLOAT (a 3.4e38 sentinel) if left unset --
  // empirically, leaving them unset produced
  // slEvaluateFeature/eErrorMissingInputParameter on this machine.
  // projYY (index 5 = row1,col1 of the row-major-flattened conformed
  // cameraViewToClip) is `1/tan(fovY/2)` for a standard perspective
  // projection — the SAME element SceneBindings.cpp's own
  // pixelSpreadRadians derivation reads (see its Update()).
  if (cameraViewToClip != nullptr)
  {
    const float projYY = cameraViewToClip[5];
    constants.cameraFOV = (projYY > 1e-6f) ? (2.0f * std::atan(1.0f / projYY)) : constants.cameraFOV;
  }
  constants.cameraAspectRatio =
      renderHeight > 0u
          ? static_cast<float>(renderWidth) / static_cast<float>(renderHeight)
          : constants.cameraAspectRatio;
  // gViewZ is tagged as kBufferTypeLinearDepth (see ToSlResource's caller
  // in DlssPass.cpp for the full citation), so `depthInverted` describes
  // the LINEAR value's own sense: larger == farther, i.e. NOT inverted.
  constants.depthInverted = sl::Boolean::eFalse;
  // Pyxis's gMotionVector already reprojects the FULL camera transform
  // (SceneBindings' prevClipFromWorld chain includes camera motion, not
  // just per-object motion) — see raytraced_gbuffer.slang's motion-vector
  // block.
  constants.cameraMotionIncluded = sl::Boolean::eTrue;
  constants.motionVectors3D = sl::Boolean::eFalse;
  constants.reset = reset ? sl::Boolean::eTrue : sl::Boolean::eFalse;
  constants.orthographicProjection = orthographic ? sl::Boolean::eTrue : sl::Boolean::eFalse;
  constants.motionVectorsDilated = sl::Boolean::eFalse;
  // raytraced_gbuffer.slang computes the motion vector from the UNJITTERED
  // pixel center (`float2(launchIndex) + 0.5f`), not the jittered primary-
  // ray sample — see DlssProvider.h's FrameInputs doc comment.
  constants.motionVectorsJittered = sl::Boolean::eFalse;
  return constants;
}

}  // namespace

DlssProvider::RenderResolution DlssProvider::GetOptimalRenderResolution(
    uint32_t displayWidth, uint32_t displayHeight, uint32_t dlssExecMode) noexcept {
  if (!_availability.usable || !_impl || _impl->slDLSSGetOptimalSettings == nullptr)
    return {};
  if (displayWidth == 0u || displayHeight == 0u)
    return {};

  sl::DLSSOptions options{};
  options.mode = ResolveDlssMode(dlssExecMode, displayHeight);
  options.outputWidth = displayWidth;
  options.outputHeight = displayHeight;

  sl::DLSSOptimalSettings settings{};
  const sl::Result result = _impl->slDLSSGetOptimalSettings(options, settings);
  if (result != sl::Result::eOk || settings.optimalRenderWidth == 0u
      || settings.optimalRenderHeight == 0u)
  {
    Logging::Get().Info(log::RENDER,
                        "DlssProvider: slDLSSGetOptimalSettings failed (sl::Result="
                            + std::to_string(static_cast<int>(result))
                            + "); falling back to native resolution");
    return {};
  }
  return RenderResolution{settings.optimalRenderWidth, settings.optimalRenderHeight};
}

bool DlssProvider::Evaluate(const FrameInputs& inputs) noexcept {
  if (!_availability.usable || !_impl)
    return false;
  auto& log = Logging::Get();

  // ProgrammingGuideDLSS.md 3.0 -- one frame token per frame, obtained
  // fresh each call ("Call this ONCE per frame").
  sl::FrameToken* frameToken = nullptr;
  const auto frameIndex = static_cast<uint32_t>(inputs.frameIndex & 0xFFFFFFFFu);
  const sl::Result tokenResult = _impl->slGetNewFrameToken(frameToken, &frameIndex);
  if (tokenResult != sl::Result::eOk || frameToken == nullptr)
  {
    log.Info(log::RENDER, "DlssProvider: Evaluate — slGetNewFrameToken failed (sl::Result="
                              + std::to_string(static_cast<int>(tokenResult)) + ")");
    return false;
  }

  // Reset the temporal history on the first-ever evaluate or whenever the
  // render resolution changes (both mean "no valid history for DLSS to
  // reproject") — same trigger set TaaPass's own _hasHistory /
  // EnsureHistory resize-detection uses.
  const bool resolutionChanged = (_impl->lastRenderWidth != inputs.renderWidth
                                  || _impl->lastRenderHeight != inputs.renderHeight);
  const bool reset = inputs.reset || !_impl->hasEvaluatedOnce || resolutionChanged;
  _impl->lastRenderWidth = inputs.renderWidth;
  _impl->lastRenderHeight = inputs.renderHeight;

  // ---- 6.0 PROVIDE COMMON CONSTANTS (ProgrammingGuideDLSS.md) ----------
  const sl::Constants constants = BuildCommonConstants(
      inputs.renderWidth, inputs.renderHeight, inputs.cameraViewToClip, inputs.clipToCameraView,
      inputs.clipToPrevClip, inputs.prevClipToClip, inputs.jitterX, inputs.jitterY, inputs.cameraPos,
      inputs.cameraUp, inputs.cameraRight, inputs.cameraFwd, inputs.cameraNear, inputs.cameraFar,
      inputs.orthographic, reset);

  const sl::Result constantsResult = _impl->slSetConstants(constants, *frameToken, _impl->viewport);
  if (constantsResult != sl::Result::eOk)
  {
    log.Info(log::RENDER, "DlssProvider: Evaluate — slSetConstants failed (sl::Result="
                              + std::to_string(static_cast<int>(constantsResult)) + ")");
    return false;
  }

  // ---- 5.0 PROVIDE DLSS OPTIONS ----------------------------------------
  sl::DLSSOptions options{};
  options.mode = ResolveDlssMode(inputs.dlssExecMode, inputs.displayHeight);
  options.outputWidth = inputs.displayWidth;
  options.outputHeight = inputs.displayHeight;
  options.colorBuffersHDR = sl::Boolean::eTrue;  // colorIn is fp32 LINEAR HDR (CompositePass output).
  // ProgrammingGuideDLSS.md 4.0: "If sl::kBufferTypeExposure is NOT
  // provided or dlssOptions.useAutoExposure is set to be true then DLSS
  // will be in auto-exposure mode" -- v1 doesn't tag kBufferTypeExposure
  // (scope cut), so this MUST be true or DLSS has no exposure signal at
  // all (empirically: eFalse + no tag produced
  // slEvaluateFeature/eErrorMissingInputParameter on this machine).
  options.useAutoExposure = sl::Boolean::eTrue;
  options.alphaUpscalingEnabled = sl::Boolean::eFalse;
  const sl::Result optionsResult = _impl->slDLSSSetOptions(_impl->viewport, options);
  if (optionsResult != sl::Result::eOk)
  {
    log.Info(log::RENDER, "DlssProvider: Evaluate — slDLSSSetOptions failed (sl::Result="
                              + std::to_string(static_cast<int>(optionsResult)) + ")");
    return false;
  }

  // ---- 4.0 TAG ALL REQUIRED RESOURCES ----------------------------------
  sl::Resource colorInRes = ToSlResource(inputs.colorIn);
  sl::Resource colorOutRes = ToSlResource(inputs.colorOut);
  sl::Resource depthRes = ToSlResource(inputs.depth);
  sl::Resource mvecRes = ToSlResource(inputs.mvec);

  const sl::Extent renderExtent{0, 0, inputs.renderWidth, inputs.renderHeight};
  const sl::Extent displayExtent{0, 0, inputs.displayWidth, inputs.displayHeight};

  sl::ResourceTag tags[] = {
      sl::ResourceTag{&colorInRes, sl::kBufferTypeScalingInputColor,
                      sl::ResourceLifecycle::eOnlyValidNow, &renderExtent},
      sl::ResourceTag{&colorOutRes, sl::kBufferTypeScalingOutputColor,
                      sl::ResourceLifecycle::eOnlyValidNow, &displayExtent},
      // kBufferTypeDepth (NOT kBufferTypeLinearDepth) -- confirmed via
      // slGetFeatureRequirements(kFeatureDLSS).requiredTags on this
      // machine (DlssProvider::Initialize's log line lists tag id 0 ==
      // kBufferTypeDepth as required; 49 == kBufferTypeLinearDepth is
      // NOT in that list). `inputs.depth` is DlssPass's own
      // dlss_depth_convert.slang output (standard normalized-device
      // depth), not raw gViewZ -- see that shader's file comment.
      // eValidUntilPresent: depth/mvec are the SAME owned textures every
      // frame (not transient scratch), matching ProgrammingGuideDLSS.md
      // 4.0's own sample tagging for these two.
      sl::ResourceTag{&depthRes, sl::kBufferTypeDepth, sl::ResourceLifecycle::eValidUntilPresent,
                      &renderExtent},
      sl::ResourceTag{&mvecRes, sl::kBufferTypeMotionVectors,
                      sl::ResourceLifecycle::eValidUntilPresent, &renderExtent},
  };
  const sl::Result tagResult = _impl->slSetTagForFrame(
      *frameToken, _impl->viewport, tags, static_cast<uint32_t>(std::size(tags)),
      inputs.vkCommandBuffer);
  if (tagResult != sl::Result::eOk)
  {
    log.Info(log::RENDER, "DlssProvider: Evaluate — slSetTagForFrame failed (sl::Result="
                              + std::to_string(static_cast<int>(tagResult)) + ")");
    return false;
  }

  // ---- 7.0 ADD DLSS TO THE RENDERING PIPELINE --------------------------
  const sl::BaseStructure* evaluateInputs[] = {&_impl->viewport};
  const sl::Result evalResult =
      _impl->slEvaluateFeature(sl::kFeatureDLSS, *frameToken, evaluateInputs,
                               static_cast<uint32_t>(std::size(evaluateInputs)),
                               inputs.vkCommandBuffer);
  if (evalResult != sl::Result::eOk)
  {
    log.Info(log::RENDER, "DlssProvider: Evaluate — slEvaluateFeature failed (sl::Result="
                              + std::to_string(static_cast<int>(evalResult)) + ")");
    return false;
  }

  _impl->hasEvaluatedOnce = true;
  _impl->resourcesEverEvaluated = true;
  _impl->lastEvaluatedFeature = sl::kFeatureDLSS;
  return true;
}

DlssProvider::RenderResolution DlssProvider::GetOptimalRenderResolutionRR(
    uint32_t displayWidth, uint32_t displayHeight, uint32_t dlssExecMode) noexcept {
  if (!_rrAvailability.usable || !_impl || _impl->slDLSSDGetOptimalSettings == nullptr)
    return {};
  if (displayWidth == 0u || displayHeight == 0u)
    return {};

  // sl_dlss_d.h's PFun_slDLSSDGetOptimalSettings signature takes
  // sl::DLSSDOptions (not the plain sl::DLSSOptions the guide's own code
  // sample shows) — see GetOptimalRenderResolutionRR's declaration comment
  // in DlssProvider.h. Only mode/outputWidth/outputHeight matter for this
  // query; the rest of DLSSDOptions (world<->view matrices, HDR flag, ...)
  // is EvaluateRR()'s concern, not the optimal-settings query's.
  sl::DLSSDOptions options{};
  options.mode = ResolveDlssMode(dlssExecMode, displayHeight);
  options.outputWidth = displayWidth;
  options.outputHeight = displayHeight;

  sl::DLSSDOptimalSettings settings{};
  const sl::Result result = _impl->slDLSSDGetOptimalSettings(options, settings);
  if (result != sl::Result::eOk || settings.optimalRenderWidth == 0u
      || settings.optimalRenderHeight == 0u)
  {
    Logging::Get().Info(log::RENDER,
                        "DlssProvider: slDLSSDGetOptimalSettings failed (sl::Result="
                            + std::to_string(static_cast<int>(result))
                            + "); falling back to DLSS-SR's optimal settings");
    return {};
  }
  return RenderResolution{settings.optimalRenderWidth, settings.optimalRenderHeight};
}

bool DlssProvider::EvaluateRR(const FrameInputsRR& inputs) noexcept {
  if (!_rrAvailability.usable || !_impl)
    return false;
  auto& log = Logging::Get();

  // ProgrammingGuideDLSS_RR.md 3.0/7.0 -- same one-token-per-frame contract
  // as Evaluate() (SR); RR and SR never both evaluate in the same frame
  // (DlssPass picks exactly one path per Execute()), so sharing
  // hasEvaluatedOnce/lastRenderWidth/lastRenderHeight below is safe.
  sl::FrameToken* frameToken = nullptr;
  const auto frameIndex = static_cast<uint32_t>(inputs.frameIndex & 0xFFFFFFFFu);
  const sl::Result tokenResult = _impl->slGetNewFrameToken(frameToken, &frameIndex);
  if (tokenResult != sl::Result::eOk || frameToken == nullptr)
  {
    log.Info(log::RENDER, "DlssProvider: EvaluateRR — slGetNewFrameToken failed (sl::Result="
                              + std::to_string(static_cast<int>(tokenResult)) + ")");
    return false;
  }

  const bool resolutionChanged = (_impl->lastRenderWidth != inputs.renderWidth
                                  || _impl->lastRenderHeight != inputs.renderHeight);
  const bool reset = !_impl->hasEvaluatedOnce || resolutionChanged;
  _impl->lastRenderWidth = inputs.renderWidth;
  _impl->lastRenderHeight = inputs.renderHeight;

  // ---- 6.0 PROVIDE COMMON CONSTANTS (shared with Evaluate()/SR) --------
  const sl::Constants constants = BuildCommonConstants(
      inputs.renderWidth, inputs.renderHeight, inputs.cameraViewToClip, inputs.clipToCameraView,
      inputs.clipToPrevClip, inputs.prevClipToClip, inputs.jitterX, inputs.jitterY, inputs.cameraPos,
      inputs.cameraUp, inputs.cameraRight, inputs.cameraFwd, inputs.cameraNear, inputs.cameraFar,
      inputs.orthographic, reset);
  const sl::Result constantsResult = _impl->slSetConstants(constants, *frameToken, _impl->viewport);
  if (constantsResult != sl::Result::eOk)
  {
    log.Info(log::RENDER, "DlssProvider: EvaluateRR — slSetConstants failed (sl::Result="
                              + std::to_string(static_cast<int>(constantsResult)) + ")");
    return false;
  }

  // ---- 5.0 PROVIDE DLSS + DLSS-RR OPTIONS ------------------------------
  // ProgrammingGuideDLSS_RR.md 5.0 NOTEs: colorBuffersHDR MUST be eTrue
  // (RR only supports HDR inputs); sharpness/useAutoExposure (plain
  // DLSSOptions fields, not present on DLSSDOptions at all) are ignored by
  // RR, so they're simply absent here. normalRoughnessMode = ePacked
  // matches gNormalRoughness's own layout (xyz normal, w roughness — see
  // raytraced_gbuffer.slang) exactly, same as the guide's own sample.
  // Presets left at DLSSDPreset::eDefault (struct default) -- v1 scope
  // cut, no per-quality-mode preset tuning yet.
  sl::DLSSDOptions options{};
  options.mode = ResolveDlssMode(inputs.dlssExecMode, inputs.displayHeight);
  options.outputWidth = inputs.displayWidth;
  options.outputHeight = inputs.displayHeight;
  options.colorBuffersHDR = sl::Boolean::eTrue;
  options.normalRoughnessMode = sl::DLSSDNormalRoughnessMode::ePacked;
  options.worldToCameraView = ToSlMatrix(inputs.worldToCameraView);
  options.cameraViewToWorld = ToSlMatrix(inputs.cameraViewToWorld);
  options.alphaUpscalingEnabled = sl::Boolean::eFalse;
  const sl::Result optionsResult = _impl->slDLSSDSetOptions(_impl->viewport, options);
  if (optionsResult != sl::Result::eOk)
  {
    log.Info(log::RENDER, "DlssProvider: EvaluateRR — slDLSSDSetOptions failed (sl::Result="
                              + std::to_string(static_cast<int>(optionsResult)) + ")");
    return false;
  }

  // ---- 4.0 TAG ALL REQUIRED RESOURCES (ProgrammingGuideDLSS_RR.md §4.1) -
  sl::Resource colorInRes = ToSlResource(inputs.colorIn);
  sl::Resource colorOutRes = ToSlResource(inputs.colorOut);
  sl::Resource depthRes = ToSlResource(inputs.depth);
  sl::Resource mvecRes = ToSlResource(inputs.mvec);
  sl::Resource albedoRes = ToSlResource(inputs.albedo);
  sl::Resource specularAlbedoRes = ToSlResource(inputs.specularAlbedo);
  sl::Resource normalRoughnessRes = ToSlResource(inputs.normalRoughness);
  sl::Resource specularHitDistanceRes = ToSlResource(inputs.specularHitDistance);

  const sl::Extent renderExtent{0, 0, inputs.renderWidth, inputs.renderHeight};
  const sl::Extent displayExtent{0, 0, inputs.displayWidth, inputs.displayHeight};

  sl::ResourceTag tags[] = {
      sl::ResourceTag{&colorInRes, sl::kBufferTypeScalingInputColor,
                      sl::ResourceLifecycle::eOnlyValidNow, &renderExtent},
      sl::ResourceTag{&colorOutRes, sl::kBufferTypeScalingOutputColor,
                      sl::ResourceLifecycle::eOnlyValidNow, &displayExtent},
      // Same kBufferTypeDepth choice as Evaluate() (SR) — reuses DlssPass's
      // existing dlss_depth_convert.slang output rather than tagging raw
      // gViewZ as kBufferTypeLinearDepth. §4.1.7 accepts either; kept
      // identical to SR's proven-working tag pending the empirical
      // slGetFeatureRequirements(kFeatureDLSS_RR) check (DlssDeviceExtensions
      // logs kFeatureDLSS_RR's own requiredTags list at startup) — switch
      // to kBufferTypeLinearDepth here (and drop the depth-convert dispatch
      // for the RR-only path) if that log shows RR wants LinearDepth instead.
      sl::ResourceTag{&depthRes, sl::kBufferTypeDepth, sl::ResourceLifecycle::eValidUntilPresent,
                      &renderExtent},
      sl::ResourceTag{&mvecRes, sl::kBufferTypeMotionVectors,
                      sl::ResourceLifecycle::eValidUntilPresent, &renderExtent},
      sl::ResourceTag{&albedoRes, sl::kBufferTypeAlbedo, sl::ResourceLifecycle::eOnlyValidNow,
                      &renderExtent},
      sl::ResourceTag{&specularAlbedoRes, sl::kBufferTypeSpecularAlbedo,
                      sl::ResourceLifecycle::eOnlyValidNow, &renderExtent},
      sl::ResourceTag{&normalRoughnessRes, sl::kBufferTypeNormalRoughness,
                      sl::ResourceLifecycle::eOnlyValidNow, &renderExtent},
      // gReflections.a (mean hitT, world-space distance; -1 on miss/rough-
      // fallback — reflections.slang) tagged whole-texture as the RR guide
      // buffer; ProgrammingGuideDLSS_RR.md §4.1.9 wants a scalar FP16/FP32
      // resource, so this is the one guide buffer worth re-checking against
      // the empirical requiredTags list / visual output (see this pass's
      // verification notes) if RR's reflections look wrong.
      sl::ResourceTag{&specularHitDistanceRes, sl::kBufferTypeSpecularHitDistance,
                      sl::ResourceLifecycle::eOnlyValidNow, &renderExtent},
  };
  const sl::Result tagResult = _impl->slSetTagForFrame(
      *frameToken, _impl->viewport, tags, static_cast<uint32_t>(std::size(tags)),
      inputs.vkCommandBuffer);
  if (tagResult != sl::Result::eOk)
  {
    log.Info(log::RENDER, "DlssProvider: EvaluateRR — slSetTagForFrame failed (sl::Result="
                              + std::to_string(static_cast<int>(tagResult)) + ")");
    return false;
  }

  // ---- 7.0 ADD DLSS-RR TO THE RENDERING PIPELINE -----------------------
  const sl::BaseStructure* evaluateInputs[] = {&_impl->viewport};
  const sl::Result evalResult =
      _impl->slEvaluateFeature(sl::kFeatureDLSS_RR, *frameToken, evaluateInputs,
                               static_cast<uint32_t>(std::size(evaluateInputs)),
                               inputs.vkCommandBuffer);
  if (evalResult != sl::Result::eOk)
  {
    log.Info(log::RENDER, "DlssProvider: EvaluateRR — slEvaluateFeature failed (sl::Result="
                              + std::to_string(static_cast<int>(evalResult)) + ")");
    return false;
  }

  _impl->hasEvaluatedOnce = true;
  _impl->resourcesEverEvaluated = true;
  _impl->lastEvaluatedFeature = sl::kFeatureDLSS_RR;
  return true;
}

void DlssProvider::ReleaseResources() noexcept {
  if (!_impl || !_impl->resourcesEverEvaluated || _impl->slFreeResources == nullptr)
    return;
  // DLSS Stage 2b — free whichever feature actually ran (RR and SR are
  // mutually exclusive per run; see Impl::lastEvaluatedFeature's comment).
  const sl::Result result = _impl->slFreeResources(_impl->lastEvaluatedFeature, _impl->viewport);
  Logging::Get().Info(log::RENDER,
                      "DlssProvider: ReleaseResources — slFreeResources "
                          + std::string{result == sl::Result::eOk ? "ok" : "reported an error"});
  _impl->resourcesEverEvaluated = false;
}

}  // namespace pyxis
