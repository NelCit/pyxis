// Pyxis app — Configuration loader.

#include "Config/Configuration.h"

#include "CliArgs.h"

#include <Pyxis/Platform/FileSystem/AssetLocator.h>
#include <Pyxis/Platform/FileSystem/Path.h>
#include <Pyxis/Platform/Logging/Log.h>
#include <Pyxis/Platform/Logging/LogCategories.h>

#include <nlohmann/json.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <ios>
#include <optional>
#include <sstream>
#include <string>

namespace pyxis::app {

namespace {

namespace fs = std::filesystem;

// Tiny convenience: read field T at jsonNode[key] into out, leaving out
// untouched (i.e., keeping the default) if the key is missing or has the
// wrong type. Returns nullopt on success, or an error message otherwise.
template <typename T>
std::string ReadField(const nlohmann::json& parent, const char* key, T& out) {
  const auto found = parent.find(key);
  if (found == parent.end() || found->is_null())
    return {};
  if constexpr (std::is_same_v<T, std::string>)
  {
    if (!found->is_string())
    {
      return std::string{key} + ": expected string";
    }
    out = found->get<std::string>();
  }
  else if constexpr (std::is_same_v<T, bool>)
  {
    if (!found->is_boolean())
    {
      return std::string{key} + ": expected bool";
    }
    out = found->get<bool>();
  }
  else
  {  // numeric (uint32_t etc.)
    if (!found->is_number_unsigned() && !found->is_number_integer())
    {
      return std::string{key} + ": expected number";
    }
    if constexpr (std::is_unsigned_v<T>)
    {
      // nlohmann distinguishes is_number_unsigned (literal had no
      // sign) from is_number_integer (any integer, signed or not).
      // For an unsigned destination, a literal like -42 is_integer
      // but not is_unsigned, and `get<uint32_t>()` would silently
      // wrap to 0xFFFFFFD6. Reject negatives with a precise error
      // instead so the user sees the real cause.
      if (!found->is_number_unsigned())
      {
        const auto signedValue = found->get<int64_t>();
        if (signedValue < 0)
        {
          return std::string{key} + ": negative value not allowed";
        }
      }
    }
    out = found->get<T>();
  }
  return {};
}

}  // namespace

std::expected<void, std::string> OverlayConfiguration(Configuration& target,
                                                      std::string_view jsonText) noexcept {
  // allow_exceptions=false keeps us /EHs-c- clean; on parse error
  // nlohmann returns json::value_t::discarded which is_discarded()=true.
  const nlohmann::json document = nlohmann::json::parse(jsonText.begin(), jsonText.end(),
                                                        /*cb*/ nullptr,
                                                        /*throw*/ false,
                                                        /*ignoreCmts*/ true);
  if (document.is_discarded())
  {
    return std::unexpected{std::string{"parameters.json: invalid JSON"}};
  }
  if (!document.is_object())
  {
    return std::unexpected{std::string{"parameters.json: top-level must be an object"}};
  }

  std::string failure;
  if (auto render = document.find("render"); render != document.end() && render->is_object())
  {
    if (failure.empty())
      failure = ReadField(*render, "width", target.render.width);
    if (failure.empty())
      failure = ReadField(*render, "height", target.render.height);
    if (failure.empty())
      failure = ReadField(*render, "samplesPerFrame", target.render.samplesPerFrame);
    if (failure.empty())
      failure = ReadField(*render, "seed", target.render.seed);
  }
  if (auto output = document.find("output"); output != document.end() && output->is_object())
  {
    if (failure.empty())
      failure = ReadField(*output, "image", target.output.image);
    if (failure.empty())
      failure = ReadField(*output, "ldr", target.output.ldr);
    if (failure.empty())
      failure = ReadField(*output, "effectiveConfig", target.output.effectiveConfig);
  }
  if (auto diag = document.find("diagnostics"); diag != document.end() && diag->is_object())
  {
    if (failure.empty())
      failure = ReadField(*diag, "validationLayer", target.diagnostics.validationLayer);
    if (failure.empty())
      failure = ReadField(*diag, "aftermath", target.diagnostics.aftermath);
  }
  if (auto limits = document.find("limits"); limits != document.end() && limits->is_object())
  {
    if (failure.empty())
      failure = ReadField(*limits, "framesInFlight", target.limits.framesInFlight);
  }
  if (!failure.empty())
  {
    return std::unexpected{"parameters.json: " + failure};
  }
  return {};
}

std::expected<Configuration, std::string> ParseConfiguration(std::string_view jsonText) noexcept {
  Configuration config{};
  if (auto result = OverlayConfiguration(config, jsonText); !result)
  {
    return std::unexpected{result.error()};
  }
  return config;
}

namespace {

// Read a whole text file. Returns nullopt when the file doesn't exist or
// can't be opened — overlay sites silently skip that layer.
std::optional<std::string> ReadFileToString(const std::string& path) noexcept {
  const std::ifstream stream(path, std::ios::binary);
  if (!stream.is_open())
    return std::nullopt;
  std::ostringstream buffer;
  buffer << stream.rdbuf();
  return buffer.str();
}

// %LOCALAPPDATA%/Pyxis/parameters.json. Mirrors AssetLocator::LocalAppData
// without forcing the directory creation that LocalAppData() does.
std::string UserParametersPath() noexcept {
  const char* localAppData = std::getenv("LOCALAPPDATA");
  if (localAppData == nullptr || *localAppData == '\0')
    return {};
  return std::string{localAppData} + "/Pyxis/parameters.json";
}

}  // namespace

std::expected<Configuration, std::string> ResolveConfiguration(const CliArgs& cli) noexcept {
  auto& log = Logging::Get();
  Configuration config{};  // Step 1: embedded defaults (C++ field initialisers).

  // Step 2: <exe-dir>/Resources/parameters.default.json. Optional —
  // missing file silently keeps the C++ defaults so a stripped binary
  // still boots.
  {
    const AssetLocator locator;
    const Path defaultsPath = locator.LocateResource("parameters.default.json");
    if (!defaultsPath.View().empty())
    {
      if (auto text = ReadFileToString(std::string{defaultsPath.View()}))
      {
        if (auto result = OverlayConfiguration(config, *text); !result)
        {
          return std::unexpected{"parameters.default.json: " + result.error()};
        }
        log.Debug(log::APP, "Configuration: overlaid <exe-dir>/Resources/parameters.default.json");
      }
    }
  }

  // Step 3: %LOCALAPPDATA%/Pyxis/parameters.json (per-user, optional).
  {
    const std::string userPath = UserParametersPath();
    if (!userPath.empty())
    {
      if (auto text = ReadFileToString(userPath))
      {
        if (auto result = OverlayConfiguration(config, *text); !result)
        {
          return std::unexpected{userPath + ": " + result.error()};
        }
        log.Debug(log::APP, "Configuration: overlaid " + userPath);
      }
    }
  }

  // Step 4: --config <path> (explicit, highest precedence among JSON
  // overlays). Failure here is fatal — the user asked for it.
  if (!cli.configPath.empty())
  {
    const std::string explicitPath{cli.configPath};
    auto text = ReadFileToString(explicitPath);
    if (!text)
    {
      return std::unexpected{"--config: cannot read " + explicitPath};
    }
    if (auto result = OverlayConfiguration(config, *text); !result)
    {
      return std::unexpected{explicitPath + ": " + result.error()};
    }
    log.Info(log::APP, "Configuration: overlaid --config " + explicitPath);
  }

  // Step 5: CLI overrides — applied last so command-line always wins.
  ApplyCliOverrides(config, cli);
  return config;
}

void ApplyCliOverrides(Configuration& config, const CliArgs& cli) noexcept {
  auto& log = Logging::Get();
  // CLI args win over JSON per §27 "CLI overrides: each CLI arg maps
  // to a JSON pointer; applied after parse, before validate."
  if (cli.adapterIndex >= 0)
  {
    // M3+ wires adapter into config.adapter; no-op for now.
    log.Info(log::APP, "--adapter parsed but not yet applied (M3+ scene config wires it).");
  }
  if (cli.enableValidation)
  {
    config.diagnostics.validationLayer = true;
  }
  if (cli.width != 0)
    config.render.width = cli.width;
  if (cli.height != 0)
    config.render.height = cli.height;
  if (cli.samples != 0)
    config.render.samplesPerFrame = cli.samples;
  if (cli.seed != 0)
    config.render.seed = cli.seed;
  if (!cli.outputPath.empty())
    config.output.image = std::string{cli.outputPath};
  // Surface CLI flags that we accept syntactically but don't yet
  // apply, so users aren't silently confused when --scene seems to do
  // nothing. M3 renders the hardcoded cube only; --scene wires in at
  // M3.5 (default scene) + M4 (ingest adapters); --camera at M3.5
  // when scenes start carrying multiple cameras.
  if (!cli.scenePath.empty())
  {
    log.Info(log::APP, std::string{"--scene "} + std::string{cli.scenePath}
                           + " parsed but ignored (M3.5 default-scene + M4 ingest).");
  }
  if (!cli.cameraSdfPath.empty())
  {
    log.Info(log::APP, std::string{"--camera "} + std::string{cli.cameraSdfPath}
                           + " parsed but ignored (M3+ scene.camera).");
  }
  if (!cli.profilePath.empty())
  {
    log.Info(log::APP, std::string{"--profile "} + std::string{cli.profilePath}
                           + " parsed but ignored (M11 profiling polish).");
  }
}

std::expected<void, std::string> ValidateForHeadless(const Configuration& config) noexcept {
  if (config.output.image.empty())
  {
    return std::unexpected{std::string{"headless requires output.image (or --output <path>)"}};
  }
  if (config.render.seed == 0)
  {
    // §33.7: zero seed defeats byte-identical EXR.
    return std::unexpected{
        std::string{"render.seed must be non-zero (§33.7 determinism contract)"}};
  }
  if (config.render.width == 0 || config.render.height == 0)
  {
    return std::unexpected{std::string{"render.width / render.height must be > 0"}};
  }
  return {};
}

std::expected<void, std::string> WriteEffectiveConfig(const Configuration& config) noexcept {
  if (config.output.effectiveConfig.empty())
  {
    return std::unexpected{std::string{"WriteEffectiveConfig: output.effectiveConfig is empty"}};
  }

  // mkdir -p the parent dir per §27 ("missing directories must never
  // abort a 30-minute Moana render").
  const fs::path effectivePath{config.output.effectiveConfig};
  if (effectivePath.has_parent_path())
  {
    std::error_code errorCode;
    fs::create_directories(effectivePath.parent_path(), errorCode);
    // Ignore errorCode — directory may already exist.
  }

  nlohmann::json document;
  document["render"]["width"] = config.render.width;
  document["render"]["height"] = config.render.height;
  document["render"]["samplesPerFrame"] = config.render.samplesPerFrame;
  document["render"]["seed"] = config.render.seed;
  document["output"]["image"] = config.output.image;
  document["output"]["ldr"] = config.output.ldr;
  document["output"]["effectiveConfig"] = config.output.effectiveConfig;
  document["diagnostics"]["validationLayer"] = config.diagnostics.validationLayer;
  document["diagnostics"]["aftermath"] = config.diagnostics.aftermath;
  document["limits"]["framesInFlight"] = config.limits.framesInFlight;

  std::ofstream stream(config.output.effectiveConfig, std::ios::binary | std::ios::trunc);
  if (!stream.is_open())
  {
    return std::unexpected{"WriteEffectiveConfig: could not open " + config.output.effectiveConfig};
  }
  // Pin every dump argument explicitly so the on-disk effective-config
  // is byte-stable across runs and across nlohmann versions: indent=2,
  // ASCII space (not tab), ensure_ascii=false (UTF-8 passthrough), and
  // error_handler=replace (no exceptions across our /EHs-c- boundary).
  // Keys are written in insertion order — nlohmann::json's underlying
  // ordered_map preserves that — so the layout above is the layout on
  // disk.
  const std::string text =
      document.dump(/*indent*/ 2,
                    /*indent_char*/ ' ',
                    /*ensure_ascii*/ false, nlohmann::json::error_handler_t::replace);
  stream.write(text.data(), static_cast<std::streamsize>(text.size()));
  if (!stream.good())
  {
    return std::unexpected{"WriteEffectiveConfig: write failed for "
                           + config.output.effectiveConfig};
  }
  return {};
}

}  // namespace pyxis::app
