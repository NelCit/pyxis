// Pyxis — mesh UV tight-packing unit tests.
//
// Pins the GPU-stride contract that the diagonal-wood-grain bug
// violated: per-vertex UVs must be packed to a TIGHT 8-byte stride
// matching the closesthit's `StructuredBuffer<float2> gMeshUvs`, NOT the
// 16-byte SIMD-padded `sizeof(hlslpp::float2)`. If `UploadMeshUvs` ever
// regresses to writing a `std::vector<hlslpp::float2>` straight to the
// buffer, the GPU would read every odd element as the padding (0,0) and
// scramble every st-mapped texture. The golden suite catches the visual
// result; these tests catch the layout directly + document why.
//
// The §35 "unit tests may peek into Private/" exemption applies:
// MeshUvPack.cpp is compiled directly into pyxis_unit_tests (same as
// BcEncoder.cpp) so the test exercises the real helper without widening
// pyxis_renderer.dll's §18.1 public surface.

#include "GpuScene/MeshUvPack.h"

#include <hlsl++.h>

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

using pyxis::gpuscene_detail::AppendTightMeshUvs;
using pyxis::gpuscene_detail::PackedUv;

// The trap that caused the bug: hlslpp::float2 is SIMD-padded to 16
// bytes, while the shader's float2 element is 8. Packing must bridge the
// two. If hlslpp ever changes float2's size this assumption is worth a
// fresh look — that is exactly what this test documents.
TEST(MeshUvPackV2, HlslppFloat2IsSimdPaddedTo16Bytes) {
  EXPECT_EQ(sizeof(hlslpp::float2), 16u);
  EXPECT_EQ(sizeof(PackedUv), 8u);
}

// Packing N UVs yields N tight 8-byte elements whose raw float stream is
// [u0,v0,u1,v1,...] with NO padding holes — i.e. a GPU reader at an
// 8-byte float2 stride recovers exactly the authored values. (The buggy
// 16-byte-stride write would interleave (0,0) between every UV.)
TEST(MeshUvPackV2, PacksContiguousEightByteStride) {
  std::vector<hlslpp::float2> src = {
      hlslpp::float2{0.10f, 0.20f},
      hlslpp::float2{0.30f, 0.40f},
      hlslpp::float2{0.50f, 0.60f},
  };
  std::vector<PackedUv> out;
  AppendTightMeshUvs(src.data(), src.size(),
                     static_cast<std::uint32_t>(src.size()), out);

  ASSERT_EQ(out.size(), 3u);
  EXPECT_FLOAT_EQ(out[0].u, 0.10f);
  EXPECT_FLOAT_EQ(out[0].v, 0.20f);
  EXPECT_FLOAT_EQ(out[1].u, 0.30f);
  EXPECT_FLOAT_EQ(out[1].v, 0.40f);
  EXPECT_FLOAT_EQ(out[2].u, 0.50f);
  EXPECT_FLOAT_EQ(out[2].v, 0.60f);

  // Reinterpret the packed buffer as a flat float stream — the GPU view.
  // A tight 8-byte stride means element i lives at floats [2i, 2i+1]; a
  // padded 16-byte stride would put (0,0) at floats [2,3], [6,7], ...
  const float* raw = reinterpret_cast<const float*>(out.data());
  const float expected[6] = {0.10f, 0.20f, 0.30f, 0.40f, 0.50f, 0.60f};
  for (int i = 0; i < 6; ++i)
    EXPECT_FLOAT_EQ(raw[i], expected[i]) << "float slot " << i;
}

// A mesh that authored fewer UVs than it has vertices pads up to
// vertexCount with (0,0) so its slice stays aligned to [0, vertexCount).
TEST(MeshUvPackV2, ShortUvArrayPadsToVertexCount) {
  std::vector<hlslpp::float2> src = {hlslpp::float2{0.25f, 0.75f}};
  std::vector<PackedUv> out;
  AppendTightMeshUvs(src.data(), src.size(), /*vertexCount*/ 4u, out);

  ASSERT_EQ(out.size(), 4u);
  EXPECT_FLOAT_EQ(out[0].u, 0.25f);
  EXPECT_FLOAT_EQ(out[0].v, 0.75f);
  for (std::size_t i = 1; i < 4u; ++i)
  {
    EXPECT_FLOAT_EQ(out[i].u, 0.0f) << "pad " << i;
    EXPECT_FLOAT_EQ(out[i].v, 0.0f) << "pad " << i;
  }
}

// Multiple meshes concatenate into one flat buffer; each call appends to
// the running vector, so per-mesh offsets are just the size before the
// call (the contract UploadMeshUvs relies on for gMeshUvOffsets).
TEST(MeshUvPackV2, AppendsAcrossMeshesPreservingOffsets) {
  std::vector<hlslpp::float2> meshA = {hlslpp::float2{1.0f, 2.0f},
                                       hlslpp::float2{3.0f, 4.0f}};
  std::vector<hlslpp::float2> meshB = {hlslpp::float2{5.0f, 6.0f}};
  std::vector<PackedUv> out;

  const std::uint32_t offsetA = static_cast<std::uint32_t>(out.size());
  AppendTightMeshUvs(meshA.data(), meshA.size(), 2u, out);
  const std::uint32_t offsetB = static_cast<std::uint32_t>(out.size());
  AppendTightMeshUvs(meshB.data(), meshB.size(), 1u, out);

  EXPECT_EQ(offsetA, 0u);
  EXPECT_EQ(offsetB, 2u);
  ASSERT_EQ(out.size(), 3u);
  EXPECT_FLOAT_EQ(out[offsetB].u, 5.0f);
  EXPECT_FLOAT_EQ(out[offsetB].v, 6.0f);
}

// uv0Count >= vertexCount contributes uv0Count elements and no padding
// (the loop copies all authored UVs; the pad branch is skipped).
TEST(MeshUvPackV2, FullUvArrayContributesNoPadding) {
  std::vector<hlslpp::float2> src = {hlslpp::float2{0.1f, 0.1f},
                                     hlslpp::float2{0.2f, 0.2f},
                                     hlslpp::float2{0.3f, 0.3f}};
  std::vector<PackedUv> out;
  AppendTightMeshUvs(src.data(), src.size(), /*vertexCount*/ 3u, out);
  EXPECT_EQ(out.size(), 3u);
}
