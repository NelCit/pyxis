// Pyxis platform — IWindow interface. Plan §1 / §5 / §28.
//
// Concrete implementation lives at Private/Window/GlfwWindow.{h,cpp}.
// The viewer device manager (§5.c VkDeviceManager) owns the window;
// the app drives event polling via PollEvents() each frame.

#pragma once

#include <Pyxis/Platform/PlatformApi.h>
#include <Pyxis/Platform/Window/InputEvent.h>
#include <Pyxis/Platform/Window/WindowDesc.h>

#include <cstdint>
#include <functional>

// Vulkan handle forwards — we expose a VkSurfaceKHR creator so the
// device manager can own the surface without including <GLFW/glfw3.h>
// in any of its public headers (§30.3).
using VkInstance = struct VkInstance_T*;
using VkSurfaceKHR = struct VkSurfaceKHR_T*;

namespace pyxis {

// Sink for input events. The app owns the lambda lifetime.
using InputEventSink = std::function<void(const InputEvent&)>;

class PYXIS_PLATFORM_API IWindow {
 public:
  virtual ~IWindow();

  IWindow(const IWindow&) = delete;
  IWindow& operator=(const IWindow&) = delete;

  // Process the OS-level event queue. Drains pending events, calls the
  // attached sink for each.
  virtual void PollEvents() = 0;

  // True iff the user has requested a close (clicked the X / Alt+F4 / etc.).
  [[nodiscard]] virtual bool ShouldClose() const = 0;

  // Current client-area size in backbuffer pixels (§5: render resolution
  // is *backbuffer pixels*, never DIPs).
  [[nodiscard]] virtual uint32_t Width() const = 0;
  [[nodiscard]] virtual uint32_t Height() const = 0;

  // Attach (or replace) the input-event sink. Pass an empty std::function
  // to detach.
  virtual void SetEventSink(InputEventSink sink) = 0;

  // Native-Vulkan-surface creator. The window is the only thing that
  // knows how to create a VkSurfaceKHR for the underlying OS handle
  // (GLFW exposes `glfwCreateWindowSurface` for exactly this).
  [[nodiscard]] virtual VkSurfaceKHR CreateVulkanSurface(VkInstance instance) = 0;

  // The opaque GLFWwindow* — exposed so ImGui's GLFW backend can hook
  // events. Renderer code never touches this. Cast at the use site.
  [[nodiscard]] virtual void* NativeHandle() const = 0;

 protected:
  IWindow() = default;
};

// Free factory. Returns nullptr if window creation failed (logged once).
[[nodiscard]] PYXIS_PLATFORM_API IWindow* CreateGlfwWindow(const WindowDesc& desc) noexcept;

}  // namespace pyxis
