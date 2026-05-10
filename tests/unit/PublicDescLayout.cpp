// Pyxis renderer — public Desc POD layout tests.
//
// Plan §18.9 / §22.3. Every public POD is byte-stable: sizeof,
// alignof, member offsets and padding are part of the contract.
// These static_asserts pin the contract at the test layer so a stray
// reorder or narrowed type fails the build instead of silently
// shipping a major-version break.

#include <Pyxis/Renderer/Descs/CameraDesc.h>
#include <Pyxis/Renderer/Descs/FrameStats.h>
#include <Pyxis/Renderer/Descs/GpuSceneCreateDesc.h>
#include <Pyxis/Renderer/Descs/InstanceDesc.h>
#include <Pyxis/Renderer/Descs/LightDesc.h>
#include <Pyxis/Renderer/Descs/MeshDesc.h>
#include <Pyxis/Renderer/Descs/OpenPBRMaterialDesc.h>
#include <Pyxis/Renderer/Descs/PickResult.h>
#include <Pyxis/Renderer/Descs/TextureKey.h>

#include <cstddef>
#include <cstring>
#include <Pyxis/Renderer/Forward.h>

#include <gtest/gtest.h>

#include <hlsl++.h>
#include <type_traits>

using namespace pyxis;

// -----------------------------------------------------------------------------
// hlslpp baseline — pin what we expect from hlslpp's float3 / float4 /
// float4x4. If a future hlslpp bump changes these sizes (e.g. by
// switching SIMD config), every public Desc that embeds them shifts
// in lockstep, which is a major-version event we want to surface
// loudly here.
// -----------------------------------------------------------------------------
static_assert(sizeof(hlslpp::float3) == 16,
              "hlslpp::float3 is 16 bytes (12 + 4 padding) under the SSE config Pyxis ships.");
static_assert(sizeof(hlslpp::float4) == 16, "hlslpp::float4 is one SIMD lane (16 bytes).");
static_assert(sizeof(hlslpp::float4x4) == 64, "hlslpp::float4x4 is four SIMD lanes (64 bytes).");
static_assert(alignof(hlslpp::float4x4) == 16, "hlslpp::float4x4 is 16-byte aligned.");

// -----------------------------------------------------------------------------
// Standard layout — every Desc must cross the SHARED DLL boundary
// with a layout the consumer and the renderer agree on. Anything
// with virtual functions, multiple base classes, mixed-access fields,
// etc. would fail this. This is the ABI lock §18.9 actually needs.
//
// We don't gate on `is_trivially_copyable_v` for Descs that embed
// hlslpp types: hlslpp::float3 / float4 / float4x4 have user-defined
// move/copy constructors (SIMD-aware) so they're not trivially
// copyable, which would cascade to every Desc embedding them. The
// PODs without hlslpp members (TextureKey / GpuSceneCreateDesc /
// FrameStats) are still gated on triviality below as a stricter
// check.
// -----------------------------------------------------------------------------
static_assert(std::is_standard_layout_v<MeshDesc>, "MeshDesc must be standard layout.");
static_assert(std::is_standard_layout_v<InstanceDesc>, "InstanceDesc must be standard layout.");
static_assert(std::is_standard_layout_v<OpenPBRMaterialDesc>,
              "OpenPBRMaterialDesc must be standard layout.");
static_assert(std::is_standard_layout_v<CameraDesc>, "CameraDesc must be standard layout.");
static_assert(std::is_standard_layout_v<LightDesc>, "LightDesc must be standard layout.");
static_assert(std::is_standard_layout_v<TextureKey>, "TextureKey must be standard layout.");
static_assert(std::is_standard_layout_v<GpuSceneCreateDesc>,
              "GpuSceneCreateDesc must be standard layout.");
static_assert(std::is_standard_layout_v<FrameStats>, "FrameStats must be standard layout.");

static_assert(std::is_trivially_copyable_v<TextureKey>,
              "TextureKey must be trivially copyable (no hlslpp members).");
static_assert(std::is_trivially_copyable_v<GpuSceneCreateDesc>,
              "GpuSceneCreateDesc must be trivially copyable (no hlslpp members).");
static_assert(std::is_trivially_copyable_v<FrameStats>,
              "FrameStats must be trivially copyable (no hlslpp members).");

// -----------------------------------------------------------------------------
// Strong handles — fixed at uint32_t with Invalid = 0 (§19.7).
// -----------------------------------------------------------------------------
static_assert(std::is_same_v<std::underlying_type_t<MeshHandle>, uint32_t>);
static_assert(std::is_same_v<std::underlying_type_t<MaterialHandle>, uint32_t>);
static_assert(std::is_same_v<std::underlying_type_t<TextureHandle>, uint32_t>);
static_assert(std::is_same_v<std::underlying_type_t<InstanceHandle>, uint32_t>);
static_assert(std::is_same_v<std::underlying_type_t<LightHandle>, uint32_t>);
static_assert(static_cast<uint32_t>(MeshHandle::Invalid) == 0u);
static_assert(static_cast<uint32_t>(MaterialHandle::Invalid) == 0u);
static_assert(static_cast<uint32_t>(TextureHandle::Invalid) == 0u);
static_assert(static_cast<uint32_t>(InstanceHandle::Invalid) == 0u);
static_assert(static_cast<uint32_t>(LightHandle::Invalid) == 0u);

