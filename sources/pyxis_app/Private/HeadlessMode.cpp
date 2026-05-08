// Pyxis app — headless + viewer mode drivers (M0).

#include "HeadlessMode.h"

#include <Pyxis/Platform/Device/DeviceCreationParams.h>
#include <Pyxis/Platform/Device/IDeviceManager.h>
#include <Pyxis/Platform/Device/Resolution.h>
#include <Pyxis/Platform/Logging/Log.h>
#include <Pyxis/Platform/Logging/LogCategories.h>
#include <Pyxis/Platform/Window/IWindow.h>
#include <Pyxis/Renderer/SceneWorldFacade.h>

#include <memory>

namespace pyxis::app {

namespace {

constexpr int EXIT_OK             = 0;
constexpr int EXIT_DEVICE_INIT_FAIL = 2;

int RunCommon(IDeviceManager* dm) noexcept {
    auto& log = Logging::Get();

    SceneWorldFacade scene;
    const SceneWorldStatus s = scene.Init();
    if (s != SceneWorldStatus::Ok) {
        log.Error(log::RENDER, "SceneWorldFacade::Init failed");
        return EXIT_DEVICE_INIT_FAIL;
    }

    // Tick once so the M0 phase pipeline runs at least one round-trip
    // (the SceneWorldInit unit test asserts the same in isolation; this
    // is the whole-process variant).
    scene.Tick();

    log.Info(log::APP, "M0 skeleton OK — exiting cleanly");
    scene.Shutdown();
    delete dm;
    return EXIT_OK;
}

}  // namespace

int RunHeadless(int adapterIndex, bool enableValidation) noexcept {
    auto& log = Logging::Get();
    DeviceCreationParams params{};
    params.adapterIndex     = adapterIndex;
    params.enableValidation = enableValidation;
    params.framesInFlight   = 3;
    params.applicationName  = "pyxis (headless)";

    const Resolution              backbuffer{ 1920, 1080 };
    DeviceManagerCreateStatus     status = DeviceManagerCreateStatus::Unknown;
    IDeviceManager* dm = CreateHeadlessDeviceManager(params, backbuffer, &status);
    if (dm == nullptr) {
        log.Error(log::PLATFORM, "CreateHeadlessDeviceManager failed");
        return EXIT_DEVICE_INIT_FAIL;
    }
    return RunCommon(dm);
}

int RunViewer(int adapterIndex, bool enableValidation) noexcept {
    auto& log = Logging::Get();

    // M1 commit 2: open a real window + swapchain. The frame loop with
    // TrianglePass + ImGui FPS panel lands at commit 5; for now we open,
    // tick SceneWorld once (smoke), and tear down cleanly.
    WindowDesc winDesc{};
    winDesc.width  = 1920;
    winDesc.height = 1080;
    winDesc.title  = "Pyxis";

    IWindow* window = CreateGlfwWindow(winDesc);
    if (!window) {
        log.Error(log::PLATFORM, "CreateGlfwWindow failed");
        return EXIT_DEVICE_INIT_FAIL;
    }

    DeviceCreationParams params{};
    params.adapterIndex     = adapterIndex;
    params.enableValidation = enableValidation;
    params.framesInFlight   = 2;
    params.applicationName  = "pyxis";

    const Resolution              backbuffer{ 1920, 1080 };
    DeviceManagerCreateStatus     status = DeviceManagerCreateStatus::Unknown;
    IDeviceManager* dm = CreateWindowedDeviceManager(params, window, backbuffer, &status);
    if (dm == nullptr) {
        log.Error(log::PLATFORM, "CreateWindowedDeviceManager failed");
        delete window;
        return EXIT_DEVICE_INIT_FAIL;
    }

    const int rc = RunCommon(dm);
    delete window;
    return rc;
}

}  // namespace pyxis::app
