// Pyxis renderer — PackMaterialGpu rows 8-14 round-trip test
// (Q3 review, openpbr-complete-design.md).
//
// Pins the Detail.h PackMaterialGpu packing <-> ShaderInterop.slang
// OpenPBRMaterialGPU struct agreement for the Q1 OpenPBR-complete
// extension block (rows 8-14). Since Q2 these rows are READ by the
// openpbr_material.slang closure stack, so a silent offset drift
// between the C++ packer and the shader struct would mis-shade every
// specular/coat/fuzz/transmission/subsurface lobe. This test builds an
// OpenPBRMaterialDesc with a DISTINCTIVE non-default value in every new
// field, packs it, and asserts:
//   (a) each value round-trips to the expected GPU field, and
//   (b) each GPU field sits at the documented float offset in the
//       240-byte layout (the row map on the struct).
//
// PackMaterialGpu + the dual-language ShaderInterop header both live on
// the renderer's Private include path — the unit-test harness is the
// documented §35 exception allowed into Private/.

#include "GpuScene/Detail.h"

#include <Pyxis/Renderer/Descs/OpenPBRMaterialDesc.h>

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>

using namespace pyxis;
using gpuscene_detail::PackMaterialGpu;
using shaderinterop::OpenPBRMaterialGPU;

namespace {

// The packer takes the desc plus the eight resolved bindless slot
// indices. Rows 8-14 don't carry any texture slots, so the values here
// are irrelevant to this test — pass INVALID_BINDLESS_TEXTURE for all.
shaderinterop::OpenPBRMaterialGPU PackNoTextures(const OpenPBRMaterialDesc& desc)
{
  constexpr std::uint32_t kInvalid = shaderinterop::INVALID_BINDLESS_TEXTURE;
  return PackMaterialGpu(desc, /*flags=*/0u, kInvalid, kInvalid, kInvalid, kInvalid,
                         kInvalid, kInvalid, kInvalid, kInvalid);
}

}  // namespace

// (a) Every rows-8-14 field round-trips its distinctive value.
TEST(PackMaterialGpuRows, ExtensionFieldsRoundTrip)
{
  OpenPBRMaterialDesc desc;
  // Row 8 — specularColor RGB + specularWeight.
  desc.specularColorR = 0.11f;
  desc.specularColorG = 0.12f;
  desc.specularColorB = 0.13f;
  desc.specularWeight = 0.14f;
  // Row 9 — coatColor RGB + coatIor.
  desc.coatColorR = 0.21f;
  desc.coatColorG = 0.22f;
  desc.coatColorB = 0.23f;
  desc.coatIor = 1.42f;
  // Row 10 — coatDarkening + fuzzWeight + fuzzRoughness + baseDiffuseRoughness.
  desc.coatDarkening = 0.31f;
  desc.fuzzWeight = 0.32f;
  desc.fuzzRoughness = 0.33f;
  desc.baseDiffuseRoughness = 0.34f;
  // Row 11 — fuzzColor RGB + specularRoughnessAnisotropy.
  desc.fuzzColorR = 0.41f;
  desc.fuzzColorG = 0.42f;
  desc.fuzzColorB = 0.43f;
  desc.specularRoughnessAnisotropy = 0.44f;
  // Row 12 — transmissionColor RGB + subsurfaceWeight.
  desc.transmissionColorR = 0.51f;
  desc.transmissionColorG = 0.52f;
  desc.transmissionColorB = 0.53f;
  desc.subsurfaceWeight = 0.54f;
  // Row 13 — subsurfaceColor RGB + baseWeight.
  desc.subsurfaceColorR = 0.61f;
  desc.subsurfaceColorG = 0.62f;
  desc.subsurfaceColorB = 0.63f;
  desc.baseWeight = 0.64f;
  // Row 14 — transmissionWeight.
  desc.transmissionWeight = 0.71f;

  const OpenPBRMaterialGPU gpu = PackNoTextures(desc);

  // Row 8.
  EXPECT_FLOAT_EQ(gpu.specularColorR, 0.11f);
  EXPECT_FLOAT_EQ(gpu.specularColorG, 0.12f);
  EXPECT_FLOAT_EQ(gpu.specularColorB, 0.13f);
  EXPECT_FLOAT_EQ(gpu.specularWeight, 0.14f);
  // Row 9.
  EXPECT_FLOAT_EQ(gpu.coatColorR, 0.21f);
  EXPECT_FLOAT_EQ(gpu.coatColorG, 0.22f);
  EXPECT_FLOAT_EQ(gpu.coatColorB, 0.23f);
  EXPECT_FLOAT_EQ(gpu.coatIor, 1.42f);
  // Row 10.
  EXPECT_FLOAT_EQ(gpu.coatDarkening, 0.31f);
  EXPECT_FLOAT_EQ(gpu.fuzzWeight, 0.32f);
  EXPECT_FLOAT_EQ(gpu.fuzzRoughness, 0.33f);
  EXPECT_FLOAT_EQ(gpu.baseDiffuseRoughness, 0.34f);
  // Row 11.
  EXPECT_FLOAT_EQ(gpu.fuzzColorR, 0.41f);
  EXPECT_FLOAT_EQ(gpu.fuzzColorG, 0.42f);
  EXPECT_FLOAT_EQ(gpu.fuzzColorB, 0.43f);
  EXPECT_FLOAT_EQ(gpu.specularRoughnessAnisotropy, 0.44f);
  // Row 12.
  EXPECT_FLOAT_EQ(gpu.transmissionColorR, 0.51f);
  EXPECT_FLOAT_EQ(gpu.transmissionColorG, 0.52f);
  EXPECT_FLOAT_EQ(gpu.transmissionColorB, 0.53f);
  EXPECT_FLOAT_EQ(gpu.subsurfaceWeight, 0.54f);
  // Row 13.
  EXPECT_FLOAT_EQ(gpu.subsurfaceColorR, 0.61f);
  EXPECT_FLOAT_EQ(gpu.subsurfaceColorG, 0.62f);
  EXPECT_FLOAT_EQ(gpu.subsurfaceColorB, 0.63f);
  EXPECT_FLOAT_EQ(gpu.baseWeight, 0.64f);
  // Row 14.
  EXPECT_FLOAT_EQ(gpu.transmissionWeight, 0.71f);
}

