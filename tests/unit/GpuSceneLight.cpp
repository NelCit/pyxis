// Pyxis renderer — GpuScene light handle table tests.
//
// Plan §18.5 + §19.7. AddLight / UpdateLight / RemoveLight against
// the public surface, CPU-only fixture. There is no `HasLight`
// probe (the public §18 surface intentionally omits it — light
// queries flow through LastFrameStats / the shader-side light list),
// so handle liveness is observed via the §18.5 `staleHandleDrops`
// counter and the `lightCount` stat.

#include <Pyxis/Renderer/Descs/GpuSceneCreateDesc.h>
#include <Pyxis/Renderer/Descs/LightDesc.h>
#include <Pyxis/Renderer/Forward.h>
#include <Pyxis/Renderer/GpuScene.h>
#include <Pyxis/Renderer/Profiler.h>

#include <gtest/gtest.h>

#include <cstdint>
#include <hlsl++.h>

using pyxis::GpuScene;
using pyxis::GpuSceneCreateDesc;
using pyxis::HANDLE_SLOT_BITS;
using pyxis::HANDLE_SLOT_MASK;
using pyxis::LightDesc;
using pyxis::LightHandle;
using pyxis::Profiler;
using pyxis::TextureHandle;

namespace {

struct CpuOnlyScene {
  Profiler profiler{nullptr};
  GpuScene scene{nullptr, profiler, GpuSceneCreateDesc{}};
};

LightDesc DefaultDistantLight() noexcept {
  LightDesc desc;
  desc.kind = LightDesc::Kind::Distant;
  desc.color = hlslpp::float3{1.0f, 0.95f, 0.9f};
  desc.intensity = 5.0f;
  desc.direction = hlslpp::float3{0.3f, -1.0f, 0.2f};
  return desc;
}

constexpr uint32_t SlotOf(LightHandle handle) noexcept {
  return static_cast<uint32_t>(handle) & HANDLE_SLOT_MASK;
}
constexpr uint8_t GenerationOf(LightHandle handle) noexcept {
  return static_cast<uint8_t>(static_cast<uint32_t>(handle) >> HANDLE_SLOT_BITS);
}

}  // namespace

// -----------------------------------------------------------------------------
// AddLight — happy path. Returns a non-Invalid handle on success.
// -----------------------------------------------------------------------------
TEST(GpuSceneLight, AddLightReturnsLiveHandle) {
  CpuOnlyScene fixture;
  const LightHandle handle = fixture.scene.AddLight(DefaultDistantLight());
  EXPECT_NE(handle, LightHandle::Invalid);
}

TEST(GpuSceneLight, AddLightAllocatesDistinctSlotsForSequentialCreates) {
  CpuOnlyScene fixture;
  const LightHandle first = fixture.scene.AddLight(DefaultDistantLight());
  const LightHandle second = fixture.scene.AddLight(DefaultDistantLight());
  EXPECT_NE(first, second);
  EXPECT_NE(SlotOf(first), SlotOf(second));
}

// -----------------------------------------------------------------------------
// RemoveLight + UpdateLight — §18.5 stale-handle policy. Same shape as
// the instance / mesh tests: Invalid is a silent no-op (no stat bump);
// recycled / out-of-range handles bump `staleHandleDrops`.
// -----------------------------------------------------------------------------
TEST(GpuSceneLight, RemoveOnInvalidIsSilentNoStatBump) {
  CpuOnlyScene fixture;
  fixture.scene.RemoveLight(LightHandle::Invalid);
  EXPECT_EQ(fixture.scene.LastFrameStats().staleHandleDrops, 0u);
}

TEST(GpuSceneLight, UpdateOnInvalidIsSilentNoStatBump) {
  CpuOnlyScene fixture;
  fixture.scene.UpdateLight(LightHandle::Invalid, DefaultDistantLight());
  EXPECT_EQ(fixture.scene.LastFrameStats().staleHandleDrops, 0u);
}

TEST(GpuSceneLight, RemoveOnRecycledHandleIncrementsStaleHandleDrops) {
  CpuOnlyScene fixture;
  const LightHandle handle = fixture.scene.AddLight(DefaultDistantLight());
  ASSERT_NE(handle, LightHandle::Invalid);

  fixture.scene.RemoveLight(handle);
  EXPECT_EQ(fixture.scene.LastFrameStats().staleHandleDrops, 0u);

  fixture.scene.RemoveLight(handle);
  EXPECT_EQ(fixture.scene.LastFrameStats().staleHandleDrops, 1u);
}

TEST(GpuSceneLight, UpdateOnRecycledHandleIncrementsStaleHandleDrops) {
  CpuOnlyScene fixture;
  const LightHandle handle = fixture.scene.AddLight(DefaultDistantLight());
  ASSERT_NE(handle, LightHandle::Invalid);

  fixture.scene.RemoveLight(handle);
  fixture.scene.UpdateLight(handle, DefaultDistantLight());
  EXPECT_EQ(fixture.scene.LastFrameStats().staleHandleDrops, 1u);
}

