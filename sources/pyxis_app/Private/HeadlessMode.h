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

// Returns the process exit code (§41: 0 ok, 2 device init fail).
// Drives the offscreen render-target path + EXR writer.
int RunHeadless(const Configuration& config) noexcept;

// Viewer mode. screenshotPath is the M1 --screenshot debug capture
// (non-empty -> render a few warmup frames, write a PNG, exit 0).
// Empty = normal interactive viewer.
int RunViewer(const Configuration& config,
              std::string_view     screenshotPath) noexcept;

}  // namespace pyxis::app
