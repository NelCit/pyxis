// Pyxis renderer — GpuScene::Clear tests.
//
// Plan §18.5. Clear() drops every mesh / material / texture / instance /
// light + the TLAS + the camera + the dedup maps + per-frame counters,
// leaving the scene in the exact post-construction shape. The viewer's
// "Open scene..." path is the only public caller today (M7 follow-up);
// the tests pin the observable side of that contract — what
// LastFrameStats / HasCamera / HasMesh report after Clear, plus the
// fact that the slot-0 sentinel survives so a fabricated handle whose
// slot decodes to 0 still resolves to nothing.
//
// CPU-only fixture (no GPU device): the GPU-side handles Clear drops
// (NVRHI buffers, NVRHI textures, TLAS) are reference-counted so the
// CPU test exercises the table-reset logic without crashing on
// undefined NVRHI calls.

#include <Pyxis/Renderer/Descs/CameraDesc.h>
#include <Pyxis/Renderer/Descs/GpuSceneCreateDesc.h>
#include <Pyxis/Renderer/Descs/InstanceDesc.h>
#include <Pyxis/Renderer/Descs/LightDesc.h>
#include <Pyxis/Renderer/Descs/MeshDesc.h>
#include <Pyxis/Renderer/Descs/OpenPBRMaterialDesc.h>
#include <Pyxis/Renderer/Forward.h>
#include <Pyxis/Renderer/GpuScene.h>
#include <Pyxis/Renderer/Profiler.h>

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <hlsl++.h>

using pyxis::CameraDesc;
using pyxis::Expected;
using pyxis::GpuScene;
using pyxis::GpuSceneCreateDesc;
using pyxis::InstanceDesc;
using pyxis::InstanceHandle;
using pyxis::LightDesc;
using pyxis::LightHandle;
using pyxis::MaterialHandle;
using pyxis::MeshDesc;
using pyxis::MeshHandle;
using pyxis::OpenPBRMaterialDesc;
using pyxis::Profiler;

namespace {

struct CpuOnlyScene {
  Profiler profiler{nullptr};
  GpuScene scene{nullptr, profiler, GpuSceneCreateDesc{}};
};

// One-triangle mesh fixture matching the GpuSceneInstance pattern.
struct TriangleFixture {
  std::array<hlslpp::float3, 3> positions{
      hlslpp::float3{0.0f, 0.0f, 0.0f},
      hlslpp::float3{1.0f, 0.0f, 0.0f},
      hlslpp::float3{0.0f, 1.0f, 0.0f},
  };
  std::array<uint32_t, 3> indices{0, 1, 2};

  [[nodiscard]] MeshDesc Desc() const noexcept {
    MeshDesc desc;
    desc.positions = positions;
    desc.indices = indices;
    desc.debugName = "clear-test.triangle";
    return desc;
  }
};

CameraDesc IdentityCamera() noexcept {
  CameraDesc desc;
  desc.viewFromWorld = hlslpp::float4x4(
      hlslpp::float4{1, 0, 0, 0}, hlslpp::float4{0, 1, 0, 0},
      hlslpp::float4{0, 0, 1, 0}, hlslpp::float4{0, 0, 0, 1});
  desc.projFromView = hlslpp::float4x4(
      hlslpp::float4{1, 0, 0, 0}, hlslpp::float4{0, 1, 0, 0},
      hlslpp::float4{0, 0, 1, 0}, hlslpp::float4{0, 0, 0, 1});
  return desc;
}

}  // namespace

