// Pyxis M15 / V2.A.5 — UsdVolVolume detection. The StageWalker stubs
// out volume rendering by detecting `UsdVolVolume` prims and logging
// a warning; full OpenVDB integration is a follow-up. This test just
// pins the detection check that the stub relies on.

#include <pxr/usd/sdf/path.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdGeom/cube.h>
#include <pxr/usd/usdVol/volume.h>

#include <gtest/gtest.h>

TEST(UsdVolDetection, IsAVolumeOnVolumePrim)
{
  const pxr::UsdStageRefPtr stage = pxr::UsdStage::CreateInMemory("vol.usda");
  const pxr::UsdVolVolume volume =
      pxr::UsdVolVolume::Define(stage, pxr::SdfPath("/Vol"));
  ASSERT_TRUE(volume);

  const pxr::UsdPrim prim = volume.GetPrim();
  EXPECT_TRUE(prim.IsA<pxr::UsdVolVolume>());
}

TEST(UsdVolDetection, IsAVolumeFalseOnCube)
{
  const pxr::UsdStageRefPtr stage = pxr::UsdStage::CreateInMemory("cube.usda");
  const pxr::UsdGeomCube cube =
      pxr::UsdGeomCube::Define(stage, pxr::SdfPath("/Cube"));
  ASSERT_TRUE(cube);

  const pxr::UsdPrim prim = cube.GetPrim();
  EXPECT_FALSE(prim.IsA<pxr::UsdVolVolume>());
}
