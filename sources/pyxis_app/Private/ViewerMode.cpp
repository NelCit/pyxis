// Pyxis app — ViewerMode frame loop.

#include "ViewerMode.h"

#include "Camera/FlyCameraController.h"
#include "Config/Configuration.h"
#include "ImGuiHost.h"

#include <imgui.h>
#include "Output/TextureReadback.h"
#include "Render/AovTextures.h"
#include "HydraEngine/HydraEngine.h"
#include "Render/HardcodedCubeScene.h"
#include "Scene/SceneResolver.h"
#include "UsdDirectEngine/UsdDirectEngine.h"

#include <Pyxis/Platform/Device/DeviceCreationParams.h>
#include <Pyxis/Platform/Device/IDeviceManager.h>
#include <Pyxis/Platform/Device/Resolution.h>
#include <Pyxis/Platform/Logging/Log.h>
#include <Pyxis/Platform/Logging/LogCategories.h>
#include <Pyxis/Platform/Window/IWindow.h>
#include <Pyxis/Renderer/Descs/GpuSceneCreateDesc.h>
#include <Pyxis/Renderer/Descs/RendererCreateDesc.h>
#include <Pyxis/Renderer/Descs/RenderSettings.h>
#include <Pyxis/Renderer/Descs/RenderTargets.h>
#include <Pyxis/Renderer/GpuScene.h>
#include <Pyxis/Renderer/Profiler.h>
#include <Pyxis/Renderer/PyxisRenderer.h>
#include <Pyxis/Renderer/SceneWorldFacade.h>

#include <nvrhi/nvrhi.h>

// stb_image_write's IMPLEMENTATION lives in its own TU
// (Private/StbImageWrite.cpp) — see the comment there. Here we only
// need the header API.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <stb_image_write.h>
#include <string>
#include <vector>

