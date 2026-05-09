// Pyxis renderer — public mesh-creation descriptor.
//
// Plan §18.4. Input to GpuScene::CreateMesh / UpdateMesh. All span
// fields are caller-owned; the renderer copies what it needs into
// internal storage before the call returns and never persists a span
// past that point (§18.9 ABI rule).
//
// Geometry contract:
//   - `positions` is required and contiguous; one float3 per vertex.
//   - `indices`   is required, triangle list (3 indices per triangle),
//     must reference positions in [0, positions.size()).
//   - `normals`, `tangents`, `uv0` are optional. Empty `normals`
//     means the renderer generates per-face normals on upload; empty
//     `tangents` triggers MikkTSpace if a normal map is bound to the
//     instance's material.
//   - `debugName` is for NVRHI markers + profiler reports; never
//     hashed and never user-visible.

#pragma once

#include <Pyxis/Renderer/RendererApi.h>

#include <cstdint>
#include <hlsl++.h>
#include <span>
#include <string_view>

namespace pyxis {

struct MeshDesc {
  std::span<const hlslpp::float3> positions;
  std::span<const uint32_t> indices;
  std::span<const hlslpp::float3> normals;
  std::span<const hlslpp::float4> tangents;
  std::span<const hlslpp::float2> uv0;
  std::string_view debugName;
};

}  // namespace pyxis
