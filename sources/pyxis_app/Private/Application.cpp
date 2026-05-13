// Pyxis app — Application::Run.
//
// Plan §41 M0 exit codes:
//   0  ok
//   2  device init fail
//   3  config fail (CLI parse / unknown flag)

#include "Application.h"

#include "CliArgs.h"
#include "Config/Configuration.h"
#include "HeadlessMode.h"
#include "Scene/SceneResolver.h"

#include <Pyxis/Platform/FileSystem/AssetLocator.h>
#include <Pyxis/Platform/FileSystem/Path.h>
#include <Pyxis/Platform/Logging/Log.h>
#include <Pyxis/Platform/Logging/LogCategories.h>
#include <Pyxis/Renderer/Version.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

namespace pyxis::app {

namespace {

constexpr int EXIT_OK = 0;
constexpr int EXIT_CONFIG_FAIL = 3;

void ConfigureLogging() noexcept {
  LogConfig cfg{};
  cfg.consoleLevel = LogLevel::Info;
  cfg.fileLevel = LogLevel::Debug;
  Logging::Get().Configure(cfg);
}

void EmitVersionBanner() noexcept {
  auto& log = Logging::Get();
  char banner[160];
  std::snprintf(banner, sizeof(banner), "Pyxis %s starting (encoded=0x%08X, sha=%s)",
                pyxis::GetVersionString(), pyxis::GetVersionEncoded(), pyxis::GetVersionGitSha());
  log.Info(log::APP, banner);
}

// Point USD's PlugRegistry at <exe-dir>/usd/ so it finds the
// vcpkg-shipped plugin tree (UsdGeom / UsdLux / UsdShade / etc.
// schema definitions). Without this, UsdStage::Open hangs trying
// to resolve TfTypes for the prim types it encounters. The pyxis
// CMakeLists POST_BUILD copies the tree from
// vcpkg_installed/x64-windows/{,debug/}bin/usd/ to
// <bin>/$<CONFIG>/usd/, so this path is correct in both dev and
// installed builds. Set with _putenv_s so the env var lives in
// the process and is inherited by every USD-touching thread we
// spawn later.
void EnsureUsdPluginPath() noexcept {
  const AssetLocator locator;
  const Path& exeDir = locator.ExecutableDirectory();
  if (exeDir.View().empty())
    return;
  const std::filesystem::path pluginRoot =
      std::filesystem::path(std::string{exeDir.View()}) / "usd";
  std::error_code errorCode;
  if (!std::filesystem::exists(pluginRoot, errorCode))
  {
    Logging::Get().Warn(log::APP,
                        "USD plugin tree not found at " + pluginRoot.string()
                            + "; UsdStage::Open will likely hang. Confirm the "
                              "pyxis_app CMake POST_BUILD copy ran.");
    return;
  }
  const std::string pathString = pluginRoot.string();
  // _putenv_s is the Windows-safe set; std::setenv would also work
  // but _putenv_s avoids the POSIX-vs-MSVCRT split-path subtlety.
  _putenv_s("PXR_PLUGINPATH_NAME", pathString.c_str());
  Logging::Get().Info(log::APP, "PXR_PLUGINPATH_NAME = " + pathString);
}

}  // namespace

int Run(int argc, char** argv) noexcept {
  ConfigureLogging();

  const CliArgs cli = Parse(argc, argv);

  if (cli.invalid)
  {
    std::fprintf(stderr, "pyxis: unknown argument '%.*s'\n",
                 static_cast<int>(cli.invalidArg.size()), cli.invalidArg.data());
    PrintUsage();
    return EXIT_CONFIG_FAIL;
  }

  if (cli.showHelp)
  {
    PrintUsage();
    return EXIT_OK;
  }

  if (cli.showVersion)
  {
    PrintVersion();
    return EXIT_OK;
  }

  // §29.4.a tooling: print the bundled default-scene path and exit
  // (no logging spin-up, no device init). Empty result = the binary
  // was deployed without its Resources/ tree; surface as exit-3 so
  // tooling that pipes the output sees the failure.
  if (cli.printDefaultScenePath)
  {
    const std::string path = BundledDefaultScenePath();
    if (path.empty())
    {
      std::fprintf(stderr, "pyxis: bundled default scene not found "
                           "(<exe-dir>/Resources/scenes/default.usd missing)\n");
      return EXIT_CONFIG_FAIL;
    }
    std::fprintf(stdout, "%s\n", path.c_str());
    return EXIT_OK;
  }

  EmitVersionBanner();
  EnsureUsdPluginPath();

  // §29.1 overlay: defaults -> exe-dir -> %LOCALAPPDATA% -> --config,
  // then ApplyCliOverrides. ResolveConfiguration handles the chain;
  // failure is fatal under M0's exit code 3 (config fail).
  auto resolved = ResolveConfiguration(cli);
  if (!resolved)
  {
    std::fprintf(stderr, "pyxis: config error: %s\n", resolved.error().c_str());
    return EXIT_CONFIG_FAIL;
  }
  const Configuration& config = *resolved;

  // §29.4.a default-scene resolution. The resolved path flows into
  // IngestUsd() (dispatched on `config.app.ingest` to the hydra or
  // usd_direct adapter); ingest failure falls back to the M3
  // hardcoded cube path inside HeadlessMode / ViewerMode so
  // pyxis.exe always produces an image.
  const ResolvedScene scene = ResolveScene(cli, config);
  Logging::Get().Info(
      log::APP, std::string{"scene.resolved.source = "}
                    + std::string{SceneSourceLabel(scene.source)} + "  path = " + scene.path);
  Logging::Get().Info(log::APP, "app.ingest = " + config.app.ingest);

  // M19 / V2.A.4 + V2.A.13 — --frame flag stub. The CLI parsing is in
  // place so future plumbing has a target; the rest of the pipeline
  // still evaluates at Default time. Log explicitly so users see that
  // their --frame request was received but not yet acted on.
  // V2.A.13 — `--frame` now propagates through IngestUsd → StageWalker
  // → UsdGeomXformCache::SetTime. Animated xforms + camera attrs read
  // at the requested time-code.
  if (cli.frameNumber >= 0)
  {
    Logging::Get().Info(
        log::APP,
        "--frame " + std::to_string(cli.frameNumber)
            + " honoured (V2.A.13). Stage will evaluate at this time-code.");
  }

  // V2.A.15 — composition load mode. `all` (default) / `none` /
  // `metadata` translate to UsdStage::InitialLoadSet inside IngestUsd;
  // `--population-mask` continues to drive UsdStage::OpenMasked. Anything
  // unrecognised falls back to LoadAll with a warning (see IngestUsd).
  if (!cli.loadMode.empty())
  {
    Logging::Get().Info(
        log::APP,
        "--load-mode " + std::string{cli.loadMode}
            + " honoured (V2.A.15 — payload-load policy applied at "
              "UsdStage::Open).");
  }

  // V2.A.2 — variant overrides flow through CLI -> IngestUsd, which
  // authors them on the stage's session layer before WalkStage. Log
  // here so the operator sees the request was received independent of
  // whether each prim path resolves (per-entry warnings fire in
  // IngestUsd).
  if (!cli.variantSelections.empty())
  {
    Logging::Get().Info(
        log::APP,
        "--variant " + std::string{cli.variantSelections}
            + " honoured (V2.A.2 — variant selections applied to "
              "session layer post-Open).");
  }

  if (cli.headless)
  {
    // V2.A.4 multi-frame headless. When `--frame-range B..E[:S]` is
    // authored, loop over each frame, override `config.output.image`
    // with a numbered filename, and re-enter RunHeadless. Single-
    // frame behaviour preserved when the range is unset.
    if (cli.frameRangeEnd >= cli.frameRangeBegin && cli.frameRangeBegin >= 0)
    {
      const int step = cli.frameRangeStep > 0 ? cli.frameRangeStep : 1;
      Logging::Get().Info(
          log::APP,
          "--frame-range " + std::to_string(cli.frameRangeBegin) + ".."
              + std::to_string(cli.frameRangeEnd) + ":" + std::to_string(step)
              + " → looping the headless render path one EXR per frame.");
      for (int frame = cli.frameRangeBegin; frame <= cli.frameRangeEnd; frame += step)
      {
        Configuration perFrameConfig = config;
        // Insert `.NNNN` before the extension so frame 12 of out.exr
        // becomes out.0012.exr. Falls back to suffix-only when the
        // path has no `.`.
        const std::string baseOut = perFrameConfig.output.image;
        const std::size_t dotPos = baseOut.rfind('.');
        char frameTag[16];
        std::snprintf(frameTag, sizeof(frameTag), ".%04d", frame);
        perFrameConfig.output.image = (dotPos == std::string::npos)
            ? baseOut + frameTag
            : baseOut.substr(0, dotPos) + frameTag + baseOut.substr(dotPos);
        const int frameRc = RunHeadless(perFrameConfig, scene, cli.saveAov,
                                        cli.benchFrames, cli.profilePath,
                                        cli.populationMask,
                                        static_cast<double>(frame),
                                        /*frameRangeBegin*/ -1,
                                        /*frameRangeEnd*/   -1,
                                        /*frameRangeStep*/   1,
                                        cli.loadMode,
                                        cli.variantSelections);
        if (frameRc != 0)
          return frameRc;
      }
      return EXIT_OK;
    }
    return RunHeadless(config, scene, cli.saveAov, cli.benchFrames, cli.profilePath,
                       cli.populationMask,
                       (cli.frameNumber >= 0)
                           ? static_cast<double>(cli.frameNumber)
                           : -1.0,
                       /*frameRangeBegin*/ -1,
                       /*frameRangeEnd*/   -1,
                       /*frameRangeStep*/   1,
                       cli.loadMode,
                       cli.variantSelections);
  }
  return RunViewer(config, scene, cli.screenshotPath, cli.shaderRebuildDir,
                   cli.loadMode, cli.variantSelections);
}

}  // namespace pyxis::app
