// Pyxis USD-direct ingest — StageWalker.
//
// Plan §25.O. One-shot importer (no UsdNotice listener — see §25.O.2):
// opens a UsdStage, walks every prim in SdfPath-sorted order, and
// calls into a supplied GpuScene to materialise meshes / camera /
// lights / materials. The renderer never holds the UsdStage* after
// `Walk` returns; subsequent edits go through the pyxis_renderer
// public API (§18.5), not USD notices.
//
// SdfPath-sorted ordering is the §25.O.3 P0 invariant: pyxis_hydra
// must produce instances in the same order so the byte-equal EXR
// regression test passes.

#pragma once

#include <Pyxis/UsdIngest/UsdIngestApi.h>

#include <pxr/usd/usd/stage.h>

#include <cstdint>
#include <string_view>

namespace pyxis {
class GpuScene;
}  // namespace pyxis

namespace pyxis::usd_ingest {

struct PYXIS_USD_INGEST_API IngestStats {
  uint32_t meshesEmitted = 0;
  uint32_t instancesEmitted = 0;
  uint32_t lightsEmitted = 0;
  uint32_t materialsEmitted = 0;
  uint32_t camerasEmitted = 0;
  uint32_t skipped = 0;       // unsupported prim types (volume, points, etc.)
};

class PYXIS_USD_INGEST_API StageWalker final {
 public:
  StageWalker() = default;
  ~StageWalker() = default;

  StageWalker(const StageWalker&) = delete;
  StageWalker& operator=(const StageWalker&) = delete;

  // Open `usdPath`, walk all prims, push mutations into `scene`.
  // Returns IngestStats for diagnostics. Errors (file not found,
  // bad layer, etc.) surface via spdlog and result in stats with
  // every counter zero.
  IngestStats WalkFile(std::string_view usdPath, GpuScene& scene);

  // Variant taking an already-opened stage. Used by the cross-adapter
  // regression harness so both paths share a single stage open. The
  // pxr::UsdStageRefPtr argument is USD's reference-counted handle —
  // the renderer never persists it past `Walk` (§25.O.2 contract).
  IngestStats WalkStage(const pxr::UsdStageRefPtr& stage, GpuScene& scene);
};

}  // namespace pyxis::usd_ingest