// -----------------------------------------------------------------------------
// Defaults — runtime checks for the field initialisers documented in
// each Desc header. Anyone who reorders or renames a field will trip
// these unless they update both halves.
// -----------------------------------------------------------------------------
TEST(PublicDescLayout, MeshDescDefaultsAreEmptySpans) {
  const MeshDesc desc;
  EXPECT_TRUE(desc.positions.empty());
  EXPECT_TRUE(desc.indices.empty());
  EXPECT_TRUE(desc.normals.empty());
  EXPECT_TRUE(desc.tangents.empty());
  EXPECT_TRUE(desc.uv0.empty());
  EXPECT_TRUE(desc.debugName.empty());
}

TEST(PublicDescLayout, InstanceDescInvalidByDefault) {
  const InstanceDesc desc;
  EXPECT_EQ(desc.mesh, MeshHandle::Invalid);
  EXPECT_EQ(desc.material, MaterialHandle::Invalid);
  EXPECT_TRUE(desc.visible);
}

TEST(PublicDescLayout, OpenPBRDefaultsMatchSpec) {
  const OpenPBRMaterialDesc desc;
  EXPECT_EQ(desc.source, OpenPBRMaterialDesc::Source::Default);
  EXPECT_EQ(desc.baseColorMap, TextureHandle::Invalid);
  EXPECT_FLOAT_EQ(desc.metalness, 0.0f);
  EXPECT_FLOAT_EQ(desc.roughness, 0.5f);
  EXPECT_FLOAT_EQ(desc.specularIor, 1.5f);
  EXPECT_FLOAT_EQ(desc.opacity, 1.0f);
}

TEST(PublicDescLayout, CameraDescDefaultsAreSensible) {
  const CameraDesc desc;
  EXPECT_FLOAT_EQ(desc.focalLengthMm, 35.0f);
  EXPECT_FLOAT_EQ(desc.apertureFStop, 0.0f);
  EXPECT_FLOAT_EQ(desc.nearClip, 0.01f);
  EXPECT_FLOAT_EQ(desc.farClip, 10000.0f);
}

TEST(PublicDescLayout, LightDescDefaultsToDistantSun) {
  const LightDesc desc;
  EXPECT_EQ(desc.kind, LightDesc::Kind::Distant);
  EXPECT_FLOAT_EQ(desc.intensity, 1.0f);
  EXPECT_EQ(desc.envMap, TextureHandle::Invalid);
  EXPECT_FALSE(desc.doubleSided);
}

TEST(PublicDescLayout, TextureKeyDefaultsToBaseColorSrgb) {
  const TextureKey key;
  EXPECT_EQ(key.role, TextureKey::Role::BaseColor);
  EXPECT_EQ(key.colorspace, TextureKey::Color::SRgb);
  EXPECT_TRUE(key.resolvedPath.empty());
}

