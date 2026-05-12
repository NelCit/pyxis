// Pyxis M20 / V2.A.4 — NURBS + skel detect/warn/skip. Mirrors the
// M15 volumes pattern. Tests pin the dispatch predicates the
// StageWalker uses.

#include <pxr/usd/sdf/path.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdGeom/cube.h>
#include <pxr/usd/usdGeom/nurbsCurves.h>
#include <pxr/usd/usdGeom/nurbsPatch.h>
#include <pxr/usd/usdSkel/root.h>
#include <pxr/usd/usdSkel/skeleton.h>

#include <gtest/gtest.h>

TEST(NurbsSkelDetection, NurbsPatchIsA)
{
  const pxr::UsdStageRefPtr stage = pxr::UsdStage::CreateInMemory("nurbsP.usda");
  const pxr::UsdGeomNurbsPatch patch =
      pxr::UsdGeomNurbsPatch::Define(stage, pxr::SdfPath("/Patch"));
  ASSERT_TRUE(patch);
  EXPECT_TRUE(patch.GetPrim().IsA<pxr::UsdGeomNurbsPatch>());
  EXPECT_FALSE(patch.GetPrim().IsA<pxr::UsdGeomNurbsCurves>());
}

TEST(NurbsSkelDetection, NurbsCurvesIsA)
{
  const pxr::UsdStageRefPtr stage = pxr::UsdStage::CreateInMemory("nurbsC.usda");
  const pxr::UsdGeomNurbsCurves curves =
      pxr::UsdGeomNurbsCurves::Define(stage, pxr::SdfPath("/Curves"));
  ASSERT_TRUE(curves);
  EXPECT_TRUE(curves.GetPrim().IsA<pxr::UsdGeomNurbsCurves>());
}

TEST(NurbsSkelDetection, SkelRootIsA)
{
  const pxr::UsdStageRefPtr stage = pxr::UsdStage::CreateInMemory("skelR.usda");
  const pxr::UsdSkelRoot root =
      pxr::UsdSkelRoot::Define(stage, pxr::SdfPath("/SkelRoot"));
  ASSERT_TRUE(root);
  EXPECT_TRUE(root.GetPrim().IsA<pxr::UsdSkelRoot>());
}

TEST(NurbsSkelDetection, SkelSkeletonIsA)
{
  const pxr::UsdStageRefPtr stage = pxr::UsdStage::CreateInMemory("skel.usda");
  const pxr::UsdSkelSkeleton skel =
      pxr::UsdSkelSkeleton::Define(stage, pxr::SdfPath("/SkelRoot/Skel"));
  ASSERT_TRUE(skel);
  EXPECT_TRUE(skel.GetPrim().IsA<pxr::UsdSkelSkeleton>());
}

TEST(NurbsSkelDetection, CubeIsNotNurbsOrSkel)
{
  const pxr::UsdStageRefPtr stage = pxr::UsdStage::CreateInMemory("cube.usda");
  const pxr::UsdGeomCube cube =
      pxr::UsdGeomCube::Define(stage, pxr::SdfPath("/Cube"));
  ASSERT_TRUE(cube);
  const pxr::UsdPrim prim = cube.GetPrim();
  EXPECT_FALSE(prim.IsA<pxr::UsdGeomNurbsPatch>());
  EXPECT_FALSE(prim.IsA<pxr::UsdGeomNurbsCurves>());
  EXPECT_FALSE(prim.IsA<pxr::UsdSkelRoot>());
  EXPECT_FALSE(prim.IsA<pxr::UsdSkelSkeleton>());
}
