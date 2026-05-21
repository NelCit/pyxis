// Pyxis V2.A.27 — UsdRender pre-scan unit tests.
//
// Pins the `PrescanRenderSettings` parser:
//   • Stage with no UsdRender prims → hasRenderSettings = false.
//   • Stage with UsdRenderSettings + resolution → fields populated.
//   • Stage with multiple UsdRenderProducts → first by SdfPath wins
//     by default; --render-product override picks by leaf name.
//   • Unmatched override falls back to first-by-SdfPath.
//
// The Application-side overlay logic (CLI > USD > JSON > defaults)
// is exercised via the `render_authored_resolution` golden — the
// renderer must produce a PNG at the USD-authored 200×150 instead
// of the shared config's 256×256.

#include <Pyxis/UsdIngest/RenderSettingsAuthored.h>

#include <pxr/base/gf/vec2i.h>
#include <pxr/base/tf/token.h>
#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/sdf/path.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdRender/product.h>
#include <pxr/usd/usdRender/settings.h>
#include <pxr/usd/usdRender/var.h>

#include <gtest/gtest.h>

#include <filesystem>
#include <string>

namespace {

// Persist an in-memory stage to a temp .usda so PrescanRenderSettings
// can open it via the same `UsdStage::Open(path)` path the
// production code uses. Returns the absolute temp file path; the
// caller is responsible for the file's lifetime (we don't bother
// cleaning up in this test pass — they're <1 KB each).
std::string PersistStage(const pxr::UsdStageRefPtr& stage,
                          const std::string& filename)
{
  const std::filesystem::path tmpDir =
      std::filesystem::temp_directory_path() / "pyxis_v2a27_prescan";
  std::error_code errorCode;
  std::filesystem::create_directories(tmpDir, errorCode);
  const std::string path = (tmpDir / filename).string();
  stage->Export(path);
  return path;
}

pxr::UsdStageRefPtr MakeStageWithRenderSettings(int width, int height) {
  const pxr::UsdStageRefPtr stage = pxr::UsdStage::CreateInMemory();
  const pxr::UsdRenderSettings settings =
      pxr::UsdRenderSettings::Define(stage, pxr::SdfPath("/Render/Settings"));
  settings.CreateResolutionAttr(pxr::VtValue(pxr::GfVec2i(width, height)));
  return stage;
}

}  // namespace

TEST(PrescanRenderSettingsV2A27, EmptyStageReportsNotAuthored) {
  const pxr::UsdStageRefPtr stage = pxr::UsdStage::CreateInMemory();
  const std::string path = PersistStage(stage, "empty.usda");

  const auto authored = pyxis::usd_ingest::PrescanRenderSettings(path);
  EXPECT_FALSE(authored.hasRenderSettings);
  EXPECT_EQ(authored.resolutionWidth,   0u);
  EXPECT_EQ(authored.resolutionHeight,  0u);
  EXPECT_EQ(authored.renderProductCount, 0u);
  EXPECT_EQ(authored.renderVarCount,     0u);
  EXPECT_STREQ(authored.outputFile, "");
  EXPECT_STREQ(authored.cameraSdfPath, "");
}

TEST(PrescanRenderSettingsV2A27, ResolutionParsesFromUsdRenderSettings) {
  const pxr::UsdStageRefPtr stage = MakeStageWithRenderSettings(1280, 720);
  const std::string path = PersistStage(stage, "settings_only.usda");

  const auto authored = pyxis::usd_ingest::PrescanRenderSettings(path);
  EXPECT_TRUE(authored.hasRenderSettings);
  EXPECT_EQ(authored.resolutionWidth,   1280u);
  EXPECT_EQ(authored.resolutionHeight,  720u);
  EXPECT_EQ(authored.renderProductCount, 0u);
}

TEST(PrescanRenderSettingsV2A27, RenderProductOutputFileFlows) {
  const pxr::UsdStageRefPtr stage = MakeStageWithRenderSettings(640, 480);
  const pxr::UsdRenderProduct product =
      pxr::UsdRenderProduct::Define(stage, pxr::SdfPath("/Render/Products/Beauty"));
  product.CreateProductNameAttr(pxr::VtValue(pxr::TfToken("beauty_output.exr")));
  const std::string path = PersistStage(stage, "with_product.usda");

  const auto authored = pyxis::usd_ingest::PrescanRenderSettings(path);
  EXPECT_TRUE(authored.hasRenderSettings);
  EXPECT_EQ(authored.renderProductCount, 1u);
  EXPECT_STREQ(authored.outputFile, "beauty_output.exr");
  EXPECT_STREQ(authored.activeProductPath, "/Render/Products/Beauty");
}

