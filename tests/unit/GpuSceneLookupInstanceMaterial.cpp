// Pyxis renderer — GpuScene::LookupInstanceMaterialBySlot tests.
//
// Plan §15 / §18.5. The picker AOV writes the §15 24-bit instance
// slot only (no generation), so the viewer's click-to-select path
// asks the scene to resolve that bare slot to the bound MaterialHandle.
// Tests pin:
//   * Slot 0 (the permanent quarantine sentinel) -> Invalid.
//   * Out-of-range slot -> Invalid.
//   * Live slot -> the material the AppendInstance recorded.
//   * Destroyed-instance slot -> Invalid (the recycled-but-not-yet-
//     reused case the picker sees one frame after a click on a
//     just-destroyed entity).
//   * UpdateInstanceMaterial flows through into the lookup result.
//
// CPU-only fixture (no GPU device): the lookup is pure CPU bookkeeping.

#include <Pyxis/Renderer/Descs/GpuSceneCreateDesc.h>
#include <Pyxis/Renderer/Descs/InstanceDesc.h>
#include <Pyxis/Renderer/Descs/MeshDesc.h>
#include <Pyxis/Renderer/Descs/OpenPBRMaterialDesc.h>
#include <Pyxis/Renderer/Forward.h>
#include <Pyxis/Renderer/GpuScene.h>
#include <Pyxis/Renderer/Profiler.h>

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <hlsl++.h>

using pyxis::Expected;
using pyxis::GpuScene;
using pyxis::GpuSceneCreateDesc;
using pyxis::HANDLE_SLOT_MASK;
using pyxis::InstanceDesc;
using pyxis::InstanceHandle;
using pyxis::MaterialHandle;
using pyxis::MeshDesc;
using pyxis::MeshHandle;
using pyxis::OpenPBRMaterialDesc;
using pyxis::Profiler;

namespace {

struct CpuOnlyScene {
  Profiler profiler{nullptr};
  GpuScene scene{nullptr, profiler, GpuSceneCreateDesc{}};

  [[nodiscard]] MeshHandle MakeMesh(float seed = 0.0f) {
    const std::array<hlslpp::float3, 3> positions{
        hlslpp::float3{seed, 0.0f, 0.0f},
        hlslpp::float3{1.0f, 0.0f, 0.0f},
        hlslpp::float3{0.0f, 1.0f, 0.0f},
    };
    const std::array<uint32_t, 3> indices{0, 1, 2};
    MeshDesc desc;
    desc.positions = positions;
    desc.indices = indices;
    desc.debugName = "lookup-test.mesh";
    auto created = scene.CreateMesh(desc);
    EXPECT_TRUE(created.has_value());
    return *created;
  }

  [[nodiscard]] MaterialHandle MakeMaterial(float redChannel) {
    OpenPBRMaterialDesc desc;
    desc.baseColor = hlslpp::float3{redChannel, 0.5f, 0.5f};
    desc.sourcePrim = "/Mat/lookup-test";
    return scene.AcquireMaterial(desc);
  }

  [[nodiscard]] InstanceDesc DescForMeshAndMaterial(MeshHandle mesh,
                                                    MaterialHandle material) const noexcept {
    InstanceDesc desc;
    desc.mesh = mesh;
    desc.material = material;
    desc.worldFromLocal = hlslpp::float4x4(
        hlslpp::float4{1, 0, 0, 0}, hlslpp::float4{0, 1, 0, 0},
        hlslpp::float4{0, 0, 1, 0}, hlslpp::float4{0, 0, 0, 1});
    desc.debugName = "lookup-test.instance";
    return desc;
  }
};

constexpr uint32_t SlotOf(InstanceHandle handle) noexcept {
  return static_cast<uint32_t>(handle) & HANDLE_SLOT_MASK;
}

}  // namespace

// -----------------------------------------------------------------------------
// Slot 0 is the permanent §15 sentinel. The picker writes 0 when no
// hit landed on that pixel (background) — the lookup must return
// Invalid so the click-to-select path silently drops it.
// -----------------------------------------------------------------------------
TEST(GpuSceneLookupInstanceMaterial, SlotZeroAlwaysReturnsInvalid) {
  const CpuOnlyScene fixture;
  EXPECT_EQ(fixture.scene.LookupInstanceMaterialBySlot(0u), MaterialHandle::Invalid);
}

// -----------------------------------------------------------------------------
// Out-of-range slot (past every live entry) -> Invalid. Guards against
// a malicious or buggy picker writing a fabricated slot index that the
// scene shouldn't accidentally treat as live.
// -----------------------------------------------------------------------------
TEST(GpuSceneLookupInstanceMaterial, OutOfRangeSlotReturnsInvalid) {
  const CpuOnlyScene fixture;
  // No instances appended -> the table is sentinel-only (size 1).
  // Any positive slot is out of range.
  EXPECT_EQ(fixture.scene.LookupInstanceMaterialBySlot(1u), MaterialHandle::Invalid);
  EXPECT_EQ(fixture.scene.LookupInstanceMaterialBySlot(1000u), MaterialHandle::Invalid);
  EXPECT_EQ(fixture.scene.LookupInstanceMaterialBySlot(0xFFFFFFu),
            MaterialHandle::Invalid);
}

