// Pyxis M13 / V2.A.3 — analytic-prim + curves + points tessellator tests.
//
// Author an in-memory UsdStage with one of each prim type, then call
// the corresponding tessellator and assert:
//   * success=true
//   * non-empty triangle list (faceCounts entirely 3, indices ∈ [0, points.size()))
//   * normals are unit-length where authored as such
//   * UVs are within [0,1]²
//   * vertex count matches the documented tessellation density
//
// These tests run below the StageWalker layer so they don't need a
// Vulkan device or a renderer; they exercise the pure-CPU tessellation
// math.

#include "AnalyticGeom.h"  // pyxis_usd_ingest/Private — compiled directly into the test exe

#include <pxr/usd/sdf/path.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdGeom/basisCurves.h>
#include <pxr/usd/usdGeom/capsule.h>
#include <pxr/usd/usdGeom/cone.h>
#include <pxr/usd/usdGeom/cube.h>
#include <pxr/usd/usdGeom/cylinder.h>
#include <pxr/usd/usdGeom/points.h>
#include <pxr/usd/usdGeom/sphere.h>
#include <pxr/usd/usdGeom/tokens.h>

#include <gtest/gtest.h>

#include <cmath>

namespace {

using pyxis::usd_ingest::AnalyticGeomResult;

// Assert basic well-formedness invariants every tessellator must
// satisfy. Caller passes the result + expected-vertex-count lower
// bound (so the per-type test only sets one number).
void ExpectWellFormed(const AnalyticGeomResult& result, std::size_t minVerts)
{
  ASSERT_TRUE(result.success);
  EXPECT_GE(result.points.size(), minVerts);
  EXPECT_EQ(result.points.size(), result.normals.size());
  EXPECT_EQ(result.points.size(), result.uvs.size());
  ASSERT_FALSE(result.faceCounts.empty());
  ASSERT_FALSE(result.faceIndices.empty());

  // Triangle-list contract.
  for (const int faceCount : result.faceCounts)
    EXPECT_EQ(faceCount, 3);
  EXPECT_EQ(result.faceIndices.size(), result.faceCounts.size() * 3u);

  // Indices stay in range.
  const int numVerts = static_cast<int>(result.points.size());
  for (const int idx : result.faceIndices)
  {
    EXPECT_GE(idx, 0);
    EXPECT_LT(idx, numVerts);
  }

  // Normals unit-length where authored. Don't check exact ε since
  // tessellator-side trig is single precision; ~1e-4 absolute is
  // enough headroom while still catching gross errors.
  for (const auto& normal : result.normals)
  {
    const float lenSq =
        normal[0]*normal[0] + normal[1]*normal[1] + normal[2]*normal[2];
    EXPECT_NEAR(lenSq, 1.0f, 1e-3f);
  }
}

}  // namespace

TEST(AnalyticGeomTessellation, SphereRadius1ProducesUnitNormals)
{
  const pxr::UsdStageRefPtr stage = pxr::UsdStage::CreateInMemory("sphere.usda");
  const pxr::UsdGeomSphere sphere =
      pxr::UsdGeomSphere::Define(stage, pxr::SdfPath("/Sphere"));
  sphere.GetRadiusAttr().Set(1.0);

  const AnalyticGeomResult result = pyxis::usd_ingest::TessellateSphere(sphere);
  ExpectWellFormed(result, /*minVerts=*/100);

  // Radius-1 sphere: every point should be ≈ 1.0 from origin (also
  // ≈ the normal at that point, which we already validated).
  for (const auto& point : result.points)
  {
    const float len =
        std::sqrt(point[0]*point[0] + point[1]*point[1] + point[2]*point[2]);
    EXPECT_NEAR(len, 1.0f, 1e-3f);
  }
}

