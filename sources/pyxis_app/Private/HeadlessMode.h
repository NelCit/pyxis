// Pyxis app — headless mode driver.
//
// Plan §1 / §41. M0 ships the no-frame variant: opens a headless device,
// initialises SceneWorld, runs world.progress() once to prove the phase
// pipeline executes, prints adapter info, exits 0. Real EXR writing lands
// at M2.

#pragma once

namespace pyxis::app {

// Returns the process exit code (§41: 0 ok, 2 device init fail).
int RunHeadless(int adapterIndex, bool enableValidation) noexcept;

// Same shape as RunHeadless but using the windowed VkDeviceManager. M0
// does not actually open a window; M1 wires GLFW.
int RunViewer(int adapterIndex, bool enableValidation) noexcept;

}  // namespace pyxis::app
