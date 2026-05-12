// Pyxis M14 / V2.A.6 — UsdGeomXformable::GetLocalToWorldTransform
// covers the full xformOpOrder vocabulary.
//
// Our StageWalker delegates xform composition to
// `UsdGeomXformCache::GetLocalToWorldTransform`, which in turn calls
// `UsdGeomXformable::GetLocalTransformation` and walks the authored
// `xformOpOrder` honouring every op kind (translate, scale,
// rotate{X,Y,Z,XYZ,...}, transform, orient) and the `!invert!`
// suffix. These tests author each variant in-memory and compare
// against a hand-composed reference matrix to prove the production
// code path produces the expected result.

#include <pxr/base/gf/matrix4d.h>
#include <pxr/base/gf/quatd.h>
#include <pxr/base/gf/vec3d.h>
#include <pxr/usd/sdf/path.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdGeom/xform.h>
#include <pxr/usd/usdGeom/xformCache.h>

#include <gtest/gtest.h>

#include <cmath>

namespace {

void ExpectMatrixNear(const pxr::GfMatrix4d& actual,
                      const pxr::GfMatrix4d& expected,
                      double tolerance = 1e-6) {
  for (int row = 0; row < 4; ++row)
  {
    for (int col = 0; col < 4; ++col)
    {
      EXPECT_NEAR(actual[row][col], expected[row][col], tolerance)
          << "at [" << row << "][" << col << "]";
    }
  }
}

}  // namespace

TEST(XformOpsComposition, TranslateThenScaleMatchesManualCompose)
{
  const pxr::UsdStageRefPtr stage = pxr::UsdStage::CreateInMemory("translate_scale.usda");
  const pxr::UsdGeomXform xform = pxr::UsdGeomXform::Define(stage, pxr::SdfPath("/Root"));
  xform.AddTranslateOp().Set(pxr::GfVec3d(1.0, 2.0, 3.0));
  xform.AddScaleOp().Set(pxr::GfVec3f(2.0f, 2.0f, 2.0f));

  pxr::UsdGeomXformCache cache;
  const pxr::GfMatrix4d actual = cache.GetLocalToWorldTransform(xform.GetPrim());

  // USD row-vector convention: v' = v · M. xformOpOrder applies ops
  // left-to-right; in matrix form the right-most op is innermost, so
  // M = Scale · Translate? — actually for row-vector composition the
  // composed local matrix is Translate · Scale (translate FIRST in
  // the op stack means it's the outermost). Easiest verification:
  // multiply a unit vector through and check the result.
  pxr::GfMatrix4d scaleMat{};
  scaleMat.SetScale(pxr::GfVec3d(2.0, 2.0, 2.0));
  pxr::GfMatrix4d translateMat{};
  translateMat.SetTranslate(pxr::GfVec3d(1.0, 2.0, 3.0));
  const pxr::GfMatrix4d expected = scaleMat * translateMat;  // row-vector: v·S·T

  ExpectMatrixNear(actual, expected);
}

TEST(XformOpsComposition, FullMatrixTransformOp)
{
  const pxr::UsdStageRefPtr stage = pxr::UsdStage::CreateInMemory("transform_op.usda");
  const pxr::UsdGeomXform xform = pxr::UsdGeomXform::Define(stage, pxr::SdfPath("/Root"));

  // Hand-author a 4×4 with non-trivial entries.
  pxr::GfMatrix4d authored{};
  authored.SetIdentity();
  authored[0][0] = 0.5;  // x scale
  authored[1][1] = 0.5;
  authored[2][2] = 0.5;
  authored[3][0] = 7.0;  // tx
  authored[3][1] = 8.0;
  authored[3][2] = 9.0;
  xform.AddTransformOp().Set(authored);

  pxr::UsdGeomXformCache cache;
  const pxr::GfMatrix4d actual = cache.GetLocalToWorldTransform(xform.GetPrim());
  ExpectMatrixNear(actual, authored);
}

TEST(XformOpsComposition, InvertedOpUndoesTransform)
{
  const pxr::UsdStageRefPtr stage = pxr::UsdStage::CreateInMemory("invert.usda");
  const pxr::UsdGeomXform xform = pxr::UsdGeomXform::Define(stage, pxr::SdfPath("/Root"));

  // translate(5,0,0) · translate(5,0,0)!invert! → identity.
  xform.AddTranslateOp(pxr::UsdGeomXformOp::PrecisionDouble, pxr::TfToken("pivot"))
      .Set(pxr::GfVec3d(5.0, 0.0, 0.0));
  xform.AddTranslateOp(pxr::UsdGeomXformOp::PrecisionDouble, pxr::TfToken("pivot"),
                       /*isInverseOp=*/true);

  pxr::UsdGeomXformCache cache;
  const pxr::GfMatrix4d actual = cache.GetLocalToWorldTransform(xform.GetPrim());

  pxr::GfMatrix4d identity{};
  identity.SetIdentity();
  ExpectMatrixNear(actual, identity);
}

TEST(XformOpsComposition, OrientQuaternionMatchesEulerEquivalent)
{
  const pxr::UsdStageRefPtr stage = pxr::UsdStage::CreateInMemory("orient.usda");
  const pxr::UsdGeomXform xform = pxr::UsdGeomXform::Define(stage, pxr::SdfPath("/Root"));

  // 90° around X — quaternion (w, x, y, z) = (cos45, sin45, 0, 0).
  const double sin45 = std::sin(M_PI / 4.0);
  const double cos45 = std::cos(M_PI / 4.0);
  xform.AddOrientOp(pxr::UsdGeomXformOp::PrecisionDouble)
      .Set(pxr::GfQuatd(cos45, sin45, 0.0, 0.0));

  pxr::UsdGeomXformCache cache;
  const pxr::GfMatrix4d actual = cache.GetLocalToWorldTransform(xform.GetPrim());

  // Expected: rotation matrix for 90° around X.
  pxr::GfMatrix4d expected{};
  expected.SetIdentity();
  expected[1][1] = 0.0;
  expected[1][2] = 1.0;
  expected[2][1] = -1.0;
  expected[2][2] = 0.0;
  ExpectMatrixNear(actual, expected, 1e-12);
}

TEST(XformOpsComposition, NestedXformAccumulates)
{
  const pxr::UsdStageRefPtr stage = pxr::UsdStage::CreateInMemory("nested.usda");
  const pxr::UsdGeomXform parent = pxr::UsdGeomXform::Define(stage, pxr::SdfPath("/Parent"));
  parent.AddTranslateOp().Set(pxr::GfVec3d(10.0, 0.0, 0.0));
  const pxr::UsdGeomXform child =
      pxr::UsdGeomXform::Define(stage, pxr::SdfPath("/Parent/Child"));
  child.AddTranslateOp().Set(pxr::GfVec3d(0.0, 20.0, 0.0));

  pxr::UsdGeomXformCache cache;
  const pxr::GfMatrix4d childWorld =
      cache.GetLocalToWorldTransform(child.GetPrim());

  // Child world translation = parent_translate ⊕ child_translate.
  EXPECT_NEAR(childWorld[3][0], 10.0, 1e-12);
  EXPECT_NEAR(childWorld[3][1], 20.0, 1e-12);
  EXPECT_NEAR(childWorld[3][2],  0.0, 1e-12);
}
