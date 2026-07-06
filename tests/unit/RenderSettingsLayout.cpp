// Pyxis renderer — RenderSettings POD layout + defaults tests.
//
// Plan §18.4 / §21. RenderSettings is the M3 subset of the eventual
// path-trace knob set; v1 ships width / height / clear-color / feature
// mask / debug-view inspector + picker. PublicDescLayout.cpp covers the
// Desc PODs that GpuScene::Add* / Update* consume; this file pins the
// settings POD that PyxisRenderer::SetRenderSettings consumes so a stray
// reorder or default flip is caught at the test layer instead of leaking
// into headless byte-equal output (§33.7) or the viewer's startup state.

#include <Pyxis/Renderer/Descs/RenderSettings.h>

#include <gtest/gtest.h>

#include <cstdint>
#include <type_traits>

using pyxis::RenderSettings;

// -----------------------------------------------------------------------------
// Layout.
// -----------------------------------------------------------------------------
static_assert(std::is_standard_layout_v<RenderSettings>,
              "RenderSettings must be standard layout for the public ABI.");
static_assert(std::is_standard_layout_v<RenderSettings::Features>,
              "RenderSettings::Features must be standard layout for the public ABI.");

// DebugView is part of the byte-stable surface — it gates the raygen's
// DEBUG_VIEW_* dispatch (resources/shaders/ShaderInterop.slang). Pinning
// the underlying type AND the Color = 0 sentinel keeps the C++ and the
// shader side in lockstep.
static_assert(std::is_same_v<std::underlying_type_t<RenderSettings::DebugView>, uint32_t>,
              "DebugView underlying type is fixed at uint32_t (matches ShaderInterop.slang).");
static_assert(static_cast<uint32_t>(RenderSettings::DebugView::Color) == 0,
              "DebugView::Color == 0 — the post-tonemap radiance branch.");
static_assert(static_cast<uint32_t>(RenderSettings::DebugView::Normal) == 1);
static_assert(static_cast<uint32_t>(RenderSettings::DebugView::Depth) == 2);
static_assert(static_cast<uint32_t>(RenderSettings::DebugView::PrimId) == 3);
static_assert(static_cast<uint32_t>(RenderSettings::DebugView::MaterialId) == 4);
static_assert(static_cast<uint32_t>(RenderSettings::DebugView::BaseColor) == 5);
static_assert(static_cast<uint32_t>(RenderSettings::DebugView::WorldPos) == 6);
// Tier-1 Hydra-canonical AOVs (per RenderSettings.h doc).
static_assert(static_cast<uint32_t>(RenderSettings::DebugView::Alpha) == 7);
static_assert(static_cast<uint32_t>(RenderSettings::DebugView::ElementId) == 8);
static_assert(static_cast<uint32_t>(RenderSettings::DebugView::NormalEye) == 9);
static_assert(static_cast<uint32_t>(RenderSettings::DebugView::WorldPosEye) == 10);

// -----------------------------------------------------------------------------
// Defaults — anyone who reorders or renames a field will trip these
// unless they update both halves.
// -----------------------------------------------------------------------------
TEST(RenderSettingsLayout, ResolutionDefaultsToFullHd) {
  const RenderSettings settings;
  EXPECT_EQ(settings.width, 1920u);
  EXPECT_EQ(settings.height, 1080u);
}

TEST(RenderSettingsLayout, ClearColorDefaultsToCharcoal) {
  const RenderSettings settings;
  EXPECT_FLOAT_EQ(settings.clearColor[0], 0.05f);
  EXPECT_FLOAT_EQ(settings.clearColor[1], 0.05f);
  EXPECT_FLOAT_EQ(settings.clearColor[2], 0.06f);
  EXPECT_FLOAT_EQ(settings.clearColor[3], 1.0f);
}

TEST(RenderSettingsLayout, FeaturesDefaultImguiOverlayOn) {
  const RenderSettings settings;
  EXPECT_TRUE(settings.features.imguiOverlay);
}

TEST(RenderSettingsLayout, DebugViewDefaultsToColor) {
  const RenderSettings settings;
  EXPECT_EQ(settings.debugView, RenderSettings::DebugView::Color);
}

TEST(RenderSettingsLayout, MousePixelDefaultsToNoHoverSentinel) {
  const RenderSettings settings;
  EXPECT_EQ(RenderSettings::MOUSE_PIXEL_NONE, 0xFFFFFFFFu);
  EXPECT_EQ(settings.mousePixelX, RenderSettings::MOUSE_PIXEL_NONE);
  EXPECT_EQ(settings.mousePixelY, RenderSettings::MOUSE_PIXEL_NONE);
}

TEST(RenderSettingsLayout, WorldPosPeriodDefaultsToTenMeters) {
  const RenderSettings settings;
  EXPECT_FLOAT_EQ(settings.worldPosPeriod, 10.0f);
}

TEST(RenderSettingsLayout, SsaaFactorDefaultsToOff) {
  const RenderSettings settings;
  EXPECT_EQ(settings.ssaaFactor, 1u);
}

// Q3 OpenPBR-complete — the feature mask MUST default to all six bits
// on (0x3F). Anything else changes headless byte-equal output (§33.7)
// and breaks §25.O.3 adapter parity. The literal-vs-interop-constant
// drift guard lives in PyxisRenderer.cpp (the public header cannot
// include ShaderInterop.slang); this test pins the public default.
TEST(RenderSettingsLayout, OpenPbrFeatureMaskDefaultsToAllOn) {
  const RenderSettings settings;
  EXPECT_EQ(settings.openPbrFeatureMask, 0x3Fu);
}

