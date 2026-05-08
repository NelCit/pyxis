// Pyxis app — ViewerMode frame loop.

#include "ViewerMode.h"

#include "Config/Configuration.h"
#include "ImGuiHost.h"

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

// stb_image_write's IMPLEMENTATION lives in its own TU
// (Private/StbImageWrite.cpp) — see the comment there. Here we only
// need the header API.
#include <stb_image_write.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

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

namespace {

// Capture the current backbuffer to PNG via an NVRHI staging texture +
// stb_image_write. BGRA → RGBA swizzle on the CPU side. Plan §35
// image-regression artefact for M1.
bool CaptureBackbufferToPng(nvrhi::IDevice*       device,
                            nvrhi::ICommandList*  commandList,
                            nvrhi::ITexture*      backbuffer,
                            std::string_view      pngPath) noexcept {
    auto& log = Logging::Get();
    if (!device || !commandList || !backbuffer || pngPath.empty()) return false;

    const auto& tdesc = backbuffer->getDesc();

    nvrhi::TextureDesc stagingDesc;
    stagingDesc.format     = tdesc.format;
    stagingDesc.width      = tdesc.width;
    stagingDesc.height     = tdesc.height;
    stagingDesc.dimension  = nvrhi::TextureDimension::Texture2D;
    stagingDesc.debugName  = "screenshot-staging";
    const nvrhi::StagingTextureHandle staging =
        device->createStagingTexture(stagingDesc, nvrhi::CpuAccessMode::Read);
    if (!staging) {
        log.Error(log::APP, "screenshot: createStagingTexture failed");
        return false;
    }

    commandList->copyTexture(staging.Get(),    nvrhi::TextureSlice{},
                             backbuffer,        nvrhi::TextureSlice{});
    commandList->setTextureState(backbuffer, nvrhi::AllSubresources, nvrhi::ResourceStates::Present);
    commandList->commitBarriers();
    commandList->close();
    device->executeCommandList(commandList);
    device->waitForIdle();

    std::size_t rowPitch = 0;
    const void* mapped = device->mapStagingTexture(staging.Get(), nvrhi::TextureSlice{},
                                                   nvrhi::CpuAccessMode::Read, &rowPitch);
    if (!mapped) {
        log.Error(log::APP, "screenshot: mapStagingTexture failed");
        return false;
    }

    // BGRA8 → RGBA8 swizzle, contiguous row pitch for stb_image_write.
    const std::size_t imageWidth  = tdesc.width;
    const std::size_t imageHeight = tdesc.height;
    std::vector<uint8_t> rgba(imageWidth * imageHeight * 4);
    const auto* src = static_cast<const uint8_t*>(mapped);
    for (std::size_t row = 0; row < imageHeight; ++row) {
        const uint8_t* srcRow = src + row * rowPitch;
        uint8_t*       dstRow = rgba.data() + (row * imageWidth * 4);
        for (std::size_t col = 0; col < imageWidth; ++col) {
            dstRow[col * 4 + 0] = srcRow[col * 4 + 2];   // R ← B
            dstRow[col * 4 + 1] = srcRow[col * 4 + 1];   // G ← G
            dstRow[col * 4 + 2] = srcRow[col * 4 + 0];   // B ← R
            dstRow[col * 4 + 3] = srcRow[col * 4 + 3];   // A ← A
        }
    }
    device->unmapStagingTexture(staging.Get());

    const std::string path{ pngPath };
    const int wrote = stbi_write_png(path.c_str(),
                                     static_cast<int>(imageWidth),
                                     static_cast<int>(imageHeight),
                                     4,
                                     rgba.data(),
                                     static_cast<int>(imageWidth * 4));
    if (wrote == 0) {
        log.Error(log::APP, "screenshot: stbi_write_png failed");
        return false;
    }

    // Quick sanity: any pixel non-black? Lets the caller assert "the
    // triangle actually rendered" without a separate harness.
    bool anyNonBlack = false;
    for (std::size_t pix = 0; pix + 2 < rgba.size(); pix += 4) {
        if (rgba[pix] != 0 || rgba[pix + 1] != 0 || rgba[pix + 2] != 0) {
            anyNonBlack = true;
            break;
        }
    }
    std::string msg = "screenshot: wrote ";
    msg += std::to_string(imageWidth) + "x" + std::to_string(imageHeight);
    msg += " PNG to ";
    msg.append(pngPath);
    msg += anyNonBlack ? "  (non-black pixels present — render OK)"
                       : "  (image is fully black — render likely broken)";
    log.Info(log::APP, msg);
    return true;
}

}  // namespace

