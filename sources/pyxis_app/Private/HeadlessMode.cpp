// Pyxis app — headless + viewer mode drivers.

#include "HeadlessMode.h"

#include "Config/Configuration.h"
#include "Output/AovExrSaver.h"
#include "Output/ExrWriter.h"
#include "Output/TextureReadback.h"
#include "Render/AovRegistry.h"
#include "Render/AovTextures.h"
#include "HydraEngine/HydraEngine.h"
#include "Render/HardcodedCubeScene.h"
#include "Scene/SceneResolver.h"
#include "UsdDirectEngine/UsdDirectEngine.h"
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

#include <nvrhi/nvrhi.h>

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

}  // namespace

int RunHeadless(const Configuration& config, const ResolvedScene& resolvedScene,
                std::string_view saveAovList) noexcept {
  auto& log = Logging::Get();

  // §27 ValidateForHeadless: non-zero seed (§33.7), non-empty
  // output.image, non-zero render dims. Surfaces a config-fail exit
  // before we even spin up Vulkan.
  if (auto validation = ValidateForHeadless(config); !validation)
  {
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
  if (config.limits.framesInFlight != HEADLESS_FRAMES_IN_FLIGHT)
  {
    log.Info(log::APP,
             "headless: §33.7 pinning framesInFlight=" + std::to_string(HEADLESS_FRAMES_IN_FLIGHT)
                 + " (config requested " + std::to_string(config.limits.framesInFlight) + ")");
  }
  {
    std::string summary = "headless: determinism pin — seed=";
    summary += std::to_string(config.render.seed);
    summary += "  framesInFlight=" + std::to_string(HEADLESS_FRAMES_IN_FLIGHT);
    summary += "  dims=" + std::to_string(config.render.width) + "x"
               + std::to_string(config.render.height);
    summary += "  samples=" + std::to_string(config.render.samplesPerFrame);
    log.Info(log::APP, summary);
  }

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
  // PathTracePass binds its TLAS + camera every frame. Headless
  // raises framesInFlight to the §33.7 byte-equal pin
  // (MAX_FRAMES_IN_FLIGHT == 3).
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

  // M4 ingest dispatch on `app.ingest`. UsdDirectEngine wires
  // through pyxis_usd_ingest's StageWalker; HydraEngine wires
  // through pyxis_hydra (P5d/P5e). Either engine failing or
  // returning "nothing emitted" falls back to the M3 hardcoded
  // cube so pyxis.exe always produces a renderable image (the
  // §29.4.a "must produce a renderable image" contract).
  bool sceneLoaded = false;
  if (!resolvedScene.path.empty())
  {
    pyxis::usd_ingest::IngestStats stats{};
    if (config.app.ingest == "usd_direct")
    {
      UsdDirectEngine engine;
      stats = engine.Load(resolvedScene.path, gpuScene);
    }
    else if (config.app.ingest == "hydra")
    {
      HydraEngine engine;
      stats = engine.Load(resolvedScene.path, gpuScene);
    }
    sceneLoaded = stats.meshesEmitted > 0 || stats.camerasEmitted > 0;
  }
  if (!sceneLoaded)
  {
    if (auto cubeResult =
            BuildHardcodedCubeScene(gpuScene, config.render.width, config.render.height);
        !cubeResult)
    {
      log.Error(log::APP, "headless: " + cubeResult.error());
      scene.Shutdown();
      return EXIT_RUNTIME_FAIL;
    }
  }

  RendererCreateDesc rendererDesc{};
  rendererDesc.initialWidth = config.render.width;
  rendererDesc.initialHeight = config.render.height;
  PyxisRenderer renderer{device, gpuScene, profiler, rendererDesc};

  const nvrhi::CommandListHandle commandListHandle = device->createCommandList();
  nvrhi::ICommandList* const commandList = commandListHandle.Get();

  // ---- One render frame ----------------------------------------------
  // Single render. M3's PathTracePass is one-sample-per-frame with
  // no accumulation, so iterating buys nothing yet —
  // samplesPerFrame > 1 wires in at M5+ when the accumulation
  // buffer lands.
  scene.Tick();
  profiler.BeginFrame();
  deviceManager->BeginFrame();

  {
    const Profiler::CpuScope frameScope(profiler, "headless.frame");

    commandList->open();

    // Drain pending GpuScene mutations onto the open command list
    // (mesh upload, BLAS build, TLAS rebuild). Failure aborts the
    // frame; partial state stays dirty so the next CommitResources
    // retries.
    if (auto commitResult = gpuScene.CommitResources(commandList); !commitResult)
    {
      log.Error(log::APP, "headless: " + std::string{commitResult.error().message.View()});
      commandList->close();
      scene.Shutdown();
      return EXIT_RUNTIME_FAIL;
    }

    RenderTargets targets{};
    targets.color = renderTarget;
    // M7 follow-up — bind the raw AOV outputs so the user can dump
    // them via --save-aov. Always bound (not just when --save-aov is
    // set) because PathTracePass's UAV writes are unconditional;
    // without these the writes go to the 1×1 fallbacks PathTracePass
    // owns, which is fine but wastes the already-allocated AovTextures
    // memory. Same RenderTargets shape ViewerMode wires.
    targets.colorHdr      = aovs.colorHdr.Get();
    targets.normalAov     = aovs.normal.Get();
    targets.depthAov      = aovs.depth.Get();
    targets.instanceIdAov = aovs.instanceId.Get();
    targets.materialIdAov = aovs.materialId.Get();
    targets.baseColorAov  = aovs.baseColor.Get();
    targets.worldPosAov   = aovs.worldPos.Get();
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
  }

  deviceManager->EndFrame();
  profiler.EndFrame();
  device->runGarbageCollection();

  // ---- Readback + EXR write ------------------------------------------
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
    scene.Shutdown();
    return EXIT_RUNTIME_FAIL;
  }
  commandList->close();
  device->executeCommandList(commandList);
  device->waitForIdle();
  device->runGarbageCollection();

  if (auto mapResult = readback->Map(); !mapResult)
  {
    log.Error(log::APP, "headless: " + mapResult.error());
    scene.Shutdown();
    return EXIT_RUNTIME_FAIL;
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

  auto writeResult = WriteExrBgra8(config.output.image, readback->Width(), readback->Height(),
                                   readback->Data(), readback->RowPitch());
  if (!writeResult)
  {
    log.Error(log::APP, "headless: " + writeResult.error());
    scene.Shutdown();
    return EXIT_RUNTIME_FAIL;
  }

  // ---- M7 follow-up: --save-aov dispatch -----------------------------
  // Parse the comma-separated list and dispatch one SaveAovAsExr per
  // entry. Output paths are `<prefix>_<aov>.exr` where `<prefix>` is
  // `config.output.image` stripped of its `.exr` extension. The "all"
  // alias expands to every recognised AOV. Unknown names log a
  // warning but don't fail the run — keeps the per-AOV write
  // independent so a typo doesn't drop legitimate outputs.
  if (!saveAovList.empty())
  {
    const std::string aovPrefix = DeriveAovPrefix(config.output.image);
    auto saveOne = [&](std::string_view aovName, nvrhi::ITexture* sourceAov) {
      if (sourceAov == nullptr)
      {
        log.Warn(log::APP, "headless: --save-aov: source for '" + std::string{aovName}
                               + "' is null; skipping");
        return;
      }
      const std::string targetPath = aovPrefix + "_" + std::string{aovName} + ".exr";
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
                               + "' (recognised: color,normal,depth,instanceId,"
                                 "materialId,baseColor,worldPos,all)");
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
              std::string_view screenshotPath) noexcept {
  // Viewer keeps the M1 entrypoint shape; M4 P5d/P5e wires
  // resolvedScene through to the engine dispatch inside
  // RunViewerLoop.
  return RunViewerLoop(config, resolvedScene, screenshotPath);
}

}  // namespace pyxis::app
