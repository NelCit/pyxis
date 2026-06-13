// Pyxis V2.A.8 — MaterialX `open_pbr_surface` / `standard_surface`
// translation. Author each shader with distinct input names, verify
// FromUsdShade maps them onto OpenPBRMaterialDesc.

#include <Pyxis/MaterialTranslation/UsdShadeToOpenPBR.h>
#include <Pyxis/Renderer/Descs/OpenPBRMaterialDesc.h>
#include <Pyxis/Renderer/Descs/TextureKey.h>
#include <Pyxis/Renderer/Forward.h>

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

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

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

// Records the AcquireTexture calls the translator makes (Q3 — MaterialX texture
// connections). Returns a distinct non-Invalid handle per call so the desc map
// slots can be asserted as "bound".
struct AcquireRecord {
  std::vector<std::string>             paths;
  std::vector<pyxis::TextureKey::Role> roles;
};

pyxis::TextureHandle RecordingAcquire(std::string_view path,
                                      pyxis::TextureKey::Role role,
                                      void* userData)
{
  auto* rec = static_cast<AcquireRecord*>(userData);
  rec->paths.emplace_back(path);
  rec->roles.push_back(role);
  return static_cast<pyxis::TextureHandle>(
      1000u + static_cast<std::uint32_t>(rec->paths.size()));
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

// Q1 OpenPBR-complete — full new-parameter extraction for
// open_pbr_surface. Every Q1 desc-extension field has a 1:1 spec
// input; author them all with distinct values and verify the
// field-for-field mapping.
TEST(MaterialXTranslation, OpenPbrCompleteParameterSetMapsToQ1Fields)
{
  const pxr::UsdStageRefPtr stage = pxr::UsdStage::CreateInMemory("opbr_full.usda");
  const pxr::UsdShadeMaterial material =
      BuildSurface(stage, pxr::TfToken("ND_open_pbr_surface_surfaceshader"));
  // NOLINTNEXTLINE(misc-const-correctness) — CreateInput mutates.
  pxr::UsdShadeShader shader{stage->GetPrimAtPath(pxr::SdfPath("/Mat/Surface"))};
  shader.CreateInput(pxr::TfToken("specular_color"), pxr::SdfValueTypeNames->Color3f)
      .Set(pxr::GfVec3f(0.9f, 0.8f, 0.7f));
  shader.CreateInput(pxr::TfToken("base_diffuse_roughness"), pxr::SdfValueTypeNames->Float)
      .Set(0.4f);
  shader.CreateInput(pxr::TfToken("specular_roughness_anisotropy"),
                     pxr::SdfValueTypeNames->Float)
      .Set(0.25f);
  shader.CreateInput(pxr::TfToken("coat_color"), pxr::SdfValueTypeNames->Color3f)
      .Set(pxr::GfVec3f(1.0f, 0.85f, 0.65f));
  shader.CreateInput(pxr::TfToken("coat_ior"), pxr::SdfValueTypeNames->Float)
      .Set(1.55f);
  shader.CreateInput(pxr::TfToken("coat_darkening"), pxr::SdfValueTypeNames->Float)
      .Set(0.35f);
  shader.CreateInput(pxr::TfToken("fuzz_weight"), pxr::SdfValueTypeNames->Float)
      .Set(0.6f);
  shader.CreateInput(pxr::TfToken("fuzz_color"), pxr::SdfValueTypeNames->Color3f)
      .Set(pxr::GfVec3f(0.1f, 0.2f, 0.3f));
  shader.CreateInput(pxr::TfToken("fuzz_roughness"), pxr::SdfValueTypeNames->Float)
      .Set(0.8f);
  shader.CreateInput(pxr::TfToken("transmission_color"), pxr::SdfValueTypeNames->Color3f)
      .Set(pxr::GfVec3f(0.95f, 0.9f, 0.85f));
  shader.CreateInput(pxr::TfToken("subsurface_weight"), pxr::SdfValueTypeNames->Float)
      .Set(0.45f);
  shader.CreateInput(pxr::TfToken("subsurface_color"), pxr::SdfValueTypeNames->Color3f)
      .Set(pxr::GfVec3f(0.7f, 0.3f, 0.2f));
  shader.CreateInput(pxr::TfToken("geometry_thin_walled"), pxr::SdfValueTypeNames->Bool)
      .Set(true);

  const pyxis::OpenPBRMaterialDesc desc =
      pyxis::material_translation::FromUsdShade(material);

  EXPECT_NEAR(desc.specularColorR, 0.9f, 1e-6f);
  EXPECT_NEAR(desc.specularColorG, 0.8f, 1e-6f);
  EXPECT_NEAR(desc.specularColorB, 0.7f, 1e-6f);
  EXPECT_NEAR(desc.baseDiffuseRoughness, 0.4f, 1e-6f);
  EXPECT_NEAR(desc.specularRoughnessAnisotropy, 0.25f, 1e-6f);
  EXPECT_NEAR(desc.coatColorR, 1.0f,  1e-6f);
  EXPECT_NEAR(desc.coatColorG, 0.85f, 1e-6f);
  EXPECT_NEAR(desc.coatColorB, 0.65f, 1e-6f);
  EXPECT_NEAR(desc.coatIor, 1.55f, 1e-6f);
  EXPECT_NEAR(desc.coatDarkening, 0.35f, 1e-6f);
  EXPECT_NEAR(desc.fuzzWeight, 0.6f, 1e-6f);
  EXPECT_NEAR(desc.fuzzColorR, 0.1f, 1e-6f);
  EXPECT_NEAR(desc.fuzzColorG, 0.2f, 1e-6f);
  EXPECT_NEAR(desc.fuzzColorB, 0.3f, 1e-6f);
  EXPECT_NEAR(desc.fuzzRoughness, 0.8f, 1e-6f);
  EXPECT_NEAR(desc.transmissionColorR, 0.95f, 1e-6f);
  EXPECT_NEAR(desc.transmissionColorG, 0.9f,  1e-6f);
  EXPECT_NEAR(desc.transmissionColorB, 0.85f, 1e-6f);
  EXPECT_NEAR(desc.subsurfaceWeight, 0.45f, 1e-6f);
  EXPECT_NEAR(desc.subsurfaceColorR, 0.7f, 1e-6f);
  EXPECT_NEAR(desc.subsurfaceColorG, 0.3f, 1e-6f);
  EXPECT_NEAR(desc.subsurfaceColorB, 0.2f, 1e-6f);
  EXPECT_EQ(desc.thinWalled, 1u);
  EXPECT_EQ(desc.source, pyxis::OpenPBRMaterialDesc::Source::MaterialX);
}

// Q1 OpenPBR-complete — unauthored open_pbr_surface inputs fall back
// to the OpenPBR spec defaults (= the desc defaults).
TEST(MaterialXTranslation, OpenPbrCompleteUnauthoredInputsKeepSpecDefaults)
{
  const pxr::UsdStageRefPtr stage = pxr::UsdStage::CreateInMemory("opbr_defaults.usda");
  const pxr::UsdShadeMaterial material =
      BuildSurface(stage, pxr::TfToken("ND_open_pbr_surface_surfaceshader"));

  const pyxis::OpenPBRMaterialDesc desc =
      pyxis::material_translation::FromUsdShade(material);

  EXPECT_NEAR(desc.specularColorR, 1.0f, 1e-6f);
  EXPECT_NEAR(desc.coatIor, 1.6f, 1e-6f);
  EXPECT_NEAR(desc.coatDarkening, 1.0f, 1e-6f);
  EXPECT_NEAR(desc.fuzzRoughness, 0.5f, 1e-6f);
  EXPECT_NEAR(desc.subsurfaceColorR, 0.8f, 1e-6f);
  EXPECT_EQ(desc.thinWalled, 0u);
}

// Q1 OpenPBR-complete — Standard Surface sheen_* maps onto the
// OpenPBR fuzz lobe; coat_color / coat_IOR / specular_color /
// specular_anisotropy / subsurface / transmission_color / thin_walled
// map onto their Q1 fields. coat_affect_color > 0 maps to
// coat_darkening = 1 (its OpenPBR replacement's default);
// coat_affect_roughness has no equivalent and is skipped.
TEST(MaterialXTranslation, StandardSurfaceSheenAndCoatMapToQ1Fields)
{
  const pxr::UsdStageRefPtr stage = pxr::UsdStage::CreateInMemory("std_full.usda");
  const pxr::UsdShadeMaterial material =
      BuildSurface(stage, pxr::TfToken("ND_standard_surface_surfaceshader"));
  // NOLINTNEXTLINE(misc-const-correctness) — CreateInput mutates.
  pxr::UsdShadeShader shader{stage->GetPrimAtPath(pxr::SdfPath("/Mat/Surface"))};
  shader.CreateInput(pxr::TfToken("sheen"), pxr::SdfValueTypeNames->Float)
      .Set(0.75f);
  shader.CreateInput(pxr::TfToken("sheen_color"), pxr::SdfValueTypeNames->Color3f)
      .Set(pxr::GfVec3f(0.9f, 0.6f, 0.3f));
  shader.CreateInput(pxr::TfToken("sheen_roughness"), pxr::SdfValueTypeNames->Float)
      .Set(0.2f);
  shader.CreateInput(pxr::TfToken("coat_color"), pxr::SdfValueTypeNames->Color3f)
      .Set(pxr::GfVec3f(0.8f, 0.7f, 0.6f));
  shader.CreateInput(pxr::TfToken("coat_IOR"), pxr::SdfValueTypeNames->Float)
      .Set(1.52f);
  shader.CreateInput(pxr::TfToken("coat_affect_color"), pxr::SdfValueTypeNames->Float)
      .Set(0.9f);
  shader.CreateInput(pxr::TfToken("coat_affect_roughness"), pxr::SdfValueTypeNames->Float)
      .Set(0.5f);  // no OpenPBR equivalent — must be skipped
  shader.CreateInput(pxr::TfToken("specular_color"), pxr::SdfValueTypeNames->Color3f)
      .Set(pxr::GfVec3f(0.5f, 0.6f, 0.7f));
  shader.CreateInput(pxr::TfToken("specular_anisotropy"), pxr::SdfValueTypeNames->Float)
      .Set(0.35f);
  shader.CreateInput(pxr::TfToken("subsurface"), pxr::SdfValueTypeNames->Float)
      .Set(0.4f);
  shader.CreateInput(pxr::TfToken("subsurface_color"), pxr::SdfValueTypeNames->Color3f)
      .Set(pxr::GfVec3f(0.6f, 0.2f, 0.1f));
  shader.CreateInput(pxr::TfToken("transmission_color"), pxr::SdfValueTypeNames->Color3f)
      .Set(pxr::GfVec3f(0.85f, 0.9f, 0.95f));
  shader.CreateInput(pxr::TfToken("thin_walled"), pxr::SdfValueTypeNames->Bool)
      .Set(true);

  const pyxis::OpenPBRMaterialDesc desc =
      pyxis::material_translation::FromUsdShade(material);

  // sheen → fuzz.
  EXPECT_NEAR(desc.fuzzWeight, 0.75f, 1e-6f);
  EXPECT_NEAR(desc.fuzzColorR, 0.9f, 1e-6f);
  EXPECT_NEAR(desc.fuzzColorG, 0.6f, 1e-6f);
  EXPECT_NEAR(desc.fuzzColorB, 0.3f, 1e-6f);
  EXPECT_NEAR(desc.fuzzRoughness, 0.2f, 1e-6f);
  // coat tint + IOR.
  EXPECT_NEAR(desc.coatColorR, 0.8f, 1e-6f);
  EXPECT_NEAR(desc.coatColorG, 0.7f, 1e-6f);
  EXPECT_NEAR(desc.coatColorB, 0.6f, 1e-6f);
  EXPECT_NEAR(desc.coatIor, 1.52f, 1e-6f);
  // coat_affect_color > 0 → coatDarkening stays at its default 1.
  EXPECT_NEAR(desc.coatDarkening, 1.0f, 1e-6f);
  // specular tint + anisotropy.
  EXPECT_NEAR(desc.specularColorR, 0.5f, 1e-6f);
  EXPECT_NEAR(desc.specularColorG, 0.6f, 1e-6f);
  EXPECT_NEAR(desc.specularColorB, 0.7f, 1e-6f);
  EXPECT_NEAR(desc.specularRoughnessAnisotropy, 0.35f, 1e-6f);
  // subsurface + transmission tint + thin-walled.
  EXPECT_NEAR(desc.subsurfaceWeight, 0.4f, 1e-6f);
  EXPECT_NEAR(desc.subsurfaceColorR, 0.6f, 1e-6f);
  EXPECT_NEAR(desc.transmissionColorR, 0.85f, 1e-6f);
  EXPECT_NEAR(desc.transmissionColorB, 0.95f, 1e-6f);
  EXPECT_EQ(desc.thinWalled, 1u);
  EXPECT_EQ(desc.source, pyxis::OpenPBRMaterialDesc::Source::MaterialX);
}

// Q3 — the dominant real-world authoring shape: surface inputs are NOT direct
// values but connections to PROMOTED INTERFACE INPUTS on the material/nodegraph
// (the OpenPBR Playground authors all 55 materials this way). Pre-Q3 the
// translator read only `.Get()` on the surface shader, saw "no value authored"
// for every connected input, and fell back to OpenPBR defaults — so glass
// (transmission_weight as an interface input) rendered opaque. The resolver
// must follow the connection to the authored constant.
TEST(MaterialXTranslation, ConnectedInterfaceInputResolvesToConstant)
{
  const pxr::UsdStageRefPtr stage = pxr::UsdStage::CreateInMemory("iface.usda");
  const pxr::UsdShadeMaterial material =
      BuildSurface(stage, pxr::TfToken("ND_open_pbr_surface_surfaceshader"));
  // NOLINTNEXTLINE(misc-const-correctness) — CreateInput mutates.
  pxr::UsdShadeShader shader{stage->GetPrimAtPath(pxr::SdfPath("/Mat/Surface"))};

  // Promote transmission_weight to a material interface input holding 0.9 and
  // connect the surface shader's input to it (the glass authoring shape).
  pxr::UsdShadeInput ifaceTransmission =
      material.CreateInput(pxr::TfToken("transmission_weight"),
                           pxr::SdfValueTypeNames->Float);
  ifaceTransmission.Set(0.9f);
  shader.CreateInput(pxr::TfToken("transmission_weight"), pxr::SdfValueTypeNames->Float)
      .ConnectToSource(ifaceTransmission);
  // Colors resolve through interface inputs too.
  pxr::UsdShadeInput ifaceColor =
      material.CreateInput(pxr::TfToken("base_color"), pxr::SdfValueTypeNames->Color3f);
  ifaceColor.Set(pxr::GfVec3f(0.2f, 0.7f, 0.3f));
  shader.CreateInput(pxr::TfToken("base_color"), pxr::SdfValueTypeNames->Color3f)
      .ConnectToSource(ifaceColor);

  const pyxis::OpenPBRMaterialDesc desc =
      pyxis::material_translation::FromUsdShade(material);

  EXPECT_NEAR(desc.transmissionWeight, 0.9f, 1e-6f);
  EXPECT_NEAR(desc.baseColor.x, 0.2f, 1e-6f);
  EXPECT_NEAR(desc.baseColor.y, 0.7f, 1e-6f);
  EXPECT_NEAR(desc.baseColor.z, 0.3f, 1e-6f);
  EXPECT_EQ(desc.source, pyxis::OpenPBRMaterialDesc::Source::MaterialX);
}

// Q3 — base_color connected to an `ND_image_color3` node resolves to a texture:
// the acquire callback fires with the file path + BaseColor role, the map slot
// is bound, and the scalar baseColor becomes white (texture × 1).
TEST(MaterialXTranslation, ConnectedImageNodeResolvesToTexture)
{
  const pxr::UsdStageRefPtr stage = pxr::UsdStage::CreateInMemory("img.usda");
  const pxr::UsdShadeMaterial material =
      BuildSurface(stage, pxr::TfToken("ND_open_pbr_surface_surfaceshader"));
  // NOLINTNEXTLINE(misc-const-correctness) — CreateInput mutates.
  pxr::UsdShadeShader shader{stage->GetPrimAtPath(pxr::SdfPath("/Mat/Surface"))};

  pxr::UsdShadeShader image =
      pxr::UsdShadeShader::Define(stage, pxr::SdfPath("/Mat/BaseTex"));
  image.CreateIdAttr(pxr::VtValue(pxr::TfToken("ND_image_color3")));
  image.CreateInput(pxr::TfToken("file"), pxr::SdfValueTypeNames->Asset)
      .Set(pxr::SdfAssetPath("textures/base_color.png"));
  const pxr::UsdShadeOutput imageOut =
      image.CreateOutput(pxr::TfToken("out"), pxr::SdfValueTypeNames->Color3f);
  shader.CreateInput(pxr::TfToken("base_color"), pxr::SdfValueTypeNames->Color3f)
      .ConnectToSource(imageOut);

  AcquireRecord rec;
  const pyxis::OpenPBRMaterialDesc desc =
      pyxis::material_translation::FromUsdShade(material, &RecordingAcquire, &rec);

  ASSERT_EQ(rec.paths.size(), 1u);
  EXPECT_EQ(rec.roles.front(), pyxis::TextureKey::Role::BaseColor);
  EXPECT_NE(desc.baseColorMap, pyxis::TextureHandle::Invalid);
  EXPECT_NEAR(desc.baseColor.x, 1.0f, 1e-6f);
  EXPECT_NEAR(desc.baseColor.y, 1.0f, 1e-6f);
  EXPECT_NEAR(desc.baseColor.z, 1.0f, 1e-6f);
}

// Q3 — geometry_normal connected through an `ND_normalmap` wrapping an
// `ND_image_vector3` resolves to the normal-map texture (one unwrap hop).
TEST(MaterialXTranslation, ConnectedNormalmapResolvesToNormalTexture)
{
  const pxr::UsdStageRefPtr stage = pxr::UsdStage::CreateInMemory("nrm.usda");
  const pxr::UsdShadeMaterial material =
      BuildSurface(stage, pxr::TfToken("ND_open_pbr_surface_surfaceshader"));
  // NOLINTNEXTLINE(misc-const-correctness) — CreateInput mutates.
  pxr::UsdShadeShader shader{stage->GetPrimAtPath(pxr::SdfPath("/Mat/Surface"))};

  pxr::UsdShadeShader image =
      pxr::UsdShadeShader::Define(stage, pxr::SdfPath("/Mat/NrmTex"));
  image.CreateIdAttr(pxr::VtValue(pxr::TfToken("ND_image_vector3")));
  image.CreateInput(pxr::TfToken("file"), pxr::SdfValueTypeNames->Asset)
      .Set(pxr::SdfAssetPath("textures/normal.png"));
  const pxr::UsdShadeOutput imageOut =
      image.CreateOutput(pxr::TfToken("out"), pxr::SdfValueTypeNames->Float3);

  pxr::UsdShadeShader normalmap =
      pxr::UsdShadeShader::Define(stage, pxr::SdfPath("/Mat/Nmap"));
  normalmap.CreateIdAttr(pxr::VtValue(pxr::TfToken("ND_normalmap_vector2")));
  normalmap.CreateInput(pxr::TfToken("in"), pxr::SdfValueTypeNames->Float3)
      .ConnectToSource(imageOut);
  const pxr::UsdShadeOutput normalmapOut =
      normalmap.CreateOutput(pxr::TfToken("out"), pxr::SdfValueTypeNames->Float3);
  shader.CreateInput(pxr::TfToken("geometry_normal"), pxr::SdfValueTypeNames->Float3)
      .ConnectToSource(normalmapOut);

  AcquireRecord rec;
  const pyxis::OpenPBRMaterialDesc desc =
      pyxis::material_translation::FromUsdShade(material, &RecordingAcquire, &rec);

  ASSERT_EQ(rec.paths.size(), 1u);
  EXPECT_EQ(rec.roles.front(), pyxis::TextureKey::Role::NormalMap);
  EXPECT_NE(desc.normalMap, pyxis::TextureHandle::Invalid);
}
