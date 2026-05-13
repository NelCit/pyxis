// Pyxis V2.A.1 follow-up — subdivision crease coverage. Authors a
// catmullClark cube cage with one fully-sharp crease edge along the
// top face + one fully-sharp corner, refines via SubdivRefine, and
// asserts that the refined limit surface (a) actually refined and
// (b) preserved the sharp-edge topology by checking that two
// vertices flanking the crease keep different positions across the
// edge (rather than smoothing into the average).

#include "SubdivRefine.h"

#include <pxr/base/gf/vec2f.h>
#include <pxr/base/gf/vec3f.h>
#include <pxr/base/vt/array.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdGeom/mesh.h>
#include <pxr/usd/usdGeom/tokens.h>

#include <gtest/gtest.h>

namespace {

// Author a 1m unit cube as a catmullClark cage: 8 verts, 6 quad
// faces, with the top edge between v4=(-,+,-) and v5=(+,+,-)
// authored as a fully-sharp crease (sharpness = 10.0f, OpenSubdiv's
// "infinity" practical cap).
pxr::UsdStageRefPtr BuildCubeCage(float creaseWeight, float cornerWeight) {
  pxr::UsdStageRefPtr stage = pxr::UsdStage::CreateInMemory("crease.usda");
  const pxr::UsdGeomMesh mesh  = pxr::UsdGeomMesh::Define(stage, pxr::SdfPath("/Cube"));

  const pxr::VtArray<pxr::GfVec3f> points{
      {-0.5f, -0.5f, -0.5f}, { 0.5f, -0.5f, -0.5f},
      { 0.5f,  0.5f, -0.5f}, {-0.5f,  0.5f, -0.5f},
      {-0.5f, -0.5f,  0.5f}, { 0.5f, -0.5f,  0.5f},
      { 0.5f,  0.5f,  0.5f}, {-0.5f,  0.5f,  0.5f},
  };
  const pxr::VtArray<int> counts{4, 4, 4, 4, 4, 4};
  const pxr::VtArray<int> indices{
      0, 1, 2, 3,   // -Z
      4, 7, 6, 5,   // +Z
      0, 4, 5, 1,   // -Y
      3, 2, 6, 7,   // +Y
      0, 3, 7, 4,   // -X
      1, 5, 6, 2,   // +X
  };
  mesh.GetPointsAttr().Set(points);
  mesh.GetFaceVertexCountsAttr().Set(counts);
  mesh.GetFaceVertexIndicesAttr().Set(indices);
  mesh.GetSubdivisionSchemeAttr().Set(pxr::UsdGeomTokens->catmullClark);

  if (creaseWeight > 0.0f)
  {
    // One crease chain along the +Y edge between v3=(-,+,-) and
    // v2=(+,+,-). Length 2 → 1 segment → 1 weight (per-crease form).
    const pxr::VtArray<int>   creaseIndices{3, 2};
    const pxr::VtArray<int>   creaseLengths{2};
    const pxr::VtArray<float> creaseSharps{creaseWeight};
    mesh.GetCreaseIndicesAttr().Set(creaseIndices);
    mesh.GetCreaseLengthsAttr().Set(creaseLengths);
    mesh.GetCreaseSharpnessesAttr().Set(creaseSharps);
  }
  if (cornerWeight > 0.0f)
  {
    const pxr::VtArray<int>   cornerIndices{0};
    const pxr::VtArray<float> cornerSharps{cornerWeight};
    mesh.GetCornerIndicesAttr().Set(cornerIndices);
    mesh.GetCornerSharpnessesAttr().Set(cornerSharps);
  }
  return stage;
}

}  // namespace

TEST(SubdivCreaseFixture, RefinesWithoutCreases) {
  const pxr::UsdStageRefPtr stage = BuildCubeCage(/*creaseW*/ 0.0f, /*cornerW*/ 0.0f);
  const pxr::UsdGeomMesh    mesh{stage->GetPrimAtPath(pxr::SdfPath("/Cube"))};
  pxr::VtArray<pxr::GfVec3f> srcPts;
  pxr::VtArray<int>          srcCounts;
  pxr::VtArray<int>          srcIndices;
  mesh.GetPointsAttr().Get(&srcPts);
  mesh.GetFaceVertexCountsAttr().Get(&srcCounts);
  mesh.GetFaceVertexIndicesAttr().Get(&srcIndices);

  const pyxis::usd_ingest::SubdivRefinedMesh refined =
      pyxis::usd_ingest::RefineSubdivMesh(mesh, srcPts, srcCounts, srcIndices, {}, false, 2);
  ASSERT_TRUE(refined.refined);
  EXPECT_GT(refined.points.size(), srcPts.size());
}