int RunViewerLoop(const Configuration& config,
                  std::string_view     screenshotPath) noexcept {
    auto& log = Logging::Get();

    // ---- Window ----------------------------------------------------------
    // §27 render.{width,height} drive the swapchain dims; the window
    // is sized to match so the backbuffer fills the client area.
    WindowDesc winDesc{};
    winDesc.width  = config.render.width;
    winDesc.height = config.render.height;
    winDesc.title  = "Pyxis";

    const std::unique_ptr<IWindow> window{ CreateGlfwWindow(winDesc) };
    if (!window) {
        log.Error(log::PLATFORM, "ViewerMode: CreateGlfwWindow failed");
        return EXIT_DEVICE_INIT_FAIL;
    }

    // Hook a close-on-Escape sink alongside the window's own close button.
    // Note: ImGui_ImplGlfw_InitForVulkan(install_callbacks=true) chains its
    // own callbacks under whatever was registered before — so we install
    // *our* GLFW callbacks first (via SetEventSink → GlfwWindow), then
    // ImGuiHost::Init() chains ImGui's on top. Both fire each frame.
    std::atomic<bool> shouldClose{false};
    window->SetEventSink([&](const InputEvent& event) {
        if (event.kind == InputEventKind::WindowClose) shouldClose.store(true);
        if (event.kind == InputEventKind::KeyDown && event.key == GLFW_ESCAPE_KEY_CODE) {
            shouldClose.store(true);
        }
    });

    // ---- Device manager --------------------------------------------------
    DeviceCreationParams params{};
    // M3+ wires config.adapter; for now defer to the discrete-first
    // picker via -1.
    params.adapterIndex     = -1;
    params.enableValidation = config.diagnostics.validationLayer;
    params.framesInFlight   = config.limits.framesInFlight;
    params.applicationName  = "pyxis";

    const Resolution backbuffer{ winDesc.width, winDesc.height };
    DeviceManagerCreateStatus status = DeviceManagerCreateStatus::Unknown;
    const std::unique_ptr<IDeviceManager> deviceManager{
        CreateWindowedDeviceManager(params, window.get(), backbuffer, &status) };
    if (!deviceManager) {
        log.Error(log::PLATFORM, "ViewerMode: device manager init failed");
        return EXIT_DEVICE_INIT_FAIL;
    }
    nvrhi::IDevice* device = deviceManager->GetDevice();
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

    // ---- Single command list --------------------------------------------
    // M1 pins framesInFlight = 1, so one command list reused across frames
    // is enough — VkDeviceManager::BeginFrame waits on its timeline so the
    // GPU has retired our last submit before we re-open this list. M2+
    // grows this back to a per-slot ring when active framesInFlight rises.
    const nvrhi::CommandListHandle commandListHandle = device->createCommandList();

    // ---- ImGui ----------------------------------------------------------
    // Init in both modes so --screenshot doubles as a visual proof of the
    // M1 dockable Performance panel: the captured PNG shows the triangle
    // plus the panel in its default position.
    ImGuiHost imguiHost;
    const bool screenshotMode = !screenshotPath.empty();
    if (!imguiHost.Init(window.get(), deviceManager.get())) {
        log.Warn(log::APP, "ViewerMode: ImGui init failed; continuing without UI");
    }
    if (screenshotMode) {
        log.Info(log::APP, "ViewerMode: --screenshot path supplied; capturing one frame after warmup");
    } else {
        log.Info(log::APP, "ViewerMode: entering frame loop");
    }

    // Capture the backbuffer at frame index = SCREENSHOT_FRAME so the
    // swapchain has settled. 3 frames is enough on FIFO; keeps the
    // smoke fast.
    // Frame 30 is well past the SLOT_COUNT-frame profiler ring warmup
    // (so the screenshot's Performance panel actually has the scope
    // tree filled in) and still well before any real-world delay.
    constexpr uint64_t SCREENSHOT_FRAME = 30;
    // Cadence for the periodic profiler log line. Every 120 frames is
    // ~1 s at 120 FPS — enough samples for the rolling FrameProfile to
    // be representative without spamming stdout.
    constexpr uint64_t PROFILER_LOG_INTERVAL = 120;

    // ---- Frame loop ------------------------------------------------------
    uint64_t frameIndex = 0;
    // Track the swapchain generation we last initialised consumers for.
    // 0 means "never seen one yet"; the very first BeginFrame after
    // device manager bringup will report 1, and we'll notify ImGuiHost.
    uint32_t lastSeenSwapchainGeneration = 0;
    while (!window->ShouldClose() && !shouldClose.load()) {
        profiler.BeginFrame();
        {
            const Profiler::CpuScope poll(profiler, "app.poll");
            window->PollEvents();
        }
        {
            const Profiler::CpuScope tick(profiler, "app.scene.tick");
            scene.Tick();
        }
        {
            // app.dm.acquire is the prime suspect for any "uncapped FPS
            // is still capped" — vkAcquireNextImageKHR will block waiting
            // for the compositor (DWM in Windows windowed mode) to
            // release a swapchain image, regardless of the present mode.
            const Profiler::CpuScope acquire(profiler, "app.dm.acquire");
            deviceManager->BeginFrame();
        }

        // Notify ImGui when the swapchain was rebuilt (resize, fullscreen
        // toggle). vkDeviceWaitIdle has already happened inside
        // CreateSwapchain so it's safe to call ImGui_ImplVulkan_
        // SetMinImageCount here.
        const uint32_t currentSwapchainGeneration = deviceManager->GetSwapchainGeneration();
        if (currentSwapchainGeneration != lastSeenSwapchainGeneration) {
            if (imguiHost.IsReady()) {
                imguiHost.OnSwapchainRebuilt(deviceManager->GetBackbufferCount());
            }
            lastSeenSwapchainGeneration = currentSwapchainGeneration;
        }

        // Build the ImGui draw data on the CPU side. The Performance panel
        // pulls from the renderer's FrameProfile snapshot, which the
        // GpuTimestampPool drained from a slot the GPU has already
        // retired — fine for human-paced FPS readouts.
        if (imguiHost.IsReady()) {
            const Profiler::CpuScope imguiCpu(profiler, "app.imgui.cpu");
            const FrameProfile frameProfile = renderer.LastFrameProfile();
            imguiHost.BeginFrame();
            imguiHost.BuildFpsPanel(frameProfile);
            imguiHost.Render();
        }

        nvrhi::ITexture* backbuffer = deviceManager->GetCurrentBackbuffer();
        if (backbuffer) {
            nvrhi::ICommandList* commandList = commandListHandle.Get();

            commandList->open();
            RenderTargets targets{};
            targets.color = backbuffer;
            RenderSettings settings{};
            settings.width  = backbuffer->getDesc().width;
            settings.height = backbuffer->getDesc().height;

            renderer.RenderFrame(commandList, settings, targets);

            // ImGui submit: NVRHI just left the backbuffer in
            // ResourceStates::RenderTarget after the renderer's draw,
            // which matches VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL — what
            // ImGuiHost::Submit's vkCmdBeginRendering expects. We ask NVRHI
            // to commit any pending barriers first so its tracker and the
            // raw command buffer agree on the layout before we begin our
            // own dynamic-rendering scope. Done *before* the screenshot
            // capture so --screenshot also doubles as visual proof of the
            // dockable Performance panel.
            if (imguiHost.IsReady()) {
                const Profiler::CpuScope imguiSubmitCpu(profiler, "app.imgui.submit");
                commandList->setTextureState(backbuffer, nvrhi::AllSubresources,
                                             nvrhi::ResourceStates::RenderTarget);
                commandList->commitBarriers();
                const Profiler::GpuScope imguiSubmitGpu(profiler, commandList, "pass.ImGui");
                imguiHost.Submit(commandList, backbuffer);
            }

            // Screenshot path: capture this frame's backbuffer to PNG and
            // exit. CaptureBackbufferToPng owns the close + execute +
            // waitForIdle, so we skip the regular Present below.
            if (screenshotMode && frameIndex == SCREENSHOT_FRAME) {
                CaptureBackbufferToPng(device, commandList, backbuffer, screenshotPath);
                deviceManager->WaitIdle();
                return EXIT_OK;
            }

            // Transition back to Present for the swapchain.
            const Profiler::CpuScope submit(profiler, "app.cmd.submit");
            commandList->setTextureState(backbuffer, nvrhi::AllSubresources, nvrhi::ResourceStates::Present);
            commandList->commitBarriers();
            commandList->close();
            device->executeCommandList(commandList);
        }

        {
            const Profiler::CpuScope present(profiler, "app.dm.present");
            deviceManager->EndFrame();
        }
        // Drain NVRHI's deferred-destruction queue. nvrhi.h's contract is
        // explicit: "Call this method at least once per frame." Without
        // it, every CommandList submission's internal RefCountPtrs on
        // textures/pipelines/buffers stay alive in the queue tracker
        // until the queue itself is destroyed — staging textures and
        // timer queries grow unbounded over a long session.
        {
            const Profiler::CpuScope gcScope(profiler, "app.nvrhi.gc");
            device->runGarbageCollection();
        }
        profiler.EndFrame();
        ++frameIndex;

        // Periodic profiler dump: prints the rolling totals plus the
        // pre-order scope tree, indented by depth. With FIF=1 the cpu
        // number tracks wall-clock frame time and fps = 1000 / cpu.
        if (frameIndex > 0 && frameIndex % PROFILER_LOG_INTERVAL == 0) {
            const FrameProfile frameProfile = renderer.LastFrameProfile();
            const double fps = frameProfile.cpuFrameMs > 0.0 ? 1000.0 / frameProfile.cpuFrameMs : 0.0;
            char buf[256];
            std::snprintf(buf, sizeof(buf),
                "profiler: frame %llu  cpu %.3f ms  gpu %.3f ms  fps %.1f",
                static_cast<unsigned long long>(frameProfile.frameIndex),
                frameProfile.cpuFrameMs, frameProfile.gpuFrameMs, fps);
            log.Info(log::APP, buf);
            for (const FrameProfile::PassTiming& timing : frameProfile.passes) {
                const char* kind = (timing.kind == FrameProfile::ScopeKind::Cpu) ? "CPU" : "GPU";
                const std::string_view name = timing.name.View();
                // Two spaces per depth level, "  CPU "/"  GPU " column.
                char line[256];
                std::snprintf(line, sizeof(line),
                    "profiler:   %*s%s %.*s  %.3f ms",
                    static_cast<int>(timing.depth * 2), "",
                    kind,
                    static_cast<int>(name.size()), name.data(),
                    timing.durationMs);
                log.Info(log::APP, line);
            }
        }
    }

    log.Info(log::APP, "ViewerMode: frame loop exited; tearing down");
    deviceManager->WaitIdle();
    imguiHost.Shutdown();
    return EXIT_OK;
}

}  // namespace pyxis::app