// -----------------------------------------------------------------------------
// Live instance with a bound material -> lookup returns that material.
// This is the load-bearing happy path the click-to-select feature
// relies on.
// -----------------------------------------------------------------------------
TEST(GpuSceneLookupInstanceMaterial, LiveInstanceReturnsBoundMaterial) {
  CpuOnlyScene fixture;
  const MeshHandle mesh = fixture.MakeMesh();
  const MaterialHandle material = fixture.MakeMaterial(0.7f);
  ASSERT_NE(material, MaterialHandle::Invalid);

  const Expected<InstanceHandle> instance =
      fixture.scene.AppendInstance(fixture.DescForMeshAndMaterial(mesh, material));
  ASSERT_TRUE(instance.has_value());

  const uint32_t slot = SlotOf(*instance);
  EXPECT_EQ(fixture.scene.LookupInstanceMaterialBySlot(slot), material);
}

// -----------------------------------------------------------------------------
// Instance appended with material=Invalid (e.g. M3 cube before M5
// materials wired) -> lookup returns Invalid. The lookup faithfully
// reports the bound state; it doesn't pretend an unbound instance has
// the slot-0 sentinel material (which would mis-select grey for every
// material-less instance).
// -----------------------------------------------------------------------------
TEST(GpuSceneLookupInstanceMaterial, InstanceWithoutMaterialReturnsInvalid) {
  CpuOnlyScene fixture;
  const MeshHandle mesh = fixture.MakeMesh();

  InstanceDesc desc;
  desc.mesh = mesh;
  desc.material = MaterialHandle::Invalid;
  desc.worldFromLocal = hlslpp::float4x4(
      hlslpp::float4{1, 0, 0, 0}, hlslpp::float4{0, 1, 0, 0},
      hlslpp::float4{0, 0, 1, 0}, hlslpp::float4{0, 0, 0, 1});
  const Expected<InstanceHandle> instance = fixture.scene.AppendInstance(desc);
  ASSERT_TRUE(instance.has_value());

  EXPECT_EQ(fixture.scene.LookupInstanceMaterialBySlot(SlotOf(*instance)),
            MaterialHandle::Invalid);
}

// -----------------------------------------------------------------------------
// Destroyed-instance slot -> Invalid. The picker is one-frame stale,
// so the user can click an entity that was destroyed BETWEEN the
// raygen write and the staging readback. The lookup must not return
// a stale material; instead the click-to-select path silently does
// nothing (no combo jump).
// -----------------------------------------------------------------------------
TEST(GpuSceneLookupInstanceMaterial, DestroyedInstanceSlotReturnsInvalid) {
  CpuOnlyScene fixture;
  const MeshHandle mesh = fixture.MakeMesh();
  const MaterialHandle material = fixture.MakeMaterial(0.4f);

  const Expected<InstanceHandle> instance =
      fixture.scene.AppendInstance(fixture.DescForMeshAndMaterial(mesh, material));
  ASSERT_TRUE(instance.has_value());
  const uint32_t slot = SlotOf(*instance);

  fixture.scene.DestroyInstance(*instance);
  EXPECT_EQ(fixture.scene.LookupInstanceMaterialBySlot(slot), MaterialHandle::Invalid);
}

// -----------------------------------------------------------------------------
// UpdateInstanceMaterial flows through into the lookup result. Pins
// the editor's "edit material via slider" round-trip — if the editor
// mutates the bound material, the next click on the same instance
// must surface the NEW material in the combo.
// -----------------------------------------------------------------------------
TEST(GpuSceneLookupInstanceMaterial, UpdateInstanceMaterialFlowsIntoLookup) {
  CpuOnlyScene fixture;
  const MeshHandle mesh = fixture.MakeMesh();
  const MaterialHandle materialA = fixture.MakeMaterial(0.1f);
  const MaterialHandle materialB = fixture.MakeMaterial(0.9f);
  ASSERT_NE(materialA, MaterialHandle::Invalid);
  ASSERT_NE(materialB, MaterialHandle::Invalid);
  ASSERT_NE(materialA, materialB);

  const Expected<InstanceHandle> instance =
      fixture.scene.AppendInstance(fixture.DescForMeshAndMaterial(mesh, materialA));
  ASSERT_TRUE(instance.has_value());
  const uint32_t slot = SlotOf(*instance);

  ASSERT_EQ(fixture.scene.LookupInstanceMaterialBySlot(slot), materialA);

  fixture.scene.UpdateInstanceMaterial(*instance, materialB);
  EXPECT_EQ(fixture.scene.LookupInstanceMaterialBySlot(slot), materialB);
}