// -----------------------------------------------------------------------------
// Clear on a fresh / never-mutated scene is a no-op + idempotent. The
// scene is already in the post-construction shape; Clear just keeps it
// there. Pins that calling Clear before any verb doesn't accidentally
// undo the slot-0 sentinel installation (which would let a fabricated
// handle whose slot decodes to 0 incorrectly resolve).
// -----------------------------------------------------------------------------
TEST(GpuSceneClear, ClearOnFreshSceneLeavesSentinelsIntact) {
  CpuOnlyScene fixture;
  fixture.scene.Clear();

  EXPECT_FALSE(fixture.scene.HasCamera());
  EXPECT_FALSE(fixture.scene.HasMesh(MeshHandle::Invalid));
  EXPECT_FALSE(fixture.scene.HasInstance(InstanceHandle::Invalid));
  EXPECT_FALSE(fixture.scene.HasMaterial(MaterialHandle::Invalid));
  EXPECT_EQ(fixture.scene.LastFrameStats().meshCount, 0u);
  EXPECT_EQ(fixture.scene.LastFrameStats().instanceCount, 0u);
  EXPECT_EQ(fixture.scene.LastFrameStats().lightCount, 0u);
  EXPECT_EQ(fixture.scene.GetLiveMaterialCount(), 0u);
  EXPECT_EQ(fixture.scene.GetLiveLightCount(), 0u);
}

// -----------------------------------------------------------------------------
// Clear on a scene with mesh + material + instance + light + camera
// drops every count back to zero AND flips HasCamera back to false.
// This is the load-bearing post-condition the viewer relies on so a
// re-run of the ingest engine starts with a clean slate.
// -----------------------------------------------------------------------------
TEST(GpuSceneClear, ClearResetsAllLiveCountsAndCamera) {
  CpuOnlyScene fixture;

  // Build a non-trivial scene.
  const TriangleFixture triangle;
  const Expected<MeshHandle> meshResult = fixture.scene.CreateMesh(triangle.Desc());
  ASSERT_TRUE(meshResult.has_value());
  const MeshHandle mesh = *meshResult;

  OpenPBRMaterialDesc materialDesc;
  materialDesc.baseColor = hlslpp::float3{0.7f, 0.2f, 0.3f};
  materialDesc.sourcePrim = "/Mat/Test";
  const MaterialHandle material = fixture.scene.AcquireMaterial(materialDesc);
  ASSERT_NE(material, MaterialHandle::Invalid);

  InstanceDesc instanceDesc;
  instanceDesc.mesh = mesh;
  instanceDesc.material = material;
  instanceDesc.worldFromLocal = hlslpp::float4x4(
      hlslpp::float4{1, 0, 0, 0}, hlslpp::float4{0, 1, 0, 0},
      hlslpp::float4{0, 0, 1, 0}, hlslpp::float4{0, 0, 0, 1});
  instanceDesc.debugName = "clear-test.instance";
  const Expected<InstanceHandle> instanceResult = fixture.scene.AppendInstance(instanceDesc);
  ASSERT_TRUE(instanceResult.has_value());

  LightDesc lightDesc;
  lightDesc.kind = LightDesc::Kind::Distant;
  lightDesc.color = hlslpp::float3{1.0f, 1.0f, 1.0f};
  lightDesc.intensity = 1.0f;
  lightDesc.direction = hlslpp::float3{0.0f, -1.0f, 0.0f};
  const LightHandle light = fixture.scene.AddLight(lightDesc);
  ASSERT_NE(light, LightHandle::Invalid);

  fixture.scene.SetCamera(IdentityCamera());

  // Pre-conditions — scene is non-empty.
  ASSERT_EQ(fixture.scene.LastFrameStats().meshCount, 1u);
  ASSERT_EQ(fixture.scene.LastFrameStats().instanceCount, 1u);
  ASSERT_EQ(fixture.scene.LastFrameStats().lightCount, 1u);
  ASSERT_EQ(fixture.scene.GetLiveMaterialCount(), 1u);
  ASSERT_TRUE(fixture.scene.HasCamera());

  // Act — wipe the scene.
  fixture.scene.Clear();

  // Post-conditions — every counter back to zero, camera gone, the
  // previously-live handles no longer resolve.
  EXPECT_EQ(fixture.scene.LastFrameStats().meshCount, 0u);
  EXPECT_EQ(fixture.scene.LastFrameStats().instanceCount, 0u);
  EXPECT_EQ(fixture.scene.LastFrameStats().lightCount, 0u);
  EXPECT_EQ(fixture.scene.GetLiveMaterialCount(), 0u);
  EXPECT_EQ(fixture.scene.GetLiveLightCount(), 0u);
  EXPECT_FALSE(fixture.scene.HasCamera());
  EXPECT_FALSE(fixture.scene.HasMesh(mesh));
  EXPECT_FALSE(fixture.scene.HasMaterial(material));
  EXPECT_FALSE(fixture.scene.HasInstance(*instanceResult));
}

