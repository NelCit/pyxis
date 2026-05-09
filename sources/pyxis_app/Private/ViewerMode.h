// Pyxis app — ViewerMode driver.
//
// Plan §1 / §41 M1. RunViewer:
//   1. Open a GLFW window.
//   2. Create the windowed VkDeviceManager (Vulkan + nvrhi::IDevice +
//      swapchain).
//   3. Construct Profiler + PyxisRenderer.
//   4. Frame loop until ShouldClose():
//        PollEvents →
//        profiler.BeginFrame →
//        deviceManager.BeginFrame (vkAcquireNextImageKHR) →
//        open commandList →
//        renderer.RenderFrame(commandList, settings, { color = currentBackbuffer }) →
//        close + execute commandList →
//        deviceManager.EndFrame (vkQueuePresentKHR) →
//        profiler.EndFrame.
//   5. Wait idle, tear down.
//
// When `screenshotPath` is non-empty, the loop runs a small fixed number
// of warmup frames, copies the backbuffer to a host-mapped staging
// texture, swizzles BGRA→RGBA, writes a PNG to <path> via stb_image_write,
// and exits with code 0. This is the §35 image-regression artefact for
// M1's hard-coded triangle.
//
// Returns the process exit code (0 ok / 2 device init fail).

#pragma once

#include <string_view>

namespace pyxis::app {

struct Configuration;
struct ResolvedScene;

int RunViewerLoop(const Configuration& config, const ResolvedScene& resolvedScene,
                  std::string_view screenshotPath) noexcept;

}  // namespace pyxis::app
