// Pyxis app — default-startup-scene resolver implementation.

#include "Scene/SceneResolver.h"

#include "CliArgs.h"
#include "Config/Configuration.h"

#include <Pyxis/Platform/FileSystem/AssetLocator.h>
#include <Pyxis/Platform/FileSystem/Path.h>
#include <Pyxis/Platform/Logging/Log.h>
#include <Pyxis/Platform/Logging/LogCategories.h>

#include <filesystem>
#include <string>
#include <string_view>

namespace pyxis::app {

namespace {

namespace fs = std::filesystem;

// True iff `path` names a regular file on disk. std::filesystem's
// query overloads take an error_code so we never throw across
// /EHs-c-.
bool FileExists(std::string_view path) noexcept {
  if (path.empty())
    return false;
  std::error_code errorCode;
  const auto status = fs::status(fs::path(path), errorCode);
  if (errorCode)
    return false;
  return fs::is_regular_file(status);
}

// Promote `path` to absolute UTF-8. Returns the input untouched if
// std::filesystem can't canonicalise it (already absolute, broken
// path, permission denied — none of those are fatal here).
std::string ToAbsolute(std::string_view path) noexcept {
  std::error_code errorCode;
  const fs::path resolved = fs::absolute(fs::path(path), errorCode);
  if (errorCode)
    return std::string{path};
  return resolved.string();
}

}  // namespace

std::string_view SceneSourceLabel(SceneSource source) noexcept {
  switch (source)
  {
    case SceneSource::Cli:     return "cli";
    case SceneSource::Config:  return "config";
    case SceneSource::Recent:  return "recent";
    case SceneSource::Bundled: return "bundled";
    case SceneSource::None:    return "none";
  }
  return "none";
}

std::string BundledDefaultScenePath() noexcept {
  const AssetLocator locator;
  const Path resolved = locator.LocateResource("scenes/default.usd");
  if (resolved.View().empty())
    return {};
  return std::string{resolved.View()};
}

ResolvedScene ResolveScene(const CliArgs& cli, const Configuration& config) noexcept {
  auto& log = Logging::Get();

  // Step 1 — --scene <path>. Highest precedence; if missing, warn and
  // fall through to the next step (user typo routes to the bundled
  // default per §29.4.a "must produce a renderable image").
  if (!cli.scenePath.empty())
  {
    const std::string absPath = ToAbsolute(cli.scenePath);
    if (FileExists(absPath))
    {
      return ResolvedScene{absPath, SceneSource::Cli};
    }
    log.Warn(log::APP, "scene: --scene " + std::string{cli.scenePath}
                           + " not found; falling through to chain step 2");
  }

  // Step 2 — parameters.json paths.scene. Same fall-through rule on
  // missing.
  if (!config.paths.scene.empty())
  {
    const std::string absPath = ToAbsolute(config.paths.scene);
    if (FileExists(absPath))
    {
      return ResolvedScene{absPath, SceneSource::Config};
    }
    log.Warn(log::APP, "scene: paths.scene = " + config.paths.scene
                           + " not found; falling through to chain step 4");
  }

  // Step 3 — %LOCALAPPDATA%/Pyxis/recent_scenes.json. Deferred to
  // M4+: that step needs a scene-load telemetry pipeline (write the
  // newly-loaded path on success, evict on missing) that only makes
  // sense once HydraEngine / UsdDirectEngine actually load scenes.
  // M3.5 plumbing skips it; the chain falls straight from step 2 to
  // step 4.

  // Step 4 — bundled default at <exe-dir>/Resources/scenes/default.usd.
  // Always exists in a correctly-built / correctly-installed Pyxis;
  // empty result means the binary was deployed without its Resources/
  // tree.
  const std::string bundled = BundledDefaultScenePath();
  if (!bundled.empty())
  {
    return ResolvedScene{bundled, SceneSource::Bundled};
  }

  log.Error(log::APP, "scene: bundled default missing at <exe-dir>/Resources/scenes/"
                      "default.usd; install is broken");
  return ResolvedScene{{}, SceneSource::None};
}

}  // namespace pyxis::app
