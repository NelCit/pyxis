// Pyxis renderer — mesh UV tight-packing helper.
//
// Split out of Commit.cpp's `UploadMeshUvs` so the unit-test harness can
// pin the GPU-stride contract without dragging in `GpuScene::Impl`. Kept
// in `pyxis::gpuscene_detail` (Private — not part of §18.1's public
// surface), same convention as BcEncoder.h.
//
// THE CONTRACT this guards. The closesthit shader declares
// `StructuredBuffer<float2> gMeshUvs`, whose element stride is FIXED at
// 8 bytes. `hlslpp::float2` is SIMD-padded to 16 bytes (it lives in a
// float4 register), so the per-vertex UVs MUST be packed tight (8 bytes
// each) before upload. Writing a `std::vector<hlslpp::float2>` straight
// to the buffer yields a 16-byte stride; the shader then reads every
// ODD element as the SIMD padding (0,0), collapsing barycentric UV
// interpolation and scrambling every st-mapped texture (the diagonal
// wood-grain bug). The float4 mesh buffers (normals/tangents/face-
// normals) are immune because `hlslpp::float4` is exactly 16 bytes.

#pragma once

#include <hlsl++.h>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace pyxis::gpuscene_detail {

// Tight 8-byte UV element matching the shader's `StructuredBuffer<float2>`
// stride. Deliberately NOT `hlslpp::float2` (which is 16 bytes).
struct PackedUv
{
  float u;
  float v;
};
static_assert(sizeof(PackedUv) == 8,
              "gMeshUvs stride must match the shader's float2 (8 bytes)");
static_assert(alignof(PackedUv) == 4, "PackedUv must be tightly packed");

// Append one mesh's UV slice to `out`, packed tight. Copies every entry
// of `uv0`, then — if the mesh authored fewer UVs than it has vertices —
// pads the remainder up to `vertexCount` with (0,0) so each mesh's slice
// stays aligned to its [0, vertexCount) vertex range in the flat buffer
// (the closesthit indexes `gMeshUvs[offset + vertexIndex]`). A mesh with
// `uv0Count >= vertexCount` contributes `uv0Count` elements and no
// padding. Mirrors UploadMeshUvs's per-mesh inner loop exactly.
void AppendTightMeshUvs(const hlslpp::float2* uv0,
                        std::size_t            uv0Count,
                        std::uint32_t          vertexCount,
                        std::vector<PackedUv>& out);

}  // namespace pyxis::gpuscene_detail
