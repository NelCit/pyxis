// Pyxis renderer — GpuScene volume handle table tests. V2.A.5.
//
// Plan §18.5 + §19.7. AddVolume / HasVolume / RemoveVolume against
// the public surface, CPU-only fixture (no NVRHI device — the GPU
// upload path skips when the scene was constructed with a null
// device, so AddVolume's CPU bookkeeping runs without firing the
// 3D-texture create that a real device would do).

#include <Pyxis/Renderer/Descs/FrameStats.h>
#include <Pyxis/Renderer/Descs/GpuSceneCreateDesc.h>
#include <Pyxis/Renderer/Descs/VolumeDesc.h>
#include <Pyxis/Renderer/Forward.h>
#include <Pyxis/Renderer/GpuScene.h>
#include <Pyxis/Renderer/Profiler.h>

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

using pyxis::FrameStats;
using pyxis::GpuScene;
using pyxis::GpuSceneCreateDesc;
using pyxis::HANDLE_SLOT_BITS;
using pyxis::HANDLE_SLOT_MASK;
using pyxis::Profiler;
using pyxis::VolumeDesc;
using pyxis::VolumeHandle;

namespace {

struct CpuOnlyScene {
  Profiler profiler{nullptr};
  GpuScene scene{nullptr, profiler, GpuSceneCreateDesc{}};
};

VolumeDesc TinyVolume(std::vector<float>& backing) {
  // 2x3x4 = 24 voxels, ramp values for distinguishability.
  backing.assign(24, 0.0f);
  for (size_t idx = 0; idx < backing.size(); ++idx)
    backing[idx] = static_cast<float>(idx) * 0.5f;
  VolumeDesc desc;
  desc.voxels       = std::span<const float>{backing};
  desc.dimensions   = {2, 3, 4};
  desc.bboxMin      = {0.0f, 0.0f, 0.0f};
  desc.bboxMax      = {1.0f, 2.0f, 3.0f};
  desc.indexToWorld = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  desc.debugName    = "test/density";
  return desc;
}

constexpr uint32_t SlotOf(VolumeHandle handle) noexcept {
  return static_cast<uint32_t>(handle) & HANDLE_SLOT_MASK;
}

}  // namespace

TEST(GpuSceneVolume, AddVolumeReturnsLiveHandle) {
  CpuOnlyScene fixture;
  std::vector<float> backing;
  const VolumeHandle handle = fixture.scene.AddVolume(TinyVolume(backing));
  EXPECT_NE(handle, VolumeHandle::Invalid);
  EXPECT_TRUE(fixture.scene.HasVolume(handle));

  const FrameStats stats = fixture.scene.LastFrameStats();
  EXPECT_EQ(stats.volumeCount, 1u);
  EXPECT_EQ(stats.volumeBytes, 24u * sizeof(float));
}

TEST(GpuSceneVolume, AddVolumeRejectsZeroDim) {
  CpuOnlyScene fixture;
  std::vector<float> backing;
  VolumeDesc desc = TinyVolume(backing);
  desc.dimensions = {0, 1, 1};
  const VolumeHandle handle = fixture.scene.AddVolume(desc);
  EXPECT_EQ(handle, VolumeHandle::Invalid);
}

TEST(GpuSceneVolume, AddVolumeRejectsVoxelMismatch) {
  CpuOnlyScene fixture;
  std::vector<float> backing(10, 0.0f);  // dim says 24, give 10
  VolumeDesc desc;
  desc.voxels       = std::span<const float>{backing};
  desc.dimensions   = {2, 3, 4};
  desc.indexToWorld = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  const VolumeHandle handle = fixture.scene.AddVolume(desc);
  EXPECT_EQ(handle, VolumeHandle::Invalid);
}

TEST(GpuSceneVolume, RemoveVolumeFreesSlot) {
  CpuOnlyScene fixture;
  std::vector<float> backing;
  const VolumeHandle handle = fixture.scene.AddVolume(TinyVolume(backing));
  ASSERT_NE(handle, VolumeHandle::Invalid);
  EXPECT_TRUE(fixture.scene.HasVolume(handle));

  fixture.scene.RemoveVolume(handle);
  EXPECT_FALSE(fixture.scene.HasVolume(handle));
  EXPECT_EQ(fixture.scene.LastFrameStats().volumeCount, 0u);

  // The freed slot should be reused — handle generation bumps so the
  // returned handle is not bit-equal to the destroyed one.
  std::vector<float> backing2;
  const VolumeHandle handle2 = fixture.scene.AddVolume(TinyVolume(backing2));
  ASSERT_NE(handle2, VolumeHandle::Invalid);
  EXPECT_EQ(SlotOf(handle2), SlotOf(handle));
  EXPECT_NE(handle2, handle);
}

TEST(GpuSceneVolume, HasVolumeRejectsInvalid) {
  const CpuOnlyScene fixture;
  EXPECT_FALSE(fixture.scene.HasVolume(VolumeHandle::Invalid));
}

TEST(GpuSceneVolume, HasVolumeRejectsRecycledHandle) {
  CpuOnlyScene fixture;
  std::vector<float> backing;
  const VolumeHandle handle = fixture.scene.AddVolume(TinyVolume(backing));
  ASSERT_NE(handle, VolumeHandle::Invalid);
  fixture.scene.RemoveVolume(handle);
  // The original handle's generation no longer matches the slot's
  // current generation; HasVolume returns false.
  EXPECT_FALSE(fixture.scene.HasVolume(handle));
}
