// Pyxis V2.A.24 + V2.A.29 + V2.A.30 — material coverage fields on
// OpenPBRMaterialDesc are populated by FromUsdShade.
//
// Author a UsdShadeMaterial with:
//   - displacement output connected
//   - volume output connected
//   - UsdUVTexture with `wrapS=mirror` + `sourceColorSpace=sRGB`
// and assert the resulting desc records each authored signal.

#include <Pyxis/MaterialTranslation/UsdShadeToOpenPBR.h>
#include <Pyxis/Renderer/Descs/OpenPBRMaterialDesc.h>

#include <pxr/base/gf/vec3f.h>
#include <pxr/base/tf/token.h>
#include <pxr/base/vt/value.h>
#include <pxr/usd/sdf/assetPath.h>
#include <pxr/usd/sdf/path.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdShade/connectableAPI.h>
#include <pxr/usd/usdShade/material.h>
#include <pxr/usd/usdShade/shader.h>

#include <gtest/gtest.h>

namespace {

pxr::UsdShadeMaterial BuildMaterial(const pxr::UsdStageRefPtr& stage) {
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
      .Set(pxr::SdfAssetPath("./tex.png"));
  texture.CreateInput(pxr::TfToken("wrapS"), pxr::SdfValueTypeNames->Token)
      .Set(pxr::TfToken("mirror"));
  texture.CreateInput(pxr::TfToken("sourceColorSpace"),
                      pxr::SdfValueTypeNames->Token)
      .Set(pxr::TfToken("sRGB"));
  const pxr::UsdShadeOutput texOut =
      texture.CreateOutput(pxr::TfToken("rgb"), pxr::SdfValueTypeNames->Float3);
  surface.CreateInput(pxr::TfToken("diffuseColor"), pxr::SdfValueTypeNames->Color3f)
      .ConnectToSource(texOut);

  // Displacement + volume outputs on the material (no real network on
  // the right-hand side; HasConnectedSource is enough to flag).
  pxr::UsdShadeShader disp =
      pxr::UsdShadeShader::Define(stage, pxr::SdfPath("/Mat/Disp"));
  disp.CreateIdAttr(pxr::VtValue(pxr::TfToken("UsdPreviewSurface")));
  const pxr::UsdShadeOutput dispOut =
      disp.CreateOutput(pxr::TfToken("displacement"), pxr::SdfValueTypeNames->Float);
  material.CreateDisplacementOutput().ConnectToSource(dispOut);

  pxr::UsdShadeShader vol =
      pxr::UsdShadeShader::Define(stage, pxr::SdfPath("/Mat/Vol"));
  vol.CreateIdAttr(pxr::VtValue(pxr::TfToken("UsdVolume")));
  const pxr::UsdShadeOutput volOut =
      vol.CreateOutput(pxr::TfToken("volume"), pxr::SdfValueTypeNames->Token);
  material.CreateVolumeOutput().ConnectToSource(volOut);

  return material;
}

pyxis::TextureHandle StubAcquire(std::string_view /*path*/,
                                 pyxis::TextureKey::Role /*role*/,
                                 void* /*userData*/)
{
  return pyxis::TextureHandle::Invalid;
}

}  // namespace

TEST(MaterialCoverageFields, DisplacementAndVolumeFlagsSet)
{
  const pxr::UsdStageRefPtr stage = pxr::UsdStage::CreateInMemory("disp_vol.usda");
  const pxr::UsdShadeMaterial material = BuildMaterial(stage);

  const pyxis::OpenPBRMaterialDesc desc =
      pyxis::material_translation::FromUsdShade(material, &StubAcquire, nullptr);
  EXPECT_EQ(desc.hasDisplacementOutput, 1u);
  EXPECT_EQ(desc.hasVolumeOutput,       1u);
}

TEST(MaterialCoverageFields, WrapAndColorSpaceHashesNonZero)
{
  const pxr::UsdStageRefPtr stage = pxr::UsdStage::CreateInMemory("wrap_cs.usda");
  const pxr::UsdShadeMaterial material = BuildMaterial(stage);

  const pyxis::OpenPBRMaterialDesc desc =
      pyxis::material_translation::FromUsdShade(material, &StubAcquire, nullptr);
  // Authored values were `mirror` + `sRGB` — both should hash to a
  // non-zero TfToken hash. Empty TfToken hashes to 0.
  EXPECT_NE(desc.baseColorWrapS, 0u);
  EXPECT_NE(desc.baseColorSourceCS, 0u);

  // wrapT wasn't authored — hash stays 0 (empty TfToken).
  EXPECT_EQ(desc.baseColorWrapT, 0u);
}
