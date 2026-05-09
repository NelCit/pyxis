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

#include <Pyxis/Platform/Logging/Log.h>
#include <Pyxis/Platform/Logging/LogCategories.h>
#include <Pyxis/Renderer/Version.h>

#include <cstdio>
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
