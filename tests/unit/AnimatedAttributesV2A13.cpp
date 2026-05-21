// Pyxis V2.A.13 — animated material attribute time samples.
//
// Author a UsdShadeMaterial with `diffuseColor` time samples at
// frame 0 (red) and frame 100 (blue). Translate at frame 0, 50, 100,
// default — assert the desc.baseColor tracks USD's linear lerp at
// each evaluation time.
//
// Sister coverage (PointInstancer time samples + animated camera
// exposure + LayerOffset) lives in the matching golden fixtures
// (`anim_pointinstancer_*`, `anim_camera_exposure`, `anim_layeroffset_*`)
// which exercise the full ingest → render path; this unit test pins
// the translator math at the call-site level.

#include <Pyxis/MaterialTranslation/UsdShadeToOpenPBR.h>
#include <Pyxis/Renderer/Descs/OpenPBRMaterialDesc.h>

#include <pxr/base/gf/vec3f.h>
#include <pxr/base/tf/token.h>
#include <pxr/base/vt/value.h>
#include <pxr/usd/sdf/path.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usd/timeCode.h>
#include <pxr/usd/usdShade/connectableAPI.h>
#include <pxr/usd/usdShade/material.h>
#include <pxr/usd/usdShade/shader.h>

#include <gtest/gtest.h>

namespace {

// Build a UsdShadeMaterial with diffuseColor authored as time samples
// at frame 0 = red, frame 100 = blue. USD will linearly interpolate
// between samples for any in-between time.
pxr::UsdShadeMaterial AuthorAnimatedDiffuseColor(const pxr::UsdStageRefPtr& stage) {
  const pxr::UsdShadeMaterial material =
      pxr::UsdShadeMaterial::Define(stage, pxr::SdfPath("/Mat"));

  pxr::UsdShadeShader surface =
      pxr::UsdShadeShader::Define(stage, pxr::SdfPath("/Mat/Surface"));
  surface.CreateIdAttr(pxr::VtValue(pxr::TfToken("UsdPreviewSurface")));

  // Author diffuseColor as a time-sampled attribute.
  const pxr::UsdShadeInput diffuse =
      surface.CreateInput(pxr::TfToken("diffuseColor"), pxr::SdfValueTypeNames->Color3f);
  diffuse.GetAttr().Set(pxr::GfVec3f(1.0f, 0.0f, 0.0f), pxr::UsdTimeCode(0.0));
  diffuse.GetAttr().Set(pxr::GfVec3f(0.0f, 0.0f, 1.0f), pxr::UsdTimeCode(100.0));

  const pxr::UsdShadeOutput surfaceOut =
      surface.CreateOutput(pxr::TfToken("surface"), pxr::SdfValueTypeNames->Token);
  material.CreateSurfaceOutput().ConnectToSource(surfaceOut);

  return material;
}

}  // namespace

// V2.A.13 — at frame 0, diffuseColor reads as the first authored sample (red).
TEST(AnimatedAttributesV2A13, MaterialDiffuseColorAtFrameZero) {
  const pxr::UsdStageRefPtr stage = pxr::UsdStage::CreateInMemory("anim_diffuse.usda");
  const pxr::UsdShadeMaterial material = AuthorAnimatedDiffuseColor(stage);

  const pyxis::OpenPBRMaterialDesc desc =
      pyxis::material_translation::FromUsdShade(material, nullptr, nullptr,
                                                pxr::UsdTimeCode(0.0));
  EXPECT_FLOAT_EQ(desc.baseColor.x, 1.0f);
  EXPECT_FLOAT_EQ(desc.baseColor.y, 0.0f);
  EXPECT_FLOAT_EQ(desc.baseColor.z, 0.0f);
}

// V2.A.13 — at frame 100, diffuseColor reads as the second sample (blue).
TEST(AnimatedAttributesV2A13, MaterialDiffuseColorAtFrame100) {
  const pxr::UsdStageRefPtr stage = pxr::UsdStage::CreateInMemory("anim_diffuse.usda");
  const pxr::UsdShadeMaterial material = AuthorAnimatedDiffuseColor(stage);

  const pyxis::OpenPBRMaterialDesc desc =
      pyxis::material_translation::FromUsdShade(material, nullptr, nullptr,
                                                pxr::UsdTimeCode(100.0));
  EXPECT_FLOAT_EQ(desc.baseColor.x, 0.0f);
  EXPECT_FLOAT_EQ(desc.baseColor.y, 0.0f);
  EXPECT_FLOAT_EQ(desc.baseColor.z, 1.0f);
}

// V2.A.13 — at the midpoint frame 50, USD linearly interpolates
// between the two samples. Pins the time-code threading through
// FromUsdShade actually reaches the per-attr Get(timeCode) calls.
TEST(AnimatedAttributesV2A13, MaterialDiffuseColorInterpolatesAtMidpoint) {
  const pxr::UsdStageRefPtr stage = pxr::UsdStage::CreateInMemory("anim_diffuse.usda");
  const pxr::UsdShadeMaterial material = AuthorAnimatedDiffuseColor(stage);

  const pyxis::OpenPBRMaterialDesc desc =
      pyxis::material_translation::FromUsdShade(material, nullptr, nullptr,
                                                pxr::UsdTimeCode(50.0));
  // lerp(red, blue, 0.5) = (0.5, 0, 0.5) — magenta.
  EXPECT_NEAR(desc.baseColor.x, 0.5f, 1e-5f);
  EXPECT_FLOAT_EQ(desc.baseColor.y, 0.0f);
  EXPECT_NEAR(desc.baseColor.z, 0.5f, 1e-5f);
}

// V2.A.13 — Default time-code on a time-sampled-only attr returns
// FALSE from USD's `Get(Default())` (no default value authored), so
// the translator falls through to its scalar fallback. The OpenPBR
// fallback for diffuseColor is grey (0.18). This pins the backward-
// compat contract: existing callers passing no time-code argument
// don't get random values lifted from time samples — they get the
// scalar default the spec mandates.
TEST(AnimatedAttributesV2A13, MaterialDiffuseColorAtDefaultTimeFallsBack) {
  const pxr::UsdStageRefPtr stage = pxr::UsdStage::CreateInMemory("anim_diffuse.usda");
  const pxr::UsdShadeMaterial material = AuthorAnimatedDiffuseColor(stage);

  // Implicit default time argument.
  const pyxis::OpenPBRMaterialDesc desc =
      pyxis::material_translation::FromUsdShade(material);
  // OpenPBR baseColor fallback (`hlslpp::float3{0.18f, 0.18f, 0.18f}`)
  // because the time-sampled attr has no Default-time value authored.
  EXPECT_FLOAT_EQ(desc.baseColor.x, 0.18f);
  EXPECT_FLOAT_EQ(desc.baseColor.y, 0.18f);
  EXPECT_FLOAT_EQ(desc.baseColor.z, 0.18f);
}
