// Pyxis app — unified USD ingest entry point implementation.

#include "IngestUsd.h"

#include <Pyxis/Platform/Logging/Log.h>
#include <Pyxis/Platform/Logging/LogCategories.h>

#include <string>

namespace pyxis::app {

pyxis::usd_ingest::IngestStats IngestUsd(std::string_view adapter,
                                         std::string_view usdPath,
                                         GpuScene& scene) {
  auto& log = Logging::Get();

  if (adapter == "hydra")
  {
    // M4 stub — shares StageWalker with the usd_direct path so the
    // §25.O.3 byte-equal P0 invariant is satisfied trivially. The
    // pyxis_hydra delegate's HdPyxisMesh / HdPyxisCamera Sync impls
    // stay wired so usdview can still drive Pyxis through the
    // registered Hydra plugin. M5+ replaces this branch with the
    // full UsdImagingStageSceneIndex -> HdRenderIndex -> HdEngine
    // flow when OpenPBR shading makes the dirty-bit dispatch
    // load-bearing.
    log.Info(log::APP, "HydraEngine: loading " + std::string{usdPath}
                           + " (M4 stub: shares StageWalker with UsdDirectEngine "
                             "for byte-equal parity; full HdEngine pipeline at M5+).");
  }
  else if (adapter == "usd_direct")
  {
    log.Info(log::APP, "UsdDirectEngine: loading " + std::string{usdPath});
  }
  else
  {
    log.Warn(log::APP, "IngestUsd: unknown adapter '" + std::string{adapter}
                           + "' (expected 'usd_direct' or 'hydra'); skipping.");
    return {};
  }

  pyxis::usd_ingest::StageWalker walker;
  return walker.WalkFile(usdPath, scene);
}

}  // namespace pyxis::app