namespace pyxis::app {

namespace {

constexpr int EXIT_OK = 0;
constexpr int EXIT_DEVICE_INIT_FAIL = 2;

// GLFW key code for Escape. Hard-coded to avoid pulling <GLFW/glfw3.h>
// into pyxis_app (glfw is PRIVATE-linked from pyxis_platform). The
// value has been 256 since GLFW 3.0 and matches USB HID Escape — stable
// enough for "Escape closes the viewer" UX.
constexpr int GLFW_ESCAPE_KEY_CODE = 256;

}  // namespace

namespace {

// Capture the current backbuffer to PNG via TextureReadback +
// stb_image_write. BGRA → RGBA swizzle on the CPU side. Plan §35
// image-regression artefact for M1. Caller passes an already-open
// command list — we record the readback copy onto it, restore the
// backbuffer to Present state (so the next swapchain acquire sees a
// sane layout), close + execute + wait, then map and write the PNG.
bool CaptureBackbufferToPng(nvrhi::IDevice* device, nvrhi::ICommandList* commandList,
                            nvrhi::ITexture* backbuffer, std::string_view pngPath) noexcept {
  auto& log = Logging::Get();
  if (!device || !commandList || !backbuffer || pngPath.empty())
    return false;

  auto readback =
      TextureReadback::RecordCopy(device, commandList, backbuffer, "screenshot-staging");
  if (!readback)
  {
    log.Error(log::APP, "screenshot: " + readback.error());
    return false;
  }

  commandList->setTextureState(backbuffer, nvrhi::AllSubresources, nvrhi::ResourceStates::Present);
  commandList->commitBarriers();
  commandList->close();
  device->executeCommandList(commandList);
  device->waitForIdle();

  if (auto mapResult = readback->Map(); !mapResult)
  {
    log.Error(log::APP, "screenshot: " + mapResult.error());
    return false;
  }

  // BGRA8 → RGBA8 swizzle, contiguous row pitch for stb_image_write.
  const std::size_t imageWidth = readback->Width();
  const std::size_t imageHeight = readback->Height();
  const std::size_t rowPitch = readback->RowPitch();
  std::vector<uint8_t> rgba(imageWidth * imageHeight * 4);
  const auto* src = static_cast<const uint8_t*>(readback->Data());
  for (std::size_t row = 0; row < imageHeight; ++row)
  {
    const uint8_t* srcRow = src + row * rowPitch;
    uint8_t* dstRow = rgba.data() + (row * imageWidth * 4);
    for (std::size_t col = 0; col < imageWidth; ++col)
    {
      dstRow[col * 4 + 0] = srcRow[col * 4 + 2];  // R ← B
      dstRow[col * 4 + 1] = srcRow[col * 4 + 1];  // G ← G
      dstRow[col * 4 + 2] = srcRow[col * 4 + 0];  // B ← R
      dstRow[col * 4 + 3] = srcRow[col * 4 + 3];  // A ← A
    }
  }

  const std::string path{pngPath};
  const int wrote =
      stbi_write_png(path.c_str(), static_cast<int>(imageWidth), static_cast<int>(imageHeight), 4,
                     rgba.data(), static_cast<int>(imageWidth * 4));
  if (wrote == 0)
  {
    log.Error(log::APP, "screenshot: stbi_write_png failed");
    return false;
  }

  // Quick sanity: any pixel non-black? Lets the caller assert "the
  // triangle actually rendered" without a separate harness.
  bool anyNonBlack = false;
  for (std::size_t pix = 0; pix + 2 < rgba.size(); pix += 4)
  {
    if (rgba[pix] != 0 || rgba[pix + 1] != 0 || rgba[pix + 2] != 0)
    {
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

int RunViewerLoop(const Configuration& config, const ResolvedScene& resolvedScene,
                  std::string_view screenshotPath) noexcept {
  auto& log = Logging::Get();

  // ---- Window ----------------------------------------------------------
  // §27 render.{width,height} drive the swapchain dims; the window
  // is sized to match so the backbuffer fills the client area.
  WindowDesc winDesc{};
  winDesc.width = config.render.width;
  winDesc.height = config.render.height;
  winDesc.title = "Pyxis";

  const std::unique_ptr<IWindow> window{CreateGlfwWindow(winDesc)};
  if (!window)
  {
    log.Error(log::PLATFORM, "ViewerMode: CreateGlfwWindow failed");
    return EXIT_DEVICE_INIT_FAIL;
  }

  // Hook a close-on-Escape sink alongside the window's own close button.
  // Note: ImGui_ImplGlfw_InitForVulkan(install_callbacks=true) chains its
  // own callbacks under whatever was registered before — so we install
  // *our* GLFW callbacks first (via SetEventSink → GlfwWindow), then
  // ImGuiHost::Init() chains ImGui's on top. Both fire each frame.
  std::atomic<bool> shouldClose{false};
  FlyCameraController cameraController;
  window->SetEventSink([&](const InputEvent& event) {
    if (event.kind == InputEventKind::WindowClose)
      shouldClose.store(true);
    if (event.kind == InputEventKind::KeyDown && event.key == GLFW_ESCAPE_KEY_CODE)
    {
      shouldClose.store(true);
    }

    // Route mouse / keyboard to the fly camera ONLY if ImGui isn't
    // consuming them this frame. Without this gate the camera rotates
    // while the user drags an ImGui window's title-bar (which holds
    // LMB + drags the mouse — exactly what the camera's RMB-equivalent
    // expects). `WantCaptureMouse` / `WantCaptureKeyboard` are set by
    // `ImGui::NewFrame()` and reflect "is the cursor over an ImGui
    // window / is an ImGui widget focused"; they're sticky across an
    // active drag.
    const ImGuiContext* imguiCtx = ImGui::GetCurrentContext();
    const ImGuiIO* imguiIO = (imguiCtx != nullptr) ? &ImGui::GetIO() : nullptr;
    const bool isMouseEvent = (event.kind == InputEventKind::MouseButtonDown
                               || event.kind == InputEventKind::MouseButtonUp
                               || event.kind == InputEventKind::MouseMove);
    const bool isKeyEvent = (event.kind == InputEventKind::KeyDown
                             || event.kind == InputEventKind::KeyUp);
    if (imguiIO != nullptr)
    {
      if (isMouseEvent && imguiIO->WantCaptureMouse)
        return;
      if (isKeyEvent && imguiIO->WantCaptureKeyboard)
        return;
    }
    cameraController.HandleEvent(event);
  });

  // ---- Device manager --------------------------------------------------
  DeviceCreationParams params{};
  // M3+ wires config.adapter; for now defer to the discrete-first
  // picker via -1.
  params.adapterIndex = -1;
  params.enableValidation = config.diagnostics.validationLayer;
  params.framesInFlight = config.limits.framesInFlight;
  params.applicationName = "pyxis";

  const Resolution backbuffer{winDesc.width, winDesc.height};
  DeviceManagerCreateStatus status = DeviceManagerCreateStatus::Unknown;
  const std::unique_ptr<IDeviceManager> deviceManager{
      CreateWindowedDeviceManager(params, window.get(), backbuffer, &status)};
  if (!deviceManager)
  {
    log.Error(log::PLATFORM, "ViewerMode: device manager init failed");
    return EXIT_DEVICE_INIT_FAIL;
  }
  nvrhi::IDevice* device = deviceManager->GetDevice();
  if (!device)
  {
    log.Error(log::PLATFORM, "ViewerMode: nvrhi::IDevice not available");
    return EXIT_DEVICE_INIT_FAIL;
  }

  // ---- SceneWorld (M0 carry-over so the phase pipeline still ticks) ---
  SceneWorldFacade scene;
  if (scene.Init() != SceneWorldStatus::Ok)
  {
    log.Error(log::RENDER, "ViewerMode: SceneWorldFacade::Init failed");
    return EXIT_DEVICE_INIT_FAIL;
  }

  // ---- Profiler + GpuScene + Renderer ---------------------------------
  // GpuScene is the canonical scene-mutation API (§18.5);
  // PyxisRenderer's ctor takes it by reference per §18.6 and
  // PathTracePass binds its TLAS + camera every frame.
  Profiler profiler{device};
  GpuScene gpuScene{device, profiler, GpuSceneCreateDesc{}};

  // M3 hardcoded cube + camera + distant light, identical to the
  // headless fixture. M3.5 + M4 replace this with the USD-loaded
  // scene chain.
  // M4 ingest dispatch on `app.ingest`. UsdDirectEngine wires
  // through pyxis_usd_ingest's StageWalker; HydraEngine lands at
  // M4 P5e. Either failing or returning "nothing emitted" falls
  // back to the M3 hardcoded cube so pyxis.exe always renders.
  bool sceneLoaded = false;
  if (!resolvedScene.path.empty())
  {
    if (config.app.ingest == "usd_direct")
    {
      UsdDirectEngine engine;
      sceneLoaded = engine.Load(resolvedScene.path, gpuScene);
    }
    else if (config.app.ingest == "hydra")
    {
      HydraEngine engine;
      sceneLoaded = engine.Load(resolvedScene.path, gpuScene);
    }
  }
  if (sceneLoaded)
  {
    log.Info(log::APP, "ViewerMode: scene loaded via " + config.app.ingest + " adapter.");
  }
  else if (auto cubeResult = BuildHardcodedCubeScene(gpuScene, winDesc.width, winDesc.height);
      !cubeResult)
  {
    log.Error(log::APP, "ViewerMode: " + cubeResult.error());
    return EXIT_DEVICE_INIT_FAIL;
  }

  // PathTracePass writes via UAV (RWTexture2D<float4>) which the
  // swapchain image can't accept (VK_FORMAT_B8G8R8A8_SRGB doesn't
  // support storage). Render into an intermediate AOV color
  // texture (BGRA8_UNORM, storage-capable) and copy to the
  // swapchain image afterward — same pattern §9 v1's render graph
  // ends with `CopyToHydraBuffer / Present` for.
  auto aovsResult = AovTextures::Create(device, winDesc.width, winDesc.height);
  if (!aovsResult)
  {
    log.Error(log::APP, "ViewerMode: " + aovsResult.error());
    return EXIT_DEVICE_INIT_FAIL;
  }
  const AovTextures aovs = std::move(*aovsResult);

  RendererCreateDesc rendererDesc{};
  rendererDesc.initialWidth = winDesc.width;
  rendererDesc.initialHeight = winDesc.height;
  PyxisRenderer renderer{device, gpuScene, profiler, rendererDesc};

  // ---- Single command list --------------------------------------------
  // M1 pins framesInFlight = 1, so one command list reused across frames
  // is enough — VkDeviceManager::BeginFrame waits on its timeline so the
  // GPU has retired our last submit before we re-open this list. M2+
  // grows this back to a per-slot ring when active framesInFlight rises.
  const nvrhi::CommandListHandle commandListHandle = device->createCommandList();

  // ---- ImGui ----------------------------------------------------------
  // Skipped in screenshot mode: the captured PNG is a clean
  // regression artefact (just the triangle on the clear colour) so
  // the §35 image-diff fixture isn't perturbed by FPS / scope-tree
  // text. The interactive viewer keeps ImGui as normal.
  ImGuiHost imguiHost;
  const bool screenshotMode = !screenshotPath.empty();
  if (!screenshotMode)
  {
    if (!imguiHost.Init(window.get(), deviceManager.get()))
    {
      log.Warn(log::APP, "ViewerMode: ImGui init failed; continuing without UI");
    }
  }
  if (screenshotMode)
  {
    log.Info(log::APP, "ViewerMode: --screenshot path supplied; capturing one frame after warmup");
  }
  else
  {
    log.Info(log::APP, "ViewerMode: entering frame loop");
  }

  // Frame 3 is the original M1 capture point — far enough past frame 0
  // that the swapchain has rotated through every image at least once
  // (FIFO + 3-image swapchain), short enough to keep the screenshot
  // smoke under a second. The profiler ring drain doesn't matter for
  // the screenshot itself (we don't draw timing text any more).
  constexpr uint64_t SCREENSHOT_FRAME = 3;
  // (Periodic profiler-log cadence removed — the profile dump now
  // fires once after the first frame completes, which captures the
  // load profile without spamming stdout every N frames.)

  // ---- Frame loop ------------------------------------------------------
  uint64_t frameIndex = 0;
  // Track the swapchain generation we last initialised consumers for.
  // 0 means "never seen one yet"; the very first BeginFrame after
  // device manager bringup will report 1, and we'll notify ImGuiHost.
  uint32_t lastSeenSwapchainGeneration = 0;

  // Per-frame dt clock for the fly-camera controller. steady_clock
  // because we don't want wall-clock jumps (NTP resyncs, DST) to
  // teleport the camera. Initialised inside the loop on first frame
  // so the first dt isn't a multi-second startup spike.
  std::chrono::steady_clock::time_point lastFrameTime{};
  bool haveLastFrameTime = false;
  while (!window->ShouldClose() && !shouldClose.load())
  {
    profiler.BeginFrame();
    {
      const Profiler::CpuScope poll(profiler, "app.poll");
      window->PollEvents();
    }
    {
      // Fly-camera controller: compute dt, integrate WASD/ZQSD held
      // keys + mouse-look delta, push the new viewFromWorld into
      // GpuScene before the renderer reads the camera. First frame
      // dt is clamped to 0 so an unboxed startup time doesn't fling
      // the camera across the scene.
      const Profiler::CpuScope camera(profiler, "app.camera.update");
      const auto now = std::chrono::steady_clock::now();
      float dtSeconds = 0.0f;
      if (haveLastFrameTime)
      {
        const auto delta = now - lastFrameTime;
        dtSeconds = std::chrono::duration<float>(delta).count();
        // Clamp to 100ms so an editor-stall doesn't fling the camera.
        dtSeconds = std::min(dtSeconds, 0.1f);
      }
      lastFrameTime = now;
      haveLastFrameTime = true;
      cameraController.Update(dtSeconds, gpuScene);
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
    if (currentSwapchainGeneration != lastSeenSwapchainGeneration)
    {
      if (imguiHost.IsReady())
      {
        imguiHost.OnSwapchainRebuilt(deviceManager->GetBackbufferCount());
      }
      lastSeenSwapchainGeneration = currentSwapchainGeneration;
    }

    // Build the ImGui draw data on the CPU side. The Performance panel
    // pulls from the renderer's FrameProfile snapshot, which the
    // GpuTimestampPool drained from a slot the GPU has already
    // retired — fine for human-paced FPS readouts.
    if (imguiHost.IsReady())
    {
      const Profiler::CpuScope imguiCpu(profiler, "app.imgui.cpu");
      const FrameProfile frameProfile = renderer.LastFrameProfile();
      const FrameStats sceneStats = gpuScene.LastFrameStats();
      imguiHost.BeginFrame();
      imguiHost.BuildFpsPanel(frameProfile);
      imguiHost.BuildScenePanel(sceneStats);
      imguiHost.BuildEditorPanel(gpuScene);

      // Drain the editor's "Reload shaders" latch. Renderer-side
      // shader-library invalidation isn't on the public surface yet
      // (PyxisRenderer doesn't expose a reload entry; adding one is
      // a v1.1 API addition under §22). For now we log the click so
      // the button has visible feedback; the real reload arrives
      // when the renderer's ShaderLibrary grows a public Invalidate
      // hook.
      if (imguiHost.TakeShaderReloadRequest())
      {
        log.Info(log::APP,
                 "ViewerMode: shader-reload requested from Editor panel "
                 "(renderer-side reload TODO)");
      }

      imguiHost.Render();
    }

    nvrhi::ITexture* backbuffer = deviceManager->GetCurrentBackbuffer();
    if (backbuffer)
    {
      nvrhi::ICommandList* commandList = commandListHandle.Get();

      commandList->open();

      // Drain pending GpuScene mutations (mesh upload + BLAS
      // build + TLAS rebuild) onto the open command list before
      // RenderFrame consumes the TLAS via PathTracePass. After
      // the M3 startup tick the scene is static, so all
      // subsequent frames find nothing dirty and CommitResources
      // is effectively a no-op.
      if (auto commitResult = gpuScene.CommitResources(commandList); !commitResult)
      {
        log.Error(log::APP, "ViewerMode: " + std::string{commitResult.error().message.View()});
      }

      // Render into the AOV color (storage-capable BGRA8_UNORM)
      // rather than directly into the swapchain backbuffer (which
      // is sRGB and rejects storage writes). After the path
      // trace finishes we copyTexture from AOV → swapchain.
      RenderTargets targets{};
      targets.color = aovs.color.Get();
      RenderSettings settings{};
      settings.width = aovs.width;
      settings.height = aovs.height;

      renderer.RenderFrame(commandList, settings, targets);

      // Copy AOV color → swapchain backbuffer. NVRHI tracks the
      // transitions for both images via `keepInitialState`, so
      // an explicit barrier isn't needed — copyTexture inserts
      // the right CopySrc/CopyDest transitions automatically.
      commandList->copyTexture(backbuffer, nvrhi::TextureSlice{}, aovs.color.Get(),
                               nvrhi::TextureSlice{});

      // ImGui submit: NVRHI just left the backbuffer in
      // ResourceStates::RenderTarget after the renderer's draw,
      // which matches VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL — what
      // ImGuiHost::Submit's vkCmdBeginRendering expects. We ask NVRHI
      // to commit any pending barriers first so its tracker and the
      // raw command buffer agree on the layout before we begin our
      // own dynamic-rendering scope. Done *before* the screenshot
      // capture so --screenshot also doubles as visual proof of the
      // dockable Performance panel.
      if (imguiHost.IsReady())
      {
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
      if (screenshotMode && frameIndex == SCREENSHOT_FRAME)
      {
        CaptureBackbufferToPng(device, commandList, backbuffer, screenshotPath);
        deviceManager->WaitIdle();
        return EXIT_OK;
      }

      // Transition back to Present for the swapchain.
      const Profiler::CpuScope submit(profiler, "app.cmd.submit");
      commandList->setTextureState(backbuffer, nvrhi::AllSubresources,
                                   nvrhi::ResourceStates::Present);
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

    // One-shot profile dump after the first complete frame — this
    // frame's CPU + GPU spend is effectively the LOAD profile (mesh
    // upload, BLAS build, TLAS rebuild, first PathTracePass dispatch).
    // Subsequent frames are just steady-state render work; logging
    // them every 120 frames was the previous behaviour, which spammed
    // the console. The Performance panel's Loading section keeps the
    // same data visually for users who want to drill in.
    if (frameIndex == 1)
    {
      const FrameProfile frameProfile = renderer.LastFrameProfile();
      const double fps = frameProfile.cpuFrameMs > 0.0
                             ? 1000.0 / frameProfile.cpuFrameMs
                             : 0.0;
      char buf[256];
      std::snprintf(buf, sizeof(buf),
                    "profiler: load complete — cpu %.3f ms  gpu %.3f ms  fps %.1f",
                    frameProfile.cpuFrameMs, frameProfile.gpuFrameMs, fps);
      log.Info(log::APP, buf);
      for (const FrameProfile::PassTiming& timing : frameProfile.passes)
      {
        const char* kind = (timing.kind == FrameProfile::ScopeKind::Cpu) ? "CPU" : "GPU";
        const std::string_view name = timing.name.View();
        char line[256];
        std::snprintf(line, sizeof(line), "profiler:   %*s%s %.*s  %.3f ms",
                      static_cast<int>(timing.depth * 2), "", kind,
                      static_cast<int>(name.size()), name.data(), timing.durationMs);
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
