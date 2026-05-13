// Pyxis USD ingest — OpenVDB grid loader. V2.A.5.
//
// VolumeLoader::LoadVdbGrid opens a `.vdb` file referenced by a
// UsdVolVolume / OpenVDBAsset prim, finds the largest float grid
// (typically `density`), walks its active bounding box, and
// flattens the sparse voxels into a dense `float` buffer + bbox +
// world transform. The output is the shape the renderer's
// Texture3D upload path will eventually consume — for V2.A.5 we
// load + log + count, but the GPU-side 3D-texture binding +
// shader sampling lands in the follow-up volume-integrator pass.
//
// Single-grid v1: we pick the largest float grid by active-voxel
// count. Multi-grid composition (density + temperature + colour)
// is post-v1 — OpenPBR's volumetric inputs (transmissionDepth /
// transmissionColor) handle one channel today.
//
// Threading: openvdb::io::File operates on the calling thread.
// StageWalker invokes us from its single-threaded volume-pass
// loop; no Initialize() / Uninitialize() bracket is needed here
// because openvdb's TLS init is lazy and idempotent across calls.

#pragma once

#include <array>
#include <cstdint>
#include <expected>
#include <string>
#include <vector>

namespace pyxis::usd_ingest {

// Dense voxel grid + metadata. Stored in source-grid units (no
// resampling); the caller composes `worldTransform4x4 * indexToWorld`
// at upload time. `voxels.size() == dimensions[0]*dimensions[1]*dimensions[2]`
// when the load succeeds, indexed as `[z * (dx * dy) + y * dx + x]`.
struct LoadedVolume
{
  std::vector<float>     voxels{};           // active-bbox flattened, dense float32 (R32_FLOAT).
  std::array<uint32_t, 3> dimensions{0, 0, 0};  // (dx, dy, dz) — number of voxels per axis.
  std::array<float, 3>    bboxMinIndex{0, 0, 0};  // active-bbox min in grid-index space.
  std::array<float, 3>    bboxMaxIndex{0, 0, 0};  // active-bbox max in grid-index space.
  std::array<float, 16>   indexToWorld{};       // row-major 4x4 grid → world-space transform.
  std::string             gridName{};            // openvdb grid name (e.g. "density").
  uint64_t                activeVoxelCount = 0;  // sparse-tree active count (sanity vs voxels.size()).
};

// Load a single .vdb file from disk. Picks the largest active-voxel
// float grid in the file. Returns the unexpected branch with a human-
// readable message on any failure (file open, grid not found, IO
// error, allocation failure).
//
// Performance note: dense flattening is O(active_voxel_count) and
// also allocates `dx*dy*dz` floats. For a hero VDB asset (Disney
// Moana-cloud-scale at 1k³ active) that's ~4 GB — far above v1's
// 8 GiB VRAM budget (§17). The caller is expected to enforce a
// reasonable size cap; this loader doesn't impose one because the
// budget knob lives in §17 / RenderSettings, not here.
[[nodiscard]] std::expected<LoadedVolume, std::string> LoadVdbGrid(
    const std::string& filePath) noexcept;

}  // namespace pyxis::usd_ingest
