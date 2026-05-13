// Pyxis V2.A.18 — material network coverage (UsdTransform2d chains,
// PrimvarReader chain walks, render-context fallback, collection +
// purpose binding strength).
//
// Each test authors a small UsdShadeMaterial network in an in-memory
// stage and asserts the translator's output matches the expected
// fields. The translator itself lives in pyxis_material_translation;
// this fixture exercises it via the real shipped binary, never a
// reimplementation.

#include <Pyxis/MaterialTranslation/UsdShadeToOpenPBR.h>
#include <Pyxis/Renderer/Descs/OpenPBRMaterialDesc.h>

#include <pxr/base/gf/vec2f.h>
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

// Stub texture acquire — never returns Invalid because callers want
// the connection-walk path (which only fires when acquire is non-null).
pyxis::TextureHandle StubAcquireValid(std::string_view /*path*/,
                                       pyxis::TextureKey::Role /*role*/,
                                       void* /*userData*/)
{
  return pyxis::TextureHandle{1u};
}

// Author UsdUVTexture.st → UsdTransform2d.in → UsdPrimvarReader_float2.
// Returns the texture shader so callers can attach it to a Material.
pxr::UsdShadeShader AuthorTextureWithTransform2d(const pxr::UsdStageRefPtr& stage,
                                                  const char* materialPath,
                                                  const pxr::GfVec2f& translation,
                                                  float rotationDegrees,
                                                  const pxr::GfVec2f& scale,
                                                  const char* varname)
{
  const pxr::SdfPath base(materialPath);

  // Texture node
  pxr::UsdShadeShader texture =
      pxr::UsdShadeShader::Define(stage, base.AppendChild(pxr::TfToken("Tex")));
  texture.CreateIdAttr(pxr::VtValue(pxr::TfToken("UsdUVTexture")));
  texture.CreateInput(pxr::TfToken("file"), pxr::SdfValueTypeNames->Asset)
      .Set(pxr::SdfAssetPath("./tex.png"));

  // Transform2d node
  pxr::UsdShadeShader xform =
      pxr::UsdShadeShader::Define(stage, base.AppendChild(pxr::TfToken("Xform")));
  xform.CreateIdAttr(pxr::VtValue(pxr::TfToken("UsdTransform2d")));
  xform.CreateInput(pxr::TfToken("translation"), pxr::SdfValueTypeNames->Float2).Set(translation);
  xform.CreateInput(pxr::TfToken("rotation"),    pxr::SdfValueTypeNames->Float).Set(rotationDegrees);
  xform.CreateInput(pxr::TfToken("scale"),       pxr::SdfValueTypeNames->Float2).Set(scale);

  // PrimvarReader node
  pxr::UsdShadeShader reader =
      pxr::UsdShadeShader::Define(stage, base.AppendChild(pxr::TfToken("Reader")));
  reader.CreateIdAttr(pxr::VtValue(pxr::TfToken("UsdPrimvarReader_float2")));
  reader.CreateInput(pxr::TfToken("varname"), pxr::SdfValueTypeNames->Token)
      .Set(pxr::TfToken(varname));

  // Wire the chain: texture.st ← xform.result; xform.in ← reader.result.
  const pxr::UsdShadeOutput readerOut =
      reader.CreateOutput(pxr::TfToken("result"), pxr::SdfValueTypeNames->Float2);
  xform.CreateInput(pxr::TfToken("in"), pxr::SdfValueTypeNames->Float2)
      .ConnectToSource(readerOut);
  const pxr::UsdShadeOutput xformOut =
      xform.CreateOutput(pxr::TfToken("result"), pxr::SdfValueTypeNames->Float2);
  texture.CreateInput(pxr::TfToken("st"), pxr::SdfValueTypeNames->Float2)
      .ConnectToSource(xformOut);

  return texture;
}

}  // namespace

