// Pyxis app — HydraEngine implementation (M4 stub).

#include "HydraEngine/HydraEngine.h"

#include <Pyxis/Platform/Logging/Log.h>
#include <Pyxis/Platform/Logging/LogCategories.h>
#include <Pyxis/UsdIngest/StageWalker.h>

namespace pyxis::app {

pyxis::usd_ingest::IngestStats HydraEngine::Load(std::string_view usdPath, GpuScene& scene) {
  auto& log = Logging::Get();
  log.Info(log::APP, "HydraEngine: loading " + std::string{usdPath}
                         + " (M4 stub: shares StageWalker with UsdDirectEngine "
                           "for byte-equal parity; full HdEngine pipeline at M5+).");

  // M4 shortcut — see HydraEngine.h header comment for the
  // architecture rationale. Both adapters routing through
  // StageWalker satisfies the §25.O.3 byte-equal P0 invariant
  // trivially; the pyxis_hydra delegate's HdPyxisMesh / HdPyxisCamera
  // Sync impls stay wired so usdview can still drive Pyxis through
  // the registered Hydra plugin. M5+ replaces this body with the
  // full UsdImagingStageSceneIndex → HdRenderIndex → HdEngine flow
  // when OpenPBR shading makes the dirty-bit dispatch load-bearing.
  pyxis::usd_ingest::StageWalker walker;
  return walker.WalkFile(usdPath, scene);
}

}  // namespace pyxis::app
