// Pyxis app — unified USD ingest entry point.
//
// Replaces the two near-empty wrapper classes UsdDirectEngine +
// HydraEngine, both of which had collapsed to single-line shims
// around `walker.WalkFile`. The string identifier ("usd_direct" /
// "hydra") still selects which adapter logs its banner; the M5+
// proper HdEngine pipeline that replaces the M4 StageWalker
// shortcut will branch here, in one place, instead of growing two
// parallel files.
//
// Plan §1 / §3 / §25.O. Both adapters MUST produce byte-identical
// IngestStats for the §25.O.3 P0 invariant — the unified call site
// makes that contract trivial: every adapter goes through the same
// StageWalker today, so any divergence is intentional and visible
// here.

#pragma once

#include <Pyxis/UsdIngest/StageWalker.h>

#include <string_view>

namespace pyxis {
class GpuScene;
}  // namespace pyxis

namespace pyxis::app {

// Open `usdPath`, walk the stage, push mutations into `scene` via the
// public §18 API. `adapter` is the value of `config.app.ingest`
// ("usd_direct" or "hydra") — at M4 both branches share the same
// StageWalker for byte-equal parity, so the dispatch is a banner-log
// only. Returns the IngestResult from StageWalker (counts + per-stage
// timings + opaque camera list); on a missing / unloadable path
// every counter is zero and the caller should fall back to the
// hardcoded cube.
//
// `populationMask` (V2.A.15): comma-separated list of SdfPath prefixes
// that scope the load. Empty = `UsdStage::Open` (full stage). Non-empty
// = `UsdStage::OpenMasked` so we only realise prims under those paths.
//
// `frameNumber` (V2.A.13): time-code at which to evaluate the stage.
// Negative = `UsdTimeCode::Default()` (no animation, default-time
// fallthrough). Animated xform / camera attrs respect this value.
//
// `loadMode` (V2.A.15): payload-load policy passed to UsdStage::Open
// / OpenMasked. Recognised values:
//   - "" / "all"            → InitialLoadSet::LoadAll (default).
//   - "none" / "metadata"   → InitialLoadSet::LoadNone — open with all
//                              payloads unloaded; intended for slicing
//                              huge stages via `populationMask`.
// Unknown values log a warning + fall back to LoadAll.
//
// `variantSelections` (V2.A.2): comma-separated
// `<primPath>:<setName>=<value>` triples authored into the stage's
// session layer after Open (so the override is transient and never
// touches the root layer on disk). The stage recomposes automatically;
// WalkStage observes the chosen variants. Empty = honour authored
// variantSelection.
pyxis::usd_ingest::IngestResult IngestUsd(std::string_view adapter,
                                          std::string_view usdPath,
                                          GpuScene& scene,
                                          std::string_view populationMask = {},
                                          double frameNumber = -1.0,
                                          std::string_view loadMode = {},
                                          std::string_view variantSelections = {});

}  // namespace pyxis::app
