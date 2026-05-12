// Pyxis M14c / V2.A.31 — bounding-box hints. UsdGeomBoundable's
// `extent` attribute carries per-prim min/max bounds; the StageWalker
// aggregates these into a scene-wide bounding box for diagnostics.
//
// We test the per-prim part below (USD's `GetExtentAttr().Get(...)`
// round-trips correctly when authored explicitly). The aggregation
// loop in StageWalker.cpp is exercised by the integration smoke test
// via the log line emitted at end-of-pass3b.

#include <pxr/base/gf/vec3f.h>
#include <pxr/base/vt/array.h>
#include <pxr/usd/sdf/path.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdGeom/boundable.h>
#include <pxr/usd/usdGeom/cube.h>
#include <pxr/usd/usdGeom/mesh.h>
#include <pxr/usd/usdGeom/sphere.h>

#include <gtest/gtest.h>

namespace {

void ExpectVec3Near(const pxr::GfVec3f& actual,
                    const pxr::GfVec3f& expected,
                    float tolerance = 1e-5f) {
  EXPECT_NEAR(actual[0], expected[0], tolerance);
  EXPECT_NEAR(actual[1], expected[1], tolerance);
  EXPECT_NEAR(actual[2], expected[2], tolerance);
}

}  // namespace

TEST(SceneBoundsAggregate, AuthoredExtentReadsBack)
{
  const pxr::UsdStageRefPtr stage = pxr::UsdStage::CreateInMemory("extent.usda");
  const pxr::UsdGeomMesh mesh = pxr::UsdGeomMesh::Define(stage, pxr::SdfPath("/Mesh"));

  const pxr::VtArray<pxr::GfVec3f> authored{
      pxr::GfVec3f{-1.0f, -2.0f, -3.0f},
      pxr::GfVec3f{ 4.0f,  5.0f,  6.0f}};
  mesh.GetExtentAttr().Set(authored);

  const pxr::UsdGeomBoundable boundable(mesh.GetPrim());
  ASSERT_TRUE(boundable);

  pxr::VtArray<pxr::GfVec3f> readBack;
  ASSERT_TRUE(boundable.GetExtentAttr().Get(&readBack));
  // (`readBack` is mutated by Get above — keeping non-const intentionally.)
  ASSERT_EQ(readBack.size(), 2u);
  ExpectVec3Near(readBack[0], pxr::GfVec3f{-1.0f, -2.0f, -3.0f});
  ExpectVec3Near(readBack[1], pxr::GfVec3f{ 4.0f,  5.0f,  6.0f});
}

TEST(SceneBoundsAggregate, ManualAggregateOverThreePrims)
{
  // Mirror StageWalker's per-axis min/max accumulation over three
  // authored extents. The actual aggregation loop is exercised by
  // the integration smoke test; this test pins the math.
  const pxr::VtArray<pxr::GfVec3f> extents[3] = {
      {pxr::GfVec3f{-1.0f, -1.0f, -1.0f}, pxr::GfVec3f{ 1.0f,  1.0f,  1.0f}},
      {pxr::GfVec3f{ 2.0f, -3.0f,  0.0f}, pxr::GfVec3f{ 5.0f,  3.0f,  4.0f}},
      {pxr::GfVec3f{-4.0f,  0.0f, -2.0f}, pxr::GfVec3f{ 0.0f,  2.0f,  1.0f}},
  };
  pxr::GfVec3f aggregateMin{ 1e9f,  1e9f,  1e9f};
  pxr::GfVec3f aggregateMax{-1e9f, -1e9f, -1e9f};
  for (const auto& extent : extents)
  {
    for (int axis = 0; axis < 3; ++axis)
    {
      aggregateMin[axis] = std::min(aggregateMin[axis], extent[0][axis]);
      aggregateMax[axis] = std::max(aggregateMax[axis], extent[1][axis]);
    }
  }
  ExpectVec3Near(aggregateMin, pxr::GfVec3f{-4.0f, -3.0f, -2.0f});
  ExpectVec3Near(aggregateMax, pxr::GfVec3f{ 5.0f,  3.0f,  4.0f});
}

TEST(SceneBoundsAggregate, AnalyticPrimitivesAreBoundableTooEvenWithoutExplicitExtent)
{
  // UsdGeomSphere / UsdGeomCube implement the Boundable schema. Without
  // an explicit `extent` they still respond to UsdGeomBoundable; our
  // walker only reads `extent` when authored — so the without-extent
  // case should NOT contribute to the scene aggregate.
  const pxr::UsdStageRefPtr stage = pxr::UsdStage::CreateInMemory("noext.usda");
  const pxr::UsdGeomSphere sphere = pxr::UsdGeomSphere::Define(stage, pxr::SdfPath("/Sphere"));
  sphere.GetRadiusAttr().Set(2.0);

  const pxr::UsdGeomBoundable boundable(sphere.GetPrim());
  ASSERT_TRUE(boundable);

  // No extent authored — HasAuthoredValue returns false.
  EXPECT_FALSE(boundable.GetExtentAttr().HasAuthoredValue());
}
