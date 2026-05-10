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

#include <Pyxis/Renderer/Descs/CameraDesc.h>
#include <Pyxis/UsdIngest/UsdIngestApi.h>

#include <pxr/usd/usd/stage.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace pyxis {
class GpuScene;
}  // namespace pyxis

namespace pyxis::usd_ingest {

// One USD camera the StageWalker picked up + translated. The full
// list is returned via IngestStats::cameras so the viewer can
// populate a "Scene Camera" combo and let the user snap the
// FlyCamera to any of them. The single "active" one (chosen by
// boundCamera hint, or first-in-SdfPath-order) is also pushed via
// GpuScene::SetCamera so headless renders + first-frame viewer work.
struct PYXIS_USD_INGEST_API NamedCamera {
  std::string name;       // SdfPath of the camera prim
  CameraDesc  desc;       // viewFromWorld + projFromView + intrinsics
};

struct PYXIS_USD_INGEST_API IngestStats {
  uint32_t meshesEmitted = 0;
  uint32_t instancesEmitted = 0;
  uint32_t instancersEmitted = 0;  // M6: UsdGeomPointInstancer prims expanded
  uint32_t lightsEmitted = 0;
  uint32_t materialsEmitted = 0;
  uint32_t camerasEmitted = 0;
  uint32_t skipped = 0;       // unsupported prim types (volume, points, etc.)

  // All cameras the walker translated (in SdfPath-sorted order). The
  // index of the one pushed via GpuScene::SetCamera lives in
  // `activeCameraIndex` (-1 if no camera was pushed). Empty vector
  // for scenes with no cameras (e.g. cube fixtures).
  std::vector<NamedCamera> cameras;
  int activeCameraIndex = -1;

  // Per-stage timings (milliseconds, std::chrono::steady_clock based).
  // Surfaced in the viewer's Performance / Loading panel so users can
  // see where USD-load latency goes. Zero on a stage that didn't run
  // (e.g. stageOpenMs is 0 when WalkStage is called with an already-
  // opened stage). totalMs is the sum of all stages plus the harness
  // overhead inside WalkFile / WalkStage.
  float    stageOpenMs       = 0.0f;  // pxr::UsdStage::Open
  float    traverseSortMs    = 0.0f;  // stage->Traverse + stable_sort
  float    materialPassMs    = 0.0f;  // pass 1: UsdShadeMaterial -> AcquireMaterial
  float    instancerPassMs   = 0.0f;  // pass 2: UsdGeomPointInstancer expansion
  float    meshLightCameraMs = 0.0f;  // pass 3: meshes / camera / lights
  float    totalMs           = 0.0f;
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
