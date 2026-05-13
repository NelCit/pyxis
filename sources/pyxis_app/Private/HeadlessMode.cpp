// Pyxis app — headless + viewer mode drivers.

#include "HeadlessMode.h"

#include "Config/Configuration.h"
#include "Output/AovExrSaver.h"
#include "Output/ExrWriter.h"
#include "Output/PngWriter.h"
#include "Output/TextureReadback.h"
#include "Render/AovRegistry.h"
#include "Render/AovTextures.h"
#include "IngestUsd.h"
#include "Render/HardcodedCubeScene.h"
#include "Scene/SceneResolver.h"
#include "ViewerMode.h"

#include <Pyxis/Platform/Device/DeviceCreationParams.h>
#include <Pyxis/Platform/Device/IDeviceManager.h>
#include <Pyxis/Platform/Device/Resolution.h>
#include <Pyxis/Platform/Logging/Log.h>
#include <Pyxis/Platform/Logging/LogCategories.h>
#include <Pyxis/Renderer/Descs/GpuSceneCreateDesc.h>
#include <Pyxis/Renderer/Descs/RendererCreateDesc.h>
#include <Pyxis/Renderer/Descs/RenderSettings.h>
#include <Pyxis/Renderer/Descs/RenderTargets.h>
#include <Pyxis/Renderer/Forward.h>  // MAX_FRAMES_IN_FLIGHT (§33.1).
#include <Pyxis/Renderer/GpuScene.h>
#include <Pyxis/Renderer/Profiler.h>
#include <Pyxis/Renderer/PyxisRenderer.h>
#include <Pyxis/Renderer/SceneWorldFacade.h>
#include <Pyxis/Renderer/Version.h>

#include <nvrhi/nvrhi.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <ios>
#include <memory>

