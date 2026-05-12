// Pyxis V2.A.4 — NURBS cubic Bezier patch tessellation.
//
// Author a flat 4x4 control net in the XY plane (z = 0); verify the
// tessellator produces a 17x17 vertex grid (16x16 quads), all
// vertices still on z = 0 (flat patch invariant), and the normals
// point along +Z.

#include "AnalyticGeom.h"

#include <pxr/base/vt/array.h>
#include <pxr/usd/sdf/path.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdGeom/nurbsPatch.h>

#include <gtest/gtest.h>

#include <cmath>

namespace {

pxr::UsdGeomNurbsPatch BuildFlatPatch(const pxr::UsdStageRefPtr& stage,
                                       float scale)
{
  const pxr::UsdGeomNurbsPatch patch =
      pxr::UsdGeomNurbsPatch::Define(stage, pxr::SdfPath("/Patch"));
  patch.GetUVertexCountAttr().Set(4);
  patch.GetVVertexCountAttr().Set(4);
  patch.GetUOrderAttr().Set(4);
  patch.GetVOrderAttr().Set(4);

  pxr::VtArray<pxr::GfVec3f> net;
  net.reserve(16);
  for (int row = 0; row < 4; ++row)
  {
    for (int col = 0; col < 4; ++col)
    {
      const float xVal = static_cast<float>(col) / 3.0f * scale;
      const float yVal = static_cast<float>(row) / 3.0f * scale;
      net.emplace_back(xVal, yVal, 0.0f);
    }
  }
  patch.GetPointsAttr().Set(net);
  return patch;
}

}  // namespace

TEST(NurbsTessellation, FlatPatchProducesFlatGrid)
{
  const pxr::UsdStageRefPtr stage = pxr::UsdStage::CreateInMemory("flat.usda");
  const pxr::UsdGeomNurbsPatch patch = BuildFlatPatch(stage, 2.0f);

  const auto result = pyxis::usd_ingest::TessellateNurbsPatch(patch);
  ASSERT_TRUE(result.success);
  // 16x16 quads → 17x17 grid.
  EXPECT_EQ(result.points.size(), 17u * 17u);
  EXPECT_EQ(result.faceCounts.size(), 16u * 16u * 2u);  // 2 tris per quad

  // Flat patch: every vertex's Z stays 0.
  for (const auto& point : result.points)
    EXPECT_NEAR(point[2], 0.0f, 1e-5f);

  // Bilinear normals on a Z-flat surface point along ±Z. Pick the
  // mid-vertex (which uses central-difference) — should be +Z to a
  // few digits.
  const auto& midNormal = result.normals[17u * 8u + 8u];
  EXPECT_NEAR(std::abs(midNormal[2]), 1.0f, 1e-3f);
}

TEST(NurbsTessellation, NonCubicConfigFailsGracefully)
{
  const pxr::UsdStageRefPtr stage = pxr::UsdStage::CreateInMemory("not_bezier.usda");
  const pxr::UsdGeomNurbsPatch patch =
      pxr::UsdGeomNurbsPatch::Define(stage, pxr::SdfPath("/Patch"));
  patch.GetUVertexCountAttr().Set(5);   // not 4
  patch.GetVVertexCountAttr().Set(4);
  patch.GetUOrderAttr().Set(4);
  patch.GetVOrderAttr().Set(4);

  const auto result = pyxis::usd_ingest::TessellateNurbsPatch(patch);
  // 5x4 control net is not the cubic Bezier case — translator
  // returns success=false; caller falls through to the detect-warn
  // path.
  EXPECT_FALSE(result.success);
}
