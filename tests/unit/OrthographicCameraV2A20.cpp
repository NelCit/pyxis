// Pyxis V2.A.20 — UsdGeomCamera.projection ingest detection.
//
// Pins the contract our StageWalker relies on: when an authored
// UsdGeomCamera has `projection = orthographic`, the typed accessor
// `GetProjectionAttr()` returns the canonical `UsdGeomTokens->
// orthographic` value. The production helper at
// `StageWalker.cpp:BuildCameraDesc` keys the `CameraDesc.projectionMode = 1u`
// branch off exactly that comparison, so locking the token round-trip
// down at the USD layer is the lowest-cost reproducible test for the
// detection path.
//
// The full ingest → CameraDesc → CameraUniforms → raygen path is
// covered end-to-end by the `camera_orthographic` golden fixture; this
// unit test exists to catch a USD-side regression (token rename,
// schema flip) before it shows up as a silently-perspective render.
//
// Also asserts the public `CameraDesc.projectionMode` POD default
// (0 = perspective) hasn't drifted — a §22.3 ABI invariant.

#include <Pyxis/Renderer/Descs/CameraDesc.h>

#include <pxr/base/tf/token.h>
#include <pxr/usd/sdf/path.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdGeom/camera.h>
#include <pxr/usd/usdGeom/tokens.h>

#include <gtest/gtest.h>

TEST(OrthographicCameraV2A20, CameraDescDefaultsToPerspective) {
  const pyxis::CameraDesc desc;
  EXPECT_EQ(desc.projectionMode, 0u)
      << "CameraDesc.projectionMode default must stay 0 (perspective)";
}

TEST(OrthographicCameraV2A20, AuthoredOrthographicRoundTrips) {
  const pxr::UsdStageRefPtr stage = pxr::UsdStage::CreateInMemory();
  const pxr::UsdGeomCamera camera =
      pxr::UsdGeomCamera::Define(stage, pxr::SdfPath("/World/Cam"));
  camera.CreateProjectionAttr(pxr::VtValue(pxr::UsdGeomTokens->orthographic));

  pxr::TfToken projection;
  ASSERT_TRUE(camera.GetProjectionAttr().Get(&projection));
  EXPECT_EQ(projection, pxr::UsdGeomTokens->orthographic);
}

TEST(OrthographicCameraV2A20, DefaultCameraReportsPerspective) {
  const pxr::UsdStageRefPtr stage = pxr::UsdStage::CreateInMemory();
  const pxr::UsdGeomCamera camera =
      pxr::UsdGeomCamera::Define(stage, pxr::SdfPath("/World/Cam"));
  // No CreateProjectionAttr() call — the schema's fallback is
  // `perspective`; StageWalker leaves projectionMode at 0.
  pxr::TfToken projection;
  ASSERT_TRUE(camera.GetProjectionAttr().Get(&projection));
  EXPECT_EQ(projection, pxr::UsdGeomTokens->perspective);
  EXPECT_NE(projection, pxr::UsdGeomTokens->orthographic);
}
