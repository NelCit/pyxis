// Pyxis app — headless / viewer mode entrypoints.
//
// Plan §1 / §41. Both modes accept the resolved Configuration (post
// JSON-overlay + CLI-override per §29.1) so adapter selection, render
// dims, output paths, validation toggles, etc. all flow through the
// same struct.

#pragma once

#include <string_view>

namespace pyxis::app {

struct Configuration;
struct ResolvedScene;

// Returns the process exit code (§41: 0 ok, 2 device init fail).
// Drives the offscreen render-target path + EXR writer. `scene` is
// the §29.4.a chain result; the mode forwards it through IngestUsd()
// (which dispatches by `config.app.ingest` to the hydra / usd_direct
// adapter), or falls back to the M3 hardcoded cube if the resolved
// path is unloadable.
//
// `saveAovList` (M7 follow-up): comma-separated names of raw AOVs to
// dump alongside the regular BGRA8 EXR. See CliArgs::saveAov for the
// recognised names ("color,normal,depth,instanceId,materialId,
// baseColor,worldPos" + the "all" alias). Each dump lands at
// `<output-prefix>_<aov>.exr` where the prefix is the BGRA8 path
// stripped of its `.exr` extension. Empty = no extra writes.
//
// `benchFrames` (M8b): when non-zero, after the regular single-frame
// render + EXR write the harness records `benchFrames` warm-up frames
// + `benchFrames` measurement frames, then prints a §34 KPI table to
// stdout (per-pass GPU + CPU min/p50/p99/max ms). Used to gate KPI
// compliance for the §41 perf milestones (M8b World Lobby, lobby, ...).
//
// `profilePath` (M10 — plan §35 / §36.6): when non-empty, after the
// run completes (incl. any benchmark window), the headless harness
// writes a single-document JSON describing GPU / driver / scene /
// per-pass timing percentiles to that path. Consumed by
// `_tools/run_regression.py` which merges it with its own image-diff
// metrics into the rolling per-test KPI CSV.
int RunHeadless(const Configuration& config, const ResolvedScene& scene,
                std::string_view saveAovList = {},
                uint32_t benchFrames = 0,
                std::string_view profilePath = {}) noexcept;

// Viewer mode. screenshotPath is the M1 --screenshot debug capture
// (non-empty -> render a few warmup frames, write a PNG, exit 0).
// Empty = normal interactive viewer.
//
// `shaderRebuildDir` (M7 follow-up): explicit CMake build directory
// the Reload Shaders button spawns ShaderMake against. Empty falls
// back to the cwd walk-up heuristic; supply when the binary lives
// outside the build tree (packaged install, demo launcher, etc.).
int RunViewer(const Configuration& config, const ResolvedScene& scene,
              std::string_view screenshotPath,
              std::string_view shaderRebuildDir = {}) noexcept;

}  // namespace pyxis::app
