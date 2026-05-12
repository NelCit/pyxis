// Pyxis app — ViewerMode frame loop.

#include "ViewerMode.h"

#include "Camera/FlyCameraController.h"
#include "Config/Configuration.h"
#include "ImGuiHost.h"

#include <imgui.h>
#include "Output/AovExrSaver.h"
#include "Output/BackbufferScreenshot.h"
#include "Output/TextureReadback.h"
#include "Render/AovRegistry.h"
#include "Render/AovTextures.h"
#include "IngestUsd.h"
#include "Render/HardcodedCubeScene.h"
#include "Scene/SceneResolver.h"

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

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

// _spawnvp for the shader-rebuild path (Windows). Avoids std::system
// (cert-env33-c) by passing argv directly without a shell.
#if defined(_WIN32)
    #include <process.h>
#endif

namespace pyxis::app {

namespace {

constexpr int EXIT_OK = 0;
constexpr int EXIT_DEVICE_INIT_FAIL = 2;

// GLFW key code for Escape. Hard-coded to avoid pulling <GLFW/glfw3.h>
// into pyxis_app (glfw is PRIVATE-linked from pyxis_platform). The
// value has been 256 since GLFW 3.0 and matches USB HID Escape — stable
// enough for "Escape closes the viewer" UX.
constexpr int GLFW_ESCAPE_KEY_CODE = 256;

// Walk up from `start` looking for a CMakeCache.txt file. Returns the
// directory containing it, or an empty path if none found within 8
// levels (covers `build/dev/bin/Release/pyxis.exe` -> `build/dev`).
// Used by the editor's "Reload shaders" path to spawn
// `cmake --build <buildDir> --target pyxis_renderer_shaders` so the
// click recompiles the .slang sources before the renderer re-reads
// the .spv from disk.
[[nodiscard]] std::filesystem::path FindCMakeBuildDir(
    const std::filesystem::path& start) noexcept {
  std::filesystem::path candidate = start;
  for (int depth = 0; depth < 8; ++depth)
  {
    std::error_code errorCode;
    const std::filesystem::path cacheFile = candidate / "CMakeCache.txt";
    if (std::filesystem::exists(cacheFile, errorCode))
      return candidate;
    if (!candidate.has_parent_path() || candidate.parent_path() == candidate)
      break;
    candidate = candidate.parent_path();
  }
  return {};
}

// Result of a ShaderMake-rebuild attempt. `errnoCode` lets the
// caller distinguish "cmake not on PATH" (ENOENT) from "build
// failed" (non-zero exit code) so the editor can show a useful
// hint instead of a generic "rebuild FAILED".
struct ShaderRebuildResult {
  bool ok = false;
  int  errnoCode = 0;   // 0 on success; ENOENT if cmake isn't found
  int  exitCode = 0;    // _spawnvp exit status when ok = false but cmake ran
};

// Spawn `cmake --build <buildDir> --target pyxis_renderer_shaders` and
// block until it completes. The editor's reload-shaders path calls
// this BEFORE asking the renderer to re-read .spv from disk so the
// click also picks up .slang source edits.
//
// Uses _spawnvp (Windows) to avoid the shell — std::system trips
// clang-tidy's cert-env33-c rule and would shell-parse paths.
// Pyxis-side `argv` doesn't go through cmd.exe so quoting / metachars
// in the build-dir path don't matter to OUR child invocation. cmake
// itself may shell out to its tooling (Ninja, MSVC) and inherit the
// path string, so a build dir named with shell metacharacters could
// still misbehave inside cmake's own scripts — out of our perimeter
// but worth knowing if a developer parks the repo at a hostile path.
[[nodiscard]] ShaderRebuildResult RebuildShaderTarget(
    const std::filesystem::path& buildDir) noexcept {
  ShaderRebuildResult result;
  if (buildDir.empty())
    return result;
#if defined(_WIN32)
  const std::string buildDirStr = buildDir.generic_string();
  const char* const argv[] = {
      "cmake", "--build", buildDirStr.c_str(),
      "--target", "pyxis_renderer_shaders",
      nullptr,
  };
  // _spawnvp searches PATH for cmake.exe; _P_WAIT blocks until exit.
  errno = 0;
  const intptr_t spawnResult = _spawnvp(_P_WAIT, "cmake",
                                        const_cast<char* const*>(argv));
  if (spawnResult == -1)
  {
    // Spawn itself failed — cmake not on PATH is the common case
    // (errno == ENOENT). Other errnos are rarer (EACCES, EAGAIN, ...)
    // but we surface whatever the OS reported so the caller can log it.
    result.errnoCode = errno;
    return result;
  }
  result.exitCode = static_cast<int>(spawnResult);
  result.ok = (spawnResult == 0);
  return result;
#else
  (void)buildDir;
  return result;
#endif
}

// Shader-rebuild state machine. Pre-async this whole flow ran
// synchronously inside the click-handler and froze the UI for ~2 s
// while cmake built the .spv. Now the spawn happens on a worker
// thread; ImGuiHost shows "Rebuilding..." while in-flight; the main
// thread polls per-frame and runs waitForIdle + ReloadShaders when
// the worker reports back. The state moves Idle -> InFlight ->
// ResultPending -> Idle. Atomics are unnecessary semantically since
// the only cross-thread write is the state flag and the worker
// writes it exactly once at the end of its body, but we use
// atomic<int> for the formal happens-before.
enum class ShaderRebuildState : int { Idle = 0, InFlight = 1, ResultPending = 2 };

struct ShaderRebuildLatch
{
  std::atomic<int>      state{static_cast<int>(ShaderRebuildState::Idle)};
  ShaderRebuildResult   result{};
  std::filesystem::path lastDir;
  std::thread           worker;
};

// Drain the editor's "Reload shaders" latch + advance the
// rebuild state machine. Click latch is consulted ONLY when the
// state is Idle so a second click during a rebuild can't queue
// parallel work. Called once per frame from the main loop.
void HandleShaderReloadStateMachine(ShaderRebuildLatch& latch,
                                    std::string_view shaderRebuildDirOverride,
                                    ImGuiHost& imguiHost,
                                    nvrhi::IDevice* device,
                                    PyxisRenderer& renderer) noexcept
{
  auto& log = Logging::Get();
  const auto current = static_cast<ShaderRebuildState>(latch.state.load());

  if (current == ShaderRebuildState::Idle && imguiHost.TakeShaderReloadRequest())
  {
    // Resolve build dir: explicit --shader-rebuild-dir override
    // wins; otherwise walk up from cwd looking for CMakeCache.txt.
    std::filesystem::path buildDir;
    if (!shaderRebuildDirOverride.empty())
      buildDir = std::filesystem::path{std::string{shaderRebuildDirOverride}};
    else
      buildDir = FindCMakeBuildDir(std::filesystem::current_path());

    if (buildDir.empty())
    {
      log.Warn(log::APP,
               "ViewerMode: shader reload — no CMakeCache.txt found near cwd "
               "(use --shader-rebuild-dir <path> to override); skipping "
               "ShaderMake rebuild and re-loading the existing .spv.");
      // Skip rebuild but still re-read .spv on the main thread —
      // immediate, no worker needed.
      device->waitForIdle();
      const bool reloadOk = renderer.ReloadShaders();
      log.Info(log::APP, std::string{"ViewerMode: shader reload "}
                              + (reloadOk ? "OK (no rebuild)"
                                          : "FAILED (kept old pipeline)"));
    }
    else
    {
      // Spawn the worker. References capture the latch — the worker
      // writes result and flips the state to ResultPending exactly
      // once before exiting.
      latch.lastDir = buildDir;
      latch.state.store(static_cast<int>(ShaderRebuildState::InFlight));
      latch.worker = std::thread([&latch, buildDir]() noexcept {
        latch.result = RebuildShaderTarget(buildDir);
        latch.state.store(static_cast<int>(ShaderRebuildState::ResultPending));
      });
    }
  }
  else if (current == ShaderRebuildState::ResultPending)
  {
    // Worker is done — join it (cheap; thread already exited),
    // log the outcome with a useful hint when cmake itself wasn't
    // found, then drain the GPU and re-read .spv from disk.
    if (latch.worker.joinable())
      latch.worker.join();
    if (latch.result.ok)
    {
      log.Info(log::APP, "ViewerMode: shader rebuild OK ("
                             + latch.lastDir.generic_string() + ")");
    }
    else if (latch.result.errnoCode == ENOENT)
    {
      log.Error(log::APP,
                "ViewerMode: shader rebuild FAILED — `cmake` not found on PATH. "
                "Install CMake or add it to PATH; re-loading the existing .spv "
                "anyway (any .slang edits won't be picked up).");
    }
    else if (latch.result.errnoCode != 0)
    {
      log.Error(log::APP, "ViewerMode: shader rebuild FAILED — _spawnvp errno="
                              + std::to_string(latch.result.errnoCode)
                              + "; re-loading the existing .spv anyway.");
    }
    else
    {
      log.Warn(log::APP, "ViewerMode: shader rebuild FAILED — cmake exit="
                             + std::to_string(latch.result.exitCode)
                             + " (your .slang has a compile error?); re-loading "
                               "the existing .spv anyway.");
    }
    device->waitForIdle();
    const bool reloadOk = renderer.ReloadShaders();
    log.Info(log::APP, std::string{"ViewerMode: shader reload "}
                            + (reloadOk ? "OK" : "FAILED (kept old pipeline)"));
    latch.state.store(static_cast<int>(ShaderRebuildState::Idle));
  }
  // Push the in-flight flag to the editor so the button can show
  // "Rebuilding..." disabled. Done unconditionally each frame.
  imguiHost.SetShaderRebuildInFlight(
      latch.state.load() != static_cast<int>(ShaderRebuildState::Idle));
}

// Per-frame log line for the load profile, fired once after the
// first complete frame. M3+: that frame's spend is effectively the
// LOAD profile (mesh upload, BLAS build, TLAS rebuild, first
// PathTracePass dispatch); the steady-state per-frame cost is
// captured by the ImGui Performance panel from then on.
void LogFirstFrameProfile(const PyxisRenderer& renderer) noexcept
{
  auto& log = Logging::Get();
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

// Resolve the picker pixel for this frame. Pinned takes the latched
// UV (renormalised against the current AOV size so the pin stays at
// the same screen location through resize); otherwise the live
// cursor, clamped to the AOV bounds, with the sentinel
// MOUSE_PIXEL_NONE for "no hover".
void ResolvePickerPixel(const ImGuiHost& imguiHost,
                        const AovTextures& aovs,
                        std::atomic<int32_t>& latestMousePixelX,
                        std::atomic<int32_t>& latestMousePixelY,
                        RenderSettings& settings) noexcept
{
  if (imguiHost.IsReady() && imguiHost.IsPickerPinned())
  {
    const uint32_t pinnedX = static_cast<uint32_t>(
        imguiHost.PickerPinnedU() * static_cast<float>(aovs.width));
    const uint32_t pinnedY = static_cast<uint32_t>(
        imguiHost.PickerPinnedV() * static_cast<float>(aovs.height));
    // Clamp to last-row / last-column in case the float
    // multiplication landed on an exclusive upper bound.
    settings.mousePixelX = (pinnedX < aovs.width)  ? pinnedX  : (aovs.width  - 1);
    settings.mousePixelY = (pinnedY < aovs.height) ? pinnedY  : (aovs.height - 1);
  }
  else
  {
    const int32_t mouseX = latestMousePixelX.load();
    const int32_t mouseY = latestMousePixelY.load();
    if (mouseX >= 0 && mouseY >= 0
        && static_cast<uint32_t>(mouseX) < aovs.width
        && static_cast<uint32_t>(mouseY) < aovs.height)
    {
      settings.mousePixelX = static_cast<uint32_t>(mouseX);
      settings.mousePixelY = static_cast<uint32_t>(mouseY);
    }
  }
}

// Drain the editor's "Save current AOV..." latch. Opens a one-shot
// command list on the existing handle, copies the chosen raw AOV
// into a CpuAccessMode::Read staging texture, runs through
// executeCommandList + waitForIdle, then maps + converts to RGBA32F
// and writes the EXR. Stalls the loop briefly — acceptable for a
// one-click human action.
void HandleSaveAovLatch(ImGuiHost& imguiHost,
                        const AovTextures& aovs,
                        nvrhi::IDevice* device,
                        nvrhi::ICommandList* commandList) noexcept
{
  if (!imguiHost.IsReady())
    return;
  std::string saveAovPath;
  if (!imguiHost.TakeSaveAovRequest(saveAovPath))
    return;

  auto& log = Logging::Get();
  const RenderSettings::DebugView pickedView = imguiHost.GetDebugView();
  // Resolve via the shared registry — single source of truth for
  // (DebugView -> texture + filename suffix) across viewer +
  // headless.
  const AovEntry* entry = FindAovByDebugView(pickedView);
  nvrhi::ITexture* sourceAov = (entry != nullptr)
                                   ? (aovs.*entry->texturePtr).Get()
                                   : nullptr;
  const std::string_view aovLabel =
      (entry != nullptr) ? entry->name : std::string_view{"color"};
  if (sourceAov == nullptr)
  {
    log.Error(log::APP, "ViewerMode: save AOV: source texture is null");
  }
  else if (auto saveResult = SaveAovAsExr(device, commandList, sourceAov,
                                          aovLabel, saveAovPath);
           !saveResult)
  {
    log.Error(log::APP, "ViewerMode: " + saveResult.error());
  }
  else
  {
    log.Info(log::APP, "ViewerMode: save AOV (" + std::string{aovLabel}
                           + ") -> " + saveAovPath);
  }
}

}  // namespace


int RunViewerLoop(const Configuration& config, const ResolvedScene& resolvedScene,
                  std::string_view screenshotPath,
                  std::string_view shaderRebuildDirOverride) noexcept {
  auto& log = Logging::Get();

  // ShaderMake rebuild latch + state machine. Click handler in
  // HandleShaderReloadStateMachine spawns a worker thread; main loop
  // polls per-frame for the result. See the helper for state-flow
  // details.
  ShaderRebuildLatch shaderRebuild;

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
  // M7 follow-up — latest cursor pixel for the AOV picker. Updated
  // by every MouseMove event below, clamped to the AOV size right
  // before each RenderFrame. Sentinel = "no hover" until a move
  // event lands.
  std::atomic<int32_t> latestMousePixelX{-1};
  std::atomic<int32_t> latestMousePixelY{-1};
  // Click-to-select state. LMB-down records the pixel; LMB-up
  // compares the displacement to a small radius to distinguish a
  // click from a camera drag. Threshold = 4 pixels matches the
  // typical OS-level drag threshold (Windows DPI-default 4 px).
  // pendingClickPixel{X,Y} latches the cursor on a confirmed click
  // for the main thread to drain after RenderFrame retires.
  constexpr int32_t CLICK_RADIUS_PX = 4;
  std::atomic<int32_t> mouseDownPixelX{-1};
  std::atomic<int32_t> mouseDownPixelY{-1};
  std::atomic<int32_t> pendingClickPixelX{-1};
  std::atomic<int32_t> pendingClickPixelY{-1};
  window->SetEventSink([&](const InputEvent& event) {
    if (event.kind == InputEventKind::WindowClose)
      shouldClose.store(true);
    if (event.kind == InputEventKind::KeyDown && event.key == GLFW_ESCAPE_KEY_CODE)
    {
      shouldClose.store(true);
    }
    if (event.kind == InputEventKind::MouseMove)
    {
      latestMousePixelX.store(static_cast<int32_t>(event.mouseX));
      latestMousePixelY.store(static_cast<int32_t>(event.mouseY));
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

    // Click-vs-drag detection (M7 follow-up). Only LMB is checked —
    // RMB is reserved for FlyCameraController's orbit. On LMB-down,
    // record the cursor pixel; on LMB-up, if the displacement is
    // smaller than CLICK_RADIUS_PX, treat it as a click and latch
    // the pixel. The main loop drains the latch after RenderFrame
    // so it can read the renderer's just-produced pick result.
    // Sentinel = -1 means "no down event in flight".
    if (event.kind == InputEventKind::MouseButtonDown && event.key == 0)
    {
      mouseDownPixelX.store(latestMousePixelX.load());
      mouseDownPixelY.store(latestMousePixelY.load());
    }
    else if (event.kind == InputEventKind::MouseButtonUp && event.key == 0)
    {
      const int32_t downX = mouseDownPixelX.exchange(-1);
      const int32_t downY = mouseDownPixelY.exchange(-1);
      if (downX >= 0 && downY >= 0)
      {
        const int32_t upX = latestMousePixelX.load();
        const int32_t upY = latestMousePixelY.load();
        const int32_t deltaX = upX - downX;
        const int32_t deltaY = upY - downY;
        if (deltaX * deltaX + deltaY * deltaY <= CLICK_RADIUS_PX * CLICK_RADIUS_PX)
        {
          pendingClickPixelX.store(upX);
          pendingClickPixelY.store(upY);
        }
      }
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

  // M4 ingest. Both adapters share the unified IngestUsd entry
  // point today (StageWalker covers both, satisfying §25.O.3 byte-
  // equal). Either branch failing or returning "nothing emitted"
  // falls back to the M3 hardcoded cube so pyxis.exe always renders.
  // Local lambda so the editor's "Open scene..." path below can reuse
  // the same dispatch. Copies the five timing fields out of IngestStats
  // into ImGuiHost::IngestProfile for the Loading-panel breakdown;
  // returns true iff any meshes / cameras landed (matches the prior
  // bool semantics).
  // Pending state captured from each ingest run. Cameras + active
  // index are pushed to ImGuiHost::SetSceneCameras after Init runs;
  // ingest profile + source label feed the Loading panel.
  std::vector<ImGuiHost::SceneCameraEntry> pendingSceneCameras;
  int                                      pendingActiveCameraIdx = -1;
  auto loadScene = [&](std::string_view             path,
                       std::string_view             adapterLabel,
                       ImGuiHost::IngestProfile&    outProfile) -> bool {
    const pyxis::usd_ingest::IngestResult result =
        IngestUsd(adapterLabel, path, gpuScene);
    const pyxis::usd_ingest::IngestStats& stats = result.Stats();
    outProfile.totalMs           = stats.timings.totalMs;
    outProfile.stageOpenMs       = stats.timings.stageOpenMs;
    outProfile.traverseSortMs    = stats.timings.traverseSortMs;
    outProfile.materialPassMs    = stats.timings.materialPassMs;
    outProfile.instancerPassMs   = stats.timings.instancerPassMs;
    outProfile.meshLightCameraMs = stats.timings.meshLightCameraMs;
    // Snapshot the camera list for the editor's Scene-Camera combo.
    // Pull each camera through the §18.9-compliant fixed-buffer view
    // accessor — std::strings live behind PIMPL on the result and
    // truncate into a 256-char inline buffer here.
    pendingSceneCameras.clear();
    const uint32_t cameraCount = result.GetCameraCount();
    pendingSceneCameras.reserve(cameraCount);
    for (uint32_t i = 0; i < cameraCount; ++i)
    {
      pyxis::usd_ingest::NamedCameraView cameraView{};
      if (!result.GetCameraAt(i, &cameraView))
        continue;
      ImGuiHost::SceneCameraEntry entry;
      entry.name = cameraView.name;
      entry.desc = cameraView.desc;
      pendingSceneCameras.push_back(std::move(entry));
    }
    pendingActiveCameraIdx = stats.activeCameraIndex;
    return stats.meshesEmitted > 0 || stats.camerasEmitted > 0;
  };

  // Startup ingest. ImGuiHost isn't built yet (Init runs further down)
  // so we stash the breakdown + adapter label in pendingIngest{Profile,
  // Source} and push them into the panel once ImGuiHost is ready below.
  ImGuiHost::IngestProfile pendingIngestProfile{};
  std::string              pendingIngestSource;
  bool sceneLoaded = false;
  if (!resolvedScene.path.empty())
  {
    sceneLoaded = loadScene(resolvedScene.path, config.app.ingest, pendingIngestProfile);
    pendingIngestSource = config.app.ingest;
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
  else if (pendingIngestSource.empty())
  {
    pendingIngestSource = "cube_fallback";
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
  // Non-const so the swapchain-rebuilt branch below can recreate it
  // when the OS window resizes. PathTracePass dispatches against
  // aovs.{width,height} and we copyTexture aovs.color -> backbuffer
  // afterwards, so a stale AOV would render a clipped picture into a
  // resized backbuffer.
  AovTextures aovs = std::move(*aovsResult);

  RendererCreateDesc rendererDesc{};
  rendererDesc.initialWidth = winDesc.width;
  rendererDesc.initialHeight = winDesc.height;
  // Thread the device-manager's active FIF so the renderer's
  // PassContext carries it through to passes that care (today only
  // PathTracePass's picker readback). Viewer pins FIF=1 today via
  // its device-creation params.
  rendererDesc.framesInFlight = deviceManager->GetFramesInFlight();
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
  // Push the startup ingest timing into the Performance / Loading
  // section now that ImGuiHost is ready. Done unconditionally — the
  // setter is a noop on the panel side until the first BuildFpsPanel
  // call rebuilds the Loading section.
  if (imguiHost.IsReady() && !pendingIngestSource.empty())
  {
    imguiHost.SetIngestProfile(pendingIngestProfile, pendingIngestSource);
  }
  // Push the startup scene-camera list into the editor's combo. Empty
  // list (cube fixture) silently disables the combo.
  if (imguiHost.IsReady())
  {
    imguiHost.SetSceneCameras(pendingSceneCameras, pendingActiveCameraIdx);
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

      // Drain the editor's Scene-Camera combo selection (if any) and
      // push the move-speed slider. Both are fire-and-forget — the
      // controller absorbs whatever the UI provides without blocking
      // the rest of the frame loop. Order: snap FIRST so the same-
      // frame Update() integrates from the new pose; speed second so
      // the WASD integration in Update() sees the latest value.
      if (imguiHost.IsReady())
      {
        int snapIdx = -1;
        if (imguiHost.TakeSnapToSceneCameraRequest(snapIdx))
        {
          const auto& cams = imguiHost.SceneCameras();
          if (snapIdx >= 0 && static_cast<std::size_t>(snapIdx) < cams.size())
          {
            const auto& chosen = cams[static_cast<std::size_t>(snapIdx)];
            cameraController.SnapToCamera(chosen.desc);
            // Also push into GpuScene so the projection / lens
            // intrinsics (focal / fStop / clip) match the chosen
            // camera, not the previously-active one. The controller
            // overwrites viewFromWorld each Update() but leaves the
            // intrinsic fields alone.
            gpuScene.SetCamera(chosen.desc);
            log.Info(log::APP,
                     "ViewerMode: snapped FlyCam to scene camera " + chosen.name);
          }
        }
        cameraController.SetMoveSpeed(imguiHost.MoveSpeed());
      }

      cameraController.Update(dtSeconds, gpuScene);

      // Push the FlyCam's post-Update pose into the editor's readout.
      // Cheap (one float3 + float4 copy); the editor's TextDisabled
      // rows update each frame so the user sees the live pose as
      // they fly around.
      if (imguiHost.IsReady())
      {
        imguiHost.SetCameraPose(cameraController.Position(),
                                cameraController.OrientationQuat());
      }
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

    // Swapchain rebuilt (resize, fullscreen toggle). vkDeviceWaitIdle
    // has already happened inside the device manager's resize path so
    // it's safe to:
    //   - notify ImGui (image-count + per-frame ring),
    //   - re-create the AOV color texture at the new backbuffer size
    //     (PathTracePass dispatches at aovs.{width,height} and we
    //     copyTexture aovs.color -> backbuffer afterwards; a stale
    //     AOV renders a clipped image into the resized backbuffer),
    //   - tell PyxisRenderer (its Resize is currently a no-op for M3
    //     but will resize accumulation buffers at M5+).
    const uint32_t currentSwapchainGeneration = deviceManager->GetSwapchainGeneration();
    if (currentSwapchainGeneration != lastSeenSwapchainGeneration)
    {
      if (imguiHost.IsReady())
      {
        imguiHost.OnSwapchainRebuilt(deviceManager->GetBackbufferCount());
      }
      if (auto* freshBackbuffer = deviceManager->GetCurrentBackbuffer(); freshBackbuffer != nullptr)
      {
        const auto& bbDesc = freshBackbuffer->getDesc();
        if (bbDesc.width != aovs.width || bbDesc.height != aovs.height)
        {
          auto rebuiltAovs = AovTextures::Create(device, bbDesc.width, bbDesc.height);
          if (rebuiltAovs)
          {
            aovs = std::move(*rebuiltAovs);
            renderer.Resize(bbDesc.width, bbDesc.height);
          }
          else
          {
            log.Error(log::APP,
                      "ViewerMode: AOV recreate failed on resize: " + rebuiltAovs.error());
          }
        }
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
      // M11 — drain rolling p50/p99/max into a stack-sized buffer so
      // the Performance panel's "Rolling" section can show per-pass
      // KPIs alongside the current-frame breakdown. 64 is comfortable
      // above our scope-count high-water (~16) without burning heap.
      Profiler::RollingStat rollingScratch[64];
      const std::uint32_t rollingCount =
          profiler.GetRollingStats(rollingScratch,
                                   static_cast<std::uint32_t>(std::size(rollingScratch)));
      imguiHost.BeginFrame();
      imguiHost.BuildFpsPanel(frameProfile,
          std::span<const Profiler::RollingStat>(rollingScratch, rollingCount));
      imguiHost.BuildScenePanel(sceneStats);
      imguiHost.BuildEditorPanel(gpuScene);

      // Drain the editor's "Reload shaders" latch + advance the
      // async-rebuild state machine. Click handler waits for the GPU
      // idle (so no in-flight command buffer references the old
      // pipeline) before asking the renderer to re-read .spv from
      // disk. Failure stays silent in the panel; the log line is the
      // user-visible feedback.
      HandleShaderReloadStateMachine(shaderRebuild, shaderRebuildDirOverride,
                                     imguiHost, device, renderer);

      // Drain the editor's "Open scene..." latch. Same waitForIdle
      // discipline plus a GpuScene::Clear before re-running the
      // ingest engine; the new ingest time pushes back into the
      // Loading section via SetIngestProfile.
      std::string sceneReloadPath;
      if (imguiHost.TakeSceneReloadRequest(sceneReloadPath))
      {
        device->waitForIdle();
        gpuScene.Clear();
        // Reset the FlyCam's seed bit so the next Update re-seeds
        // from whatever camera the new scene authors. Without this
        // the FlyCam would silently keep the old scene's pose.
        // Move-speed survives the reset (user preference).
        cameraController.Reset();
        ImGuiHost::IngestProfile reloadProfile{};
        const bool reloadOk = loadScene(sceneReloadPath, config.app.ingest, reloadProfile);
        // Push the new scene-camera list regardless of reloadOk —
        // empty list on failure clears the old combo entries.
        imguiHost.SetSceneCameras(pendingSceneCameras, pendingActiveCameraIdx);
        if (reloadOk)
        {
          imguiHost.SetIngestProfile(reloadProfile, config.app.ingest);
          log.Info(log::APP, "ViewerMode: scene reload OK (" + sceneReloadPath + ")");
        }
        else
        {
          // Fall back to the hardcoded cube so the viewer keeps
          // rendering instead of going dark on a bad path.
          if (auto cubeResult =
                  BuildHardcodedCubeScene(gpuScene, winDesc.width, winDesc.height);
              !cubeResult)
          {
            log.Error(log::APP, "ViewerMode: scene reload + cube fallback both failed: "
                                    + cubeResult.error());
          }
          imguiHost.SetIngestProfile(reloadProfile, "cube_fallback");
          log.Warn(log::APP,
                   "ViewerMode: scene reload FAILED (" + sceneReloadPath
                       + "); falling back to cube");
        }
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
      // M7 follow-up — wire the raw AOV outputs + pick buffer pair so
      // the AOV inspector / picker / Save EXR all have data to read.
      // The pass tolerates nullptr (binds 1×1 fallbacks); we feed the
      // real AovTextures handles unconditionally for the viewer path.
      targets.colorHdr          = aovs.colorHdr.Get();
      targets.normalAov         = aovs.normal.Get();
      targets.depthAov          = aovs.depth.Get();
      targets.primIdAov         = aovs.primId.Get();
      targets.materialIdAov     = aovs.materialId.Get();
      targets.baseColorAov      = aovs.baseColor.Get();
      targets.worldPosAov       = aovs.worldPos.Get();
      targets.alphaAov          = aovs.alpha.Get();
      targets.elementIdAov      = aovs.elementId.Get();
      targets.normalEyeAov      = aovs.normalEye.Get();
      targets.worldPosEyeAov    = aovs.worldPosEye.Get();
      targets.pickResult        = aovs.pickResult.Get();
      targets.pickResultStaging = aovs.pickResultStaging.Get();

      RenderSettings settings{};
      settings.width = aovs.width;
      settings.height = aovs.height;
      // Push the AOV inspector state into the renderer. The ImGui
      // Editor combo flips _editorDebugView; the cursor pump above
      // captures latestMousePixelXY from MouseMove events and we
      // clamp here to the viewport bounds (raygen treats the sentinel
      // PICK_PIXEL_NONE / RenderSettings::MOUSE_PIXEL_NONE as "no
      // hover -> skip pick write").
      if (imguiHost.IsReady())
      {
        settings.debugView = imguiHost.GetDebugView();
        settings.worldPosPeriod = imguiHost.GetWorldPosPeriod();
      }
      // Picker pixel: pinned takes the latched normalised UV
      // (renormalised against the current AOV size each frame so a
      // resize keeps the pin at the same screen location);
      // otherwise the live cursor clamped to AOV bounds.
      ResolvePickerPixel(imguiHost, aovs, latestMousePixelX, latestMousePixelY, settings);

      renderer.RenderFrame(commandList, settings, targets);

      // Push the (one-frame-stale) pick readback into the Editor so
      // the AOV inspector's Picker readout updates each frame. Cheap
      // copy of a 32-byte POD; safe even when the renderer hasn't
      // produced a real pick yet (default-constructed = depth -1).
      // Also push the current AOV aspect so the Camera section can
      // rebuild projFromView correctly when the user drags FOV /
      // focal length / near / far. Re-sized AOVs flow through the
      // same path on the next frame after a window resize.
      if (imguiHost.IsReady())
      {
        const PickResult pickThisFrame = renderer.LastPickResult();
        imguiHost.SetLastPickResult(pickThisFrame);
        imguiHost.SetRenderDims(aovs.width, aovs.height);
        // Drain the click latch the event sink set on a non-drag
        // LMB release. The picker is one frame stale by design (raygen
        // wrote pickResult last frame -> staging copy retired now ->
        // map at top of this Execute), so the instance id we read
        // here corresponds to the click pixel within ~16 ms — easily
        // good enough for human "I clicked the orange sphere".
        // First-frame guard: pickThisFrame.pixelX == PICK_PIXEL_NONE
        // means the renderer hasn't produced ANY pick yet (no map
        // happened, the staging buffer holds default-constructed
        // garbage from PickResult{}). Drop the click in that case
        // so we don't snap to a fabricated instance id.
        // Background-click guard: instanceId == INSTANCE_ID_NONE
        // means the cursor was over the background; silently drop
        // so the Material combo doesn't reset on a stray miss.
        const int32_t clickX = pendingClickPixelX.exchange(-1);
        const int32_t clickY = pendingClickPixelY.exchange(-1);
        if (clickX >= 0 && clickY >= 0
            && pickThisFrame.pixelX != PICK_PIXEL_NONE
            && pickThisFrame.instanceId != INSTANCE_ID_NONE)
        {
          imguiHost.SetClickedInstance(pickThisFrame.instanceId);
        }
      }

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
        const bool screenshotOk =
            CaptureBackbufferToPng(device, commandList, backbuffer, screenshotPath);
        if (!screenshotOk)
          log.Error(log::APP, "ViewerMode: --screenshot capture failed (see prior errors)");
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

    // Drain the editor's "Save current AOV..." latch AFTER the
    // frame's main submit + present so the AOV holds freshly-written
    // data. The helper opens a one-shot command list on the existing
    // handle and stalls briefly on waitForIdle — acceptable for a
    // one-click human action.
    HandleSaveAovLatch(imguiHost, aovs, device, commandListHandle.Get());

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
      LogFirstFrameProfile(renderer);
  }

  log.Info(log::APP, "ViewerMode: frame loop exited; tearing down");
  // Block on any in-flight shader rebuild before destroying the
  // worker thread — std::thread's destructor calls std::terminate on
  // a joinable thread. The worker is just spawning cmake so the join
  // is bounded by ShaderMake's runtime (~2 s); fine for shutdown.
  if (shaderRebuild.worker.joinable())
    shaderRebuild.worker.join();
  deviceManager->WaitIdle();
  imguiHost.Shutdown();
  return EXIT_OK;
}

}  // namespace pyxis::app