TEST(PublicDescLayout, GpuSceneCreateDescDefaults) {
  const GpuSceneCreateDesc desc;
  EXPECT_EQ(desc.bindlessCapacity, 80'000u);
  EXPECT_EQ(desc.stagingMib, 256u);
  EXPECT_EQ(desc.framesInFlight, 2u);
  EXPECT_TRUE(desc.compactBlas);
}

TEST(PublicDescLayout, FrameStatsDefaultsToZeros) {
  const FrameStats stats;
  EXPECT_EQ(stats.meshCount, 0u);
  EXPECT_EQ(stats.instanceCount, 0u);
  EXPECT_EQ(stats.staleHandleDrops, 0u);
  EXPECT_FALSE(stats.degraded);
}

// -----------------------------------------------------------------------------
// PickResult — byte-stable layout. The shader-side
// `pyxis::shaderinterop::PickResult` has matching static_asserts on
// total size; here we pin the C++-side field offsets so a future
// reorder (which would silently corrupt the picker readback) fails
// the test instead of just shipping garbage values to the editor.
// -----------------------------------------------------------------------------
TEST(PublicDescLayout, PickResultLayoutMatchesShaderInterop) {
  EXPECT_EQ(sizeof(PickResult), 112u);
  EXPECT_EQ(alignof(PickResult), 4u);

  // Row 0 — color + depth.
  EXPECT_EQ(offsetof(PickResult, colorR),         0u);
  EXPECT_EQ(offsetof(PickResult, colorG),         4u);
  EXPECT_EQ(offsetof(PickResult, colorB),         8u);
  EXPECT_EQ(offsetof(PickResult, depth),         12u);
  // Row 1 — normal + instance id (Hydra primId).
  EXPECT_EQ(offsetof(PickResult, normalX),       16u);
  EXPECT_EQ(offsetof(PickResult, normalY),       20u);
  EXPECT_EQ(offsetof(PickResult, normalZ),       24u);
  EXPECT_EQ(offsetof(PickResult, instanceId),    28u);
  // Row 2 — baseColor + material id.
  EXPECT_EQ(offsetof(PickResult, baseColorR),    32u);
  EXPECT_EQ(offsetof(PickResult, baseColorG),    36u);
  EXPECT_EQ(offsetof(PickResult, baseColorB),    40u);
  EXPECT_EQ(offsetof(PickResult, materialId),    44u);
  // Row 3 — world hit position + pad.
  EXPECT_EQ(offsetof(PickResult, worldHitX),     48u);
  EXPECT_EQ(offsetof(PickResult, worldHitY),     52u);
  EXPECT_EQ(offsetof(PickResult, worldHitZ),     56u);
  // Row 4 — pixel coords + pad.
  EXPECT_EQ(offsetof(PickResult, pixelX),        64u);
  EXPECT_EQ(offsetof(PickResult, pixelY),        68u);
  // Row 5 — eye-space normal (Hydra Neye) + Hydra elementId.
  EXPECT_EQ(offsetof(PickResult, normalEyeX),    80u);
  EXPECT_EQ(offsetof(PickResult, normalEyeY),    84u);
  EXPECT_EQ(offsetof(PickResult, normalEyeZ),    88u);
  EXPECT_EQ(offsetof(PickResult, elementId),     92u);
  // Row 6 — eye-space hit position (Hydra Peye) + alpha.
  EXPECT_EQ(offsetof(PickResult, worldPosEyeX),  96u);
  EXPECT_EQ(offsetof(PickResult, worldPosEyeY), 100u);
  EXPECT_EQ(offsetof(PickResult, worldPosEyeZ), 104u);
  EXPECT_EQ(offsetof(PickResult, alpha),        108u);
}

TEST(PublicDescLayout, PickResultDefaultsToNoHitSentinels) {
  const PickResult pick;
  EXPECT_FLOAT_EQ(pick.colorR, 0.0f);
  EXPECT_FLOAT_EQ(pick.colorG, 0.0f);
  EXPECT_FLOAT_EQ(pick.colorB, 0.0f);
  EXPECT_FLOAT_EQ(pick.depth,  -1.0f);
  EXPECT_EQ(pick.instanceId,   0xFFFFFFFFu);
  EXPECT_EQ(pick.materialId,   0xFFFFFFFFu);
  EXPECT_EQ(pick.pixelX,       0xFFFFFFFFu);
  EXPECT_EQ(pick.pixelY,       0xFFFFFFFFu);
}

// Round-trip a known byte pattern through the struct to verify the
// shader-side write order matches the C++ read order. If the shader
// emits row-major and C++ reads field-by-field, the bytes have to land
// in the documented field. Catches the class of bug where a future
// shader edit reorders the struct without bumping the C++ POD.
TEST(PublicDescLayout, PickResultByteRoundTripMatchesFields) {
  alignas(PickResult) unsigned char buffer[80] = {};
  // Row 0: colorR=1.0, colorG=2.0, colorB=3.0, depth=4.0
  const float row0[4] = {1.0f, 2.0f, 3.0f, 4.0f};
  std::memcpy(buffer + 0, row0, sizeof(row0));
  // Row 1: normal + instance.
  const float row1Floats[3] = {0.5f, 0.6f, 0.7f};
  std::memcpy(buffer + 16, row1Floats, sizeof(row1Floats));
  const uint32_t instanceVal = 0xDEADBEEFu;
  std::memcpy(buffer + 28, &instanceVal, sizeof(instanceVal));
  // Row 4: pixel coords.
  const uint32_t pixelXVal = 1234u;
  const uint32_t pixelYVal = 5678u;
  std::memcpy(buffer + 64, &pixelXVal, sizeof(pixelXVal));
  std::memcpy(buffer + 68, &pixelYVal, sizeof(pixelYVal));

  PickResult pick;
  std::memcpy(&pick, buffer, sizeof(pick));

  EXPECT_FLOAT_EQ(pick.colorR, 1.0f);
  EXPECT_FLOAT_EQ(pick.colorG, 2.0f);
  EXPECT_FLOAT_EQ(pick.colorB, 3.0f);
  EXPECT_FLOAT_EQ(pick.depth,  4.0f);
  EXPECT_FLOAT_EQ(pick.normalX, 0.5f);
  EXPECT_FLOAT_EQ(pick.normalY, 0.6f);
  EXPECT_FLOAT_EQ(pick.normalZ, 0.7f);
  EXPECT_EQ(pick.instanceId, 0xDEADBEEFu);
  EXPECT_EQ(pick.pixelX,     1234u);
  EXPECT_EQ(pick.pixelY,     5678u);
}
