// Pyxis platform — GLFW window implementation (Private/).

#pragma once

#include <Pyxis/Platform/Window/IWindow.h>

struct GLFWwindow;

namespace pyxis {

class GlfwWindow final : public IWindow {
 public:
  explicit GlfwWindow(const WindowDesc& desc) noexcept;
  ~GlfwWindow() override;

  void PollEvents() override;
  [[nodiscard]] bool ShouldClose() const override;
  [[nodiscard]] uint32_t Width() const override;
  [[nodiscard]] uint32_t Height() const override;
  void SetEventSink(InputEventSink sink) override;
  [[nodiscard]] VkSurfaceKHR CreateVulkanSurface(VkInstance instance) override;
  [[nodiscard]] void* NativeHandle() const override;

 private:
  static GlfwWindow* From(GLFWwindow* window) noexcept;
  static void OnSize(GLFWwindow* window, int width, int height);
  static void OnKey(GLFWwindow* window, int key, int scancode, int action, int mods);
  static void OnMouseButton(GLFWwindow* window, int button, int action, int mods);
  static void OnCursorPos(GLFWwindow* window, double posX, double posY);
  static void OnScroll(GLFWwindow* window, double deltaX, double deltaY);
  static void OnClose(GLFWwindow* window);

  GLFWwindow* _handle = nullptr;
  InputEventSink _sink;
};

}  // namespace pyxis
