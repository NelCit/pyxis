// Pyxis V2.A.7 — UDIM multi-tile acquire. Author a UsdShadeMaterial
// whose diffuseColor texture asset path contains `<UDIM>`, drop a few
// fake tile files on disk under a tmp directory, and verify the
// translator calls AcquireTextureFn once per existing tile (in
// addition to the primary 1001).

#include <Pyxis/MaterialTranslation/UsdShadeToOpenPBR.h>
#include <Pyxis/Renderer/Descs/OpenPBRMaterialDesc.h>

#include <pxr/base/tf/token.h>
#include <pxr/base/vt/value.h>
#include <pxr/usd/sdf/assetPath.h>
#include <pxr/usd/sdf/path.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdShade/connectableAPI.h>
#include <pxr/usd/usdShade/material.h>
#include <pxr/usd/usdShade/shader.h>

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_set>
#include <vector>

namespace {

// Capture every path the translator asked to acquire so we can assert
// the multi-tile loop fired with the right candidates.
struct AcquireCapture {
  std::vector<std::string> paths;
};

pyxis::TextureHandle CaptureAcquire(std::string_view path,
                                    pyxis::TextureKey::Role /*role*/,
                                    void* userData)
{
  auto* capture = static_cast<AcquireCapture*>(userData);
  capture->paths.emplace_back(path);
  return pyxis::TextureHandle::Invalid;
}

}  // namespace

TEST(UdimMultiTile, AcquiresEveryOnDiskTile)
{
  // Drop three fake tiles (1001 / 1002 / 1011) under a tmp dir; leave
  // 1003 and 1099 absent so we exercise the existence check too.
  const auto tmpDir = std::filesystem::temp_directory_path()
                      / "pyxis_udim_test";
  std::filesystem::remove_all(tmpDir);
  std::filesystem::create_directories(tmpDir);

  for (const int tile : {1001, 1002, 1011})
  {
    const auto path = tmpDir / ("tex." + std::to_string(tile) + ".png");
    std::ofstream stub(path, std::ios::binary);
    stub << "stub";  // contents don't matter — translator only checks existence
  }

  const std::string pattern = (tmpDir / "tex.<UDIM>.png").string();

  const pxr::UsdStageRefPtr stage = pxr::UsdStage::CreateInMemory("udim.usda");
  const pxr::UsdShadeMaterial material =
      pxr::UsdShadeMaterial::Define(stage, pxr::SdfPath("/Mat"));

  pxr::UsdShadeShader surface =
      pxr::UsdShadeShader::Define(stage, pxr::SdfPath("/Mat/Surface"));
  surface.CreateIdAttr(pxr::VtValue(pxr::TfToken("UsdPreviewSurface")));
  const pxr::UsdShadeOutput surfaceOut =
      surface.CreateOutput(pxr::TfToken("surface"), pxr::SdfValueTypeNames->Token);
  material.CreateSurfaceOutput().ConnectToSource(surfaceOut);

  pxr::UsdShadeShader texture =
      pxr::UsdShadeShader::Define(stage, pxr::SdfPath("/Mat/Tex"));
  texture.CreateIdAttr(pxr::VtValue(pxr::TfToken("UsdUVTexture")));
  texture.CreateInput(pxr::TfToken("file"), pxr::SdfValueTypeNames->Asset)
      .Set(pxr::SdfAssetPath(pattern));
  const pxr::UsdShadeOutput texOut =
      texture.CreateOutput(pxr::TfToken("rgb"), pxr::SdfValueTypeNames->Float3);
  surface.CreateInput(pxr::TfToken("diffuseColor"),
                      pxr::SdfValueTypeNames->Color3f)
      .ConnectToSource(texOut);

  AcquireCapture capture;
  const pyxis::OpenPBRMaterialDesc desc =
      pyxis::material_translation::FromUsdShade(material, &CaptureAcquire, &capture);
  (void)desc;

  // Translator should have asked to acquire exactly 3 tiles
  // (1001 — the primary — plus 1002 and 1011). Tiles that don't exist
  // on disk are skipped via the `std::filesystem::exists` check.
  std::unordered_set<std::string> got;
  for (const auto& path : capture.paths)
    got.insert(path);
  EXPECT_EQ(got.size(), 3u);
  EXPECT_TRUE(got.contains((tmpDir / "tex.1001.png").string()));
  EXPECT_TRUE(got.contains((tmpDir / "tex.1002.png").string()));
  EXPECT_TRUE(got.contains((tmpDir / "tex.1011.png").string()));
  // Sanity: a missing tile (1003) was NOT requested.
  EXPECT_FALSE(got.contains((tmpDir / "tex.1003.png").string()));

  std::filesystem::remove_all(tmpDir);
}

TEST(UdimMultiTile, NonUdimPathOnlyAcquiresOnce)
{
  const pxr::UsdStageRefPtr stage = pxr::UsdStage::CreateInMemory("regular.usda");
  const pxr::UsdShadeMaterial material =
      pxr::UsdShadeMaterial::Define(stage, pxr::SdfPath("/Mat"));

  pxr::UsdShadeShader surface =
      pxr::UsdShadeShader::Define(stage, pxr::SdfPath("/Mat/Surface"));
  surface.CreateIdAttr(pxr::VtValue(pxr::TfToken("UsdPreviewSurface")));
  const pxr::UsdShadeOutput surfaceOut =
      surface.CreateOutput(pxr::TfToken("surface"), pxr::SdfValueTypeNames->Token);
  material.CreateSurfaceOutput().ConnectToSource(surfaceOut);

  pxr::UsdShadeShader texture =
      pxr::UsdShadeShader::Define(stage, pxr::SdfPath("/Mat/Tex"));
  texture.CreateIdAttr(pxr::VtValue(pxr::TfToken("UsdUVTexture")));
  texture.CreateInput(pxr::TfToken("file"), pxr::SdfValueTypeNames->Asset)
      .Set(pxr::SdfAssetPath("regular_no_udim.png"));
  const pxr::UsdShadeOutput texOut =
      texture.CreateOutput(pxr::TfToken("rgb"), pxr::SdfValueTypeNames->Float3);
  surface.CreateInput(pxr::TfToken("diffuseColor"),
                      pxr::SdfValueTypeNames->Color3f)
      .ConnectToSource(texOut);

  AcquireCapture capture;
  const pyxis::OpenPBRMaterialDesc desc =
      pyxis::material_translation::FromUsdShade(material, &CaptureAcquire, &capture);
  (void)desc;

  // No UDIM pattern → exactly one acquire (the single texture).
  EXPECT_EQ(capture.paths.size(), 1u);
}
