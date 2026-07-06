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
  sl::Constants constants{};
  constants.cameraViewToClip = ToSlMatrix(inputs.cameraViewToClip);
  constants.clipToCameraView = ToSlMatrix(inputs.clipToCameraView);
  constants.clipToPrevClip = ToSlMatrix(inputs.clipToPrevClip);
  constants.prevClipToClip = ToSlMatrix(inputs.prevClipToClip);
  constants.jitterOffset = {inputs.jitterX, inputs.jitterY};
  // gMotionVector is pixel-space (RenderTargets.h: "screen-space motion
  // vector, in PIXELS") — ProgrammingGuideDLSS.md 10.0's troubleshooting
  // section is explicit about this scale for pixel-space motion vectors.
  constants.mvecScale = {
      inputs.renderWidth > 0u ? 1.0f / static_cast<float>(inputs.renderWidth) : 0.0f,
      inputs.renderHeight > 0u ? 1.0f / static_cast<float>(inputs.renderHeight) : 0.0f};
  constants.cameraPos = {inputs.cameraPos[0], inputs.cameraPos[1], inputs.cameraPos[2]};
  constants.cameraUp = {inputs.cameraUp[0], inputs.cameraUp[1], inputs.cameraUp[2]};
  constants.cameraRight = {inputs.cameraRight[0], inputs.cameraRight[1], inputs.cameraRight[2]};
  constants.cameraFwd = {inputs.cameraFwd[0], inputs.cameraFwd[1], inputs.cameraFwd[2]};
  constants.cameraNear = inputs.cameraNear;
  constants.cameraFar = inputs.cameraFar;
  // cameraFOV / cameraAspectRatio have NO "Optional -" prefix in
  // sl_consts.h (unlike cameraPinholeOffset / clipToLensClip / etc.) and
  // default to sl::INVALID_FLOAT (a 3.4e38 sentinel) if left unset --
  // empirically, leaving them unset produced
  // slEvaluateFeature/eErrorMissingInputParameter on this machine.
  // projYY (index 5 = row1,col1 of the row-major-flattened conformed
  // cameraViewToClip) is `1/tan(fovY/2)` for a standard perspective
  // projection — the SAME element SceneBindings.cpp's own
  // pixelSpreadRadians derivation reads (see its Update()).
  if (inputs.cameraViewToClip != nullptr)
  {
    const float projYY = inputs.cameraViewToClip[5];
    constants.cameraFOV =
        (projYY > 1e-6f) ? (2.0f * std::atan(1.0f / projYY)) : constants.cameraFOV;
  }
  constants.cameraAspectRatio = inputs.renderHeight > 0u
                                    ? static_cast<float>(inputs.renderWidth)
                                          / static_cast<float>(inputs.renderHeight)
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
  constants.orthographicProjection = inputs.orthographic ? sl::Boolean::eTrue : sl::Boolean::eFalse;
  constants.motionVectorsDilated = sl::Boolean::eFalse;
  // raytraced_gbuffer.slang computes the motion vector from the UNJITTERED
  // pixel center (`float2(launchIndex) + 0.5f`), not the jittered primary-
  // ray sample — see DlssProvider.h's FrameInputs doc comment.
  constants.motionVectorsJittered = sl::Boolean::eFalse;

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
  return true;
}

void DlssProvider::ReleaseResources() noexcept {
  if (!_impl || !_impl->resourcesEverEvaluated || _impl->slFreeResources == nullptr)
    return;
  const sl::Result result = _impl->slFreeResources(sl::kFeatureDLSS, _impl->viewport);
  Logging::Get().Info(log::RENDER,
                      "DlssProvider: ReleaseResources — slFreeResources "
                          + std::string{result == sl::Result::eOk ? "ok" : "reported an error"});
  _impl->resourcesEverEvaluated = false;
}

}  // namespace pyxis
