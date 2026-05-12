// Pyxis M21 / V2.A.27 — UsdRender schema detection. Pins the dispatch
// predicates the StageWalker uses for `UsdRenderSettings`,
// `UsdRenderProduct`, `UsdRenderVar`.

#include <pxr/usd/sdf/path.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdGeom/cube.h>
#include <pxr/usd/usdRender/product.h>
#include <pxr/usd/usdRender/settings.h>
#include <pxr/usd/usdRender/var.h>

#include <gtest/gtest.h>

TEST(UsdRenderDetection, SettingsIsA)
{
  const pxr::UsdStageRefPtr stage = pxr::UsdStage::CreateInMemory("rs.usda");
  const pxr::UsdRenderSettings settings =
      pxr::UsdRenderSettings::Define(stage, pxr::SdfPath("/Render"));
  ASSERT_TRUE(settings);
  EXPECT_TRUE(settings.GetPrim().IsA<pxr::UsdRenderSettings>());
}

TEST(UsdRenderDetection, ProductIsA)
{
  const pxr::UsdStageRefPtr stage = pxr::UsdStage::CreateInMemory("rp.usda");
  const pxr::UsdRenderProduct product =
      pxr::UsdRenderProduct::Define(stage, pxr::SdfPath("/Render/Product"));
  ASSERT_TRUE(product);
  EXPECT_TRUE(product.GetPrim().IsA<pxr::UsdRenderProduct>());
}

TEST(UsdRenderDetection, VarIsA)
{
  const pxr::UsdStageRefPtr stage = pxr::UsdStage::CreateInMemory("rv.usda");
  const pxr::UsdRenderVar var =
      pxr::UsdRenderVar::Define(stage, pxr::SdfPath("/Render/Var"));
  ASSERT_TRUE(var);
  EXPECT_TRUE(var.GetPrim().IsA<pxr::UsdRenderVar>());
}

TEST(UsdRenderDetection, CubeIsNotRenderSchema)
{
  const pxr::UsdStageRefPtr stage = pxr::UsdStage::CreateInMemory("cube.usda");
  const pxr::UsdGeomCube cube =
      pxr::UsdGeomCube::Define(stage, pxr::SdfPath("/Cube"));
  ASSERT_TRUE(cube);
  const pxr::UsdPrim prim = cube.GetPrim();
  EXPECT_FALSE(prim.IsA<pxr::UsdRenderSettings>());
  EXPECT_FALSE(prim.IsA<pxr::UsdRenderProduct>());
  EXPECT_FALSE(prim.IsA<pxr::UsdRenderVar>());
}