// (b) Pin the byte offsets — the row map says rows 0-7 are the frozen
// 128-B base layout, so row 8 starts at byte 128 and each subsequent
// float advances by 4. A drift here is exactly the mis-shade hazard the
// Q2 closure introduced; the static_assert on sizeof catches a length
// change, this catches a field-order/offset change within the block.
TEST(PackMaterialGpuRows, ExtensionFieldOffsetsMatchRowMap)
{
  static_assert(sizeof(OpenPBRMaterialGPU) == 240,
                "OpenPBRMaterialGPU must be 240 bytes (15 rows of 16).");

  // Row 8 @ 128.
  EXPECT_EQ(offsetof(OpenPBRMaterialGPU, specularColorR), 128u);
  EXPECT_EQ(offsetof(OpenPBRMaterialGPU, specularColorG), 132u);
  EXPECT_EQ(offsetof(OpenPBRMaterialGPU, specularColorB), 136u);
  EXPECT_EQ(offsetof(OpenPBRMaterialGPU, specularWeight), 140u);
  // Row 9 @ 144.
  EXPECT_EQ(offsetof(OpenPBRMaterialGPU, coatColorR), 144u);
  EXPECT_EQ(offsetof(OpenPBRMaterialGPU, coatColorG), 148u);
  EXPECT_EQ(offsetof(OpenPBRMaterialGPU, coatColorB), 152u);
  EXPECT_EQ(offsetof(OpenPBRMaterialGPU, coatIor), 156u);
  // Row 10 @ 160.
  EXPECT_EQ(offsetof(OpenPBRMaterialGPU, coatDarkening), 160u);
  EXPECT_EQ(offsetof(OpenPBRMaterialGPU, fuzzWeight), 164u);
  EXPECT_EQ(offsetof(OpenPBRMaterialGPU, fuzzRoughness), 168u);
  EXPECT_EQ(offsetof(OpenPBRMaterialGPU, baseDiffuseRoughness), 172u);
  // Row 11 @ 176.
  EXPECT_EQ(offsetof(OpenPBRMaterialGPU, fuzzColorR), 176u);
  EXPECT_EQ(offsetof(OpenPBRMaterialGPU, fuzzColorG), 180u);
  EXPECT_EQ(offsetof(OpenPBRMaterialGPU, fuzzColorB), 184u);
  EXPECT_EQ(offsetof(OpenPBRMaterialGPU, specularRoughnessAnisotropy), 188u);
  // Row 12 @ 192.
  EXPECT_EQ(offsetof(OpenPBRMaterialGPU, transmissionColorR), 192u);
  EXPECT_EQ(offsetof(OpenPBRMaterialGPU, transmissionColorG), 196u);
  EXPECT_EQ(offsetof(OpenPBRMaterialGPU, transmissionColorB), 200u);
  EXPECT_EQ(offsetof(OpenPBRMaterialGPU, subsurfaceWeight), 204u);
  // Row 13 @ 208.
  EXPECT_EQ(offsetof(OpenPBRMaterialGPU, subsurfaceColorR), 208u);
  EXPECT_EQ(offsetof(OpenPBRMaterialGPU, subsurfaceColorG), 212u);
  EXPECT_EQ(offsetof(OpenPBRMaterialGPU, subsurfaceColorB), 216u);
  EXPECT_EQ(offsetof(OpenPBRMaterialGPU, baseWeight), 220u);
  // Row 14 @ 224 (+ 3 reserved floats to 240).
  EXPECT_EQ(offsetof(OpenPBRMaterialGPU, transmissionWeight), 224u);
}
