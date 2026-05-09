// Pyxis app — UsdDirectEngine.
//
// Plan §1 / §3 / §25.O. Selected at startup when `app.ingest =
// "usd_direct"`. Owns the UsdStage briefly, drives StageWalker
// once into the supplied GpuScene, then releases the stage. The
// renderer never holds a UsdStage* after Load() returns
// (§25.O.2 — direct-ingest is one-shot; subsequent edits go
// through the pyxis_renderer public API).

#pragma once

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
  // the public §18 API. Returns true on success; false (with an
  // already-logged Error line) if the stage couldn't open. Either
  // outcome leaves `scene` in a renderable state — failure means the
  // M3 hardcoded-cube fallback path lights up.
  [[nodiscard]] bool Load(std::string_view usdPath, GpuScene& scene);
};

}  // namespace pyxis::app
