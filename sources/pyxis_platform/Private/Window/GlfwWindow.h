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
    [[nodiscard]] bool     ShouldClose() const override;
    [[nodiscard]] uint32_t Width()       const override;
    [[nodiscard]] uint32_t Height()      const override;
    void SetEventSink(InputEventSink sink) override;
    [[nodiscard]] VkSurfaceKHR CreateVulkanSurface(VkInstance instance) override;
    [[nodiscard]] void*    NativeHandle() const override;

private:
    static GlfwWindow* From(GLFWwindow* w) noexcept;
    static void OnSize(GLFWwindow* w, int width, int height);
    static void OnKey(GLFWwindow* w, int key, int scancode, int action, int mods);
    static void OnMouseButton(GLFWwindow* w, int button, int action, int mods);
    static void OnCursorPos(GLFWwindow* w, double x, double y);
    static void OnScroll(GLFWwindow* w, double dx, double dy);
    static void OnClose(GLFWwindow* w);

    GLFWwindow*    _handle = nullptr;
    InputEventSink _sink;
};

}  // namespace pyxis
