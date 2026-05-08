// Pyxis app — headless mode driver.
//
// Plan §1 / §41. M0 ships the no-frame variant: opens a headless device,
// initialises SceneWorld, runs world.progress() once to prove the phase
// pipeline executes, prints adapter info, exits 0. Real EXR writing lands
// at M2.

#pragma once

#include <string_view>

namespace pyxis::app {

// Returns the process exit code (§41: 0 ok, 2 device init fail).
int RunHeadless(int adapterIndex, bool enableValidation) noexcept;

// Same shape as RunHeadless but using the windowed VkDeviceManager.
// `screenshotPath` is the optional --screenshot <path> CLI flag — when
// non-empty, the viewer runs a small fixed number of warmup frames,
// captures the backbuffer to PNG at <path>, and exits with code 0.
int RunViewer(int adapterIndex, bool enableValidation,
              std::string_view screenshotPath) noexcept;

}  // namespace pyxis::app