// V2.A.18 — UsdTransform2d in the texture's UV-chain populates the
// translator's UV-xform fields on the desc.
TEST(MaterialNetworkV2A18, UsdTransform2dStampsUvXformOnDesc)
{
  const pxr::UsdStageRefPtr stage = pxr::UsdStage::CreateInMemory("transform2d.usda");
  const pxr::UsdShadeMaterial material =
      pxr::UsdShadeMaterial::Define(stage, pxr::SdfPath("/Mat"));

  pxr::UsdShadeShader surface =
      pxr::UsdShadeShader::Define(stage, pxr::SdfPath("/Mat/Surface"));
  surface.CreateIdAttr(pxr::VtValue(pxr::TfToken("UsdPreviewSurface")));
  const pxr::UsdShadeOutput surfaceOut =
      surface.CreateOutput(pxr::TfToken("surface"), pxr::SdfValueTypeNames->Token);
  material.CreateSurfaceOutput().ConnectToSource(surfaceOut);

  // Author UsdUVTexture → UsdTransform2d → PrimvarReader, then plug
  // the texture's `rgb` into `diffuseColor`.
  pxr::UsdShadeShader texture = AuthorTextureWithTransform2d(
      stage, "/Mat",
      pxr::GfVec2f(0.25f, -0.10f),  // translation
      45.0f,                         // rotation (degrees)
      pxr::GfVec2f(2.0f, 3.0f),      // scale
      "st");                         // varname
  const pxr::UsdShadeOutput texOut =
      texture.CreateOutput(pxr::TfToken("rgb"), pxr::SdfValueTypeNames->Float3);
  surface.CreateInput(pxr::TfToken("diffuseColor"), pxr::SdfValueTypeNames->Color3f)
      .ConnectToSource(texOut);

  const pyxis::OpenPBRMaterialDesc desc =
      pyxis::material_translation::FromUsdShade(material, &StubAcquireValid, nullptr);

  // Source must remain UsdPreviewSurface (we're authoring under universal).
  EXPECT_EQ(desc.source, pyxis::OpenPBRMaterialDesc::Source::UsdPreviewSurface);

  // UV-xform fields must reflect the authored Transform2d.
  EXPECT_FLOAT_EQ(desc.baseColorUvTranslationX, 0.25f);
  EXPECT_FLOAT_EQ(desc.baseColorUvTranslationY, -0.10f);
  EXPECT_FLOAT_EQ(desc.baseColorUvRotationDeg,  45.0f);
  EXPECT_FLOAT_EQ(desc.baseColorUvScaleX,       2.0f);
  EXPECT_FLOAT_EQ(desc.baseColorUvScaleY,       3.0f);
}