// -----------------------------------------------------------------------------
// Generation roll-over (§19.7) — same contract as Mesh / Instance.
// -----------------------------------------------------------------------------
TEST(GpuSceneLight, RecycledSlotBumpsGeneration) {
  CpuOnlyScene fixture;

  const LightHandle first = fixture.scene.AddLight(DefaultDistantLight());
  ASSERT_NE(first, LightHandle::Invalid);
  const uint32_t originalSlot = SlotOf(first);
  const uint8_t originalGen = GenerationOf(first);

  fixture.scene.RemoveLight(first);

  const LightHandle second = fixture.scene.AddLight(DefaultDistantLight());
  ASSERT_NE(second, LightHandle::Invalid);
  EXPECT_EQ(SlotOf(second), originalSlot);
  EXPECT_EQ(GenerationOf(second), originalGen + 1u);
}

// -----------------------------------------------------------------------------
// LastFrameStats — lightCount tracks the live-handle count.
// -----------------------------------------------------------------------------
TEST(GpuSceneLight, LastFrameStatsTracksLiveLightCount) {
  CpuOnlyScene fixture;

  EXPECT_EQ(fixture.scene.LastFrameStats().lightCount, 0u);

  const LightHandle first = fixture.scene.AddLight(DefaultDistantLight());
  const LightHandle second = fixture.scene.AddLight(DefaultDistantLight());
  EXPECT_EQ(fixture.scene.LastFrameStats().lightCount, 2u);

  fixture.scene.RemoveLight(first);
  EXPECT_EQ(fixture.scene.LastFrameStats().lightCount, 1u);

  fixture.scene.RemoveLight(second);
  EXPECT_EQ(fixture.scene.LastFrameStats().lightCount, 0u);
}

// -----------------------------------------------------------------------------
// M7 audit closeout — verify the three LightDesc::Kind variants
// (Distant / Dome / Rect) all round-trip through AddLight and bump
// `lightCount`. Pre-M7 only Distant was tested; the M7 EmitLight path
// emits Dome (color + envMap) and Rect (position + axes) as well. A
// regression that drops one kind silently would leave its handle
// returned Invalid; this test catches that.
// -----------------------------------------------------------------------------
TEST(GpuSceneLight, AddLightDomeKindReturnsLiveHandle) {
  CpuOnlyScene fixture;
  LightDesc desc;
  desc.kind = LightDesc::Kind::Dome;
  desc.color = hlslpp::float3{0.95f, 0.10f, 0.65f};  // recognisable magenta
  desc.intensity = 1.0f;
  desc.envMap = TextureHandle::Invalid;  // color-only dome (M7 audit case)
  const LightHandle handle = fixture.scene.AddLight(desc);
  EXPECT_NE(handle, LightHandle::Invalid);
  EXPECT_EQ(fixture.scene.LastFrameStats().lightCount, 1u);
}

TEST(GpuSceneLight, AddLightRectKindReturnsLiveHandle) {
  CpuOnlyScene fixture;
  LightDesc desc;
  desc.kind = LightDesc::Kind::Rect;
  desc.color = hlslpp::float3{1.0f, 1.0f, 1.0f};
  desc.intensity = 2.0f;
  desc.position = hlslpp::float3{0.0f, 2.0f, 0.0f};
  desc.axisU = hlslpp::float3{1.0f, 0.0f, 0.0f};
  desc.axisV = hlslpp::float3{0.0f, 0.0f, 1.0f};
  desc.doubleSided = true;
  const LightHandle handle = fixture.scene.AddLight(desc);
  EXPECT_NE(handle, LightHandle::Invalid);
  EXPECT_EQ(fixture.scene.LastFrameStats().lightCount, 1u);
}

// All three kinds in one scene — `lightCount` reaches 3 + each handle
// is distinct. Mirrors the multi-kind layout that
// `m7_lit_scene.usd` + the bundled `default.usd` author at runtime.
TEST(GpuSceneLight, AddLightAllThreeKindsCoexistInTable) {
  CpuOnlyScene fixture;

  LightDesc distant;
  distant.kind = LightDesc::Kind::Distant;
  const LightHandle distantHandle = fixture.scene.AddLight(distant);

  LightDesc dome;
  dome.kind = LightDesc::Kind::Dome;
  const LightHandle domeHandle = fixture.scene.AddLight(dome);

  LightDesc rect;
  rect.kind = LightDesc::Kind::Rect;
  const LightHandle rectHandle = fixture.scene.AddLight(rect);

  EXPECT_NE(distantHandle, LightHandle::Invalid);
  EXPECT_NE(domeHandle,    LightHandle::Invalid);
  EXPECT_NE(rectHandle,    LightHandle::Invalid);
  EXPECT_NE(distantHandle, domeHandle);
  EXPECT_NE(domeHandle,    rectHandle);
  EXPECT_NE(distantHandle, rectHandle);
  EXPECT_EQ(fixture.scene.LastFrameStats().lightCount, 3u);
}
