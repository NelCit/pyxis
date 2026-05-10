// Pyxis renderer — GpuScene instance handle table tests.
//
// Plan §18.5 + §19.7. Exercises AppendInstance / HasInstance /
// UpdateInstance* / SetInstanceVisibility / DestroyInstance against
// the public surface. CPU-only fixture (no GPU device) — instance
// metadata is pure CPU bookkeeping until CommitResources runs the
// TLAS rebuild on the supplied command list.
//
// Same rationale as GpuSceneMesh.cpp: pin the byte-stable §18.5
// observable behaviour (handle round-trip, stale-handle policy, stat
// counters) rather than the impl detail. Future ingest adapters
// (Hydra, USD-direct) consume the same surface.

#include <Pyxis/Renderer/Descs/GpuSceneCreateDesc.h>
#include <Pyxis/Renderer/Descs/InstanceDesc.h>
#include <Pyxis/Renderer/Descs/MeshDesc.h>
#include <Pyxis/Renderer/Forward.h>
#include <Pyxis/Renderer/GpuScene.h>
#include <Pyxis/Renderer/Profiler.h>

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <hlsl++.h>

using pyxis::ErrorKind;
using pyxis::Expected;
using pyxis::GpuScene;
using pyxis::GpuSceneCreateDesc;
using pyxis::HANDLE_SLOT_BITS;
using pyxis::HANDLE_SLOT_MASK;
using pyxis::InstanceDesc;
using pyxis::InstanceHandle;
using pyxis::MaterialHandle;
using pyxis::MeshDesc;
using pyxis::MeshHandle;
using pyxis::Profiler;

namespace {

// Reused single-triangle mesh — every test that needs a live mesh
// builds one from this fixture so AppendInstance has a valid
// `instanceDesc.mesh` to point at. The `seed` constructor argument
// nudges vertex 0's X coord so two fixtures with different seeds
// hash to different §15 dedup buckets — needed any time a test
// wants two DISTINCT mesh slots in the same scene.
struct TriangleFixture {
  std::array<hlslpp::float3, 3> positions;
  std::array<uint32_t, 3> indices{0, 1, 2};

  explicit TriangleFixture(float seed = 0.0f) noexcept {
    positions = {
        hlslpp::float3{seed, 0.0f, 0.0f},
        hlslpp::float3{1.0f, 0.0f, 0.0f},
        hlslpp::float3{0.0f, 1.0f, 0.0f},
    };
  }

  [[nodiscard]] MeshDesc Desc() const noexcept {
    MeshDesc desc;
    desc.positions = positions;
    desc.indices = indices;
    desc.debugName = "unit-test.triangle";
    return desc;
  }
};

struct CpuOnlyScene {
  Profiler profiler{nullptr};
  GpuScene scene{nullptr, profiler, GpuSceneCreateDesc{}};

  // Convenience: every instance test needs at least one live mesh.
  // Returns a valid MeshHandle the caller can drop into InstanceDesc.
  // Default seed=0 — repeated calls with no seed dedup to the same
  // MeshHandle (§15). Pass a distinct seed when you need a DIFFERENT
  // mesh slot from a prior MakeMesh call in the same scene.
  [[nodiscard]] MeshHandle MakeMesh(float seed = 0.0f) {
    const TriangleFixture triangle{seed};
    auto created = scene.CreateMesh(triangle.Desc());
    EXPECT_TRUE(created.has_value());
    return *created;
  }

  [[nodiscard]] InstanceDesc DescForMesh(MeshHandle mesh) const noexcept {
    InstanceDesc desc;
    desc.mesh = mesh;
    desc.worldFromLocal = hlslpp::float4x4(
        hlslpp::float4{1, 0, 0, 0}, hlslpp::float4{0, 1, 0, 0},
        hlslpp::float4{0, 0, 1, 0}, hlslpp::float4{0, 0, 0, 1});
    desc.debugName = "unit-test.instance";
    return desc;
  }
};

constexpr uint32_t SlotOf(InstanceHandle handle) noexcept {
  return static_cast<uint32_t>(handle) & HANDLE_SLOT_MASK;
}
constexpr uint8_t GenerationOf(InstanceHandle handle) noexcept {
  return static_cast<uint8_t>(static_cast<uint32_t>(handle) >> HANDLE_SLOT_BITS);
}

}  // namespace