// V2.A.18 — when there's no Transform2d in the chain, the desc's
// UV-xform fields stay at the identity defaults (translation=(0,0),
// rotation=0, scale=(1,1)). Pins the "Transform2d is opt-in, defaults
// don't lie about the chain shape" contract.
TEST(MaterialNetworkV2A18, NoTransform2dKeepsIdentityXform)
{
  const pxr::UsdStageRefPtr stage = pxr::UsdStage::CreateInMemory("identity.usda");
  const pxr::UsdShadeMaterial material =
      pxr::UsdShadeMaterial::Define(stage, pxr::SdfPath("/Mat"));
  pxr::UsdShadeShader surface =
      pxr::UsdShadeShader::Define(stage, pxr::SdfPath("/Mat/Surface"));
  surface.CreateIdAttr(pxr::VtValue(pxr::TfToken("UsdPreviewSurface")));
  const pxr::UsdShadeOutput surfaceOut =
      surface.CreateOutput(pxr::TfToken("surface"), pxr::SdfValueTypeNames->Token);
  material.CreateSurfaceOutput().ConnectToSource(surfaceOut);

  // Simple texture, no Transform2d.
  pxr::UsdShadeShader texture =
      pxr::UsdShadeShader::Define(stage, pxr::SdfPath("/Mat/Tex"));
  texture.CreateIdAttr(pxr::VtValue(pxr::TfToken("UsdUVTexture")));
  texture.CreateInput(pxr::TfToken("file"), pxr::SdfValueTypeNames->Asset)
      .Set(pxr::SdfAssetPath("./tex.png"));
  const pxr::UsdShadeOutput texOut =
      texture.CreateOutput(pxr::TfToken("rgb"), pxr::SdfValueTypeNames->Float3);
  surface.CreateInput(pxr::TfToken("diffuseColor"), pxr::SdfValueTypeNames->Color3f)
      .ConnectToSource(texOut);

  const pyxis::OpenPBRMaterialDesc desc =
      pyxis::material_translation::FromUsdShade(material, &StubAcquireValid, nullptr);

  EXPECT_FLOAT_EQ(desc.baseColorUvTranslationX, 0.0f);
  EXPECT_FLOAT_EQ(desc.baseColorUvTranslationY, 0.0f);
  EXPECT_FLOAT_EQ(desc.baseColorUvRotationDeg,  0.0f);
  EXPECT_FLOAT_EQ(desc.baseColorUvScaleX,       1.0f);
  EXPECT_FLOAT_EQ(desc.baseColorUvScaleY,       1.0f);
}

// V2.A.18 — PrimvarReader chain through a Transform2d still resolves
// the varname. Pre-PR3 the chain walk stopped at the first non-reader
// node and silently fell back to "implicit st"; this test pins the
// chain-walk fix. We assert via behaviour: the texture must be
// ACQUIRED (i.e. baseColorMap != Invalid) since varname=="st" is the
// supported UV set. If the chain walk dropped the varname to empty,
// the legacy "empty → st default" path still acquires too — so to
// distinguish, we author varname="st_1" which the translator drops
// (the texture stays Invalid). This proves the chain walk reaches the
// reader and honours the authored varname.
TEST(MaterialNetworkV2A18, PrimvarReaderVarnameThroughTransform2dHonored)
{
  const pxr::UsdStageRefPtr stage = pxr::UsdStage::CreateInMemory("chain_varname.usda");
  const pxr::UsdShadeMaterial material =
      pxr::UsdShadeMaterial::Define(stage, pxr::SdfPath("/Mat"));
  pxr::UsdShadeShader surface =
      pxr::UsdShadeShader::Define(stage, pxr::SdfPath("/Mat/Surface"));
  surface.CreateIdAttr(pxr::VtValue(pxr::TfToken("UsdPreviewSurface")));
  const pxr::UsdShadeOutput surfaceOut =
      surface.CreateOutput(pxr::TfToken("surface"), pxr::SdfValueTypeNames->Token);
  material.CreateSurfaceOutput().ConnectToSource(surfaceOut);

  // Chain authors varname="st_1" — a UV set the translator
  // intentionally drops (Pyxis's MeshDesc::uv0 is only the primary).
  pxr::UsdShadeShader texture = AuthorTextureWithTransform2d(
      stage, "/Mat",
      pxr::GfVec2f(0.0f, 0.0f), 0.0f, pxr::GfVec2f(1.0f, 1.0f),
      "st_1");
  const pxr::UsdShadeOutput texOut =
      texture.CreateOutput(pxr::TfToken("rgb"), pxr::SdfValueTypeNames->Float3);
  surface.CreateInput(pxr::TfToken("diffuseColor"), pxr::SdfValueTypeNames->Color3f)
      .ConnectToSource(texOut);

  const pyxis::OpenPBRMaterialDesc desc =
      pyxis::material_translation::FromUsdShade(material, &StubAcquireValid, nullptr);

  // varname="st_1" → translator drops the texture → baseColorMap
  // stays Invalid. If the chain walk had stopped at Transform2d (pre-
  // PR3 behaviour), varname would resolve to empty (= implicit st)
  // and the texture would be acquired — that's the regression this
  // test guards against.
  EXPECT_EQ(desc.baseColorMap, pyxis::TextureHandle::Invalid);
}