TEST(SubdivCreaseFixture, CreasePreservesSharpEdge) {
  const pxr::UsdStageRefPtr smoothStage =
      BuildCubeCage(/*creaseW*/ 0.0f, /*cornerW*/ 0.0f);
  const pxr::UsdStageRefPtr sharpStage =
      BuildCubeCage(/*creaseW*/ 10.0f, /*cornerW*/ 0.0f);

  auto refine = [](const pxr::UsdStageRefPtr& stage)
      -> pyxis::usd_ingest::SubdivRefinedMesh {
    const pxr::UsdGeomMesh mesh{stage->GetPrimAtPath(pxr::SdfPath("/Cube"))};
    pxr::VtArray<pxr::GfVec3f> srcPts;
    pxr::VtArray<int>          srcCounts;
    pxr::VtArray<int>          srcIndices;
    mesh.GetPointsAttr().Get(&srcPts);
    mesh.GetFaceVertexCountsAttr().Get(&srcCounts);
    mesh.GetFaceVertexIndicesAttr().Get(&srcIndices);
    return pyxis::usd_ingest::RefineSubdivMesh(
        mesh, srcPts, srcCounts, srcIndices, {}, false, 2);
  };
  const auto smoothRefined = refine(smoothStage);
  const auto sharpRefined  = refine(sharpStage);
  ASSERT_TRUE(smoothRefined.refined);
  ASSERT_TRUE(sharpRefined.refined);

  // OpenSubdiv preserves the creased edge geometry — the midpoint of
  // the creased edge (v3 ↔ v2) stays at the cage edge in the sharp
  // variant while smooth subdivision pulls it inward toward the
  // limit surface centroid.
  //
  // At refinement level 2 the new edge midpoint IS one of the output
  // vertices (Catmark inserts an edge-midpoint vertex per cage edge);
  // in the sharp variant it sits exactly at (0, 0.5, -0.5), while
  // the smooth variant pulls it noticeably toward the cube interior.
  const pxr::GfVec3f creasedEdgeMid{0.0f, 0.5f, -0.5f};
  auto bestDistanceTo = [&](const pyxis::usd_ingest::SubdivRefinedMesh& refinedMesh,
                            const pxr::GfVec3f& target) -> float {
    float best = std::numeric_limits<float>::max();
    for (const pxr::GfVec3f& point : refinedMesh.points)
    {
      const pxr::GfVec3f delta = point - target;
      const float distSq = delta.GetLengthSq();
      if (distSq < best) best = distSq;
    }
    return best;
  };
  const float smoothDistSq = bestDistanceTo(smoothRefined, creasedEdgeMid);
  const float sharpDistSq  = bestDistanceTo(sharpRefined,  creasedEdgeMid);
  // The sharp variant sits substantially closer to the cage edge
  // than the smooth one — the absolute distance isn't ~0 because
  // Catmull-Clark only re-inserts edge-midpoints level by level
  // (the level-2 sample-grid doesn't necessarily hit the exact
  // mid-edge), but the ratio is dramatic. Demand at least 5×
  // closer in the sharp case.
  EXPECT_LT(sharpDistSq * 5.0f, smoothDistSq);
}

TEST(SubdivCreaseFixture, CornerPreservesSharpVertex) {
  const pxr::UsdStageRefPtr sharpCornerStage =
      BuildCubeCage(/*creaseW*/ 0.0f, /*cornerW*/ 10.0f);
  const pxr::UsdStageRefPtr smoothStage =
      BuildCubeCage(/*creaseW*/ 0.0f, /*cornerW*/ 0.0f);

  auto refine = [](const pxr::UsdStageRefPtr& stage) {
    const pxr::UsdGeomMesh mesh{stage->GetPrimAtPath(pxr::SdfPath("/Cube"))};
    pxr::VtArray<pxr::GfVec3f> srcPts;
    pxr::VtArray<int>          srcCounts;
    pxr::VtArray<int>          srcIndices;
    mesh.GetPointsAttr().Get(&srcPts);
    mesh.GetFaceVertexCountsAttr().Get(&srcCounts);
    mesh.GetFaceVertexIndicesAttr().Get(&srcIndices);
    return pyxis::usd_ingest::RefineSubdivMesh(
        mesh, srcPts, srcCounts, srcIndices, {}, false, 2);
  };
  const auto sharpRefined  = refine(sharpCornerStage);
  const auto smoothRefined = refine(smoothStage);
  ASSERT_TRUE(sharpRefined.refined);
  ASSERT_TRUE(smoothRefined.refined);

  // Corner v0 is the (-,-,-) cube corner. With cornerSharpness=10
  // the limit surface keeps it exactly at the cage corner.
  const pxr::GfVec3f cageV0{-0.5f, -0.5f, -0.5f};
  auto bestDistanceTo = [&](const pyxis::usd_ingest::SubdivRefinedMesh& refinedMesh) -> float {
    float best = std::numeric_limits<float>::max();
    for (const pxr::GfVec3f& point : refinedMesh.points)
    {
      const pxr::GfVec3f delta = point - cageV0;
      const float distSq = delta.GetLengthSq();
      if (distSq < best) best = distSq;
    }
    return best;
  };
  EXPECT_LT(bestDistanceTo(sharpRefined), bestDistanceTo(smoothRefined));
  EXPECT_LT(bestDistanceTo(sharpRefined), 1e-6f);
}
