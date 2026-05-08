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
//        dm.BeginFrame (vkAcquireNextImageKHR) →
//        open commandList →
//        renderer.RenderFrame(cl, settings, { color = currentBackbuffer }) →
//        close + execute commandList →
//        dm.EndFrame (vkQueuePresentKHR) →
//        profiler.EndFrame.
//   5. Wait idle, tear down.
//
// Returns the process exit code (0 ok / 2 device init fail).

#pragma once

namespace pyxis::app {

int RunViewerLoop(int adapterIndex, bool enableValidation) noexcept;

}  // namespace pyxis::app
