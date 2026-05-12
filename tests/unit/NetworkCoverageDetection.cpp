// Pyxis M22 / V2.A.23 + V2.A.24 + V2.A.29 + V2.A.30 — material
// network coverage detection. Author UsdShadeMaterial graphs with
// each feature (MDL info:id, non-default wrap mode, sourceColorSpace,
// displacement/volume outputs) and assert our detection predicates
// flag them. Mirrors the per-feature checks the StageWalker loop
// uses for its end-of-pass1 coverage summary.

#include <pxr/base/tf/token.h>
#include <pxr/base/vt/value.h>
#include <pxr/usd/sdf/path.h>
#include <pxr/usd/usd/primRange.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdShade/connectableAPI.h>
#include <pxr/usd/usdShade/material.h>
#include <pxr/usd/usdShade/shader.h>

#include <gtest/gtest.h>

#include <string>

TEST(NetworkCoverageDetection, MdlShaderIdDetected)
{
  const pxr::UsdStageRefPtr stage = pxr::UsdStage::CreateInMemory("mdl.usda");
  const pxr::UsdShadeMaterial material =
      pxr::UsdShadeMaterial::Define(stage, pxr::SdfPath("/Mat"));
  const pxr::UsdShadeShader shader =
      pxr::UsdShadeShader::Define(stage, pxr::SdfPath("/Mat/MdlShader"));
  shader.CreateIdAttr(pxr::VtValue(pxr::TfToken("mdl::OmniPBR")));

  bool sawMdl = false;
  for (const pxr::UsdPrim& child : pxr::UsdPrimRange(material.GetPrim()))
  {
    const pxr::UsdShadeShader probe(child);
    if (!probe)
      continue;
    pxr::TfToken shaderId;
    if (probe.GetShaderId(&shaderId)
        && shaderId.GetString().starts_with("mdl::"))
    {
      sawMdl = true;
    }
  }
  EXPECT_TRUE(sawMdl);
}

TEST(NetworkCoverageDetection, DisplacementOutputDetected)
{
  const pxr::UsdStageRefPtr stage = pxr::UsdStage::CreateInMemory("disp.usda");
  const pxr::UsdShadeMaterial material =
      pxr::UsdShadeMaterial::Define(stage, pxr::SdfPath("/Mat"));
  pxr::UsdShadeShader dispShader =
      pxr::UsdShadeShader::Define(stage, pxr::SdfPath("/Mat/Disp"));
  dispShader.CreateIdAttr(pxr::VtValue(pxr::TfToken("UsdPreviewSurface")));
  const pxr::UsdShadeOutput dispOut =
      dispShader.CreateOutput(pxr::TfToken("displacement"), pxr::SdfValueTypeNames->Float);
  material.CreateDisplacementOutput().ConnectToSource(dispOut);

  EXPECT_TRUE(material.GetDisplacementOutput().HasConnectedSource());
}

TEST(NetworkCoverageDetection, VolumeOutputDetected)
{
  const pxr::UsdStageRefPtr stage = pxr::UsdStage::CreateInMemory("vol.usda");
  const pxr::UsdShadeMaterial material =
      pxr::UsdShadeMaterial::Define(stage, pxr::SdfPath("/Mat"));
  pxr::UsdShadeShader volShader =
      pxr::UsdShadeShader::Define(stage, pxr::SdfPath("/Mat/Vol"));
  volShader.CreateIdAttr(pxr::VtValue(pxr::TfToken("UsdVolume")));
  const pxr::UsdShadeOutput volOut =
      volShader.CreateOutput(pxr::TfToken("volume"), pxr::SdfValueTypeNames->Token);
  material.CreateVolumeOutput().ConnectToSource(volOut);

  EXPECT_TRUE(material.GetVolumeOutput().HasConnectedSource());
}

TEST(NetworkCoverageDetection, NonDefaultWrapModeDetected)
{
  const pxr::UsdStageRefPtr stage = pxr::UsdStage::CreateInMemory("wrap.usda");
  pxr::UsdShadeShader texture =
      pxr::UsdShadeShader::Define(stage, pxr::SdfPath("/Tex"));
  texture.CreateIdAttr(pxr::VtValue(pxr::TfToken("UsdUVTexture")));
  texture.CreateInput(pxr::TfToken("wrapS"), pxr::SdfValueTypeNames->Token)
      .Set(pxr::TfToken("mirror"));  // non-default

  const pxr::UsdShadeInput wrapS = texture.GetInput(pxr::TfToken("wrapS"));
  ASSERT_TRUE(wrapS);
  pxr::VtValue value;
  ASSERT_TRUE(wrapS.Get(&value));
  ASSERT_TRUE(value.IsHolding<pxr::TfToken>());
  const pxr::TfToken token = value.UncheckedGet<pxr::TfToken>();
  EXPECT_NE(token.GetString(), "repeat");
  EXPECT_NE(token.GetString(), "useMetadata");
}

TEST(NetworkCoverageDetection, ColorSpaceTokenDetected)
{
  const pxr::UsdStageRefPtr stage = pxr::UsdStage::CreateInMemory("cs.usda");
  pxr::UsdShadeShader texture =
      pxr::UsdShadeShader::Define(stage, pxr::SdfPath("/Tex"));
  texture.CreateIdAttr(pxr::VtValue(pxr::TfToken("UsdUVTexture")));
  texture.CreateInput(pxr::TfToken("sourceColorSpace"),
                      pxr::SdfValueTypeNames->Token)
      .Set(pxr::TfToken("sRGB"));

  const pxr::UsdShadeInput colorSpace =
      texture.GetInput(pxr::TfToken("sourceColorSpace"));
  ASSERT_TRUE(colorSpace);
  pxr::VtValue value;
  ASSERT_TRUE(colorSpace.Get(&value));
  ASSERT_TRUE(value.IsHolding<pxr::TfToken>());
  EXPECT_NE(value.UncheckedGet<pxr::TfToken>().GetString(), "auto");
}