TEST(PrescanRenderSettingsV2A27, MultipleProductsFirstBySdfPathWins) {
  const pxr::UsdStageRefPtr stage = MakeStageWithRenderSettings(640, 480);
  // Two products. SdfPath sort places "Beauty" before "Denoise".
  const pxr::UsdRenderProduct beauty =
      pxr::UsdRenderProduct::Define(stage, pxr::SdfPath("/Render/Products/Beauty"));
  beauty.CreateProductNameAttr(pxr::VtValue(pxr::TfToken("beauty.exr")));
  const pxr::UsdRenderProduct denoise =
      pxr::UsdRenderProduct::Define(stage, pxr::SdfPath("/Render/Products/Denoise"));
  denoise.CreateProductNameAttr(pxr::VtValue(pxr::TfToken("denoise.exr")));
  const std::string path = PersistStage(stage, "multi_product.usda");

  const auto authored = pyxis::usd_ingest::PrescanRenderSettings(path);
  EXPECT_EQ(authored.renderProductCount, 2u);
  EXPECT_STREQ(authored.activeProductPath, "/Render/Products/Beauty");
  EXPECT_STREQ(authored.outputFile,       "beauty.exr");
}

TEST(PrescanRenderSettingsV2A27, RenderProductOverridePicksByLeafName) {
  const pxr::UsdStageRefPtr stage = MakeStageWithRenderSettings(640, 480);
  const pxr::UsdRenderProduct beauty =
      pxr::UsdRenderProduct::Define(stage, pxr::SdfPath("/Render/Products/Beauty"));
  beauty.CreateProductNameAttr(pxr::VtValue(pxr::TfToken("beauty.exr")));
  const pxr::UsdRenderProduct denoise =
      pxr::UsdRenderProduct::Define(stage, pxr::SdfPath("/Render/Products/Denoise"));
  denoise.CreateProductNameAttr(pxr::VtValue(pxr::TfToken("denoise.exr")));
  const std::string path = PersistStage(stage, "multi_product_override.usda");

  // Operator passes --render-product Denoise → the SECOND product
  // wins instead of the SdfPath-first one.
  const auto authored =
      pyxis::usd_ingest::PrescanRenderSettings(path, "Denoise");
  EXPECT_STREQ(authored.activeProductPath, "/Render/Products/Denoise");
  EXPECT_STREQ(authored.outputFile,       "denoise.exr");
}

TEST(PrescanRenderSettingsV2A27, UnmatchedOverrideFallsBackToFirst) {
  const pxr::UsdStageRefPtr stage = MakeStageWithRenderSettings(640, 480);
  const pxr::UsdRenderProduct beauty =
      pxr::UsdRenderProduct::Define(stage, pxr::SdfPath("/Render/Products/Beauty"));
  beauty.CreateProductNameAttr(pxr::VtValue(pxr::TfToken("beauty.exr")));
  const std::string path = PersistStage(stage, "unmatched_override.usda");

  // No `/Render/Products/Cryptomatte` exists; the fall-back picks
  // the first-by-SdfPath product instead.
  const auto authored =
      pyxis::usd_ingest::PrescanRenderSettings(path, "Cryptomatte");
  EXPECT_STREQ(authored.activeProductPath, "/Render/Products/Beauty");
  EXPECT_STREQ(authored.outputFile,       "beauty.exr");
}

TEST(PrescanRenderSettingsV2A27, RenderVarsCountedSeparately) {
  const pxr::UsdStageRefPtr stage = MakeStageWithRenderSettings(640, 480);
  pxr::UsdRenderVar::Define(stage, pxr::SdfPath("/Render/Vars/Beauty"));
  pxr::UsdRenderVar::Define(stage, pxr::SdfPath("/Render/Vars/Normal"));
  pxr::UsdRenderVar::Define(stage, pxr::SdfPath("/Render/Vars/Depth"));
  const std::string path = PersistStage(stage, "vars_only.usda");

  const auto authored = pyxis::usd_ingest::PrescanRenderSettings(path);
  EXPECT_EQ(authored.renderVarCount, 3u);
}

TEST(PrescanRenderSettingsV2A27, MissingFileReturnsDefaultStruct) {
  const auto authored =
      pyxis::usd_ingest::PrescanRenderSettings("D:/this/path/does/not/exist.usda");
  EXPECT_FALSE(authored.hasRenderSettings);
  EXPECT_EQ(authored.resolutionWidth, 0u);
  EXPECT_EQ(authored.renderProductCount, 0u);
}