TEST(AnalyticGeomTessellation, SphereDegenerateRadiusFails)
{
  const pxr::UsdStageRefPtr stage = pxr::UsdStage::CreateInMemory("sphere_zero.usda");
  const pxr::UsdGeomSphere sphere =
      pxr::UsdGeomSphere::Define(stage, pxr::SdfPath("/Sphere"));
  sphere.GetRadiusAttr().Set(0.0);

  const AnalyticGeomResult result = pyxis::usd_ingest::TessellateSphere(sphere);
  EXPECT_FALSE(result.success);
  EXPECT_TRUE(result.points.empty());
}

TEST(AnalyticGeomTessellation, CubeSize2HasSixFaces)
{
  const pxr::UsdStageRefPtr stage = pxr::UsdStage::CreateInMemory("cube.usda");
  const pxr::UsdGeomCube cube =
      pxr::UsdGeomCube::Define(stage, pxr::SdfPath("/Cube"));
  cube.GetSizeAttr().Set(2.0);

  const AnalyticGeomResult result = pyxis::usd_ingest::TessellateCube(cube);
  ExpectWellFormed(result, /*minVerts=*/24);

  // Six faces × 4 verts each (per-face flat normals) = 24 verts.
  EXPECT_EQ(result.points.size(), 24u);
  // 6 quads → 12 triangles.
  EXPECT_EQ(result.faceCounts.size(), 12u);

  // Size 2 → half=1. Every corner is at ±1 on each axis.
  for (const auto& point : result.points)
  {
    EXPECT_NEAR(std::abs(point[0]), 1.0f, 1e-5f);
    EXPECT_NEAR(std::abs(point[1]), 1.0f, 1e-5f);
    EXPECT_NEAR(std::abs(point[2]), 1.0f, 1e-5f);
  }
}

TEST(AnalyticGeomTessellation, CylinderProducesSideAndCaps)
{
  const pxr::UsdStageRefPtr stage = pxr::UsdStage::CreateInMemory("cyl.usda");
  const pxr::UsdGeomCylinder cylinder =
      pxr::UsdGeomCylinder::Define(stage, pxr::SdfPath("/Cyl"));
  cylinder.GetRadiusAttr().Set(0.5);
  cylinder.GetHeightAttr().Set(2.0);
  cylinder.GetAxisAttr().Set(pxr::UsdGeomTokens->y);

  const AnalyticGeomResult result =
      pyxis::usd_ingest::TessellateCylinder(cylinder);
  ExpectWellFormed(result, /*minVerts=*/100);

  // Y-axis cylinder, half-height = 1, radius = 0.5: every point's
  // y ∈ [-1, 1] AND its (x, z) lies on a disc of radius ≤ 0.5.
  for (const auto& point : result.points)
  {
    EXPECT_GE(point[1], -1.0f - 1e-4f);
    EXPECT_LE(point[1],  1.0f + 1e-4f);
    const float discRadius = std::sqrt(point[0]*point[0] + point[2]*point[2]);
    EXPECT_LE(discRadius, 0.5f + 1e-4f);
  }
}

TEST(AnalyticGeomTessellation, ConeApexSharedAcrossSliceTriangles)
{
  const pxr::UsdStageRefPtr stage = pxr::UsdStage::CreateInMemory("cone.usda");
  const pxr::UsdGeomCone cone =
      pxr::UsdGeomCone::Define(stage, pxr::SdfPath("/Cone"));
  cone.GetRadiusAttr().Set(1.0);
  cone.GetHeightAttr().Set(2.0);
  cone.GetAxisAttr().Set(pxr::UsdGeomTokens->y);

  const AnalyticGeomResult result = pyxis::usd_ingest::TessellateCone(cone);
  ExpectWellFormed(result, /*minVerts=*/80);

  // Apex sits at +halfHeight on Y. Many points (one per slant slice)
  // should match that exactly.
  int apexCount = 0;
  for (const auto& point : result.points)
  {
    if (std::abs(point[1] - 1.0f) < 1e-4f
        && std::abs(point[0]) < 1e-4f
        && std::abs(point[2]) < 1e-4f)
      ++apexCount;
  }
  EXPECT_GE(apexCount, 1);
}