namespace pyxis::app {

namespace {

constexpr int EXIT_OK = 0;
constexpr int EXIT_DEVICE_INIT_FAIL = 2;  // Vulkan / NVRHI init failure.
constexpr int EXIT_CONFIG_FAIL = 3;       // ValidateForHeadless rejected the config.
constexpr int EXIT_RUNTIME_FAIL = 4;      // Render-time failure: scene init, staging,
                                          // mapping, or EXR write.

// Strip a trailing ".exr" (case-insensitive) from `path` to produce
// the per-AOV output prefix. The headless flow writes
// `<prefix>_<aov>.exr` for each --save-aov entry; the prefix is the
// `--output` path stripped of its extension so a user passing
// `--output frame.exr --save-aov color,normal` gets `frame.exr` (the
// existing BGRA8) plus `frame_color.exr` and `frame_normal.exr`. If
// the path didn't end in `.exr`, return it unchanged so a path with
// no extension still composes naturally.
std::string DeriveAovPrefix(std::string_view outputPath) noexcept {
  std::string result{outputPath};
  if (result.size() >= 4)
  {
    const std::string tail = result.substr(result.size() - 4);
    if ((tail[0] == '.')
        && (tail[1] == 'e' || tail[1] == 'E')
        && (tail[2] == 'x' || tail[2] == 'X')
        && (tail[3] == 'r' || tail[3] == 'R'))
    {
      result.resize(result.size() - 4);
    }
  }
  return result;
}

// Log the §33.7 determinism pin summary at headless startup. Pure
// logging — no state mutation — extracted so the orchestrator stays
// linear.
void LogDeterminismPin(const Configuration& config, uint32_t framesInFlight) noexcept
{
  auto& log = Logging::Get();
  if (config.limits.framesInFlight != framesInFlight)
  {
    log.Info(log::APP,
             "headless: §33.7 pinning framesInFlight=" + std::to_string(framesInFlight)
                 + " (config requested " + std::to_string(config.limits.framesInFlight) + ")");
  }
  std::string summary = "headless: determinism pin — seed=";
  summary += std::to_string(config.render.seed);
  summary += "  framesInFlight=" + std::to_string(framesInFlight);
  summary += "  dims=" + std::to_string(config.render.width) + "x"
             + std::to_string(config.render.height);
  summary += "  samples=" + std::to_string(config.render.samplesPerFrame);
  log.Info(log::APP, summary);
}

// Load the resolved scene through the active ingest adapter, falling
// back to the M3 hardcoded cube if the path is empty or no meshes /
// cameras landed. Returns false only if the cube fallback itself
// failed (catastrophic — no renderable scene available).
[[nodiscard]] bool LoadSceneOrFallback(const Configuration& config,
                                       const ResolvedScene& resolvedScene,
                                       GpuScene& gpuScene,
                                       pyxis::usd_ingest::IngestStats* outStats,
                                       std::string_view populationMask = {},
                                       double frameNumber = -1.0,
                                       std::string_view loadMode = {},
                                       std::string_view variantSelections = {}) noexcept
{
  bool sceneLoaded = false;
  if (!resolvedScene.path.empty())
  {
    const pyxis::usd_ingest::IngestResult result =
        IngestUsd(config.app.ingest, resolvedScene.path, gpuScene,
                  populationMask, frameNumber, loadMode, variantSelections);
    const pyxis::usd_ingest::IngestStats& stats = result.Stats();
    sceneLoaded = stats.meshesEmitted > 0 || stats.camerasEmitted > 0;
    if (outStats != nullptr)
      *outStats = stats;
  }
  if (!sceneLoaded)
  {
    if (auto cubeResult =
            BuildHardcodedCubeScene(gpuScene, config.render.width, config.render.height);
        !cubeResult)
    {
      Logging::Get().Error(log::APP, "headless: " + cubeResult.error());
      return false;
    }
  }
  return true;
}

// Open the command list, drain GpuScene mutations, dispatch one
// PathTracePass via PyxisRenderer, transition the offscreen RT to
// CopySource, close + execute. Returns false if CommitResources
// failed; caller's responsibility to clean up after a false return.
[[nodiscard]] bool RecordAndExecuteRenderFrame(nvrhi::IDevice* device,
                                               nvrhi::ICommandList* commandList,
                                               GpuScene& gpuScene,
                                               PyxisRenderer& renderer,
                                               const AovTextures& aovs,
                                               nvrhi::ITexture* renderTarget) noexcept
{
  commandList->open();

  // Drain pending GpuScene mutations onto the open command list
  // (mesh upload, BLAS build, TLAS rebuild). Failure aborts the
  // frame; partial state stays dirty so the next CommitResources
  // retries.
  if (auto commitResult = gpuScene.CommitResources(commandList); !commitResult)
  {
    Logging::Get().Error(log::APP,
                         "headless: " + std::string{commitResult.error().message.View()});
    commandList->close();
    return false;
  }

  RenderTargets targets{};
  targets.color = renderTarget;
  // M7 follow-up — bind the raw AOV outputs so the user can dump
  // them via --save-aov. Always bound (not just when --save-aov is
  // set) because PathTracePass's UAV writes are unconditional;
  // without these the writes go to the 1×1 fallbacks PathTracePass
  // owns, which is fine but wastes the already-allocated AovTextures
  // memory. Same RenderTargets shape ViewerMode wires.
  targets.colorHdr       = aovs.colorHdr.Get();
  targets.normalAov      = aovs.normal.Get();
  targets.depthAov       = aovs.depth.Get();
  targets.primIdAov      = aovs.primId.Get();
  targets.materialIdAov  = aovs.materialId.Get();
  targets.baseColorAov   = aovs.baseColor.Get();
  targets.worldPosAov    = aovs.worldPos.Get();
  targets.alphaAov       = aovs.alpha.Get();
  targets.elementIdAov   = aovs.elementId.Get();
  targets.normalEyeAov   = aovs.normalEye.Get();
  targets.worldPosEyeAov = aovs.worldPosEye.Get();
  RenderSettings settings{};
  settings.width = renderTarget->getDesc().width;
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
  return true;
}

// True iff `path` ends with `.png` (case-insensitive). Lets the
// readback path pick PNG (golden tests, human-inspectable) vs EXR
// (HDR / AOV) by file extension alone — no extra CLI flag.
[[nodiscard]] bool PathHasPngExtension(std::string_view path) noexcept
{
  if (path.size() < 4)
    return false;
  const auto tail = path.substr(path.size() - 4);
  if (tail[0] != '.')
    return false;
  const auto lower = [](char chr) -> char {
    return (chr >= 'A' && chr <= 'Z') ? static_cast<char>(chr + ('a' - 'A')) : chr;
  };
  return lower(tail[1]) == 'p' && lower(tail[2]) == 'n' && lower(tail[3]) == 'g';
}

// Run TextureReadback through phase 1 (record copy) + phase 2 (map
// the staging buffer), warn if the entire image is black (silent
// PathTracePass no-op detector), then write the BGRA8 image to disk.
// File format is selected by the `outputPath` extension: `.png`
// dispatches to WritePngBgra8 (sRGB-encoded, human-inspectable,
// golden-test friendly); anything else dispatches to WriteExrBgra8
// (linear-float, HDR-friendly, the historical default).
// Returns false on any failure.
[[nodiscard]] bool ReadbackAndWriteExr(nvrhi::IDevice* device,
                                       nvrhi::ICommandList* commandList,
                                       nvrhi::ITexture* renderTarget,
                                       std::string_view outputPath) noexcept
{
  auto& log = Logging::Get();

  // TextureReadback is the shared helper — same pattern is used by
  // ViewerMode::CaptureBackbufferToPng. Phase 1 (RecordCopy) records
  // the staging-texture allocation + copy on a fresh command list;
  // we then close + execute + waitForIdle so the GPU has retired the
  // copy before Phase 2 (Map) hands us the host pointer. The handle
  // unmaps on scope exit.
  commandList->open();
  auto readback =
      TextureReadback::RecordCopy(device, commandList, renderTarget, "headless-readback-staging");
  if (!readback)
  {
    log.Error(log::APP, "headless: " + readback.error());
    return false;
  }
  commandList->close();
  device->executeCommandList(commandList);
  device->waitForIdle();
  device->runGarbageCollection();

  if (auto mapResult = readback->Map(); !mapResult)
  {
    log.Error(log::APP, "headless: " + mapResult.error());
    return false;
  }
  // Sanity check: a render that worked has SOME non-black pixels
  // (cube colour from closesthit or sky from miss). All-zero output
  // would mean PathTracePass silently no-op'd; surface that as a
  // log warning so a regression doesn't go quietly.
  {
    const auto* bytes = static_cast<const uint8_t*>(readback->Data());
    bool anyNonBlack = false;
    for (uint32_t row = 0; row < readback->Height() && !anyNonBlack; ++row)
    {
      const uint8_t* rowPtr = bytes + (row * readback->RowPitch());
      for (uint32_t col = 0; col < readback->Width(); ++col)
      {
        if (rowPtr[col * 4 + 0] != 0 || rowPtr[col * 4 + 1] != 0 || rowPtr[col * 4 + 2] != 0)
        {
          anyNonBlack = true;
          break;
        }
      }
    }
    log.Info(log::APP,
             anyNonBlack ? "headless: render produced non-black pixels (looks valid)"
                         : "headless: render output is fully black — PathTracePass likely skipped");
  }

  auto writeResult = PathHasPngExtension(outputPath)
                         ? WritePngBgra8(outputPath, readback->Width(), readback->Height(),
                                         readback->Data(), readback->RowPitch())
                         : WriteExrBgra8(outputPath, readback->Width(), readback->Height(),
                                         readback->Data(), readback->RowPitch());
  if (!writeResult)
  {
    log.Error(log::APP, "headless: " + writeResult.error());
    return false;
  }
  return true;
}

// Parse the --save-aov comma-separated list and dispatch one
// SaveAovAsExr per entry. The "all" alias expands to every entry in
// AOV_REGISTRY. Unknown names log a warning but don't fail the run —
// keeps each per-AOV write independent so a typo doesn't drop
// legitimate outputs. Always succeeds (errors are per-entry warnings).
void SaveAovsFromList(std::string_view saveAovList,
                      std::string_view aovPrefix,
                      const AovTextures& aovs,
                      nvrhi::IDevice* device,
                      nvrhi::ICommandList* commandList) noexcept
{
  auto& log = Logging::Get();
  auto saveOne = [&](std::string_view aovName, nvrhi::ITexture* sourceAov) {
    if (sourceAov == nullptr)
    {
      log.Warn(log::APP, "headless: --save-aov: source for '" + std::string{aovName}
                             + "' is null; skipping");
      return;
    }
    const std::string targetPath = BuildAovOutputPath(aovPrefix, aovName);
    if (auto saveResult = SaveAovAsExr(device, commandList, sourceAov, aovName, targetPath);
        !saveResult)
    {
      log.Error(log::APP, "headless: " + saveResult.error());
    }
    else
    {
      log.Info(log::APP, "headless: --save-aov " + std::string{aovName}
                             + " -> " + targetPath);
    }
  };
  auto resolveAndSave = [&](std::string_view aovName) {
    // Single source of truth for the name -> texture mapping —
    // matches ViewerMode's Save AOV button via the same registry.
    const AovEntry* entry = FindAovByName(aovName);
    if (entry == nullptr)
    {
      log.Warn(log::APP, "headless: --save-aov: unknown AOV name '"
                             + std::string{aovName}
                             + "' (recognised: color,normal,depth,primId,"
                               "materialId,baseColor,worldPos,alpha,elementId,"
                               "normalEye,worldPosEye,all)");
      return;
    }
    saveOne(entry->name, (aovs.*entry->texturePtr).Get());
  };

  // Token-iterate the comma-list. Each name dispatches independently;
  // an "all" token expands to every entry in AOV_REGISTRY.
  std::size_t cursor = 0;
  while (cursor < saveAovList.size())
  {
    const std::size_t comma = saveAovList.find(',', cursor);
    const std::string_view name = (comma == std::string_view::npos)
                                      ? saveAovList.substr(cursor)
                                      : saveAovList.substr(cursor, comma - cursor);
    cursor = (comma == std::string_view::npos) ? saveAovList.size() : (comma + 1);
    if (name.empty())
      continue;
    if (name == "all")
    {
      for (const AovEntry& entry : AOV_REGISTRY)
        saveOne(entry.name, (aovs.*entry.texturePtr).Get());
    }
    else
    {
      resolveAndSave(name);
    }
  }
}

}  // namespace

int RunHeadless(const Configuration& config, const ResolvedScene& resolvedScene,
                std::string_view saveAovList, uint32_t benchFrames,
                std::string_view profilePath,
                std::string_view populationMask,
                double frameNumber,
                int /*frameRangeBegin*/,
                int /*frameRangeEnd*/,
                int /*frameRangeStep*/,
                std::string_view loadMode,
                std::string_view variantSelections) noexcept {
  // V2.A.4 — the multi-frame loop lives in Application::Run (it
  // overrides config.output.image per frame and re-enters here). The
  // frameRangeBegin/End/Step params are accepted on the signature so
  // future in-process loops have a target; the single-frame path
  // ignores them.
  auto& log = Logging::Get();

  // §27 ValidateForHeadless: non-zero seed (§33.7), non-empty
  // output.image, non-zero render dims. Surfaces a config-fail exit
  // before we even spin up Vulkan.
  if (auto validation = ValidateForHeadless(config); !validation)
  {
    log.Error(log::APP, "headless: " + validation.error());
    return EXIT_CONFIG_FAIL;
  }

  // §33.7 determinism pin: headless raises framesInFlight to the §33.1
  // compile-time cap regardless of config. Rationale: M3+'s
  // samplesPerFrame * accumulationFrameLimit loop submits work in
  // flight, and the byte-identical EXR contract requires consistent
  // FIF across runs (different FIF changes the submission
  // interleaving + thus floating-point reduction order for
  // accumulation). The static_assert ties the headless pin to the
  // renderer's compile-time cap so an RFC bumping
  // MAX_FRAMES_IN_FLIGHT can't silently drift.
  constexpr uint32_t HEADLESS_FRAMES_IN_FLIGHT = MAX_FRAMES_IN_FLIGHT;
  static_assert(HEADLESS_FRAMES_IN_FLIGHT == 3,
                "Headless §33.7 determinism pin assumes a 3-deep ring; "
                "revisit before changing MAX_FRAMES_IN_FLIGHT.");
  LogDeterminismPin(config, HEADLESS_FRAMES_IN_FLIGHT);

  // ---- Device manager ------------------------------------------------
  DeviceCreationParams params{};
  params.adapterIndex = -1;  // M3+ wires config.adapter; for now use the discrete-first picker.
  params.enableValidation = config.diagnostics.validationLayer;
  params.framesInFlight = HEADLESS_FRAMES_IN_FLIGHT;
  params.applicationName = "pyxis (headless)";

  const Resolution backbuffer{config.render.width, config.render.height};
  DeviceManagerCreateStatus status = DeviceManagerCreateStatus::Unknown;
  const std::unique_ptr<IDeviceManager> deviceManager{
      CreateHeadlessDeviceManager(params, backbuffer, &status)};
  if (!deviceManager)
  {
    log.Error(log::PLATFORM, "CreateHeadlessDeviceManager failed");
    return EXIT_DEVICE_INIT_FAIL;
  }
  nvrhi::IDevice* device = deviceManager->GetDevice();
  if (!device)
  {
    log.Error(log::PLATFORM, "headless: nvrhi::IDevice not available");
    return EXIT_DEVICE_INIT_FAIL;
  }

  // ---- AOV textures (caller-allocated per §18.4) ---------------------
  // Declared after deviceManager so RAII destroys it BEFORE the device
  // dies — NVRHI's deferred-destruction queue must drain against a
  // still-live VkDevice.
  auto aovsResult = AovTextures::Create(device, config.render.width, config.render.height);
  if (!aovsResult)
  {
    log.Error(log::APP, "headless: " + aovsResult.error());
    return EXIT_RUNTIME_FAIL;
  }
  const AovTextures aovs = std::move(*aovsResult);
  nvrhi::ITexture* const renderTarget = aovs.color.Get();

  // ---- SceneWorld + Profiler + GpuScene + Renderer -------------------
  // GpuScene is the canonical scene-mutation API (§18.5);
  // PyxisRenderer's ctor takes it by reference per §18.6 and
  // PathTracePass binds its TLAS + camera every frame.
  SceneWorldFacade scene;
  if (scene.Init() != SceneWorldStatus::Ok)
  {
    log.Error(log::RENDER, "SceneWorldFacade::Init failed");
    return EXIT_RUNTIME_FAIL;
  }
  Profiler profiler{device};
  GpuSceneCreateDesc gpuSceneDesc{};
  gpuSceneDesc.framesInFlight = HEADLESS_FRAMES_IN_FLIGHT;
  GpuScene gpuScene{device, profiler, gpuSceneDesc};

  // M4 ingest with cube fallback so pyxis.exe always produces a
  // renderable image (the §29.4.a contract). False return means the
  // cube fallback also failed — catastrophic. `ingestStats` is
  // out-filled with the per-pass timings StageWalker captured (so
  // the M11 §34.1 end-of-load summary can break them down); zero-
  // init when the cube fallback fires.
  pyxis::usd_ingest::IngestStats ingestStats{};
  const auto loadStartNs = std::chrono::steady_clock::now();
  if (!LoadSceneOrFallback(config, resolvedScene, gpuScene, &ingestStats,
                           populationMask, frameNumber,
                           loadMode, variantSelections))
  {
    scene.Shutdown();
    return EXIT_RUNTIME_FAIL;
  }
  const auto loadEndNs = std::chrono::steady_clock::now();
  const double loadWallMs =
      std::chrono::duration<double, std::milli>(loadEndNs - loadStartNs).count();

  RendererCreateDesc rendererDesc{};
  rendererDesc.initialWidth = config.render.width;
  rendererDesc.initialHeight = config.render.height;
  // Headless raises FIF to MAX_FRAMES_IN_FLIGHT (= 3) for §33.7
  // byte-equal EXR — propagate so the renderer's PassContext sees
  // the real value. Picker isn't driven in headless so the FIF=1
  // assert in PathTracePass doesn't fire.
  rendererDesc.framesInFlight = deviceManager->GetFramesInFlight();
  PyxisRenderer renderer{device, gpuScene, profiler, rendererDesc};

  const nvrhi::CommandListHandle commandListHandle = device->createCommandList();
  nvrhi::ICommandList* const commandList = commandListHandle.Get();

  // ---- One render frame ----------------------------------------------
  // Single render. PathTracePass is one-sample-per-frame with no
  // accumulation today, so iterating buys nothing —
  // `samplesPerFrame * accumulationFrameLimit` wires in once the
  // accumulation buffer lands (post-M7).
  scene.Tick();
  profiler.BeginFrame();
  deviceManager->BeginFrame();

  {
    const Profiler::CpuScope frameScope(profiler, "headless.frame");
    if (!RecordAndExecuteRenderFrame(device, commandList, gpuScene, renderer, aovs, renderTarget))
    {
      scene.Shutdown();
      return EXIT_RUNTIME_FAIL;
    }
  }

  deviceManager->EndFrame();
  profiler.EndFrame();
  device->runGarbageCollection();

  // Post-commit GpuScene snapshot — surfaces what actually landed on
  // the GPU after the first CommitResources retired. Useful to
  // diagnose "is anything actually loading" questions on M8a-scale
  // scenes (was a texture acquired? Did the BLAS build fire? Did
  // the material table grow?). Cumulative counters; sourcePrim-side
  // debug names are too verbose for a one-liner.
  {
    const FrameStats sceneStats = gpuScene.LastFrameStats();
    const std::uint64_t textureMiB = sceneStats.textureBytes >> 20;
    log.Info(log::APP,
             "headless: GpuScene committed — "
                 + std::to_string(sceneStats.meshCount)     + " meshes, "
                 + std::to_string(sceneStats.blasCount)     + " BLAS, "
                 + std::to_string(sceneStats.instanceCount) + " instances, "
                 + std::to_string(sceneStats.materialCount) + " materials, "
                 + std::to_string(sceneStats.textureCount)  + " textures ("
                 + std::to_string(textureMiB)               + " MiB), "
                 + std::to_string(sceneStats.lightCount)    + " lights.");

    // M16 / V2.A.10 + V2.A.11 — texture memory budget warning. Real
    // streaming + eviction is a follow-up (needs an LRU + the §17
    // memory-budget knob wired into a per-frame eviction pass); for
    // now we emit a one-shot warning when the decoded texture cache
    // crosses a soft threshold so the operator sees the cost. The
    // 4 GiB ceiling matches the lobby's actual ~1.4 GiB load with
    // generous headroom for Moana-scale scenes inside an 8 GiB VRAM
    // budget (§17). The number lives here, not in a header, until a
    // proper RenderSettings::textureMemoryBudgetMiB knob lands.
    constexpr std::uint64_t TEXTURE_BUDGET_MIB = 4096u;
    if (textureMiB > TEXTURE_BUDGET_MIB)
    {
      log.Warn(log::APP,
               "headless: texture memory "
                   + std::to_string(textureMiB)
                   + " MiB exceeds soft budget "
                   + std::to_string(TEXTURE_BUDGET_MIB)
                   + " MiB — V2.A.11 streaming/eviction not yet active.");
    }
  }

  // ---- M11 — §34.1 end-of-load summary table -------------------------
  // Plan §34.1 mandates "spdlog end-of-load summary (always emitted)":
  // one table with each named ingest sub-phase's wall-time + first-
  // frame CPU/GPU + total time-to-first-image. Surfaced here, after
  // the first frame has rendered, so a developer scanning headless
  // stdout (or a CI log) sees one canonical block instead of having
  // to assemble it from scattered Info lines.
  //
  // The right-aligned numeric column makes columnar reads easy in any
  // monospace terminal. Per-phase percentages are computed against
  // wall-clock load time, NOT sum-of-phases — those don't add up
  // when ingest passes run partly in parallel (M10 §B).
  {
    const auto& itim = ingestStats.timings;
    auto pctOfLoad = [loadWallMs](double phaseMs) noexcept -> double {
      return loadWallMs > 0.0 ? (100.0 * phaseMs / loadWallMs) : 0.0;
    };
    char line[160];
    log.Info(log::APP, "===== §34.1 Load summary =====");
    log.Info(log::APP, "phase                                ms          % of load");
    std::snprintf(line, sizeof(line),
                  "  ingest.usd.stageOpen               %8.1f       %5.1f%%",
                  static_cast<double>(itim.stageOpenMs), pctOfLoad(itim.stageOpenMs));
    log.Info(log::APP, std::string{line});
    std::snprintf(line, sizeof(line),
                  "  ingest.usd.traverseSort            %8.1f       %5.1f%%",
                  static_cast<double>(itim.traverseSortMs), pctOfLoad(itim.traverseSortMs));
    log.Info(log::APP, std::string{line});
    std::snprintf(line, sizeof(line),
                  "  ingest.shared.material.pass        %8.1f       %5.1f%%",
                  static_cast<double>(itim.materialPassMs), pctOfLoad(itim.materialPassMs));
    log.Info(log::APP, std::string{line});
    std::snprintf(line, sizeof(line),
                  "  ingest.usd.instancerPass           %8.1f       %5.1f%%",
                  static_cast<double>(itim.instancerPassMs), pctOfLoad(itim.instancerPassMs));
    log.Info(log::APP, std::string{line});
    std::snprintf(line, sizeof(line),
                  "  ingest.usd.meshLightCameraPass     %8.1f       %5.1f%%",
                  static_cast<double>(itim.meshLightCameraMs),
                  pctOfLoad(itim.meshLightCameraMs));
    log.Info(log::APP, std::string{line});
    std::snprintf(line, sizeof(line),
                  "  ingest.usd.total                   %8.1f       %5.1f%%",
                  static_cast<double>(itim.totalMs), pctOfLoad(itim.totalMs));
    log.Info(log::APP, std::string{line});
    // Time-to-first-image: wall-clock from headless start through the
    // first frame's EndFrame. Per-frame CPU/GPU breakdown lives in
    // the §34 KPI table below (only emitted when --bench-frames > 0,
    // since single-frame headless doesn't drain the profiler ring
    // far enough to resolve the GPU timestamps).
    std::snprintf(line, sizeof(line),
                  "  render.frame.timeToFirstImage      %8.1f (load wall-clock)",
                  loadWallMs);
    log.Info(log::APP, std::string{line});
    log.Info(log::APP, "==============================");
  }

  // ---- Readback + EXR write ------------------------------------------
  if (!ReadbackAndWriteExr(device, commandList, renderTarget, config.output.image))
  {
    scene.Shutdown();
    return EXIT_RUNTIME_FAIL;
  }

  // ---- M7 follow-up: --save-aov dispatch -----------------------------
  // Output paths are `<prefix>_<aov>.exr` where `<prefix>` is
  // `config.output.image` stripped of its `.exr` extension.
  if (!saveAovList.empty())
  {
    const std::string aovPrefix = DeriveAovPrefix(config.output.image);
    SaveAovsFromList(saveAovList, aovPrefix, aovs, device, commandList);
  }

  // ---- M8b benchmark loop --------------------------------------------
  // After the regular EXR write, optionally render N warm-up + N
  // measurement frames and print a §34 KPI table. The warm-up frames
  // amortise the first-frame BLAS-build / pipeline-cache / TLAS-bake
  // costs that the single-frame above already paid; the measurement
  // window captures steady-state numbers comparable to the §34.3 KPIs
  // (pass.PathTrace < 12 ms, frame.cpu.commitResources < 2 ms).
  if (benchFrames > 0)
  {
    // Per-pass min / sorted percentile aggregator. Vectors keep sorted
    // values for direct percentile lookup; pass count is small (<= 8
    // top-level scopes today) so the O(N log N) sort per print is
    // fine.
    struct PassAcc {
      std::string name;
      FrameProfile::ScopeKind kind = FrameProfile::ScopeKind::Cpu;
      std::vector<double> samples;
    };
    std::vector<PassAcc> accumulators;
    accumulators.reserve(16);

    auto recordFrame = [&](const FrameProfile& profile) {
      for (const FrameProfile::PassTiming& pass : profile.passes)
      {
        const std::string_view nameView = pass.name.View();
        auto found = std::find_if(accumulators.begin(), accumulators.end(),
                                  [nameView](const PassAcc& acc) { return acc.name == nameView; });
        if (found == accumulators.end())
        {
          PassAcc fresh;
          fresh.name.assign(nameView);
          fresh.kind = pass.kind;
          fresh.samples.reserve(benchFrames);
          accumulators.push_back(std::move(fresh));
          found = accumulators.end() - 1;
        }
        found->samples.push_back(pass.durationMs);
      }
    };

    auto runOneFrame = [&]() -> bool {
      scene.Tick();
      profiler.BeginFrame();
      deviceManager->BeginFrame();
      bool frameOk = true;
      {
        const Profiler::CpuScope frameScope(profiler, "headless.frame");
        frameOk = RecordAndExecuteRenderFrame(device, commandList, gpuScene, renderer,
                                              aovs, renderTarget);
      }
      deviceManager->EndFrame();
      profiler.EndFrame();
      device->runGarbageCollection();
      return frameOk;
    };

    log.Info(log::APP, "headless: benchmark — " + std::to_string(benchFrames)
                           + " warm-up + " + std::to_string(benchFrames) + " measurement frames");
    for (uint32_t i = 0; i < benchFrames; ++i)
    {
      if (!runOneFrame())
      {
        scene.Shutdown();
        return EXIT_RUNTIME_FAIL;
      }
    }
    for (uint32_t i = 0; i < benchFrames; ++i)
    {
      if (!runOneFrame())
      {
        scene.Shutdown();
        return EXIT_RUNTIME_FAIL;
      }
      recordFrame(profiler.LastFrameProfile());
    }

    // ---- KPI table ---------------------------------------------------
    auto percentile = [](std::vector<double>& samples, double pct) {
      std::sort(samples.begin(), samples.end());
      const auto idx = static_cast<std::size_t>(
          (samples.size() - 1) * std::clamp(pct, 0.0, 1.0));
      return samples[idx];
    };
    log.Info(log::APP, "===== §34 KPI table (steady state) =====");
    log.Info(log::APP, "scope                              kind   p50      p99      max");
    for (PassAcc& acc : accumulators)
    {
      if (acc.samples.empty())
        continue;
      const double p50  = percentile(acc.samples, 0.50);
      const double p99  = percentile(acc.samples, 0.99);
      const double pmax = percentile(acc.samples, 1.00);
      char line[160];
      std::snprintf(line, sizeof(line),
                    "%-34s %-6s %7.3fms %7.3fms %7.3fms",
                    acc.name.c_str(),
                    acc.kind == FrameProfile::ScopeKind::Gpu ? "GPU" : "CPU",
                    p50, p99, pmax);
      log.Info(log::APP, std::string{line});
    }
    log.Info(log::APP, "========================================");

    // ---- M10 — profile JSON sidecar ---------------------------------
    // §35 / §36.6 KPI tracking. When --profile <path> is supplied,
    // write a single JSON document describing GPU / driver / per-pass
    // percentiles to that path so the regression harness can merge it
    // with image-diff metrics into the rolling per-test KPI CSV.
    // Hand-rolled writer (no nlohmann_json dep on this path so the
    // pyxis_app's /EHs-c- perimeter stays exception-free) — the
    // payload is small and the schema is fixed.
    if (!profilePath.empty())
    {
      const AdapterInfo& adapter = deviceManager->GetAdapterInfo();
      const auto unixSec =
          std::chrono::duration_cast<std::chrono::seconds>(
              std::chrono::system_clock::now().time_since_epoch()).count();
      const char* adapterTypeStr = "other";
      switch (adapter.type)
      {
        case AdapterType::Integrated: adapterTypeStr = "integrated"; break;
        case AdapterType::Discrete:   adapterTypeStr = "discrete";   break;
        case AdapterType::Virtual:    adapterTypeStr = "virtual";    break;
        case AdapterType::Cpu:        adapterTypeStr = "cpu";        break;
        default:                      adapterTypeStr = "other";      break;
      }

      const std::string adapterName{adapter.NameView()};
      // Crude JSON escape: backslash + double-quote only. Adapter
      // names are vendor strings (well-behaved ASCII in practice) so
      // we don't need full Unicode-aware escaping.
      std::string escapedName;
      escapedName.reserve(adapterName.size());
      for (const char chr : adapterName)
      {
        if (chr == '"' || chr == '\\')
          escapedName.push_back('\\');
        escapedName.push_back(chr);
      }

      std::ofstream out(std::string{profilePath}, std::ios::binary | std::ios::trunc);
      if (!out)
      {
        log.Warn(log::APP, "headless: profile JSON write failed (could not open "
                              + std::string{profilePath} + ")");
      }
      else
      {
        out << "{\n";
        out << "  \"pyxis_version\": \"" << GetVersionString() << "\",\n";
        out << "  \"pyxis_git_sha\": \"" << GetVersionGitSha() << "\",\n";
        out << "  \"timestamp_unix\": " << unixSec << ",\n";
        out << "  \"gpu\": {\n";
        out << "    \"name\": \"" << escapedName << "\",\n";
        out << "    \"type\": \"" << adapterTypeStr << "\",\n";
        out << "    \"vendor_id\": " << adapter.vendorId << ",\n";
        out << "    \"device_id\": " << adapter.deviceId << ",\n";
        out << "    \"driver_version_raw\": " << adapter.driverVersionRaw << ",\n";
        out << "    \"vram_bytes\": " << adapter.totalDeviceLocalBytes << "\n";
        out << "  },\n";
        out << "  \"render\": {\n";
        out << "    \"width\": "  << config.render.width  << ",\n";
        out << "    \"height\": " << config.render.height << ",\n";
        out << "    \"samples_per_frame\": " << config.render.samplesPerFrame << ",\n";
        out << "    \"seed\": " << config.render.seed << ",\n";
        out << "    \"frames_in_flight\": " << deviceManager->GetFramesInFlight() << "\n";
        out << "  },\n";
        out << "  \"scene\": \"";
        for (const char chr : resolvedScene.path)
        {
          if (chr == '\\' || chr == '"')
            out << '\\';
          out << chr;
        }
        out << "\",\n";
        out << "  \"bench\": {\n";
        out << "    \"frames\": " << benchFrames << ",\n";
        out << "    \"passes\": [";
        bool firstPass = true;
        char numBuf[64];
        for (PassAcc& acc : accumulators)
        {
          if (acc.samples.empty())
            continue;
          const double p50  = percentile(acc.samples, 0.50);
          const double p99  = percentile(acc.samples, 0.99);
          const double pmax = percentile(acc.samples, 1.00);
          out << (firstPass ? "\n      " : ",\n      ");
          firstPass = false;
          out << "{\"name\": \"" << acc.name << "\", "
              << "\"kind\": \""
              << (acc.kind == FrameProfile::ScopeKind::Gpu ? "Gpu" : "Cpu")
              << "\", ";
          std::snprintf(numBuf, sizeof(numBuf), "%.6f", p50);
          out << "\"p50_ms\": " << numBuf << ", ";
          std::snprintf(numBuf, sizeof(numBuf), "%.6f", p99);
          out << "\"p99_ms\": " << numBuf << ", ";
          std::snprintf(numBuf, sizeof(numBuf), "%.6f", pmax);
          out << "\"max_ms\": " << numBuf << "}";
        }
        out << (firstPass ? "]" : "\n    ]");
        out << "\n  }\n";
        out << "}\n";
        log.Info(log::APP, "headless: profile JSON written to "
                              + std::string{profilePath});
      }
    }
  }
  else if (!profilePath.empty())
  {
    // No --bench-frames, but the caller asked for a profile sidecar.
    // Emit a minimal JSON without the bench/passes block so the
    // downstream perf tooling can still record GPU/driver/scene
    // metadata + the harness's own wall-clock measurement.
    const AdapterInfo& adapter = deviceManager->GetAdapterInfo();
    const auto unixSec =
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    const char* adapterTypeStr =
        adapter.type == AdapterType::Discrete   ? "discrete"
        : adapter.type == AdapterType::Integrated ? "integrated"
        : adapter.type == AdapterType::Virtual    ? "virtual"
        : adapter.type == AdapterType::Cpu        ? "cpu"
                                                  : "other";
    const std::string adapterName{adapter.NameView()};
    std::string escapedName;
    escapedName.reserve(adapterName.size());
    for (const char chr : adapterName)
    {
      if (chr == '"' || chr == '\\')
        escapedName.push_back('\\');
      escapedName.push_back(chr);
    }
    std::ofstream out(std::string{profilePath}, std::ios::binary | std::ios::trunc);
    if (!out)
    {
      log.Warn(log::APP, "headless: profile JSON write failed (could not open "
                            + std::string{profilePath} + ")");
    }
    else
    {
      out << "{\n";
      out << "  \"pyxis_version\": \"" << GetVersionString() << "\",\n";
      out << "  \"pyxis_git_sha\": \"" << GetVersionGitSha() << "\",\n";
      out << "  \"timestamp_unix\": " << unixSec << ",\n";
      out << "  \"gpu\": {\n";
      out << "    \"name\": \"" << escapedName << "\",\n";
      out << "    \"type\": \"" << adapterTypeStr << "\",\n";
      out << "    \"vendor_id\": " << adapter.vendorId << ",\n";
      out << "    \"device_id\": " << adapter.deviceId << ",\n";
      out << "    \"driver_version_raw\": " << adapter.driverVersionRaw << ",\n";
      out << "    \"vram_bytes\": " << adapter.totalDeviceLocalBytes << "\n";
      out << "  },\n";
      out << "  \"render\": {\n";
      out << "    \"width\": "  << config.render.width  << ",\n";
      out << "    \"height\": " << config.render.height << ",\n";
      out << "    \"samples_per_frame\": " << config.render.samplesPerFrame << ",\n";
      out << "    \"seed\": " << config.render.seed << ",\n";
      out << "    \"frames_in_flight\": " << deviceManager->GetFramesInFlight() << "\n";
      out << "  },\n";
      out << "  \"scene\": \"";
      for (const char chr : resolvedScene.path)
      {
        if (chr == '\\' || chr == '"')
          out << '\\';
        out << chr;
      }
      out << "\",\n";
      out << "  \"bench\": { \"frames\": 0, \"passes\": [] }\n";
      out << "}\n";
      log.Info(log::APP, "headless: profile JSON (no-bench) written to "
                            + std::string{profilePath});
    }
  }

  // ---- Effective-config dump + teardown ------------------------------
  // The EXR is the primary artefact and is already on disk; a failure
  // to write the sidecar effective-config is logged but does not fail
  // the run.
  if (!config.output.effectiveConfig.empty())
  {
    if (auto effectiveResult = WriteEffectiveConfig(config); !effectiveResult)
    {
      log.Warn(log::APP, "headless: " + effectiveResult.error());
    }
  }
  scene.Shutdown();
  return EXIT_OK;
}

int RunViewer(const Configuration& config, const ResolvedScene& resolvedScene,
              std::string_view screenshotPath, std::string_view shaderRebuildDir,
              std::string_view loadMode,
              std::string_view variantSelections) noexcept {
  // Viewer keeps the M1 entrypoint shape; M4 P5d/P5e wires
  // resolvedScene through to the engine dispatch inside
  // RunViewerLoop. shaderRebuildDir overrides the cwd walk-up
  // heuristic for the editor's Reload Shaders button (see
  // ViewerMode.cpp's FindCMakeBuildDir).
  return RunViewerLoop(config, resolvedScene, screenshotPath, shaderRebuildDir,
                       loadMode, variantSelections);
}

}  // namespace pyxis::app
