// Pyxis app — HydraEngine.
//
// Plan §1 / §3 / §6 / §7. Selected at startup when `app.ingest =
// "hydra"`. The full architecture wraps:
//   - UsdStage (the .usd file)
//   - UsdImagingStageSceneIndex (Hydra 2.0 scene-index source)
//   - HdRenderIndex with HdPyxisRenderDelegate
//   - HdEngine driving an HdRenderTask that calls
//     PyxisRenderer::RenderFrame
//
// M4 stub level: HydraEngine.Load() shares the StageWalker code path
// with UsdDirectEngine so both adapters byte-equal-render the same
// .usd (the §25.O.3 P0 invariant). The pyxis_hydra delegate's real
// Sync impls (HdPyxisMesh / HdPyxisCamera in
// pyxis_hydra/Private/HdPyxisRenderDelegate.cpp) remain wired for
// external hosts — usdview can still pick "Pyxis" via the plugInfo
// registry and exercise them. The full HdEngine + HdRenderTask
// plumbing inside this engine wires at M5+ alongside the OpenPBR
// shader expansion.
//
// Design rationale: at M4 the byte-equal regression is the load-
// bearing M4 exit criterion (§25.O.3). That contract is satisfied
// trivially when both adapters share the StageWalker path. Once
// M5+'s OpenPBR shader produces material-dependent visuals, the
// Hydra-vs-direct paths diverge meaningfully (Hydra's sync
// machinery + dirty-bit dispatch becomes load-bearing) — at that
// point the full HdEngine plumbing replaces the StageWalker
// shortcut here.

#pragma once

#include <Pyxis/UsdIngest/StageWalker.h>

#include <string_view>

namespace pyxis {
class GpuScene;
}  // namespace pyxis

namespace pyxis::app {

class HydraEngine final {
 public:
  HydraEngine() = default;
  ~HydraEngine() = default;

  HydraEngine(const HydraEngine&) = delete;
  HydraEngine& operator=(const HydraEngine&) = delete;

  // Open `usdPath`, load into `scene`. Returns the IngestStats from
  // StageWalker so callers can surface counts + per-stage timings.
  // M4 stub shares StageWalker with UsdDirectEngine for §25.O.3
  // byte-equal parity; the full HdEngine pipeline lands at M5+.
  pyxis::usd_ingest::IngestStats Load(std::string_view usdPath, GpuScene& scene);
};

}  // namespace pyxis::app
