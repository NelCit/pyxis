// Pyxis renderer — GpuScene mesh handle table tests.
//
// Plan §18.5 + §19.7. Exercises CreateMesh / HasMesh / DestroyMesh
// against the public surface — no GPU device required because the
// M3 mesh-table impl is pure CPU bookkeeping. Once the next M3
// commit lands the GPU upload path, these tests stay green; new
// tests cover the upload branch separately.
//
// Rationale for testing through the public API (rather than peeking
// into Private/GpuScene/): the byte-stable contract is what
// pyxis_app + future Hydra / USD-direct adapters consume. Pin the
// observable behaviour, not the impl detail.

#include <Pyxis/Renderer/Descs/GpuSceneCreateDesc.h>
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
using pyxis::MeshDesc;
using pyxis::MeshHandle;
using pyxis::Profiler;

namespace {

// Single-triangle fixture used by most tests. Three vertices, one
// triangle, no normals/tangents/uv0 (optional spans empty). The
// `seed` constructor argument nudges vertex 0's X coord so two
// fixtures with different seeds hash to different §15 dedup buckets
// — needed any time a test wants to allocate two DISTINCT mesh
// slots in the same scene (since CreateMesh content-dedups
// byte-identical descs).
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

// Minimal CPU-only profiler — Profiler{nullptr} is the documented
// test mode (Profiler.h §18.7 doc-comment).
struct CpuOnlyScene {
  Profiler profiler{nullptr};
  GpuScene scene{nullptr, profiler, GpuSceneCreateDesc{}};
};

// Pull slot + generation out of an encoded handle the same way the
// renderer does (§19.7). Used to verify generation roll-over.
constexpr uint32_t SlotOf(MeshHandle handle) noexcept {
  return static_cast<uint32_t>(handle) & HANDLE_SLOT_MASK;
}
constexpr uint8_t GenerationOf(MeshHandle handle) noexcept {
  return static_cast<uint8_t>(static_cast<uint32_t>(handle) >> HANDLE_SLOT_BITS);
}

}  // namespace

// -----------------------------------------------------------------------------
// CreateMesh — happy path.
// -----------------------------------------------------------------------------
TEST(GpuSceneMesh, CreateMeshReturnsLiveHandle) {
  CpuOnlyScene fixture;
  const TriangleFixture triangle;

  const Expected<MeshHandle> result = fixture.scene.CreateMesh(triangle.Desc());
  ASSERT_TRUE(result.has_value()) << result.error().message.View();
  EXPECT_NE(*result, MeshHandle::Invalid);
  EXPECT_TRUE(fixture.scene.HasMesh(*result));
}

TEST(GpuSceneMesh, CreateMeshAllocatesDistinctSlotsForDistinctContent) {
  CpuOnlyScene fixture;
  // Two fixtures with different seeds → byte-distinct MeshDescs →
  // §15 content-dedup misses → fresh slot allocation each time.
  const TriangleFixture triangleA{0.0f};
  const TriangleFixture triangleB{2.0f};

  const Expected<MeshHandle> first = fixture.scene.CreateMesh(triangleA.Desc());
  const Expected<MeshHandle> second = fixture.scene.CreateMesh(triangleB.Desc());
  ASSERT_TRUE(first.has_value());
  ASSERT_TRUE(second.has_value());
  EXPECT_NE(*first, *second);
  EXPECT_NE(SlotOf(*first), SlotOf(*second));
  EXPECT_TRUE(fixture.scene.HasMesh(*first));
  EXPECT_TRUE(fixture.scene.HasMesh(*second));
}

// -----------------------------------------------------------------------------
// §15 content-dedup: two CreateMesh calls with byte-identical
// MeshDescs return the SAME MeshHandle (and therefore share one
// BLAS once CommitResources runs the build). Pins the audit-fix
// behaviour where three identical sphere prims in default.usd
// previously allocated three handles + three BLAS even though they
// were geometrically indistinguishable.
// -----------------------------------------------------------------------------
TEST(GpuSceneMesh, CreateMeshDeduplicatesByContent) {
  CpuOnlyScene fixture;
  const TriangleFixture triangle;

  const Expected<MeshHandle> first = fixture.scene.CreateMesh(triangle.Desc());
  const Expected<MeshHandle> second = fixture.scene.CreateMesh(triangle.Desc());
  ASSERT_TRUE(first.has_value());
  ASSERT_TRUE(second.has_value());
  EXPECT_EQ(*first, *second)
      << "Identical MeshDescs must dedup to one MeshHandle (§15 BLAS sharing).";
  EXPECT_EQ(fixture.scene.LastFrameStats().meshCount, 1u)
      << "meshCount should reflect the dedup'd live count, not the call count.";
}

