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

  // §29.4.a default-scene resolution. M4 hands the resolved path to
  // the ingest engine (HydraEngine / UsdDirectEngine selected via
  // `config.app.ingest`); engine load failure falls back to the M3
  // hardcoded cube path inside HeadlessMode / ViewerMode so
  // pyxis.exe always produces an image.
  const ResolvedScene scene = ResolveScene(cli, config);
  Logging::Get().Info(
      log::APP, std::string{"scene.resolved.source = "}
                    + std::string{SceneSourceLabel(scene.source)} + "  path = " + scene.path);
  Logging::Get().Info(log::APP, "app.ingest = " + config.app.ingest);

  if (cli.headless)
  {
    return RunHeadless(config, scene);
  }
  return RunViewer(config, scene, cli.screenshotPath);
}

}  // namespace pyxis::app
