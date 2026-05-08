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
#include <Pyxis/Renderer/Forward.h>          // MAX_FRAMES_IN_FLIGHT (§33.1).
#include <Pyxis/Renderer/Profiler.h>
#include <Pyxis/Renderer/PyxisRenderer.h>
#include <Pyxis/Renderer/SceneWorldFacade.h>

#include <nvrhi/nvrhi.h>

#include <memory>

namespace pyxis::app {

namespace {

constexpr int EXIT_OK               = 0;
constexpr int EXIT_DEVICE_INIT_FAIL = 2;   // Vulkan / NVRHI init failure.
constexpr int EXIT_CONFIG_FAIL      = 3;   // ValidateForHeadless rejected the config.
constexpr int EXIT_RUNTIME_FAIL     = 4;   // Render-time failure: scene init, staging,
                                            // mapping, or EXR write.

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

    // ---- §33.7 determinism pinning -------------------------------------
    // Headless raises framesInFlight to the §33.1 compile-time cap
    // regardless of config. Rationale: M3+'s samplesPerFrame *
    // accumulationFrameLimit loop submits work in flight, and the
    // byte-identical EXR contract requires consistent FIF across runs
    // (different FIF changes the submission interleaving + thus
    // floating-point reduction order for accumulation). RNG seed != 0
    // + non-empty output.image were already validated.
    //
    // The static_assert below ties the headless determinism pin to the
    // renderer's compile-time cap: if a future RFC bumps
    // MAX_FRAMES_IN_FLIGHT, this site fails to compile and forces the
    // author to reconsider whether headless determinism is still
    // satisfied at the new cap. Without it the two constants could
    // silently drift.
    constexpr uint32_t HEADLESS_FRAMES_IN_FLIGHT = MAX_FRAMES_IN_FLIGHT;
    static_assert(HEADLESS_FRAMES_IN_FLIGHT == 3,
                  "Headless §33.7 determinism pin assumes a 3-deep ring; "
                  "revisit before changing MAX_FRAMES_IN_FLIGHT.");
    if (config.limits.framesInFlight != HEADLESS_FRAMES_IN_FLIGHT) {
        log.Info(log::APP, "headless: §33.7 pinning framesInFlight=" +
                           std::to_string(HEADLESS_FRAMES_IN_FLIGHT) +
                           " (config requested " +
                           std::to_string(config.limits.framesInFlight) + ")");
    }
    {
        std::string summary = "headless: determinism pin — seed=";
        summary += std::to_string(config.render.seed);
        summary += "  framesInFlight=" + std::to_string(HEADLESS_FRAMES_IN_FLIGHT);
        summary += "  dims=" + std::to_string(config.render.width) +
                   "x" + std::to_string(config.render.height);
        summary += "  samples=" + std::to_string(config.render.samplesPerFrame);
        log.Info(log::APP, summary);
    }

    // ---- Device manager + offscreen render target ----------------------
    DeviceCreationParams params{};
    params.adapterIndex     = -1;     // M3+ wires config.adapter; for now use the discrete-first picker.
    params.enableValidation = config.diagnostics.validationLayer;
    params.framesInFlight   = HEADLESS_FRAMES_IN_FLIGHT;
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
        return EXIT_RUNTIME_FAIL;
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
        // Force the offscreen RT into CopySource before we close the
        // command list. Without this we relied on NVRHI's auto-barrier
        // logic spanning two separate executeCommandList submissions —
        // technically fine today (NVRHI tracks state across submits) but
        // brittle: a future refactor that takes the readback path through
        // a separate command-list pool would lose that tracking and we'd
        // get UB on the copy. Explicit is cheaper than mysterious.
        commandList->setTextureState(renderTarget, nvrhi::AllSubresources,
                                     nvrhi::ResourceStates::CopySource);
        commandList->commitBarriers();
        commandList->close();
        device->executeCommandList(commandList);
    }

    deviceManager->EndFrame();
    profiler.EndFrame();
    device->runGarbageCollection();

    // ---- Readback to a host-mapped staging texture ---------------------
    // Same pattern as ViewerMode::CaptureBackbufferToPng — copy the
    // offscreen RT into a CpuAccessMode::Read staging texture, wait
    // idle, map, hand `mapped` to the EXR writer.
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
        scene.Shutdown();
        return EXIT_RUNTIME_FAIL;
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
        scene.Shutdown();
        return EXIT_RUNTIME_FAIL;
    }
    // ---- EXR write -----------------------------------------------------
    auto writeResult = WriteExrBgra8(config.output.image,
                                     rtDesc.width, rtDesc.height,
                                     mapped, rowPitch);
    device->unmapStagingTexture(staging.Get());
    if (!writeResult) {
        log.Error(log::APP, "headless: " + writeResult.error());
        scene.Shutdown();
        return EXIT_RUNTIME_FAIL;
    }

    // ---- Effective-config dump + teardown ------------------------------
    // The EXR is the primary artefact and is already on disk; a failure
    // to write the sidecar effective-config is logged but does not fail
    // the run.
    if (!config.output.effectiveConfig.empty()) {
        if (auto effectiveResult = WriteEffectiveConfig(config); !effectiveResult) {
            log.Warn(log::APP, "headless: " + effectiveResult.error());
        }
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