// V2.A.18 — render-context fallback. A material authoring BOTH
// UsdPreviewSurface (universal) and ND_open_pbr_surface (mtlx context)
// must resolve to the MaterialX surface (mtlx wins over universal per
// the V2.A.18 priority chain). Pre-PR3 the translator always picked
// up the universal surface.
TEST(MaterialNetworkV2A18, RenderContextMtlxWinsOverUniversalUps)
{
  const pxr::UsdStageRefPtr stage = pxr::UsdStage::CreateInMemory("ctx_mtlx_wins.usda");
  const pxr::UsdShadeMaterial material =
      pxr::UsdShadeMaterial::Define(stage, pxr::SdfPath("/Mat"));

  // UsdPreviewSurface (universal context)
  pxr::UsdShadeShader ups =
      pxr::UsdShadeShader::Define(stage, pxr::SdfPath("/Mat/UPS"));
  ups.CreateIdAttr(pxr::VtValue(pxr::TfToken("UsdPreviewSurface")));
  ups.CreateInput(pxr::TfToken("diffuseColor"), pxr::SdfValueTypeNames->Color3f)
      .Set(pxr::GfVec3f(0.10f, 0.10f, 0.10f));  // recognisable: dark grey
  const pxr::UsdShadeOutput upsOut =
      ups.CreateOutput(pxr::TfToken("surface"), pxr::SdfValueTypeNames->Token);
  material.CreateSurfaceOutput().ConnectToSource(upsOut);

  // ND_open_pbr_surface (mtlx context) with a distinct base_color so
  // we can tell which surface was picked.
  pxr::UsdShadeShader mtlx =
      pxr::UsdShadeShader::Define(stage, pxr::SdfPath("/Mat/Mtlx"));
  mtlx.CreateIdAttr(pxr::VtValue(pxr::TfToken("ND_open_pbr_surface_surfaceshader")));
  mtlx.CreateInput(pxr::TfToken("base_color"), pxr::SdfValueTypeNames->Color3f)
      .Set(pxr::GfVec3f(0.80f, 0.20f, 0.50f));  // recognisable: hot pink
  const pxr::UsdShadeOutput mtlxOut =
      mtlx.CreateOutput(pxr::TfToken("surface"), pxr::SdfValueTypeNames->Token);
  material.CreateSurfaceOutput(pxr::TfToken("mtlx")).ConnectToSource(mtlxOut);

  const pyxis::OpenPBRMaterialDesc desc =
      pyxis::material_translation::FromUsdShade(material);

  EXPECT_EQ(desc.source, pyxis::OpenPBRMaterialDesc::Source::MaterialX);
  EXPECT_FLOAT_EQ(desc.baseColor.x, 0.80f);
  EXPECT_FLOAT_EQ(desc.baseColor.y, 0.20f);
  EXPECT_FLOAT_EQ(desc.baseColor.z, 0.50f);
}

