// Pyxis app — ViewerMode frame loop.

#include "ViewerMode.h"

#include <Pyxis/Platform/Device/DeviceCreationParams.h>
#include <Pyxis/Platform/Device/IDeviceManager.h>
#include <Pyxis/Platform/Device/Resolution.h>
#include <Pyxis/Platform/Logging/Log.h>
#include <Pyxis/Platform/Logging/LogCategories.h>
#include <Pyxis/Platform/Window/IWindow.h>
#include <Pyxis/Renderer/Descs/RenderSettings.h>
#include <Pyxis/Renderer/Descs/RenderTargets.h>
#include <Pyxis/Renderer/Descs/RendererCreateDesc.h>
#include <Pyxis/Renderer/Profiler.h>
#include <Pyxis/Renderer/PyxisRenderer.h>
#include <Pyxis/Renderer/SceneWorldFacade.h>

#include <nvrhi/nvrhi.h>

#include <atomic>
#include <memory>

namespace pyxis::app {

namespace {

constexpr int EXIT_OK               = 0;
constexpr int EXIT_DEVICE_INIT_FAIL = 2;

// GLFW key code for Escape. Hard-coded to avoid pulling <GLFW/glfw3.h>
// into pyxis_app (glfw is PRIVATE-linked from pyxis_platform). The
// value has been 256 since GLFW 3.0 and matches USB HID Escape — stable
// enough for "Escape closes the viewer" UX.
constexpr int GLFW_ESCAPE_KEY_CODE  = 256;

}  // namespace

int RunViewerLoop(int adapterIndex, bool enableValidation) noexcept {
    auto& log = Logging::Get();

    // ---- Window ----------------------------------------------------------
    WindowDesc winDesc{};
    winDesc.width  = 1920;
    winDesc.height = 1080;
    winDesc.title  = "Pyxis";

    const std::unique_ptr<IWindow> window{ CreateGlfwWindow(winDesc) };
    if (!window) {
        log.Error(log::PLATFORM, "ViewerMode: CreateGlfwWindow failed");
        return EXIT_DEVICE_INIT_FAIL;
    }

    // Hook a close-on-Escape sink alongside the window's own close button.
    std::atomic<bool> shouldClose{false};
    window->SetEventSink([&](const InputEvent& e) {
        if (e.kind == InputEventKind::WindowClose) shouldClose.store(true);
        if (e.kind == InputEventKind::KeyDown && e.key == GLFW_ESCAPE_KEY_CODE) {
            shouldClose.store(true);
        }
    });

    // ---- Device manager --------------------------------------------------
    DeviceCreationParams params{};
    params.adapterIndex     = adapterIndex;
    params.enableValidation = enableValidation;
    params.framesInFlight   = 2;
    params.applicationName  = "pyxis";

    const Resolution backbuffer{ winDesc.width, winDesc.height };
    DeviceManagerCreateStatus status = DeviceManagerCreateStatus::Unknown;
    const std::unique_ptr<IDeviceManager> dm{
        CreateWindowedDeviceManager(params, window.get(), backbuffer, &status) };
    if (!dm) {
        log.Error(log::PLATFORM, "ViewerMode: device manager init failed");
        return EXIT_DEVICE_INIT_FAIL;
    }
    nvrhi::IDevice* device = dm->GetDevice();
    if (!device) {
        log.Error(log::PLATFORM, "ViewerMode: nvrhi::IDevice not available");
        return EXIT_DEVICE_INIT_FAIL;
    }

    // ---- SceneWorld (M0 carry-over so the phase pipeline still ticks) ---
    SceneWorldFacade scene;
    if (scene.Init() != SceneWorldStatus::Ok) {
        log.Error(log::RENDER, "ViewerMode: SceneWorldFacade::Init failed");
        return EXIT_DEVICE_INIT_FAIL;
    }

    // ---- Profiler + Renderer --------------------------------------------
    Profiler profiler{ device };
    RendererCreateDesc rendererDesc{};
    rendererDesc.initialWidth  = winDesc.width;
    rendererDesc.initialHeight = winDesc.height;
    PyxisRenderer renderer{ device, profiler, rendererDesc };

    // ---- Per-frame command-list ring -------------------------------------
    constexpr uint32_t MAX_FRAMES_IN_FLIGHT = 3;
    std::array<nvrhi::CommandListHandle, MAX_FRAMES_IN_FLIGHT> commandLists;
    for (auto& cl : commandLists) {
        cl = device->createCommandList();
    }

    log.Info(log::APP, "ViewerMode: entering frame loop");

    // ---- Frame loop ------------------------------------------------------
    uint64_t frameIndex = 0;
    const uint32_t framesInFlight = dm->GetFramesInFlight();
    while (!window->ShouldClose() && !shouldClose.load()) {
        window->PollEvents();
        scene.Tick();
        profiler.BeginFrame();
        dm->BeginFrame();

        nvrhi::ITexture* backbuffer = dm->GetCurrentBackbuffer();
        if (backbuffer) {
            const uint32_t slot = static_cast<uint32_t>(frameIndex % framesInFlight);
            nvrhi::ICommandList* cl = commandLists[slot].Get();

            cl->open();
            RenderTargets targets{};
            targets.color = backbuffer;
            RenderSettings settings{};
            settings.width  = backbuffer->getDesc().width;
            settings.height = backbuffer->getDesc().height;

            renderer.RenderFrame(cl, settings, targets);

            // Transition back to Present for the swapchain.
            cl->setTextureState(backbuffer, nvrhi::AllSubresources, nvrhi::ResourceStates::Present);
            cl->commitBarriers();
            cl->close();
            device->executeCommandList(cl);
        }

        dm->EndFrame();
        profiler.EndFrame();
        ++frameIndex;
    }

    log.Info(log::APP, "ViewerMode: frame loop exited; tearing down");
    dm->WaitIdle();
    return EXIT_OK;
}

}  // namespace pyxis::app
