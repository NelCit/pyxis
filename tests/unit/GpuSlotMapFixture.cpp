// Pyxis renderer — GpuSlotMap white-box tests (RFC 0009).
//
// GpuSlotMap is the Flecs-backed handle table every GpuScene entity type uses:
// gpuscene_detail handle encoding (slot 0 = Invalid sentinel, 24-bit slot + 8-bit
// generation, §19.7), LIFO free-slot reuse, and a slot->entity map. The handle
// lifecycle is also exercised end-to-end through the public GpuScene verb tests
// (GpuSceneMesh/Instance/Light/...); this fixture pins the primitive directly. §35
// allows the unit-test harness to peek into Private/.

#include "GpuScene/GpuSlotMap.h"

#include <Pyxis/Renderer/Forward.h>

#include <gtest/gtest.h>

#include <cstdint>
#include <flecs.h>
#include <vector>

using pyxis::GpuSlotMap;

namespace {

TEST(GpuSlotMap, FreshMapHasOnlyTheSlot0Sentinel) {
  flecs::world world;
  const GpuSlotMap map{world};
  EXPECT_EQ(map.SlotCount(), 1u);  // slot 0 reserved.
  EXPECT_EQ(map.LiveCount(), 0u);
  EXPECT_FALSE(map.IsLive(0));
}

TEST(GpuSlotMap, AllocateAssignsSequentialSlotsStartingAt1) {
  flecs::world world;
  GpuSlotMap map{world};
  flecs::entity entityOne;
  flecs::entity entityTwo;
  const uint32_t handleOne = map.Allocate(entityOne);
  const uint32_t handleTwo = map.Allocate(entityTwo);
  EXPECT_EQ(GpuSlotMap::SlotOf(handleOne), 1u);  // slot 0 is the sentinel.
  EXPECT_EQ(GpuSlotMap::SlotOf(handleTwo), 2u);
  EXPECT_NE(handleOne, 0u);  // handle 0 stays Invalid.
  EXPECT_NE(handleTwo, 0u);
  EXPECT_EQ(map.LiveCount(), 2u);
  EXPECT_TRUE(map.IsLive(1));
  EXPECT_TRUE(map.IsLive(2));
}

TEST(GpuSlotMap, ResolveReturnsTheAllocatedEntity) {
  flecs::world world;
  GpuSlotMap map{world};
  flecs::entity entity;
  const uint32_t handle = map.Allocate(entity);
  EXPECT_EQ(map.Resolve(handle).id(), entity.id());
  EXPECT_EQ(map.EntityAtSlot(GpuSlotMap::SlotOf(handle)).id(), entity.id());
}

TEST(GpuSlotMap, ResolveRejectsInvalidAndOutOfRangeHandles) {
  flecs::world world;
  GpuSlotMap map{world};
  flecs::entity entity;
  (void)map.Allocate(entity);
  EXPECT_EQ(map.Resolve(0u).id(), 0u);           // Invalid sentinel.
  EXPECT_EQ(map.Resolve(0x00FFFFFFu).id(), 0u);  // slot way out of range.
}

TEST(GpuSlotMap, FreeRetiresTheSlotAndStalesTheOldHandle) {
  flecs::world world;
  GpuSlotMap map{world};
  flecs::entity entity;
  const uint32_t handle = map.Allocate(entity);
  ASSERT_NE(map.Resolve(handle).id(), 0u);
  map.Free(handle);
  EXPECT_EQ(map.Resolve(handle).id(), 0u);  // old handle no longer resolves.
  EXPECT_EQ(map.LiveCount(), 0u);
  EXPECT_FALSE(map.IsLive(GpuSlotMap::SlotOf(handle)));
}

TEST(GpuSlotMap, FreeRecyclesTheSlotWithABumpedGeneration) {
  flecs::world world;
  GpuSlotMap map{world};
  flecs::entity firstEntity;
  const uint32_t firstHandle = map.Allocate(firstEntity);
  map.Free(firstHandle);
  flecs::entity secondEntity;
  const uint32_t secondHandle = map.Allocate(secondEntity);
  // LIFO reuse → same slot, but a bumped generation → a distinct handle, and the
  // stale firstHandle must not resolve to the recycled entity.
  EXPECT_EQ(GpuSlotMap::SlotOf(secondHandle), GpuSlotMap::SlotOf(firstHandle));
  EXPECT_EQ(GpuSlotMap::GenerationOf(secondHandle),
            GpuSlotMap::GenerationOf(firstHandle) + 1u);
  EXPECT_NE(firstHandle, secondHandle);
  EXPECT_EQ(map.Resolve(firstHandle).id(), 0u);  // stale generation.
  EXPECT_EQ(map.Resolve(secondHandle).id(), secondEntity.id());
}

TEST(GpuSlotMap, HandleForSlotReturnsTheLiveHandle) {
  flecs::world world;
  GpuSlotMap map{world};
  flecs::entity entity;
  const uint32_t handle = map.Allocate(entity);
  EXPECT_EQ(map.HandleForSlot(GpuSlotMap::SlotOf(handle)), handle);
  EXPECT_EQ(map.HandleForSlot(0u), 0u);  // sentinel.
}

TEST(GpuSlotMap, ForEachLiveSlotVisitsLiveSlotsInAscendingOrder) {
  flecs::world world;
  GpuSlotMap map{world};
  flecs::entity entityOne;
  flecs::entity entityTwo;
  flecs::entity entityThree;
  const uint32_t handleOne = map.Allocate(entityOne);    // slot 1
  const uint32_t handleTwo = map.Allocate(entityTwo);    // slot 2
  const uint32_t handleThree = map.Allocate(entityThree);  // slot 3
  map.Free(handleTwo);  // free slot 2 → 1 and 3 stay live.
  std::vector<uint32_t> visited;
  map.ForEachLiveSlot([&](uint32_t slot, flecs::entity) { visited.push_back(slot); });
  ASSERT_EQ(visited.size(), 2u);
  EXPECT_EQ(visited[0], 1u);
  EXPECT_EQ(visited[1], 3u);
  EXPECT_EQ(GpuSlotMap::SlotOf(handleOne), 1u);
  EXPECT_EQ(GpuSlotMap::SlotOf(handleThree), 3u);
}

TEST(GpuSlotMap, ResetReturnsToSentinelOnlyState) {
  flecs::world world;
  GpuSlotMap map{world};
  flecs::entity firstEntity;
  flecs::entity secondEntity;
  (void)map.Allocate(firstEntity);
  (void)map.Allocate(secondEntity);
  map.Reset();
  EXPECT_EQ(map.SlotCount(), 1u);
  EXPECT_EQ(map.LiveCount(), 0u);
  // A fresh allocation after Reset lands back at slot 1.
  flecs::entity reusedEntity;
  EXPECT_EQ(GpuSlotMap::SlotOf(map.Allocate(reusedEntity)), 1u);
}

TEST(GpuSlotMap, EncodeDecodeRoundTrips) {
  EXPECT_EQ(GpuSlotMap::SlotOf(GpuSlotMap::Encode(1234u, 7u)), 1234u);
  EXPECT_EQ(GpuSlotMap::GenerationOf(GpuSlotMap::Encode(1234u, 7u)), 7u);
  // Slot 0 + generation 0 encodes to 0 (the Invalid sentinel).
  EXPECT_EQ(GpuSlotMap::Encode(0u, 0u), 0u);
}

}  // namespace
