// Pyxis app — UsdDirectEngine implementation.

#include "UsdDirectEngine/UsdDirectEngine.h"

#include <Pyxis/Platform/Logging/Log.h>
#include <Pyxis/Platform/Logging/LogCategories.h>
#include <Pyxis/UsdIngest/StageWalker.h>

namespace pyxis::app {

pyxis::usd_ingest::IngestStats UsdDirectEngine::Load(std::string_view usdPath, GpuScene& scene) {
  auto& log = Logging::Get();
  log.Info(log::APP, "UsdDirectEngine: loading " + std::string{usdPath});

  pyxis::usd_ingest::StageWalker walker;
  return walker.WalkFile(usdPath, scene);
}

}  // namespace pyxis::app
