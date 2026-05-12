// Pyxis V2.A.8 — MaterialX `open_pbr_surface` / `standard_surface`
// translation. Author each shader with distinct input names, verify
// FromUsdShade maps them onto OpenPBRMaterialDesc.

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

pxr::UsdShadeMaterial BuildSurface(const pxr::UsdStageRefPtr& stage,
                                   const pxr::TfToken& shaderIdToken)
{
  const pxr::UsdShadeMaterial material =
      pxr::UsdShadeMaterial::Define(stage, pxr::SdfPath("/Mat"));
  pxr::UsdShadeShader shader =
      pxr::UsdShadeShader::Define(stage, pxr::SdfPath("/Mat/Surface"));
  shader.CreateIdAttr(pxr::VtValue(shaderIdToken));
  const pxr::UsdShadeOutput shaderOut =
      shader.CreateOutput(pxr::TfToken("surface"), pxr::SdfValueTypeNames->Token);
  material.CreateSurfaceOutput().ConnectToSource(shaderOut);
  return material;
}

}  // namespace

TEST(MaterialXTranslation, OpenPbrInputsMapToOpenPBRFields)
{
  const pxr::UsdStageRefPtr stage = pxr::UsdStage::CreateInMemory("opbr.usda");
  const pxr::UsdShadeMaterial material =
      BuildSurface(stage, pxr::TfToken("ND_open_pbr_surface_surfaceshader"));
  // NOLINTNEXTLINE(misc-const-correctness) — CreateInput mutates.
  pxr::UsdShadeShader shader{stage->GetPrimAtPath(pxr::SdfPath("/Mat/Surface"))};
  shader.CreateInput(pxr::TfToken("base_color"), pxr::SdfValueTypeNames->Color3f)
      .Set(pxr::GfVec3f(0.4f, 0.5f, 0.6f));
  shader.CreateInput(pxr::TfToken("base_metalness"), pxr::SdfValueTypeNames->Float)
      .Set(0.8f);
  shader.CreateInput(pxr::TfToken("specular_roughness"), pxr::SdfValueTypeNames->Float)
      .Set(0.15f);
  shader.CreateInput(pxr::TfToken("emission_luminance"), pxr::SdfValueTypeNames->Float)
      .Set(10.0f);
  shader.CreateInput(pxr::TfToken("specular_ior"), pxr::SdfValueTypeNames->Float)
      .Set(1.6f);

  const pyxis::OpenPBRMaterialDesc desc =
      pyxis::material_translation::FromUsdShade(material);

  EXPECT_NEAR(desc.baseColor.x, 0.4f, 1e-6f);
  EXPECT_NEAR(desc.metalness,    0.8f, 1e-6f);
  EXPECT_NEAR(desc.roughness,    0.15f, 1e-6f);
  EXPECT_NEAR(desc.emissionLuminance, 10.0f, 1e-6f);
  EXPECT_NEAR(desc.specularIor,  1.6f, 1e-6f);
  EXPECT_EQ(desc.source, pyxis::OpenPBRMaterialDesc::Source::MaterialX);
}

TEST(MaterialXTranslation, StandardSurfaceInputsMapToOpenPBRFields)
{
  const pxr::UsdStageRefPtr stage = pxr::UsdStage::CreateInMemory("std.usda");
  const pxr::UsdShadeMaterial material =
      BuildSurface(stage, pxr::TfToken("ND_standard_surface_surfaceshader"));
  // NOLINTNEXTLINE(misc-const-correctness) — CreateInput mutates.
  pxr::UsdShadeShader shader{stage->GetPrimAtPath(pxr::SdfPath("/Mat/Surface"))};
  shader.CreateInput(pxr::TfToken("base"), pxr::SdfValueTypeNames->Float)
      .Set(0.85f);
  shader.CreateInput(pxr::TfToken("base_color"), pxr::SdfValueTypeNames->Color3f)
      .Set(pxr::GfVec3f(0.9f, 0.1f, 0.05f));
  shader.CreateInput(pxr::TfToken("metalness"), pxr::SdfValueTypeNames->Float)
      .Set(0.0f);
  shader.CreateInput(pxr::TfToken("specular_roughness"), pxr::SdfValueTypeNames->Float)
      .Set(0.35f);
  shader.CreateInput(pxr::TfToken("emission"), pxr::SdfValueTypeNames->Float)
      .Set(3.0f);
  shader.CreateInput(pxr::TfToken("specular_IOR"), pxr::SdfValueTypeNames->Float)
      .Set(1.45f);

  const pyxis::OpenPBRMaterialDesc desc =
      pyxis::material_translation::FromUsdShade(material);

  EXPECT_NEAR(desc.baseWeight,   0.85f, 1e-6f);
  EXPECT_NEAR(desc.baseColor.x,  0.9f,  1e-6f);
  EXPECT_NEAR(desc.metalness,    0.0f,  1e-6f);
  EXPECT_NEAR(desc.roughness,    0.35f, 1e-6f);
  EXPECT_NEAR(desc.emissionLuminance, 3.0f, 1e-6f);
  EXPECT_NEAR(desc.specularIor,  1.45f, 1e-6f);
  EXPECT_EQ(desc.source, pyxis::OpenPBRMaterialDesc::Source::MaterialX);
}