// RTX-alignment design (rtx-realtime-alignment-design.md), Phase C —
// RenderSettings' §22.3 reserved tail (`_reserved[2]`) was fully consumed
// by exposureMode + exposureResponsivity (see RenderSettings.h); the old
// "stays zeroed" assertion no longer applies — pin the new fields'
// defaults instead (Legacy exposure mode, ovrtx's own responsivity
// calibration constant).
TEST(RenderSettingsLayout, PhysicalCameraExposureDefaultsToLegacy) {
  const RenderSettings settings;
  EXPECT_EQ(settings.exposureMode, 0u);  // 0 = Legacy
  EXPECT_FLOAT_EQ(settings.exposureResponsivity, 0.8821367311933349f);
}

// RTX-alignment design (rtx-realtime-alignment-design.md), WP2-final —
// RealTimeQuality mirrors ovrtx 0.3.0's own omni:rtx:rt:* applied-API-
// schema defaults 1:1; a default flip here silently diverges Pyxis's
// stock look from Omniverse RTX Real-Time's.
static_assert(std::is_standard_layout_v<RenderSettings::RealTimeQuality>,
              "RenderSettings::RealTimeQuality must be standard layout for the public ABI.");
TEST(RenderSettingsLayout, RealTimeQualityDefaultsMirrorOvrtx) {
  const RenderSettings settings;
  EXPECT_EQ(settings.realTimeQuality.passMask, 0x1Fu);  // all five signal passes on
  EXPECT_EQ(settings.realTimeQuality.directSamples, 2u);
  EXPECT_EQ(settings.realTimeQuality.indirectSamples, 1u);
  EXPECT_EQ(settings.realTimeQuality.indirectMaxBounces, 2u);
  EXPECT_EQ(settings.realTimeQuality.reflectionSamples, 1u);
  EXPECT_FLOAT_EQ(settings.realTimeQuality.reflectionMaxRoughness, 0.3f);
  EXPECT_EQ(settings.realTimeQuality.refractionMaxBounces, 6u);
  // RTX-alignment design, Phase C — 0.35 m (35 ovrtx stage units == 35 cm
  // for World Lobby's metersPerUnit=0.01), not the pre-Phase-C 35.0 m
  // (100x too long — see the field's own doc comment).
  EXPECT_FLOAT_EQ(settings.realTimeQuality.aoRayLength, 0.35f);
  EXPECT_FLOAT_EQ(settings.realTimeQuality.maxRayIntensityDirect, 6400.0f);
  EXPECT_FLOAT_EQ(settings.realTimeQuality.maxRayIntensityIndirect, 6400.0f);
  EXPECT_FLOAT_EQ(settings.realTimeQuality.maxRayIntensityReflections, 19200.0f);
}

// RTX-alignment design (rtx-realtime-alignment-design.md), Phase C —
// RealTimeQuality's own §22.3 reserved tail (`_reserved[4]`) was fully
// consumed by the tonemap operator selector + the physical-camera exposure
// core fields (see RenderSettings.h); pin their defaults here instead of
// the old "stays zeroed" assertion.
TEST(RenderSettingsLayout, TonemapAndPhysicalCameraDefaultsMirrorOvrtx) {
  const RenderSettings settings;
  EXPECT_EQ(settings.realTimeQuality.tonemapOperator, 6u);  // AcesApproximation
  EXPECT_FLOAT_EQ(settings.realTimeQuality.physicalCameraFStop, 5.0f);
  EXPECT_FLOAT_EQ(settings.realTimeQuality.physicalCameraIso, 100.0f);
  // 0.02 s = ovrtx's effective (legacy carb) default, empirically matched
  // against World Lobby captures — see RenderSettings.h's field comment.
  EXPECT_FLOAT_EQ(settings.realTimeQuality.physicalCameraExposureTimeSeconds, 0.02f);
}

// DLSS Stage 1 (rtx-realtime-alignment-design.md, "DLSS — corrected
// stance" + "DLSS scope includes upscaling") — denoiser DEFAULTS to
// DENOISER_DLSS per the owner's directive; PyxisRenderer's capability
// probe (Private/Dlss/DlssProvider.h) downgrades to Builtin whenever the
// Streamline SDK isn't staged (always true in CI today), so headless
// goldens stay byte-equal regardless of this default (§33.7 — passMask's
// own denoise/TAA bits already default OFF, see the test above).
TEST(RenderSettingsLayout, DlssDefaultsToDlssRequestedAutoExecMode) {
  const RenderSettings settings;
  EXPECT_EQ(settings.realTimeQuality.denoiser, pyxis::DENOISER_DLSS);
  EXPECT_EQ(settings.realTimeQuality.dlssExecMode, pyxis::DLSS_EXEC_MODE_AUTO);
  EXPECT_EQ(pyxis::DENOISER_DLSS, 0u);
  EXPECT_EQ(pyxis::DENOISER_BUILTIN, 1u);
  EXPECT_EQ(pyxis::DENOISER_OFF, 2u);
  EXPECT_EQ(pyxis::DLSS_EXEC_MODE_AUTO, 0u);
  EXPECT_EQ(pyxis::DLSS_EXEC_MODE_QUALITY, 1u);
  EXPECT_EQ(pyxis::DLSS_EXEC_MODE_BALANCED, 2u);
  EXPECT_EQ(pyxis::DLSS_EXEC_MODE_PERFORMANCE, 3u);
  EXPECT_EQ(pyxis::DLSS_EXEC_MODE_DLAA, 4u);
}