// -----------------------------------------------------------------------------
// Per-frame counters (staleHandleDrops) reset alongside the live ones —
// otherwise a scene reload would inherit the pre-Clear stale-drop
// count, polluting the stats panel's "this frame" semantics.
// -----------------------------------------------------------------------------
TEST(GpuSceneClear, ClearResetsStaleHandleDropsCounter) {
  CpuOnlyScene fixture;

  // Stage a stale-handle drop by destroying then re-destroying a mesh.
  const TriangleFixture triangle;
  const Expected<MeshHandle> mesh = fixture.scene.CreateMesh(triangle.Desc());
  ASSERT_TRUE(mesh.has_value());
  fixture.scene.DestroyMesh(*mesh);
  fixture.scene.DestroyMesh(*mesh);  // counts a stale-handle drop
  ASSERT_GT(fixture.scene.LastFrameStats().staleHandleDrops, 0u);

  fixture.scene.Clear();
  EXPECT_EQ(fixture.scene.LastFrameStats().staleHandleDrops, 0u);
}

// -----------------------------------------------------------------------------
// After Clear, the scene must still be MUTABLE — the next CreateMesh /
// AppendInstance / etc. has to succeed cleanly with a fresh slot. This
// pins the viewer's reload flow: device->waitForIdle → Clear →
// engine.Load(newPath) must produce live handles, not Invalid.
// -----------------------------------------------------------------------------
TEST(GpuSceneClear, ClearLeavesSceneMutable) {
  CpuOnlyScene fixture;

  const TriangleFixture triangle;
  ASSERT_TRUE(fixture.scene.CreateMesh(triangle.Desc()).has_value());
  fixture.scene.Clear();

  // Re-populate from scratch.
  const Expected<MeshHandle> reborn = fixture.scene.CreateMesh(triangle.Desc());
  ASSERT_TRUE(reborn.has_value()) << reborn.error().message.View();
  EXPECT_NE(*reborn, MeshHandle::Invalid);
  EXPECT_TRUE(fixture.scene.HasMesh(*reborn));
  EXPECT_EQ(fixture.scene.LastFrameStats().meshCount, 1u);
}

// -----------------------------------------------------------------------------
// §15 dedup map remains FUNCTIONAL after Clear — the next CreateMesh
// with byte-identical content allocates a single fresh entry, and a
// SECOND CreateMesh with the same content collapses to that same
// entry via the dedup hit. Tests the rebuilt-after-Clear state of
// `meshDescHashToHandle` rather than the encoding of the returned
// handle (CreateMesh's self-healing fallback already drops stale
// dedup entries on the fly, so a "first must not resolve" assertion
// is observationally meaningless either way).
// -----------------------------------------------------------------------------
TEST(GpuSceneClear, ClearLeavesContentDedupFunctional) {
  CpuOnlyScene fixture;
  const TriangleFixture triangle;
  ASSERT_TRUE(fixture.scene.CreateMesh(triangle.Desc()).has_value());

  fixture.scene.Clear();

  // Two CreateMesh calls with byte-identical content post-Clear must
  // collapse to ONE mesh entry via the §15 content-dedup path. If
  // Clear had left the map in a broken state (e.g. cleared the
  // table but left orphan map entries pointing at out-of-range slots),
  // the second CreateMesh would either crash, return Invalid, or
  // allocate a second entry — every one of which the
  // `meshCount == 1u` check below catches.
  const Expected<MeshHandle> first = fixture.scene.CreateMesh(triangle.Desc());
  const Expected<MeshHandle> second = fixture.scene.CreateMesh(triangle.Desc());
  ASSERT_TRUE(first.has_value()) << first.error().message.View();
  ASSERT_TRUE(second.has_value()) << second.error().message.View();
  EXPECT_EQ(*first, *second) << "§15 content-dedup must still collapse identical content";
  EXPECT_TRUE(fixture.scene.HasMesh(*first));
  EXPECT_EQ(fixture.scene.LastFrameStats().meshCount, 1u);
}
