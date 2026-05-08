// Pyxis app — headless + viewer mode drivers.

#include "HeadlessMode.h"

#include "Config/Configuration.h"
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

constexpr int EXIT_OK               = 0;
constexpr int EXIT_DEVICE_INIT_FAIL = 2;
constexpr int EXIT_CONFIG_FAIL      = 3;

}  // namespace

int RunHeadless(const Configuration& config) noexcept {
    auto& log = Logging::Get();

    // §27 ValidateForHeadless: non-zero seed (§33.7), non-empty
    // output.image, non-zero render dims. Surfaces a config-fail exit
    // before we even spin up Vulkan.
    if (auto validation = ValidateForHeadless(config); !validation) {
        log.Error(log::APP, "headless: " + validation.error());
        return EXIT_CONFIG_FAIL;
    }

    DeviceCreationParams params{};
    params.adapterIndex     = -1;     // M3+ wires config.adapter; for now use the discrete-first picker.
    params.enableValidation = config.diagnostics.validationLayer;
    params.framesInFlight   = config.limits.framesInFlight;
    params.applicationName  = "pyxis (headless)";

    const Resolution              backbuffer{ config.render.width, config.render.height };
    DeviceManagerCreateStatus     status = DeviceManagerCreateStatus::Unknown;
    // The pointee will be mutated in the next M2 commit (BeginFrame /
    // EndFrame / createCommandList for the offscreen render). Until
    // then, take the IDeviceManager* and delete-cleanup-on-error
    // pattern below; the const-correctness fix lands with that commit.
    IDeviceManager* deviceManager = CreateHeadlessDeviceManager(params, backbuffer, &status);  // NOLINT(misc-const-correctness)
    if (deviceManager == nullptr) {
        log.Error(log::PLATFORM, "CreateHeadlessDeviceManager failed");
        return EXIT_DEVICE_INIT_FAIL;
    }

    SceneWorldFacade scene;
    const SceneWorldStatus initStatus = scene.Init();
    if (initStatus != SceneWorldStatus::Ok) {
        log.Error(log::RENDER, "SceneWorldFacade::Init failed");
        delete deviceManager;
        return EXIT_DEVICE_INIT_FAIL;
    }

    // Tick once so the M0 phase pipeline runs at least one round-trip
    // (the SceneWorldInit unit test asserts the same in isolation; this
    // is the whole-process variant). M2's render-to-EXR path lands in
    // the next commit on this branch — for now this matches the M0
    // behaviour with the new Configuration plumbing in place.
    scene.Tick();

    // Best-effort: write the resolved config back to disk so callers can
    // diff their actual run against parameters.json. Non-fatal on
    // failure (logs a warning).
    if (!config.output.effectiveConfig.empty()) {
        (void)WriteEffectiveConfig(config);
    }

    log.Info(log::APP, "headless: M2 config plumbing OK; EXR writer wired in next commit");
    scene.Shutdown();
    delete deviceManager;
    return EXIT_OK;
}

int RunViewer(const Configuration& config, std::string_view screenshotPath) noexcept {
    // Viewer keeps the M1 entrypoint shape — config feeds adapter +
    // validation + render dims, screenshotPath is the orthogonal
    // --screenshot capture flag.
    return RunViewerLoop(config, screenshotPath);
}

}  // namespace pyxis::app
