// Pyxis app — headless + viewer mode drivers (M0).

#include "HeadlessMode.h"

#include "ViewerMode.h"

#include <Pyxis/Platform/Device/DeviceCreationParams.h>
#include <Pyxis/Platform/Device/IDeviceManager.h>
#include <Pyxis/Platform/Device/Resolution.h>
#include <Pyxis/Platform/Logging/Log.h>
#include <Pyxis/Platform/Logging/LogCategories.h>
#include <Pyxis/Renderer/SceneWorldFacade.h>

#include <memory>

namespace pyxis::app {

namespace {

constexpr int EXIT_OK             = 0;
constexpr int EXIT_DEVICE_INIT_FAIL = 2;

int RunCommon(IDeviceManager* deviceManager) noexcept {
    auto& log = Logging::Get();

    SceneWorldFacade scene;
    const SceneWorldStatus initStatus = scene.Init();
    if (initStatus != SceneWorldStatus::Ok) {
        log.Error(log::RENDER, "SceneWorldFacade::Init failed");
        return EXIT_DEVICE_INIT_FAIL;
    }

    // Tick once so the M0 phase pipeline runs at least one round-trip
    // (the SceneWorldInit unit test asserts the same in isolation; this
    // is the whole-process variant).
    scene.Tick();

    log.Info(log::APP, "M0 skeleton OK — exiting cleanly");
    scene.Shutdown();
    delete deviceManager;
    return EXIT_OK;
}

}  // namespace

int RunHeadless(int adapterIndex, bool enableValidation) noexcept {
    auto& log = Logging::Get();
    DeviceCreationParams params{};
    params.adapterIndex     = adapterIndex;
    params.enableValidation = enableValidation;
    params.framesInFlight   = 1;
    params.applicationName  = "pyxis (headless)";

    const Resolution              backbuffer{ 1920, 1080 };
    DeviceManagerCreateStatus     status = DeviceManagerCreateStatus::Unknown;
    IDeviceManager* deviceManager = CreateHeadlessDeviceManager(params, backbuffer, &status);
    if (deviceManager == nullptr) {
        log.Error(log::PLATFORM, "CreateHeadlessDeviceManager failed");
        return EXIT_DEVICE_INIT_FAIL;
    }
    return RunCommon(deviceManager);
}

int RunViewer(int adapterIndex, bool enableValidation,
              std::string_view screenshotPath) noexcept {
    return RunViewerLoop(adapterIndex, enableValidation, screenshotPath);
}

}  // namespace pyxis::app