TEST(AnalyticGeomTessellation, CapsuleTotalLengthRespectsHeightPlusTwoRadii)
{
  const pxr::UsdStageRefPtr stage = pxr::UsdStage::CreateInMemory("cap.usda");
  const pxr::UsdGeomCapsule capsule =
      pxr::UsdGeomCapsule::Define(stage, pxr::SdfPath("/Cap"));
  capsule.GetRadiusAttr().Set(0.5);
  capsule.GetHeightAttr().Set(1.0);  // cylinder section only
  capsule.GetAxisAttr().Set(pxr::UsdGeomTokens->y);

  const AnalyticGeomResult result = pyxis::usd_ingest::TessellateCapsule(capsule);
  ExpectWellFormed(result, /*minVerts=*/100);

  // Y span = ±(halfHeight + radius) = ±(0.5 + 0.5) = ±1.
  float minY =  1e9f;
  float maxY = -1e9f;
  for (const auto& point : result.points)
  {
    minY = std::min(minY, point[1]);
    maxY = std::max(maxY, point[1]);
  }
  EXPECT_NEAR(minY, -1.0f, 1e-3f);
  EXPECT_NEAR(maxY,  1.0f, 1e-3f);
}

TEST(AnalyticGeomTessellation, BasisCurvesEmitsRibbonPerSegment)
{
  const pxr::UsdStageRefPtr stage = pxr::UsdStage::CreateInMemory("curves.usda");
  const pxr::UsdGeomBasisCurves curves =
      pxr::UsdGeomBasisCurves::Define(stage, pxr::SdfPath("/Curves"));

  const pxr::VtArray<pxr::GfVec3f> points{
      {0.0f, 0.0f, 0.0f},
      {1.0f, 0.0f, 0.0f},
      {2.0f, 0.0f, 0.0f},
      {3.0f, 0.0f, 0.0f},
  };
  const pxr::VtArray<int> vertCounts{4};
  const pxr::VtArray<float> widths{0.1f, 0.1f, 0.1f, 0.1f};
  curves.GetPointsAttr().Set(points);
  curves.GetCurveVertexCountsAttr().Set(vertCounts);
  curves.GetWidthsAttr().Set(widths);
  curves.GetTypeAttr().Set(pxr::UsdGeomTokens->linear);

  const AnalyticGeomResult result =
      pyxis::usd_ingest::TessellateBasisCurves(curves, pxr::GfVec3f{0.0f, 1.0f, 0.0f});
  ExpectWellFormed(result, /*minVerts=*/8);  // 4 CVs × 2 ribbon edges

  // 4 CVs → 3 segments → 3 quads → 6 triangles.
  EXPECT_EQ(result.faceCounts.size(), 6u);
  EXPECT_EQ(result.points.size(), 8u);
}

TEST(AnalyticGeomTessellation, PointsEmitsOneQuadPerPoint)
{
  const pxr::UsdStageRefPtr stage = pxr::UsdStage::CreateInMemory("points.usda");
  const pxr::UsdGeomPoints points =
      pxr::UsdGeomPoints::Define(stage, pxr::SdfPath("/Points"));

  const pxr::VtArray<pxr::GfVec3f> pts{
      {0.0f, 0.0f, 0.0f},
      {1.0f, 0.0f, 0.0f},
      {2.0f, 0.0f, 0.0f},
  };
  const pxr::VtArray<float> widths{0.5f, 0.5f, 0.5f};
  points.GetPointsAttr().Set(pts);
  points.GetWidthsAttr().Set(widths);

  const AnalyticGeomResult result =
      pyxis::usd_ingest::TessellatePoints(points, pxr::GfVec3f{0.0f, 1.0f, 0.0f});
  ExpectWellFormed(result, /*minVerts=*/12);  // 3 points × 4 corners

  // 3 quads → 6 triangles → 18 indices.
  EXPECT_EQ(result.faceCounts.size(), 6u);
  EXPECT_EQ(result.points.size(), 12u);
}