// V2.A.18 — render-context fallback. A material authoring BOTH MDL
// (mdl context) and MaterialX (mtlx context) must resolve to MDL (mdl
// wins over mtlx wins over universal per the V2.A.18 priority chain).
TEST(MaterialNetworkV2A18, RenderContextMdlWinsOverMtlx)
{
  const pxr::UsdStageRefPtr stage = pxr::UsdStage::CreateInMemory("ctx_mdl_wins.usda");
  const pxr::UsdShadeMaterial material =
      pxr::UsdShadeMaterial::Define(stage, pxr::SdfPath("/Mat"));

  // MaterialX OpenPBR (mtlx context)
  pxr::UsdShadeShader mtlx =
      pxr::UsdShadeShader::Define(stage, pxr::SdfPath("/Mat/Mtlx"));
  mtlx.CreateIdAttr(pxr::VtValue(pxr::TfToken("ND_open_pbr_surface_surfaceshader")));
  mtlx.CreateInput(pxr::TfToken("base_color"), pxr::SdfValueTypeNames->Color3f)
      .Set(pxr::GfVec3f(0.30f, 0.70f, 0.20f));  // green
  const pxr::UsdShadeOutput mtlxOut =
      mtlx.CreateOutput(pxr::TfToken("surface"), pxr::SdfValueTypeNames->Token);
  material.CreateSurfaceOutput(pxr::TfToken("mtlx")).ConnectToSource(mtlxOut);

  // MDL OmniPBR (mdl context) — diffuse_color_constant authored
  // distinctly so we can detect which surface was picked.
  pxr::UsdShadeShader mdl =
      pxr::UsdShadeShader::Define(stage, pxr::SdfPath("/Mat/Mdl"));
  mdl.CreateIdAttr(pxr::VtValue(pxr::TfToken("mdl::OmniPBR")));
  mdl.CreateInput(pxr::TfToken("diffuse_color_constant"), pxr::SdfValueTypeNames->Color3f)
      .Set(pxr::GfVec3f(0.95f, 0.10f, 0.05f));  // red
  const pxr::UsdShadeOutput mdlOut =
      mdl.CreateOutput(pxr::TfToken("surface"), pxr::SdfValueTypeNames->Token);
  material.CreateSurfaceOutput(pxr::TfToken("mdl")).ConnectToSource(mdlOut);

  const pyxis::OpenPBRMaterialDesc desc =
      pyxis::material_translation::FromUsdShade(material);

  // mdl wins → diffuse_color_constant (0.95, 0.10, 0.05) is what we see.
  EXPECT_FLOAT_EQ(desc.baseColor.x, 0.95f);
  EXPECT_FLOAT_EQ(desc.baseColor.y, 0.10f);
  EXPECT_FLOAT_EQ(desc.baseColor.z, 0.05f);
}

// V2.A.18 — when only universal (UsdPreviewSurface) is authored, the
// translator still resolves correctly via the universal-context
// fallback at the end of the priority chain. Pins "no regression on
// the common-case material" — previously the universal context was
// tried FIRST and unconditionally; now it's the LAST fallback.
TEST(MaterialNetworkV2A18, UniversalUpsResolvesWhenNothingElseAuthored)
{
  const pxr::UsdStageRefPtr stage = pxr::UsdStage::CreateInMemory("universal_only.usda");
  const pxr::UsdShadeMaterial material =
      pxr::UsdShadeMaterial::Define(stage, pxr::SdfPath("/Mat"));

  pxr::UsdShadeShader ups =
      pxr::UsdShadeShader::Define(stage, pxr::SdfPath("/Mat/UPS"));
  ups.CreateIdAttr(pxr::VtValue(pxr::TfToken("UsdPreviewSurface")));
  ups.CreateInput(pxr::TfToken("diffuseColor"), pxr::SdfValueTypeNames->Color3f)
      .Set(pxr::GfVec3f(0.42f, 0.42f, 0.42f));
  const pxr::UsdShadeOutput upsOut =
      ups.CreateOutput(pxr::TfToken("surface"), pxr::SdfValueTypeNames->Token);
  material.CreateSurfaceOutput().ConnectToSource(upsOut);

  const pyxis::OpenPBRMaterialDesc desc =
      pyxis::material_translation::FromUsdShade(material);

  EXPECT_EQ(desc.source, pyxis::OpenPBRMaterialDesc::Source::UsdPreviewSurface);
  EXPECT_FLOAT_EQ(desc.baseColor.x, 0.42f);
  EXPECT_FLOAT_EQ(desc.baseColor.y, 0.42f);
  EXPECT_FLOAT_EQ(desc.baseColor.z, 0.42f);
}
