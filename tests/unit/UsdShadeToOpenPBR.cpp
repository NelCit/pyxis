// Pyxis material translation — UsdShadeMaterial → OpenPBRMaterialDesc
// field-by-field test. Plan §41 M5 exit: "UsdPreviewSurface inputs
// translate per-field into OpenPBRMaterialDesc; both adapters share
// the same translator (§25.E)."
//
// The fixture builds an anonymous UsdStage in memory, authors a
// UsdPreviewSurface shader with every input we currently translate
// set to a recognisable value, then asserts every translated field
// in the resulting OpenPBRMaterialDesc.

#include <Pyxis/MaterialTranslation/UsdShadeToOpenPBR.h>
#include <Pyxis/Renderer/Descs/OpenPBRMaterialDesc.h>

#include <pxr/base/gf/vec3f.h>
#include <pxr/base/tf/token.h>
#include <pxr/base/vt/value.h>
#include <pxr/usd/sdf/path.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdShade/connectableAPI.h>
#include <pxr/usd/usdShade/material.h>
#include <pxr/usd/usdShade/shader.h>

#include <gtest/gtest.h>

namespace {

pxr::UsdShadeMaterial AuthorPreviewSurfaceMaterial(const pxr::UsdStageRefPtr& stage,
                                                   const char* primPath) {
  const pxr::SdfPath materialPath(primPath);
  const pxr::UsdShadeMaterial material =
      pxr::UsdShadeMaterial::Define(stage, materialPath);

  const pxr::SdfPath shaderPath = materialPath.AppendChild(pxr::TfToken("Surface"));
  pxr::UsdShadeShader shader = pxr::UsdShadeShader::Define(stage, shaderPath);
  shader.CreateIdAttr(pxr::VtValue(pxr::TfToken("UsdPreviewSurface")));

  shader.CreateInput(pxr::TfToken("diffuseColor"), pxr::SdfValueTypeNames->Color3f)
      .Set(pxr::GfVec3f(0.85f, 0.15f, 0.12f));
  shader.CreateInput(pxr::TfToken("metallic"), pxr::SdfValueTypeNames->Float).Set(0.0f);
  shader.CreateInput(pxr::TfToken("roughness"), pxr::SdfValueTypeNames->Float).Set(0.35f);
  shader.CreateInput(pxr::TfToken("opacity"), pxr::SdfValueTypeNames->Float).Set(1.0f);
  shader.CreateInput(pxr::TfToken("ior"), pxr::SdfValueTypeNames->Float).Set(1.5f);
  shader.CreateInput(pxr::TfToken("clearcoat"), pxr::SdfValueTypeNames->Float).Set(0.25f);
  shader.CreateInput(pxr::TfToken("clearcoatRoughness"), pxr::SdfValueTypeNames->Float)
      .Set(0.07f);
  shader.CreateInput(pxr::TfToken("emissiveColor"), pxr::SdfValueTypeNames->Color3f)
      .Set(pxr::GfVec3f(0.1f, 0.2f, 0.3f));

  const pxr::UsdShadeOutput shaderOutput =
      shader.CreateOutput(pxr::TfToken("surface"), pxr::SdfValueTypeNames->Token);
  const pxr::UsdShadeOutput materialOutput = material.CreateSurfaceOutput();
  pxr::UsdShadeConnectableAPI::ConnectToSource(materialOutput, shaderOutput);

  return material;
}

}  // namespace

// Translation of every UsdPreviewSurface input we currently consume
// must round-trip into OpenPBRMaterialDesc with the source tag set
// to UsdPreviewSurface.
TEST(UsdShadeToOpenPBR, UsdPreviewSurfaceInputsRoundTripFieldByField) {
  const pxr::UsdStageRefPtr stage = pxr::UsdStage::CreateInMemory();
  ASSERT_TRUE(stage);
  const pxr::UsdShadeMaterial material =
      AuthorPreviewSurfaceMaterial(stage, "/Materials/RedPlastic");

  const pyxis::OpenPBRMaterialDesc desc =
      pyxis::material_translation::FromUsdShade(material);

  EXPECT_EQ(desc.source, pyxis::OpenPBRMaterialDesc::Source::UsdPreviewSurface);

  EXPECT_FLOAT_EQ(desc.baseColor.x, 0.85f);
  EXPECT_FLOAT_EQ(desc.baseColor.y, 0.15f);
  EXPECT_FLOAT_EQ(desc.baseColor.z, 0.12f);
  EXPECT_FLOAT_EQ(desc.metalness, 0.0f);
  EXPECT_FLOAT_EQ(desc.roughness, 0.35f);
  EXPECT_FLOAT_EQ(desc.opacity, 1.0f);
  EXPECT_FLOAT_EQ(desc.specularIor, 1.5f);
  EXPECT_FLOAT_EQ(desc.coatWeight, 0.25f);
  EXPECT_FLOAT_EQ(desc.coatRoughness, 0.07f);
  EXPECT_FLOAT_EQ(desc.emissionColor.x, 0.1f);
  EXPECT_FLOAT_EQ(desc.emissionColor.y, 0.2f);
  EXPECT_FLOAT_EQ(desc.emissionColor.z, 0.3f);

  // Opacity == 1 → no transmission heuristic kicks in.
  EXPECT_FLOAT_EQ(desc.transmissionWeight, 0.0f);
}

// Opacity < 1 + metalness < 0.5 should kick the M4 transmission
// heuristic in (1 - opacity) per UsdShadeToOpenPBR.cpp's stub.
TEST(UsdShadeToOpenPBR, OpacityBelowOneSetsTransmissionWeight) {
  const pxr::UsdStageRefPtr stage = pxr::UsdStage::CreateInMemory();
  ASSERT_TRUE(stage);
  const pxr::UsdShadeMaterial material =
      AuthorPreviewSurfaceMaterial(stage, "/Materials/Glass");

  // Override opacity to drop below 1.
  const pxr::UsdShadeShader shader(
      stage->GetPrimAtPath(pxr::SdfPath("/Materials/Glass/Surface")));
  shader.GetInput(pxr::TfToken("opacity")).Set(0.10f);

  const pyxis::OpenPBRMaterialDesc desc =
      pyxis::material_translation::FromUsdShade(material);

  EXPECT_FLOAT_EQ(desc.opacity, 0.10f);
  EXPECT_FLOAT_EQ(desc.transmissionWeight, 0.90f);
}

// A material with no surface output / no UsdPreviewSurface shader
// returns the grey default with Source::Default — the renderer's
// degraded path picks that up + flags the prim.
TEST(UsdShadeToOpenPBR, MissingPreviewSurfaceFallsBackToDefault) {
  const pxr::UsdStageRefPtr stage = pxr::UsdStage::CreateInMemory();
  ASSERT_TRUE(stage);
  // Define a material with NO surface output → translator falls back.
  const pxr::UsdShadeMaterial material =
      pxr::UsdShadeMaterial::Define(stage, pxr::SdfPath("/Materials/Empty"));

  const pyxis::OpenPBRMaterialDesc desc =
      pyxis::material_translation::FromUsdShade(material);

  EXPECT_EQ(desc.source, pyxis::OpenPBRMaterialDesc::Source::Default);
  // The fallback values come from OpenPBRMaterialDesc's POD defaults
  // (baseColor 0.8 grey).
  EXPECT_FLOAT_EQ(desc.baseColor.x, 0.8f);
  EXPECT_FLOAT_EQ(desc.baseColor.y, 0.8f);
  EXPECT_FLOAT_EQ(desc.baseColor.z, 0.8f);
}
