// Pyxis app — unified USD ingest entry point implementation.

#include "IngestUsd.h"

#include "VariantParser.h"

#include <Pyxis/Platform/Logging/Log.h>
#include <Pyxis/Platform/Logging/LogCategories.h>

#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/sdf/path.h>
#include <pxr/usd/usd/editContext.h>
#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usd/stagePopulationMask.h>
#include <pxr/usd/usd/variantSets.h>

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

// V2.A.15 — translate the --load-mode value into USD's payload-load
// enum. Unknown strings warn + fall back to LoadAll so a typo doesn't
// silently open the stage without payloads.
[[nodiscard]] pxr::UsdStage::InitialLoadSet ResolveLoadMode(
    std::string_view loadMode) noexcept
{
  if (loadMode.empty() || loadMode == "all")
    return pxr::UsdStage::LoadAll;
  if (loadMode == "none" || loadMode == "metadata")
    return pxr::UsdStage::LoadNone;
  Logging::Get().Warn(log::APP,
      "IngestUsd: unknown --load-mode '" + std::string{loadMode}
          + "' (expected 'all' / 'none' / 'metadata'); falling back to LoadAll.");
  return pxr::UsdStage::LoadAll;
}

[[nodiscard]] const char* LoadModeLabel(pxr::UsdStage::InitialLoadSet set) noexcept
{
  switch (set)
  {
    case pxr::UsdStage::LoadAll:  return "LoadAll";
    case pxr::UsdStage::LoadNone: return "LoadNone";
  }
  return "<unknown>";
}

// V2.A.2 — apply the parsed `--variant` entries to the stage's session
// layer. Session-layer authoring is transient (UsdStage::GetSessionLayer
// is a memory-resident scratch layer; nothing reaches the root layer on
// disk), so the override lasts only for this ingest. The stage
// recomposes after each SetVariantSelection call; WalkStage's
// stage->Traverse() picks up the new composition automatically.
//
// Returns the number of entries actually authored (failed lookups + bad
// VariantSet names are logged as warnings and skipped).
uint32_t ApplyVariantSelections(const pxr::UsdStageRefPtr& stage,
                                std::string_view spec) noexcept
{
  if (!stage || spec.empty())
    return 0u;
  const std::vector<VariantSelection> entries = ParseVariantSelections(spec);
  if (entries.empty())
  {
    Logging::Get().Warn(log::APP,
        "IngestUsd: --variant '" + std::string{spec}
            + "' parsed to zero usable entries — expected "
              "<primPath>:<setName>=<value>[,...]; ignored.");
    return 0u;
  }

  // EditTarget the session layer so the override never touches the
  // root layer's on-disk USD. The UsdEditContext destructor restores
  // the previous edit target when this function returns.
  const pxr::UsdEditContext editCtx(stage, stage->GetSessionLayer());
  auto& log = Logging::Get();
  uint32_t applied = 0u;
  for (const VariantSelection& entry : entries)
  {
    const pxr::SdfPath path(entry.primPath);
    if (!path.IsAbsolutePath())
    {
      log.Warn(log::APP, "IngestUsd: --variant entry '"
                             + entry.primPath + ":" + entry.setName + "="
                             + entry.value + "' has a non-absolute path; skipping.");
      continue;
    }
    const pxr::UsdPrim prim = stage->GetPrimAtPath(path);
    if (!prim.IsValid())
    {
      log.Warn(log::APP, "IngestUsd: --variant entry path '"
                             + entry.primPath
                             + "' resolves to no prim on the stage; skipping.");
      continue;
    }
    pxr::UsdVariantSet variantSet = prim.GetVariantSet(entry.setName);
    if (!variantSet.IsValid())
    {
      log.Warn(log::APP, "IngestUsd: --variant entry '"
                             + entry.primPath + ":" + entry.setName
                             + "' has no such variant set on the prim; skipping.");
      continue;
    }
    if (!variantSet.HasAuthoredVariant(entry.value))
    {
      // Authored variant names are case-sensitive and exhaustive;
      // we don't fuzzy-match. Surface as a warning + skip so the
      // stage stays on its authored selection.
      log.Warn(log::APP, "IngestUsd: --variant entry '"
                             + entry.primPath + ":" + entry.setName + "=" + entry.value
                             + "' references an unknown variant; skipping.");
      continue;
    }
    if (variantSet.SetVariantSelection(entry.value))
    {
      log.Info(log::APP, "IngestUsd: --variant applied "
                             + entry.primPath + ":" + entry.setName + "=" + entry.value);
      ++applied;
    }
    else
    {
      log.Warn(log::APP, "IngestUsd: --variant SetVariantSelection failed for '"
                             + entry.primPath + ":" + entry.setName + "=" + entry.value
                             + "' (USD declined the edit).");
    }
  }
  return applied;
}

}  // namespace

