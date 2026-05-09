// Pyxis app — UsdDirectEngine.
//
// Plan §1 / §3 / §25.O. Selected at startup when `app.ingest =
// "usd_direct"`. Owns the UsdStage briefly, drives StageWalker
// once into the supplied GpuScene, then releases the stage. The
// renderer never holds a UsdStage* after Load() returns
// (§25.O.2 — direct-ingest is one-shot; subsequent edits go
// through the pyxis_renderer public API).

#pragma once

#include <Pyxis/UsdIngest/StageWalker.h>

#include <string_view>

namespace pyxis {
class GpuScene;
}  // namespace pyxis

namespace pyxis::app {

class UsdDirectEngine final {
 public:
  UsdDirectEngine() = default;
  ~UsdDirectEngine() = default;

  UsdDirectEngine(const UsdDirectEngine&) = delete;
  UsdDirectEngine& operator=(const UsdDirectEngine&) = delete;

  // Open `usdPath`, walk the stage in SdfPath-sorted order, push the
  // resulting MeshDesc / InstanceDesc / CameraDesc into `scene` via
  // the public §18 API. Returns the IngestStats from StageWalker so
  // callers can surface counts + per-stage timings (pxr::UsdStage::Open
  // / pass1 materials / pass2 instancers / pass3 meshes-camera-lights).
  // On failure the returned stats has every counter zero — the caller
  // (ViewerMode) detects this and falls back to the hardcoded cube.
  pyxis::usd_ingest::IngestStats Load(std::string_view usdPath, GpuScene& scene);
};

}  // namespace pyxis::app