// -----------------------------------------------------------------------------
// AppendInstance — happy path.
// -----------------------------------------------------------------------------
TEST(GpuSceneInstance, AppendInstanceReturnsLiveHandle) {
  CpuOnlyScene fixture;
  const MeshHandle mesh = fixture.MakeMesh();

  const Expected<InstanceHandle> result = fixture.scene.AppendInstance(fixture.DescForMesh(mesh));
  ASSERT_TRUE(result.has_value()) << result.error().message.View();
  EXPECT_NE(*result, InstanceHandle::Invalid);
  EXPECT_TRUE(fixture.scene.HasInstance(*result));
}

TEST(GpuSceneInstance, AppendInstanceAllocatesDistinctSlotsForSequentialCreates) {
  CpuOnlyScene fixture;
  const MeshHandle mesh = fixture.MakeMesh();

  const Expected<InstanceHandle> first = fixture.scene.AppendInstance(fixture.DescForMesh(mesh));
  const Expected<InstanceHandle> second = fixture.scene.AppendInstance(fixture.DescForMesh(mesh));
  ASSERT_TRUE(first.has_value());
  ASSERT_TRUE(second.has_value());
  EXPECT_NE(*first, *second);
  EXPECT_NE(SlotOf(*first), SlotOf(*second));
  EXPECT_TRUE(fixture.scene.HasInstance(*first));
  EXPECT_TRUE(fixture.scene.HasInstance(*second));
}

// -----------------------------------------------------------------------------
// AppendInstance — input validation. Mesh handle is the one required
// linkage; a missing or stale mesh handle is a hard reject (not the
// silent §18.5 stale-handle policy, since AppendInstance returns
// Expected<T>).
// -----------------------------------------------------------------------------
TEST(GpuSceneInstance, AppendInstanceRejectsInvalidMesh) {
  CpuOnlyScene fixture;
  InstanceDesc desc;
  desc.mesh = MeshHandle::Invalid;

  const Expected<InstanceHandle> result = fixture.scene.AppendInstance(desc);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().kind, ErrorKind::InvalidArgument);
}

TEST(GpuSceneInstance, AppendInstanceRejectsRecycledMesh) {
  CpuOnlyScene fixture;
  const MeshHandle mesh = fixture.MakeMesh();
  fixture.scene.DestroyMesh(mesh);

  // The handle now points at a recycled slot — generation mismatch.
  const Expected<InstanceHandle> result = fixture.scene.AppendInstance(fixture.DescForMesh(mesh));
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().kind, ErrorKind::InvalidHandle);
}

// -----------------------------------------------------------------------------
// HasInstance — spec checks (matches the GpuSceneMesh pattern).
// -----------------------------------------------------------------------------
TEST(GpuSceneInstance, HasInstanceIsFalseForInvalid) {
  const CpuOnlyScene fixture;
  EXPECT_FALSE(fixture.scene.HasInstance(InstanceHandle::Invalid));
}

TEST(GpuSceneInstance, HasInstanceIsFalseForFabricatedHandle) {
  const CpuOnlyScene fixture;
  EXPECT_FALSE(fixture.scene.HasInstance(static_cast<InstanceHandle>(1u)));
}

// -----------------------------------------------------------------------------
// DestroyInstance + Update*/SetInstanceVisibility — §18.5 stale-handle
// policy: every void-returning verb silently no-ops on Invalid (no
// stat bump) and bumps `staleHandleDrops` on a recycled / out-of-range
// handle.
// -----------------------------------------------------------------------------
TEST(GpuSceneInstance, DestroyInstanceFlipsHasInstanceFalse) {
  CpuOnlyScene fixture;
  const MeshHandle mesh = fixture.MakeMesh();
  const Expected<InstanceHandle> created = fixture.scene.AppendInstance(fixture.DescForMesh(mesh));
  ASSERT_TRUE(created.has_value());

  fixture.scene.DestroyInstance(*created);
  EXPECT_FALSE(fixture.scene.HasInstance(*created));
}

TEST(GpuSceneInstance, DestroyOnInvalidIsSilentNoStatBump) {
  CpuOnlyScene fixture;
  fixture.scene.DestroyInstance(InstanceHandle::Invalid);
  EXPECT_EQ(fixture.scene.LastFrameStats().staleHandleDrops, 0u);
}

