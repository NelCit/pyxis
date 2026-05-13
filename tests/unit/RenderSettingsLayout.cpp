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