pyxis::usd_ingest::IngestResult IngestUsd(std::string_view adapter,
                                          std::string_view usdPath,
                                          GpuScene& scene,
                                          std::string_view populationMask,
                                          double frameNumber,
                                          std::string_view loadMode,
                                          std::string_view variantSelections) {
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

  // V2.A.15 — payload-load policy. Default `LoadAll` matches v1
  // behaviour; `none`/`metadata` open the stage with payloads
  // unloaded so the operator can scope a 100 GB asset down to the
  // root prim + `--population-mask` before paying for any payload
  // I/O. The label is logged for diagnosability.
  const pxr::UsdStage::InitialLoadSet loadSet = ResolveLoadMode(loadMode);
  if (loadSet != pxr::UsdStage::LoadAll || !loadMode.empty())
  {
    log.Info(log::APP, std::string{"IngestUsd: --load-mode "}
                           + std::string{loadMode.empty() ? "all" : loadMode}
                           + " -> InitialLoadSet::" + LoadModeLabel(loadSet) + ".");
  }

  // Common case: no load-mode override, no mask, no variants → fall
  // through to WalkFile which opens the stage itself + populates the
  // full per-phase timing breakdown (§34.1 load summary). This keeps
  // the common path byte-for-byte identical to the M4 / V2.A.13
  // baseline.
  const bool needsCustomOpen =
      !populationMask.empty()
      || !variantSelections.empty()
      || (loadSet != pxr::UsdStage::LoadAll);
  if (!needsCustomOpen)
    return walker.WalkFile(usdPath, scene, frameNumber);

  // Custom-open path: we open the stage ourselves so we can apply
  // variant overrides + pick the population-mask + load-set. WalkStage
  // doesn't fold its own stage-open timing into the result (the caller
  // owns the open); the §34.1 summary's stageOpenMs is 0 on this
  // branch, which is acceptable — the masked / variant runs aren't
  // the perf-baselined cold-cache load.
  pxr::UsdStageRefPtr stage;
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
      stage = pxr::UsdStage::Open(std::string{usdPath}, loadSet);
    }
    else
    {
      log.Info(log::APP, "IngestUsd: opening masked stage ("
                             + std::to_string(mask.GetPaths().size())
                             + " entr" + (mask.GetPaths().size() == 1 ? "y" : "ies") + ").");
      stage = pxr::UsdStage::OpenMasked(std::string{usdPath}, mask, loadSet);
    }
  }
  else
  {
    stage = pxr::UsdStage::Open(std::string{usdPath}, loadSet);
  }

  if (!stage)
  {
    log.Error(log::APP, "IngestUsd: stage open failed for "
                            + std::string{usdPath} + "; returning empty result.");
    return pyxis::usd_ingest::IngestResult{};
  }

  // V2.A.2 — variant overrides authored on the session layer (transient,
  // never touches root layer on disk). Done AFTER Open so the stage's
  // VariantSet lookups see the authored composition; done BEFORE
  // WalkStage so the recomposition is visible to stage->Traverse().
  if (!variantSelections.empty())
  {
    const uint32_t applied = ApplyVariantSelections(stage, variantSelections);
    log.Info(log::APP, "IngestUsd: --variant applied " + std::to_string(applied)
                           + " selection(s) to session layer.");
  }

  return walker.WalkStage(stage, scene, frameNumber);
}

}  // namespace pyxis::app
