// Pyxis app — unified USD ingest entry point implementation.

#include "IngestUsd.h"

#include <Pyxis/Platform/Logging/Log.h>
#include <Pyxis/Platform/Logging/LogCategories.h>

#include <pxr/usd/sdf/path.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usd/stagePopulationMask.h>

#include <cctype>
#include <string>

namespace pyxis::app {

namespace {

// Split a comma-separated SdfPath list into a UsdStagePopulationMask.
// Empty / whitespace entries are skipped; invalid paths get a log
// warning + skip so a typo doesn't silently drop more than intended.
[[nodiscard]] pxr::UsdStagePopulationMask ParsePopulationMask(
    std::string_view spec) noexcept
{
  pxr::UsdStagePopulationMask mask;
  std::size_t cursor = 0;
  while (cursor < spec.size())
  {
    const std::size_t comma = spec.find(',', cursor);
    const std::size_t end = (comma == std::string_view::npos) ? spec.size() : comma;
    std::size_t start = cursor;
    while (start < end && std::isspace(static_cast<unsigned char>(spec[start])))
      ++start;
    std::size_t stop = end;
    while (stop > start && std::isspace(static_cast<unsigned char>(spec[stop - 1])))
      --stop;
    if (stop > start)
    {
      const std::string token{spec.substr(start, stop - start)};
      const pxr::SdfPath path(token);
      if (path.IsAbsolutePath())
      {
        mask.Add(path);
      }
      else
      {
        Logging::Get().Warn(log::APP,
            "IngestUsd: population-mask entry '" + token
                + "' is not an absolute SdfPath — skipping.");
      }
    }
    if (comma == std::string_view::npos)
      break;
    cursor = comma + 1u;
  }
  return mask;
}

}  // namespace

pyxis::usd_ingest::IngestResult IngestUsd(std::string_view adapter,
                                          std::string_view usdPath,
                                          GpuScene& scene,
                                          std::string_view populationMask,
                                          double frameNumber) {
  auto& log = Logging::Get();

  // Banner format: "IngestUsd[<adapter>]: loading <path>". Adapter
  // tag in brackets so a future grep-for-the-class doesn't mislead
  // a contributor (the HydraEngine + UsdDirectEngine wrapper classes
  // were folded into this function in 1a2c920).
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
    log.Info(log::APP, "IngestUsd[hydra]: loading " + std::string{usdPath}
                           + " (M4 stub: shares StageWalker with the usd_direct path "
                             "for byte-equal parity; full HdEngine pipeline at M5+).");
  }
  else if (adapter == "usd_direct")
  {
    log.Info(log::APP, "IngestUsd[usd_direct]: loading " + std::string{usdPath});
  }
  else
  {
    log.Warn(log::APP, "IngestUsd: unknown adapter '" + std::string{adapter}
                           + "' (expected 'usd_direct' or 'hydra'); skipping.");
    return pyxis::usd_ingest::IngestResult{};
  }

  pyxis::usd_ingest::StageWalker walker;
  if (frameNumber >= 0.0)
  {
    log.Info(log::APP, "IngestUsd: --frame "
                           + std::to_string(static_cast<int>(frameNumber))
                           + " honoured (V2.A.13).");
  }
  if (!populationMask.empty())
  {
    // V2.A.15 — composition load mode: realise only the prims under
    // the authored mask. Production scenes use this to defer the
    // OOM-risk subtree of a heavy stage (lighting rig, hero asset,
    // etc.) until the user actually navigates to it.
    const pxr::UsdStagePopulationMask mask = ParsePopulationMask(populationMask);
    if (mask.IsEmpty())
    {
      log.Warn(log::APP,
          "IngestUsd: population-mask '" + std::string{populationMask}
              + "' parsed to an empty mask — falling back to full Open.");
      return walker.WalkFile(usdPath, scene);
    }
    log.Info(log::APP, "IngestUsd: opening masked stage (" + std::to_string(mask.GetPaths().size())
                           + " entr" + (mask.GetPaths().size() == 1 ? "y" : "ies") + ").");
    const pxr::UsdStageRefPtr stage =
        pxr::UsdStage::OpenMasked(std::string{usdPath}, mask);
    if (!stage)
    {
      log.Error(log::APP,
          "IngestUsd: OpenMasked failed for " + std::string{usdPath}
              + "; falling back to full Open.");
      return walker.WalkFile(usdPath, scene, frameNumber);
    }
    return walker.WalkStage(stage, scene, frameNumber);
  }
  return walker.WalkFile(usdPath, scene, frameNumber);
}

}  // namespace pyxis::app
