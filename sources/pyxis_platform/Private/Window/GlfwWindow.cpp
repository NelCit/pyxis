// Pyxis platform — GLFW window implementation.
//
// glfwInit is reference-counted via a process-wide guard so multiple
// IWindow instances can coexist (regression / soak tests). The headless
// device manager (§5.c) deliberately never reaches this TU.

#include "Window/GlfwWindow.h"

#include <Pyxis/Platform/Logging/Log.h>
#include <Pyxis/Platform/Logging/LogCategories.h>

// GLFW_INCLUDE_VULKAN comes from CMake (target_compile_definitions on
// pyxis_platform's GLFW use site) so the macro doesn't have to be a
// PYXIS_*-prefixed define here, which clang-tidy's identifier-naming
// would otherwise flag.
#include <GLFW/glfw3.h>

#include <atomic>
#include <string>

namespace pyxis {

namespace {

std::atomic<int> sGlfwRefcount{0};

bool EnsureGlfwInit() noexcept {
    if (sGlfwRefcount.fetch_add(1) > 0) return true;
    if (!glfwInit()) {
        Logging::Get().Error(log::PLATFORM, "glfwInit failed");
        sGlfwRefcount.fetch_sub(1);
        return false;
    }
    glfwSetErrorCallback([](int code, const char* msg) {
        const std::string formatted = "GLFW error " + std::to_string(code) + ": " + (msg ? msg : "?");
        Logging::Get().Error(log::PLATFORM, formatted);
    });
    return true;
}

void ReleaseGlfwInit() noexcept {
    if (sGlfwRefcount.fetch_sub(1) == 1) {
        glfwTerminate();
    }
}

}  // namespace

IWindow::~IWindow() = default;

// ---------------------------------------------------------------------------
// GlfwWindow
// ---------------------------------------------------------------------------

GlfwWindow::GlfwWindow(const WindowDesc& desc) noexcept {
    if (!EnsureGlfwInit()) return;

    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);          // we drive Vulkan ourselves
    glfwWindowHint(GLFW_RESIZABLE,  desc.resizable ? GLFW_TRUE : GLFW_FALSE);
    glfwWindowHint(GLFW_VISIBLE,    GLFW_TRUE);

    const std::string title{ desc.title };
    _handle = glfwCreateWindow(static_cast<int>(desc.width),
                               static_cast<int>(desc.height),
                               title.c_str(), nullptr, nullptr);
    if (!_handle) {
        Logging::Get().Error(log::PLATFORM, "glfwCreateWindow failed");
        ReleaseGlfwInit();
        return;
    }

    glfwSetWindowUserPointer(_handle, this);
    glfwSetWindowSizeCallback (_handle, &GlfwWindow::OnSize);
    glfwSetKeyCallback        (_handle, &GlfwWindow::OnKey);
    glfwSetMouseButtonCallback(_handle, &GlfwWindow::OnMouseButton);
    glfwSetCursorPosCallback  (_handle, &GlfwWindow::OnCursorPos);
    glfwSetScrollCallback     (_handle, &GlfwWindow::OnScroll);
    glfwSetWindowCloseCallback(_handle, &GlfwWindow::OnClose);
}

GlfwWindow::~GlfwWindow() {
    if (_handle) {
        glfwDestroyWindow(_handle);
        _handle = nullptr;
    }
    ReleaseGlfwInit();
}

void GlfwWindow::PollEvents() {
    if (_handle) glfwPollEvents();
}

bool GlfwWindow::ShouldClose() const {
    return _handle && glfwWindowShouldClose(_handle);
}

uint32_t GlfwWindow::Width() const {
    if (!_handle) return 0;
    int width = 0, height = 0;
    glfwGetFramebufferSize(_handle, &width, &height);
    return static_cast<uint32_t>(width);
}

uint32_t GlfwWindow::Height() const {
    if (!_handle) return 0;
    int width = 0, height = 0;
    glfwGetFramebufferSize(_handle, &width, &height);
    return static_cast<uint32_t>(height);
}

void GlfwWindow::SetEventSink(InputEventSink sink) {
    _sink = std::move(sink);
}

VkSurfaceKHR GlfwWindow::CreateVulkanSurface(VkInstance instance) {
    if (!_handle) return VK_NULL_HANDLE;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    const VkResult result = glfwCreateWindowSurface(instance, _handle, nullptr, &surface);
    if (result != VK_SUCCESS) {
        Logging::Get().Error(log::PLATFORM, "glfwCreateWindowSurface failed");
        return VK_NULL_HANDLE;
    }
    return surface;
}

void* GlfwWindow::NativeHandle() const {
    return _handle;
}

GlfwWindow* GlfwWindow::From(GLFWwindow* window) noexcept {
    return static_cast<GlfwWindow*>(glfwGetWindowUserPointer(window));
}

void GlfwWindow::OnSize(GLFWwindow* window, int width, int height) {
    auto* self = From(window);
    if (!self || !self->_sink) return;
    InputEvent event{};
    event.kind   = InputEventKind::WindowResize;
    event.width  = static_cast<uint32_t>(width  > 0 ? width  : 0);
    event.height = static_cast<uint32_t>(height > 0 ? height : 0);
    self->_sink(event);
}

void GlfwWindow::OnKey(GLFWwindow* window, int key, int /*scancode*/, int action, int mods) {
    auto* self = From(window);
    if (!self || !self->_sink) return;
    InputEvent event{};
    event.kind = (action == GLFW_RELEASE) ? InputEventKind::KeyUp : InputEventKind::KeyDown;
    event.key  = key;
    event.mods = static_cast<uint32_t>(mods);
    self->_sink(event);
}

void GlfwWindow::OnMouseButton(GLFWwindow* window, int button, int /*action*/, int mods) {
    auto* self = From(window);
    if (!self || !self->_sink) return;
    InputEvent event{};
    event.kind = InputEventKind::MouseButton;
    event.key  = button;
    event.mods = static_cast<uint32_t>(mods);
    self->_sink(event);
}

void GlfwWindow::OnCursorPos(GLFWwindow* window, double posX, double posY) {
    auto* self = From(window);
    if (!self || !self->_sink) return;
    InputEvent event{};
    event.kind   = InputEventKind::MouseMove;
    event.mouseX = posX;
    event.mouseY = posY;
    self->_sink(event);
}

void GlfwWindow::OnScroll(GLFWwindow* window, double deltaX, double deltaY) {
    auto* self = From(window);
    if (!self || !self->_sink) return;
    InputEvent event{};
    event.kind     = InputEventKind::MouseScroll;
    event.scrollDx = deltaX;
    event.scrollDy = deltaY;
    self->_sink(event);
}

void GlfwWindow::OnClose(GLFWwindow* window) {
    auto* self = From(window);
    if (!self || !self->_sink) return;
    InputEvent event{};
    event.kind = InputEventKind::WindowClose;
    self->_sink(event);
}

// ---------------------------------------------------------------------------
// Public factory.
// ---------------------------------------------------------------------------

IWindow* CreateGlfwWindow(const WindowDesc& desc) noexcept {
    auto* window = new GlfwWindow(desc);
    if (window->NativeHandle() == nullptr) { delete window; return nullptr; }
    return window;
}

}  // namespace pyxis
