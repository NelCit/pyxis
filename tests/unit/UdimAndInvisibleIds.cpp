// Pyxis M14b / V2.A.7 + V2.A.9 — UDIM detection in material translation
// + PointInstancer invisibleIds masking.
//
// Both tests author a tiny in-memory USD stage and assert the
// resulting behaviour. UDIM coverage exercises the
// `ResolveUVTextureBinding` substitution by translating a
// UsdPreviewSurface whose diffuseColor texture path contains the
// `<UDIM>` placeholder. invisibleIds coverage builds a
// UsdGeomPointInstancer with 5 instances + 2 invisibleIds and walks
// the array logic the StageWalker uses (the actual StageWalker is
// only callable via WalkFile which needs a GpuScene; here we duplicate
// the array-filter logic and assert it filters the right indices).

#include <Pyxis/MaterialTranslation/UsdShadeToOpenPBR.h>
#include <Pyxis/Renderer/Descs/OpenPBRMaterialDesc.h>

#include <pxr/base/gf/vec3f.h>
#include <pxr/base/tf/token.h>
#include <pxr/base/vt/value.h>
#include <pxr/usd/sdf/assetPath.h>
#include <pxr/usd/sdf/path.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdGeom/pointInstancer.h>
#include <pxr/usd/usdShade/connectableAPI.h>
#include <pxr/usd/usdShade/material.h>
#include <pxr/usd/usdShade/shader.h>

#include <gtest/gtest.h>

#include <string>
#include <unordered_set>
#include <vector>

TEST(UdimAndInvisibleIds, UdimPlaceholderRewritesToTile1001)
{
  const pxr::UsdStageRefPtr stage = pxr::UsdStage::CreateInMemory("udim.usda");
  const pxr::UsdShadeMaterial material =
      pxr::UsdShadeMaterial::Define(stage, pxr::SdfPath("/Mat"));

  pxr::UsdShadeShader surface =
      pxr::UsdShadeShader::Define(stage, pxr::SdfPath("/Mat/Surface"));
  surface.CreateIdAttr(pxr::VtValue(pxr::TfToken("UsdPreviewSurface")));
  const pxr::UsdShadeOutput surfaceOut =
      surface.CreateOutput(pxr::TfToken("surface"), pxr::SdfValueTypeNames->Token);
  material.CreateSurfaceOutput().ConnectToSource(surfaceOut);

  // Texture shader with a <UDIM> placeholder in its file input.
  pxr::UsdShadeShader texture =
      pxr::UsdShadeShader::Define(stage, pxr::SdfPath("/Mat/Tex"));
  texture.CreateIdAttr(pxr::VtValue(pxr::TfToken("UsdUVTexture")));
  texture.CreateInput(pxr::TfToken("file"), pxr::SdfValueTypeNames->Asset)
      .Set(pxr::SdfAssetPath("./textures/wood.<UDIM>.png"));
  const pxr::UsdShadeOutput texOut =
      texture.CreateOutput(pxr::TfToken("rgb"), pxr::SdfValueTypeNames->Float3);
  surface.CreateInput(pxr::TfToken("diffuseColor"), pxr::SdfValueTypeNames->Color3f)
      .ConnectToSource(texOut);

  // Capture the path passed to AcquireTexture. v2.0 substitutes
  // <UDIM> with 1001 so the decoder gets a real filename rather than
  // hitting a "file not found" with the unresolved placeholder.
  std::string capturedPath;
  auto acquireFn = [](std::string_view path,
                      pyxis::TextureKey::Role /*role*/,
                      void* userData) -> pyxis::TextureHandle
  {
    auto* outPath = static_cast<std::string*>(userData);
    *outPath = std::string{path};
    return pyxis::TextureHandle::Invalid;
  };

  const pyxis::OpenPBRMaterialDesc desc =
      pyxis::material_translation::FromUsdShade(material, acquireFn, &capturedPath);
  (void)desc;

  // Expectations: the <UDIM> placeholder has been replaced with 1001,
  // and nothing else in the path changed.
  EXPECT_NE(capturedPath.find("1001"), std::string::npos)
      << "UDIM placeholder should have been substituted with tile 1001; "
      << "captured path: " << capturedPath;
  EXPECT_EQ(capturedPath.find("<UDIM>"), std::string::npos)
      << "Raw <UDIM> placeholder must not reach the texture decoder; "
      << "captured path: " << capturedPath;
}

TEST(UdimAndInvisibleIds, InvisibleIdsFilterMatchingInstanceIndices)
{
  const pxr::UsdStageRefPtr stage = pxr::UsdStage::CreateInMemory("instancer.usda");
  const pxr::UsdGeomPointInstancer instancer =
      pxr::UsdGeomPointInstancer::Define(stage, pxr::SdfPath("/Instancer"));

  const pxr::VtArray<int> protoIndices{0, 0, 0, 0, 0};
  const pxr::VtArray<std::int64_t> invisibleIdsArr{1, 3};
  instancer.GetProtoIndicesAttr().Set(protoIndices);
  instancer.GetInvisibleIdsAttr().Set(invisibleIdsArr);

  pxr::VtArray<int> outProtoIndices;
  pxr::VtArray<std::int64_t> outInvisible;
  instancer.GetProtoIndicesAttr().Get(&outProtoIndices);
  instancer.GetInvisibleIdsAttr().Get(&outInvisible);

  std::unordered_set<std::int64_t> invisibleSet;
  for (const std::int64_t identifier : outInvisible)
    invisibleSet.insert(identifier);
  EXPECT_EQ(invisibleSet.size(), 2u);
  EXPECT_TRUE(invisibleSet.contains(1));
  EXPECT_TRUE(invisibleSet.contains(3));

  // Mirror the StageWalker filter loop. Should yield indices 0, 2, 4
  // (the ones NOT in invisibleSet).
  std::vector<std::int64_t> emitted;
  for (std::size_t instIdx = 0; instIdx < outProtoIndices.size(); ++instIdx)
  {
    if (invisibleSet.contains(static_cast<std::int64_t>(instIdx)))
      continue;
    emitted.push_back(static_cast<std::int64_t>(instIdx));
  }
  const std::vector<std::int64_t> expected{0, 2, 4};
  EXPECT_EQ(emitted, expected);
}