// After DestroyMesh, the dedup map must drop its stale entry so a
// future CreateMesh of the same content allocates fresh — verified
// by checking the recycled-slot generation bumped (which would not
// happen if dedup mistakenly handed back the destroyed handle).
TEST(GpuSceneMesh, DestroyMeshClearsContentDedupEntry) {
  CpuOnlyScene fixture;
  const TriangleFixture triangle;

  const Expected<MeshHandle> first = fixture.scene.CreateMesh(triangle.Desc());
  ASSERT_TRUE(first.has_value());
  const uint8_t firstGen = GenerationOf(*first);

  fixture.scene.DestroyMesh(*first);

  const Expected<MeshHandle> second = fixture.scene.CreateMesh(triangle.Desc());
  ASSERT_TRUE(second.has_value());
  EXPECT_EQ(SlotOf(*second), SlotOf(*first))
      << "Slot must be recycled (dedup map should not return the dead entry).";
  EXPECT_EQ(GenerationOf(*second), static_cast<uint8_t>(firstGen + 1u))
      << "Generation must bump exactly once (proves a fresh allocation, not a dedup hit).";
  EXPECT_FALSE(fixture.scene.HasMesh(*first));
  EXPECT_TRUE(fixture.scene.HasMesh(*second));
}

// -----------------------------------------------------------------------------
// CreateMesh — input validation. The plan §18.5 contract is that
// CreateMesh returns an Expected with a precise InvalidArgument
// diagnostic; the wrong kind here would propagate confusion through
// every ingest path.
// -----------------------------------------------------------------------------
TEST(GpuSceneMesh, CreateMeshRejectsEmptyPositions) {
  CpuOnlyScene fixture;
  MeshDesc desc;
  const std::array<uint32_t, 3> indices{0, 1, 2};
  desc.indices = indices;

  const Expected<MeshHandle> result = fixture.scene.CreateMesh(desc);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().kind, ErrorKind::InvalidArgument);
}

TEST(GpuSceneMesh, CreateMeshRejectsEmptyIndices) {
  CpuOnlyScene fixture;
  MeshDesc desc;
  const std::array<hlslpp::float3, 1> positions{hlslpp::float3{0.0f, 0.0f, 0.0f}};
  desc.positions = positions;

  const Expected<MeshHandle> result = fixture.scene.CreateMesh(desc);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().kind, ErrorKind::InvalidArgument);
}

TEST(GpuSceneMesh, CreateMeshRejectsIndexCountNotMultipleOfThree) {
  CpuOnlyScene fixture;
  const TriangleFixture triangle;
  MeshDesc desc = triangle.Desc();
  const std::array<uint32_t, 4> truncated{0, 1, 2, 0};  // 4 indices = bad
  desc.indices = truncated;

  const Expected<MeshHandle> result = fixture.scene.CreateMesh(desc);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().kind, ErrorKind::InvalidArgument);
}

TEST(GpuSceneMesh, CreateMeshRejectsOutOfRangeIndex) {
  CpuOnlyScene fixture;
  MeshDesc desc;
  const std::array<hlslpp::float3, 3> positions{
      hlslpp::float3{0.0f, 0.0f, 0.0f},
      hlslpp::float3{1.0f, 0.0f, 0.0f},
      hlslpp::float3{0.0f, 1.0f, 0.0f},
  };
  const std::array<uint32_t, 3> indices{0, 1, 99};  // index 99 out of range
  desc.positions = positions;
  desc.indices = indices;

  const Expected<MeshHandle> result = fixture.scene.CreateMesh(desc);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().kind, ErrorKind::InvalidArgument);
}

TEST(GpuSceneMesh, CreateMeshRejectsMismatchedNormalsLength) {
  CpuOnlyScene fixture;
  const TriangleFixture triangle;
  MeshDesc desc = triangle.Desc();
  const std::array<hlslpp::float3, 2> normalsTooShort{
      hlslpp::float3{0.0f, 0.0f, 1.0f},
      hlslpp::float3{0.0f, 0.0f, 1.0f},
  };
  desc.normals = normalsTooShort;

  const Expected<MeshHandle> result = fixture.scene.CreateMesh(desc);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().kind, ErrorKind::InvalidArgument);
}

// -----------------------------------------------------------------------------
// HasMesh — spec checks.
// -----------------------------------------------------------------------------
TEST(GpuSceneMesh, HasMeshIsFalseForInvalid) {
  const CpuOnlyScene fixture;
  EXPECT_FALSE(fixture.scene.HasMesh(MeshHandle::Invalid));
}

