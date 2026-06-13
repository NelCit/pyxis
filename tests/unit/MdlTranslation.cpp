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
  // V2.A.23 follow-up — MDL's `enable_emission` defaults to FALSE, and
  // the translator now honours that (a missing input = disabled).
  // Author it true here so the emissive_color / emissive_intensity
  // values flow through to the desc; the "off" case is covered by the
  // EnableEmissionOffSuppressesLuminance test below.
  shader.CreateInput(pxr::TfToken("enable_emission"),
                     pxr::SdfValueTypeNames->Bool)
      .Set(true);
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

  // V2.A.23 — MDL materials report the dedicated Mdl Source tag
  // (previously grouped under MaterialX; split out so the M18 health
  // report shows the MDL slice separately from MaterialX).
  EXPECT_EQ(desc.source, pyxis::OpenPBRMaterialDesc::Source::Mdl);
}

TEST(MdlTranslation, EnableEmissionOffSuppressesLuminance)
{
  const pxr::UsdStageRefPtr stage = pxr::UsdStage::CreateInMemory("mdl_no_emit.usda");
  const pxr::UsdShadeMaterial material = BuildMdlMaterial(stage);
  // NOLINTNEXTLINE(misc-const-correctness) — CreateInput mutates.
  pxr::UsdShadeShader shader{stage->GetPrimAtPath(pxr::SdfPath("/Mat/MdlSurface"))};
  ASSERT_TRUE(shader);
  // Override the BuildMdlMaterial default (enable_emission = true)
  // back to false. Bool to match the input's authored type.
  shader.CreateInput(pxr::TfToken("enable_emission"),
                     pxr::SdfValueTypeNames->Bool)
      .Set(false);

  const pyxis::OpenPBRMaterialDesc desc =
      pyxis::material_translation::FromUsdShade(material);

  // enable_emission off → emissionLuminance + color zeroed regardless
  // of authored emissive_intensity / emissive_color.
  EXPECT_FLOAT_EQ(desc.emissionLuminance, 0.0f);
  EXPECT_FLOAT_EQ(desc.emissionColor.x, 0.0f);
  EXPECT_FLOAT_EQ(desc.emissionColor.y, 0.0f);
  EXPECT_FLOAT_EQ(desc.emissionColor.z, 0.0f);
}

// Q1 OpenPBR-complete — OmniPBR clearcoat → OpenPBR coat. Gated on
// enable_clearcoat (MDL function default false), mirroring the
// enable_emission convention.
TEST(MdlTranslation, OmniPBRClearcoatMapsToCoatFields)
{
  const pxr::UsdStageRefPtr stage = pxr::UsdStage::CreateInMemory("mdl_coat.usda");
  const pxr::UsdShadeMaterial material = BuildMdlMaterial(stage);
  // NOLINTNEXTLINE(misc-const-correctness) — CreateInput mutates.
  pxr::UsdShadeShader shader{stage->GetPrimAtPath(pxr::SdfPath("/Mat/MdlSurface"))};
  ASSERT_TRUE(shader);
  shader.CreateInput(pxr::TfToken("enable_clearcoat"), pxr::SdfValueTypeNames->Bool)
      .Set(true);
  shader.CreateInput(pxr::TfToken("clearcoat_weight"), pxr::SdfValueTypeNames->Float)
      .Set(0.8f);
  shader.CreateInput(pxr::TfToken("clearcoat_tint"), pxr::SdfValueTypeNames->Color3f)
      .Set(pxr::GfVec3f(1.0f, 0.9f, 0.8f));
  shader.CreateInput(pxr::TfToken("clearcoat_ior"), pxr::SdfValueTypeNames->Float)
      .Set(1.45f);
  shader.CreateInput(pxr::TfToken("clearcoat_reflection_roughness"),
                     pxr::SdfValueTypeNames->Float)
      .Set(0.12f);

  const pyxis::OpenPBRMaterialDesc desc =
      pyxis::material_translation::FromUsdShade(material);

  EXPECT_NEAR(desc.coatWeight, 0.8f, 1e-6f);
  EXPECT_NEAR(desc.coatColorR, 1.0f, 1e-6f);
  EXPECT_NEAR(desc.coatColorG, 0.9f, 1e-6f);
  EXPECT_NEAR(desc.coatColorB, 0.8f, 1e-6f);
  EXPECT_NEAR(desc.coatIor, 1.45f, 1e-6f);
  EXPECT_NEAR(desc.coatRoughness, 0.12f, 1e-6f);
}