TEST(GpuSceneInstance, DestroyOnRecycledHandleIncrementsStaleHandleDrops) {
  CpuOnlyScene fixture;
  const MeshHandle mesh = fixture.MakeMesh();
  const Expected<InstanceHandle> created = fixture.scene.AppendInstance(fixture.DescForMesh(mesh));
  ASSERT_TRUE(created.has_value());

  fixture.scene.DestroyInstance(*created);
  EXPECT_EQ(fixture.scene.LastFrameStats().staleHandleDrops, 0u);

  fixture.scene.DestroyInstance(*created);
  EXPECT_EQ(fixture.scene.LastFrameStats().staleHandleDrops, 1u);
}

TEST(GpuSceneInstance, UpdateTransformOnRecycledIncrementsStaleHandleDrops) {
  CpuOnlyScene fixture;
  const MeshHandle mesh = fixture.MakeMesh();
  const Expected<InstanceHandle> created = fixture.scene.AppendInstance(fixture.DescForMesh(mesh));
  ASSERT_TRUE(created.has_value());

  fixture.scene.DestroyInstance(*created);
  fixture.scene.UpdateInstanceTransform(*created, hlslpp::float4x4{});
  EXPECT_EQ(fixture.scene.LastFrameStats().staleHandleDrops, 1u);
}

TEST(GpuSceneInstance, UpdateMaterialOnRecycledIncrementsStaleHandleDrops) {
  CpuOnlyScene fixture;
  const MeshHandle mesh = fixture.MakeMesh();
  const Expected<InstanceHandle> created = fixture.scene.AppendInstance(fixture.DescForMesh(mesh));
  ASSERT_TRUE(created.has_value());

  fixture.scene.DestroyInstance(*created);
  fixture.scene.UpdateInstanceMaterial(*created, MaterialHandle::Invalid);
  EXPECT_EQ(fixture.scene.LastFrameStats().staleHandleDrops, 1u);
}

TEST(GpuSceneInstance, SetVisibilityOnRecycledIncrementsStaleHandleDrops) {
  CpuOnlyScene fixture;
  const MeshHandle mesh = fixture.MakeMesh();
  const Expected<InstanceHandle> created = fixture.scene.AppendInstance(fixture.DescForMesh(mesh));
  ASSERT_TRUE(created.has_value());

  fixture.scene.DestroyInstance(*created);
  fixture.scene.SetInstanceVisibility(*created, false);
  EXPECT_EQ(fixture.scene.LastFrameStats().staleHandleDrops, 1u);
}

// -----------------------------------------------------------------------------
// Generation roll-over (§19.7): destroy + re-append must reuse the
// retired slot AND bump the stored generation by exactly one.
// -----------------------------------------------------------------------------
TEST(GpuSceneInstance, RecycledSlotBumpsGeneration) {
  CpuOnlyScene fixture;
  const MeshHandle mesh = fixture.MakeMesh();

  const Expected<InstanceHandle> first = fixture.scene.AppendInstance(fixture.DescForMesh(mesh));
  ASSERT_TRUE(first.has_value());
  const uint32_t originalSlot = SlotOf(*first);
  const uint8_t originalGen = GenerationOf(*first);

  fixture.scene.DestroyInstance(*first);

  const Expected<InstanceHandle> second = fixture.scene.AppendInstance(fixture.DescForMesh(mesh));
  ASSERT_TRUE(second.has_value());
  EXPECT_EQ(SlotOf(*second), originalSlot);
  EXPECT_EQ(GenerationOf(*second), originalGen + 1u);

  EXPECT_FALSE(fixture.scene.HasInstance(*first));
  EXPECT_TRUE(fixture.scene.HasInstance(*second));
}

// -----------------------------------------------------------------------------
// LastFrameStats — instanceCount reflects the live-handle count.
// -----------------------------------------------------------------------------
TEST(GpuSceneInstance, LastFrameStatsTracksLiveInstanceCount) {
  CpuOnlyScene fixture;
  const MeshHandle mesh = fixture.MakeMesh();

  EXPECT_EQ(fixture.scene.LastFrameStats().instanceCount, 0u);

  const Expected<InstanceHandle> first = fixture.scene.AppendInstance(fixture.DescForMesh(mesh));
  const Expected<InstanceHandle> second = fixture.scene.AppendInstance(fixture.DescForMesh(mesh));
  ASSERT_TRUE(first.has_value());
  ASSERT_TRUE(second.has_value());
  EXPECT_EQ(fixture.scene.LastFrameStats().instanceCount, 2u);

  fixture.scene.DestroyInstance(*first);
  EXPECT_EQ(fixture.scene.LastFrameStats().instanceCount, 1u);

  fixture.scene.DestroyInstance(*second);
  EXPECT_EQ(fixture.scene.LastFrameStats().instanceCount, 0u);
}

