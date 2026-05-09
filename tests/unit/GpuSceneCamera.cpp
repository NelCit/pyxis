// Pyxis renderer — GpuScene camera tests.
//
// Plan §18.5. SetCamera / HasCamera / GetCamera are the simplest
// surface in GpuScene: no handle table, no stale-handle dance, just
// a single optional CameraDesc that the path-trace pass binds when
// HasCamera() returns true. The tests pin the round-trip and the
// last-write-wins semantics that the §29.4 default-scene chain
// (M3.5+) and Hydra `HdSprim` push (M4+) both rely on.

#include <Pyxis/Renderer/Descs/CameraDesc.h>
#include <Pyxis/Renderer/Descs/GpuSceneCreateDesc.h>
#include <Pyxis/Renderer/Forward.h>
#include <Pyxis/Renderer/GpuScene.h>
#include <Pyxis/Renderer/Profiler.h>

#include <gtest/gtest.h>

#include <hlsl++.h>

using pyxis::CameraDesc;
using pyxis::GpuScene;
using pyxis::GpuSceneCreateDesc;
using pyxis::Profiler;

namespace {

struct CpuOnlyScene {
  Profiler profiler{nullptr};
  GpuScene scene{nullptr, profiler, GpuSceneCreateDesc{}};
};

CameraDesc DefaultPerspectiveCamera() noexcept {
  CameraDesc desc;
  // Identity view + a recognisable focal length so GetCamera()
  // round-trips a value distinct from the all-zero default-init.
  desc.viewFromWorld = hlslpp::float4x4(
      hlslpp::float4{1, 0, 0, 0}, hlslpp::float4{0, 1, 0, 0},
      hlslpp::float4{0, 0, 1, 0}, hlslpp::float4{0, 0, 0, 1});
  desc.projFromView = hlslpp::float4x4(
      hlslpp::float4{1, 0, 0, 0}, hlslpp::float4{0, 1, 0, 0},
      hlslpp::float4{0, 0, 1, 0}, hlslpp::float4{0, 0, 0, 1});
  desc.focalLengthMm = 50.0f;
  desc.apertureFStop = 0.0f;  // pinhole sentinel
  desc.focusDistance = 2.5f;
  desc.nearClip = 0.1f;
  desc.farClip = 1000.0f;
  return desc;
}

}  // namespace

// -----------------------------------------------------------------------------
// HasCamera — false until the first SetCamera call. PathTracePass
// keys off this exact bit to early-out on an empty scene (§18.5).
// -----------------------------------------------------------------------------
TEST(GpuSceneCamera, HasCameraIsFalseInitially) {
  const CpuOnlyScene fixture;
  EXPECT_FALSE(fixture.scene.HasCamera());
}

TEST(GpuSceneCamera, SetCameraFlipsHasCameraTrue) {
  CpuOnlyScene fixture;
  fixture.scene.SetCamera(DefaultPerspectiveCamera());
  EXPECT_TRUE(fixture.scene.HasCamera());
}

// -----------------------------------------------------------------------------
// GetCamera — returns the last desc set, by reference. The renderer
// borrows this for the lifetime of the scene; verifying the round-
// trip pins the contract that consumers (PathTracePass binding into
// CameraUniforms) rely on.
// -----------------------------------------------------------------------------
TEST(GpuSceneCamera, GetCameraReturnsLastSetDesc) {
  CpuOnlyScene fixture;
  const CameraDesc set = DefaultPerspectiveCamera();
  fixture.scene.SetCamera(set);

  const CameraDesc& got = fixture.scene.GetCamera();
  EXPECT_FLOAT_EQ(got.focalLengthMm, set.focalLengthMm);
  EXPECT_FLOAT_EQ(got.apertureFStop, set.apertureFStop);
  EXPECT_FLOAT_EQ(got.focusDistance, set.focusDistance);
  EXPECT_FLOAT_EQ(got.nearClip, set.nearClip);
  EXPECT_FLOAT_EQ(got.farClip, set.farClip);
}

TEST(GpuSceneCamera, SetCameraIsLastWriteWins) {
  CpuOnlyScene fixture;
  CameraDesc first = DefaultPerspectiveCamera();
  first.focalLengthMm = 24.0f;
  fixture.scene.SetCamera(first);

  CameraDesc second = DefaultPerspectiveCamera();
  second.focalLengthMm = 85.0f;
  fixture.scene.SetCamera(second);

  EXPECT_TRUE(fixture.scene.HasCamera());
  EXPECT_FLOAT_EQ(fixture.scene.GetCamera().focalLengthMm, 85.0f);
}
