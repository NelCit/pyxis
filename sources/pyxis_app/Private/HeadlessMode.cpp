// Pyxis app — headless + viewer mode drivers.

#include "HeadlessMode.h"

#include "Config/Configuration.h"
#include "Output/ExrWriter.h"
#include "ViewerMode.h"

#include <Pyxis/Platform/Device/DeviceCreationParams.h>
#include <Pyxis/Platform/Device/IDeviceManager.h>
#include <Pyxis/Platform/Device/Resolution.h>
#include <Pyxis/Platform/Logging/Log.h>
#include <Pyxis/Platform/Logging/LogCategories.h>
#include <Pyxis/Renderer/Descs/RenderSettings.h>
#include <Pyxis/Renderer/Descs/RenderTargets.h>
#include <Pyxis/Renderer/Descs/RendererCreateDesc.h>
#include <Pyxis/Renderer/Profiler.h>
#include <Pyxis/Renderer/PyxisRenderer.h>
#include <Pyxis/Renderer/SceneWorldFacade.h>

#include <nvrhi/nvrhi.h>

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

    // ---- Device manager + offscreen render target ----------------------
    DeviceCreationParams params{};
    params.adapterIndex     = -1;     // M3+ wires config.adapter; for now use the discrete-first picker.
    params.enableValidation = config.diagnostics.validationLayer;
    params.framesInFlight   = config.limits.framesInFlight;
    params.applicationName  = "pyxis (headless)";

    const Resolution             backbuffer{ config.render.width, config.render.height };
    DeviceManagerCreateStatus    status = DeviceManagerCreateStatus::Unknown;
    const std::unique_ptr<IDeviceManager> deviceManager{
        CreateHeadlessDeviceManager(params, backbuffer, &status) };
    if (!deviceManager) {
        log.Error(log::PLATFORM, "CreateHeadlessDeviceManager failed");
        return EXIT_DEVICE_INIT_FAIL;
    }
    nvrhi::IDevice* device = deviceManager->GetDevice();
    if (!device) {
        log.Error(log::PLATFORM, "headless: nvrhi::IDevice not available");
        return EXIT_DEVICE_INIT_FAIL;
    }
    nvrhi::ITexture* renderTarget = deviceManager->GetCurrentBackbuffer();
    if (!renderTarget) {
        log.Error(log::PLATFORM, "headless: offscreen render target not available");
        return EXIT_DEVICE_INIT_FAIL;
    }

    // ---- SceneWorld + Profiler + Renderer ------------------------------
    SceneWorldFacade scene;
    if (scene.Init() != SceneWorldStatus::Ok) {
        log.Error(log::RENDER, "SceneWorldFacade::Init failed");
        return EXIT_DEVICE_INIT_FAIL;
    }
    Profiler              profiler{ device };
    RendererCreateDesc    rendererDesc{};
    rendererDesc.initialWidth  = config.render.width;
    rendererDesc.initialHeight = config.render.height;
    PyxisRenderer         renderer{ device, profiler, rendererDesc };

    const nvrhi::CommandListHandle commandListHandle = device->createCommandList();
    nvrhi::ICommandList* const     commandList       = commandListHandle.Get();

    // ---- One render frame ----------------------------------------------
    // M2 ships a single render. samplesPerFrame > 1 is M3+'s
    // accumulation territory — for the hardcoded triangle every frame
    // would paint the same pixels anyway, so iterating buys nothing.
    scene.Tick();
    profiler.BeginFrame();
    deviceManager->BeginFrame();

    {
        const Profiler::CpuScope frameScope(profiler, "headless.frame");

        commandList->open();
        RenderTargets targets{};
        targets.color = renderTarget;
        RenderSettings settings{};
        settings.width  = renderTarget->getDesc().width;
        settings.height = renderTarget->getDesc().height;
        renderer.RenderFrame(commandList, settings, targets);
        commandList->close();
        device->executeCommandList(commandList);
    }

    deviceManager->EndFrame();
    profiler.EndFrame();
    device->runGarbageCollection();

    // ---- Readback to a host-mapped staging texture ---------------------
    // Same pattern as ViewerMode::CaptureBackbufferToPng — copy the
    // offscreen RT into a CpuAccessMode::Read staging texture, wait
    // idle, map. The next M2 commit hands `mapped` to the EXR writer.
    const auto& rtDesc = renderTarget->getDesc();
    nvrhi::TextureDesc stagingDesc;
    stagingDesc.format    = rtDesc.format;
    stagingDesc.width     = rtDesc.width;
    stagingDesc.height    = rtDesc.height;
    stagingDesc.dimension = nvrhi::TextureDimension::Texture2D;
    stagingDesc.debugName = "headless-readback-staging";
    const nvrhi::StagingTextureHandle staging =
        device->createStagingTexture(stagingDesc, nvrhi::CpuAccessMode::Read);
    if (!staging) {
        log.Error(log::APP, "headless: createStagingTexture failed");
        return EXIT_DEVICE_INIT_FAIL;
    }

    {
        commandList->open();
        commandList->copyTexture(staging.Get(),  nvrhi::TextureSlice{},
                                 renderTarget,    nvrhi::TextureSlice{});
        commandList->close();
        device->executeCommandList(commandList);
    }
    device->waitForIdle();
    device->runGarbageCollection();

    std::size_t rowPitch = 0;
    const void* mapped = device->mapStagingTexture(staging.Get(), nvrhi::TextureSlice{},
                                                   nvrhi::CpuAccessMode::Read, &rowPitch);
    if (!mapped) {
        log.Error(log::APP, "headless: mapStagingTexture failed");
        return EXIT_DEVICE_INIT_FAIL;
    }
    // ---- EXR write -----------------------------------------------------
    auto writeResult = WriteExrBgra8(config.output.image,
                                     rtDesc.width, rtDesc.height,
                                     mapped, rowPitch);
    device->unmapStagingTexture(staging.Get());
    if (!writeResult) {
        log.Error(log::APP, "headless: " + writeResult.error());
        scene.Shutdown();
        return EXIT_DEVICE_INIT_FAIL;
    }

    // ---- Effective-config dump + teardown ------------------------------
    if (!config.output.effectiveConfig.empty()) {
        (void)WriteEffectiveConfig(config);
    }
    scene.Shutdown();
    return EXIT_OK;
}

int RunViewer(const Configuration& config, std::string_view screenshotPath) noexcept {
    // Viewer keeps the M1 entrypoint shape — config feeds adapter +
    // validation + render dims, screenshotPath is the orthogonal
    // --screenshot capture flag.
    return RunViewerLoop(config, screenshotPath);
}

}  // namespace pyxis::app