// Q1 OpenPBR-complete — clearcoat inputs WITHOUT enable_clearcoat
// stay inert (MDL function default false; missing input = MDL default).
TEST(MdlTranslation, OmniPBRClearcoatDisabledKeepsCoatDefaults)
{
  const pxr::UsdStageRefPtr stage = pxr::UsdStage::CreateInMemory("mdl_nocoat.usda");
  const pxr::UsdShadeMaterial material = BuildMdlMaterial(stage);
  // NOLINTNEXTLINE(misc-const-correctness) — CreateInput mutates.
  pxr::UsdShadeShader shader{stage->GetPrimAtPath(pxr::SdfPath("/Mat/MdlSurface"))};
  ASSERT_TRUE(shader);
  shader.CreateInput(pxr::TfToken("clearcoat_weight"), pxr::SdfValueTypeNames->Float)
      .Set(0.8f);
  shader.CreateInput(pxr::TfToken("clearcoat_tint"), pxr::SdfValueTypeNames->Color3f)
      .Set(pxr::GfVec3f(0.2f, 0.3f, 0.4f));

  const pyxis::OpenPBRMaterialDesc desc =
      pyxis::material_translation::FromUsdShade(material);

  EXPECT_FLOAT_EQ(desc.coatWeight, 0.0f);
  EXPECT_FLOAT_EQ(desc.coatColorR, 1.0f);
  EXPECT_FLOAT_EQ(desc.coatIor, 1.6f);
}

// Q1 OpenPBR-complete — OmniGlass → OpenPBR transmission. glass_color
// becomes the constant transmission tint, transmission is
// unconditionally on, glass_ior drives the dielectric Fresnel,
// frosting_roughness replaces the OmniPBR roughness chain, and
// thin_walled maps onto the flag field.
TEST(MdlTranslation, OmniGlassMapsToTransmissionFields)
{
  const pxr::UsdStageRefPtr stage = pxr::UsdStage::CreateInMemory("mdl_glass.usda");
  const pxr::UsdShadeMaterial material =
      pxr::UsdShadeMaterial::Define(stage, pxr::SdfPath("/Mat"));
  pxr::UsdShadeShader shader =
      pxr::UsdShadeShader::Define(stage, pxr::SdfPath("/Mat/MdlSurface"));
  shader.CreateIdAttr(pxr::VtValue(pxr::TfToken("mdl::OmniGlass")));
  shader.CreateInput(pxr::TfToken("glass_color"), pxr::SdfValueTypeNames->Color3f)
      .Set(pxr::GfVec3f(0.6f, 0.8f, 0.9f));
  shader.CreateInput(pxr::TfToken("glass_ior"), pxr::SdfValueTypeNames->Float)
      .Set(1.33f);
  shader.CreateInput(pxr::TfToken("frosting_roughness"), pxr::SdfValueTypeNames->Float)
      .Set(0.25f);
  shader.CreateInput(pxr::TfToken("thin_walled"), pxr::SdfValueTypeNames->Bool)
      .Set(true);
  const pxr::UsdShadeOutput shaderOut =
      shader.CreateOutput(pxr::TfToken("surface"), pxr::SdfValueTypeNames->Token);
  material.CreateSurfaceOutput().ConnectToSource(shaderOut);

  const pyxis::OpenPBRMaterialDesc desc =
      pyxis::material_translation::FromUsdShade(material);

  EXPECT_FLOAT_EQ(desc.transmissionWeight, 1.0f);
  EXPECT_NEAR(desc.transmissionColorR, 0.6f, 1e-6f);
  EXPECT_NEAR(desc.transmissionColorG, 0.8f, 1e-6f);
  EXPECT_NEAR(desc.transmissionColorB, 0.9f, 1e-6f);
  EXPECT_NEAR(desc.specularIor, 1.33f, 1e-6f);
  EXPECT_NEAR(desc.roughness, 0.25f, 1e-6f);
  EXPECT_EQ(desc.thinWalled, 1u);
  EXPECT_EQ(desc.source, pyxis::OpenPBRMaterialDesc::Source::Mdl);
}
