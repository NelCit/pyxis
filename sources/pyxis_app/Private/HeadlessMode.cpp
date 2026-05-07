// Pyxis app — headless + viewer mode drivers (M0).

#include "HeadlessMode.h"

#include <Pyxis/Platform/Device/DeviceCreationParams.h>
#include <Pyxis/Platform/Device/IDeviceManager.h>
#include <Pyxis/Platform/Device/Resolution.h>
#include <Pyxis/Platform/Logging/Log.h>
#include <Pyxis/Platform/Logging/LogCategories.h>
#include <Pyxis/Renderer/SceneWorldFacade.h>

namespace pyxis::app {

namespace {

constexpr int kExitOk             = 0;
constexpr int kExitDeviceInitFail = 2;

int RunCommon(IDeviceManager* dm) noexcept {
    auto& log = Logging::Get();

    SceneWorldFacade scene;
    const SceneWorldStatus s = scene.Init();
    if (s != SceneWorldStatus::Ok) {
        log.Error(log::kRender, "SceneWorldFacade::Init failed");
        return kExitDeviceInitFail;
    }

    // Tick once so the M0 phase pipeline runs at least one round-trip
    // (the SceneWorldInit unit test asserts the same in isolation; this
    // is the whole-process variant).
    scene.Tick();

    log.Info(log::kApp, "M0 skeleton OK — exiting cleanly");
    scene.Shutdown();
    delete dm;
    return kExitOk;
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
        log.Error(log::kPlatform, "CreateHeadlessDeviceManager failed");
        return kExitDeviceInitFail;
    }
    return RunCommon(dm);
}

int RunViewer(int adapterIndex, bool enableValidation) noexcept {
    auto& log = Logging::Get();
    DeviceCreationParams params{};
    params.adapterIndex     = adapterIndex;
    params.enableValidation = enableValidation;
    params.framesInFlight   = 2;
    params.applicationName  = "pyxis";

    const Resolution              backbuffer{ 1920, 1080 };
    DeviceManagerCreateStatus     status = DeviceManagerCreateStatus::Unknown;
    IDeviceManager* dm = CreateWindowedDeviceManager(params, backbuffer, &status);
    if (dm == nullptr) {
        log.Error(log::kPlatform, "CreateWindowedDeviceManager failed");
        return kExitDeviceInitFail;
    }
    return RunCommon(dm);
}

}  // namespace pyxis::app
