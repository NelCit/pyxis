// Pyxis renderer — public volume-creation descriptor. V2.A.5.
//
// Plan §18.4. Input to GpuScene::AddVolume. All span fields are
// caller-owned; the renderer copies what it needs into internal
// storage and uploads a 3D texture during the next CommitResources
// (§18.9 ABI rule).
//
// Geometry contract:
//   * `voxels` is required, dense float32, sized exactly
//     `dimensions[0] * dimensions[1] * dimensions[2]`. Indexed
//     `[z * dx*dy + y * dx + x]` (matching VolumeLoader's layout).
//   * `dimensions` is the (dx, dy, dz) voxel count per axis. Each
//     axis must be > 0 and ≤ 2048 (NVRHI 3D-texture cap on every
//     vendor we target). Larger volumes need to be tiled by the
//     loader before reaching this API.
//   * `bboxMin` / `bboxMax` are the active bbox in the source
//     grid's index-space (informational; closesthit doesn't sample
//     in v2).
//   * `indexToWorld` is the row-major 4x4 grid → world transform.
//     Combined with the owning instance's xform at sampling time
//     when the volume integrator pass lands.
//   * `debugName` is for NVRHI markers + spdlog logs only; never
//     hashed, never user-visible.
//
// Today the bound 3D texture has no shader sampler — closesthit
// declares a placeholder slot but doesn't read it. The descriptor
// + GPU upload is wired so the volume-integrator follow-up has a
// stable contract to land against.

#pragma once

#include <Pyxis/Renderer/RendererApi.h>

#include <array>
#include <cstdint>
#include <span>
#include <string_view>

namespace pyxis {

struct VolumeDesc {
  std::span<const float>  voxels;
  std::array<uint32_t, 3> dimensions{0, 0, 0};
  std::array<float, 3>    bboxMin{0.0f, 0.0f, 0.0f};
  std::array<float, 3>    bboxMax{0.0f, 0.0f, 0.0f};
  std::array<float, 16>   indexToWorld{1.0f, 0.0f, 0.0f, 0.0f,
                                       0.0f, 1.0f, 0.0f, 0.0f,
                                       0.0f, 0.0f, 1.0f, 0.0f,
                                       0.0f, 0.0f, 0.0f, 1.0f};
  std::string_view        debugName;
};

}  // namespace pyxis
