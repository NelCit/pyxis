// Pyxis V2.A.23 — MDL OmniPBR translation. Author a UsdShadeMaterial
// whose surface output connects to a shader with `info:id = "mdl::OmniPBR"`
// and verify that FromUsdShade reads the MDL inputs into OpenPBR fields.

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

pxr::UsdShadeMaterial BuildMdlMaterial(const pxr::UsdStageRefPtr& stage)
{
  const pxr::UsdShadeMaterial material =
      pxr::UsdShadeMaterial::Define(stage, pxr::SdfPath("/Mat"));

  pxr::UsdShadeShader shader =
      pxr::UsdShadeShader::Define(stage, pxr::SdfPath("/Mat/MdlSurface"));
  shader.CreateIdAttr(pxr::VtValue(pxr::TfToken("mdl::OmniPBR")));

  shader.CreateInput(pxr::TfToken("diffuse_color_constant"),
                     pxr::SdfValueTypeNames->Color3f)
      .Set(pxr::GfVec3f(0.7f, 0.2f, 0.1f));
  shader.CreateInput(pxr::TfToken("metallic_constant"),
                     pxr::SdfValueTypeNames->Float)
      .Set(0.9f);
  shader.CreateInput(pxr::TfToken("reflection_roughness_constant"),
                     pxr::SdfValueTypeNames->Float)
      .Set(0.2f);
  shader.CreateInput(pxr::TfToken("emissive_color"),
                     pxr::SdfValueTypeNames->Color3f)
      .Set(pxr::GfVec3f(0.0f, 1.0f, 0.5f));
  shader.CreateInput(pxr::TfToken("emissive_intensity"),
                     pxr::SdfValueTypeNames->Float)
      .Set(50.0f);
  shader.CreateInput(pxr::TfToken("ior_constant"),
                     pxr::SdfValueTypeNames->Float)
      .Set(1.7f);

  const pxr::UsdShadeOutput shaderOut =
      shader.CreateOutput(pxr::TfToken("surface"), pxr::SdfValueTypeNames->Token);
  material.CreateSurfaceOutput().ConnectToSource(shaderOut);

  return material;
}

}  // namespace

TEST(MdlTranslation, OmniPBRInputsMapToOpenPBRFields)
{
  const pxr::UsdStageRefPtr stage = pxr::UsdStage::CreateInMemory("mdl.usda");
  const pxr::UsdShadeMaterial material = BuildMdlMaterial(stage);

  const pyxis::OpenPBRMaterialDesc desc =
      pyxis::material_translation::FromUsdShade(material);

  EXPECT_NEAR(desc.baseColor.x, 0.7f, 1e-6f);
  EXPECT_NEAR(desc.baseColor.y, 0.2f, 1e-6f);
  EXPECT_NEAR(desc.baseColor.z, 0.1f, 1e-6f);
  EXPECT_NEAR(desc.metalness, 0.9f, 1e-6f);
  EXPECT_NEAR(desc.roughness, 0.2f, 1e-6f);
  EXPECT_NEAR(desc.emissionColor.x, 0.0f, 1e-6f);
  EXPECT_NEAR(desc.emissionColor.y, 1.0f, 1e-6f);
  EXPECT_NEAR(desc.emissionColor.z, 0.5f, 1e-6f);
  EXPECT_NEAR(desc.emissionLuminance, 50.0f, 1e-6f);
  EXPECT_NEAR(desc.specularIor, 1.7f, 1e-6f);

  // V2.A.23 — MDL materials report the MaterialX Source tag (groups
  // with non-UsdPreviewSurface networks in the M18 health report).
  EXPECT_EQ(desc.source, pyxis::OpenPBRMaterialDesc::Source::MaterialX);
}

TEST(MdlTranslation, EnableEmissionOffSuppressesLuminance)
{
  const pxr::UsdStageRefPtr stage = pxr::UsdStage::CreateInMemory("mdl_no_emit.usda");
  const pxr::UsdShadeMaterial material = BuildMdlMaterial(stage);
  // NOLINTNEXTLINE(misc-const-correctness) — CreateInput mutates.
  pxr::UsdShadeShader shader{stage->GetPrimAtPath(pxr::SdfPath("/Mat/MdlSurface"))};
  ASSERT_TRUE(shader);
  shader.CreateInput(pxr::TfToken("enable_emission"),
                     pxr::SdfValueTypeNames->Float)
      .Set(0.0f);

  const pyxis::OpenPBRMaterialDesc desc =
      pyxis::material_translation::FromUsdShade(material);

  // enable_emission off → emissionLuminance zeroed regardless of
  // emissive_intensity.
  EXPECT_FLOAT_EQ(desc.emissionLuminance, 0.0f);
}