// -----------------------------------------------------------------------------
// §15 BLAS sharing rule, M6 P1: many TLAS instances of the same mesh
// share one BLAS slot (one MeshEntry, one `entry.blas` allocation).
// CommitResources can't run CPU-only (no device → createAccelStruct
// would crash), so the GPU-side blasCount stays 0; what we DO pin is
// the necessary upstream invariant: N AppendInstance calls against
// one MeshHandle yield meshCount=1, instanceCount=N, and (since BLAS
// is keyed on MeshHandle) blasCount can never exceed meshCount.
//
// Without this contract, a future refactor that accidentally
// allocates a BLAS per AppendInstance instead of per MeshEntry
// would blow GPU memory on Bistro-class scenes (10⁴ instances of the
// same prototype mesh would mean 10⁴ duplicate BLAS).
// -----------------------------------------------------------------------------
TEST(GpuSceneInstance, ManyInstancesSharePrototypeMeshSlot) {
  CpuOnlyScene fixture;
  const MeshHandle prototype = fixture.MakeMesh();

  constexpr int INSTANCE_COUNT = 100;
  for (int i = 0; i < INSTANCE_COUNT; ++i)
  {
    const Expected<InstanceHandle> instance =
        fixture.scene.AppendInstance(fixture.DescForMesh(prototype));
    ASSERT_TRUE(instance.has_value()) << "AppendInstance " << i << " failed";
  }

  const auto stats = fixture.scene.LastFrameStats();
  EXPECT_EQ(stats.meshCount, 1u)
      << "100 instances must reference the single prototype MeshHandle, "
         "not allocate one MeshEntry each";
  EXPECT_EQ(stats.instanceCount, static_cast<uint64_t>(INSTANCE_COUNT));
  // §15 BLAS sharing invariant: at most one BLAS per MeshEntry.
  EXPECT_LE(stats.blasCount, stats.meshCount)
      << "BLAS count must never exceed mesh count — §15 BLAS sharing rule";
}

// -----------------------------------------------------------------------------
// §15 BLAS sharing rule, M6 P1: a scene with K distinct prototype
// meshes + N instances of each yields meshCount=K, instanceCount=K*N,
// and blasCount<=K. Pins the "one BLAS per prototype" rule from the
// other direction (per-mesh allocation, not per-instance).
// -----------------------------------------------------------------------------
TEST(GpuSceneInstance, MultiplePrototypeMeshesEachOwnAtMostOneBlasSlot) {
  CpuOnlyScene fixture;
  // Distinct seeds so the §15 content-dedup misses and each
  // MakeMesh allocates a separate MeshHandle (the test wants three
  // *distinct* prototype meshes; same-content MakeMesh calls would
  // collapse to one slot).
  const MeshHandle prototypeA = fixture.MakeMesh(0.0f);
  const MeshHandle prototypeB = fixture.MakeMesh(2.0f);
  const MeshHandle prototypeC = fixture.MakeMesh(4.0f);

  constexpr int INSTANCES_PER_PROTOTYPE = 30;
  for (int i = 0; i < INSTANCES_PER_PROTOTYPE; ++i)
  {
    ASSERT_TRUE(fixture.scene.AppendInstance(fixture.DescForMesh(prototypeA)).has_value());
    ASSERT_TRUE(fixture.scene.AppendInstance(fixture.DescForMesh(prototypeB)).has_value());
    ASSERT_TRUE(fixture.scene.AppendInstance(fixture.DescForMesh(prototypeC)).has_value());
  }

  const auto stats = fixture.scene.LastFrameStats();
  EXPECT_EQ(stats.meshCount, 3u);
  EXPECT_EQ(stats.instanceCount, static_cast<uint64_t>(3 * INSTANCES_PER_PROTOTYPE));
  EXPECT_LE(stats.blasCount, stats.meshCount);
}