TEST(GpuSceneMesh, HasMeshIsFalseForFabricatedHandle) {
  const CpuOnlyScene fixture;
  // Slot 1, generation 0 — looks plausible but never allocated.
  EXPECT_FALSE(fixture.scene.HasMesh(static_cast<MeshHandle>(1u)));
}

// -----------------------------------------------------------------------------
// DestroyMesh — round-trip + stale-handle policy (§18.5).
// -----------------------------------------------------------------------------
TEST(GpuSceneMesh, DestroyMeshFlipsHasMeshFalse) {
  CpuOnlyScene fixture;
  const TriangleFixture triangle;
  const Expected<MeshHandle> created = fixture.scene.CreateMesh(triangle.Desc());
  ASSERT_TRUE(created.has_value());

  fixture.scene.DestroyMesh(*created);
  EXPECT_FALSE(fixture.scene.HasMesh(*created));
}

TEST(GpuSceneMesh, DestroyOnInvalidIsSilentNoStatBump) {
  CpuOnlyScene fixture;
  fixture.scene.DestroyMesh(MeshHandle::Invalid);
  EXPECT_EQ(fixture.scene.LastFrameStats().staleHandleDrops, 0u);
}

TEST(GpuSceneMesh, DestroyOnRecycledHandleIncrementsStaleHandleDrops) {
  CpuOnlyScene fixture;
  const TriangleFixture triangle;
  const Expected<MeshHandle> created = fixture.scene.CreateMesh(triangle.Desc());
  ASSERT_TRUE(created.has_value());

  fixture.scene.DestroyMesh(*created);
  EXPECT_EQ(fixture.scene.LastFrameStats().staleHandleDrops, 0u);

  // Second destroy of the same handle: the slot's generation has
  // already advanced, so the handle is stale and the §18.5
  // staleHandleDrops counter should bump.
  fixture.scene.DestroyMesh(*created);
  EXPECT_EQ(fixture.scene.LastFrameStats().staleHandleDrops, 1u);
}

// -----------------------------------------------------------------------------
// Generation roll-over (§19.7 packing). After Destroy + Create reusing
// the same slot, the new handle must encode a bumped generation so
// the original handle no longer round-trips.
// -----------------------------------------------------------------------------
TEST(GpuSceneMesh, RecycledSlotBumpsGeneration) {
  CpuOnlyScene fixture;
  const TriangleFixture triangle;

  const Expected<MeshHandle> first = fixture.scene.CreateMesh(triangle.Desc());
  ASSERT_TRUE(first.has_value());
  const uint32_t originalSlot = SlotOf(*first);
  const uint8_t originalGen = GenerationOf(*first);

  fixture.scene.DestroyMesh(*first);

  const Expected<MeshHandle> second = fixture.scene.CreateMesh(triangle.Desc());
  ASSERT_TRUE(second.has_value());
  EXPECT_EQ(SlotOf(*second), originalSlot)
      << "Expected the retired slot to be reused on the next Create.";
  EXPECT_EQ(GenerationOf(*second), originalGen + 1u)
      << "Expected the recycled slot's generation to bump by exactly one.";

  EXPECT_FALSE(fixture.scene.HasMesh(*first))
      << "Original handle must be stale after its slot is reused.";
  EXPECT_TRUE(fixture.scene.HasMesh(*second));
}

// -----------------------------------------------------------------------------
// LastFrameStats — meshCount reflects the live-handle count.
// -----------------------------------------------------------------------------
TEST(GpuSceneMesh, LastFrameStatsTracksLiveMeshCount) {
  CpuOnlyScene fixture;
  // Distinct seeds → distinct content → §15 content-dedup misses →
  // each CreateMesh allocates a fresh slot, meshCount counts both.
  const TriangleFixture triangleA{0.0f};
  const TriangleFixture triangleB{2.0f};

  EXPECT_EQ(fixture.scene.LastFrameStats().meshCount, 0u);

  const Expected<MeshHandle> first = fixture.scene.CreateMesh(triangleA.Desc());
  const Expected<MeshHandle> second = fixture.scene.CreateMesh(triangleB.Desc());
  ASSERT_TRUE(first.has_value());
  ASSERT_TRUE(second.has_value());
  EXPECT_EQ(fixture.scene.LastFrameStats().meshCount, 2u);

  fixture.scene.DestroyMesh(*first);
  EXPECT_EQ(fixture.scene.LastFrameStats().meshCount, 1u);

  fixture.scene.DestroyMesh(*second);
  EXPECT_EQ(fixture.scene.LastFrameStats().meshCount, 0u);
}
