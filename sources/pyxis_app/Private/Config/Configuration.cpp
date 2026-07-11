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
  else if constexpr (std::is_floating_point_v<T>)
  {
    // Floating-point destination (e.g. render.exposure in stops) — accept
    // any JSON number, integer or real, positive or negative.
    if (!found->is_number())
    {
      return std::string{key} + ": expected number";
    }
    out = found->get<T>();
  }
  else
  {  // integral (uint32_t etc.)
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

// DLSS Stage 1 (rtx-realtime-alignment-design.md) — render.realTimeQuality.
// denoiser accepts either the JSON string form ("dlss"/"builtin"/"off") or
// a raw integer (0/1/2, falling back to ReadField's normal integral path).
// Unrecognised strings are rejected with a precise error rather than
// silently keeping the default — a denoiser typo silently picking a
// different chain would be a nasty footgun.
std::string ReadDenoiserField(const nlohmann::json& parent, const char* key, uint32_t& out) {
  const auto found = parent.find(key);
  if (found == parent.end() || found->is_null())
    return {};
  if (found->is_string())
  {
    const std::string value = found->get<std::string>();
    if (value == "dlss") { out = 0u; return {}; }
    if (value == "builtin") { out = 1u; return {}; }
    if (value == "off") { out = 2u; return {}; }
    if (value == "nrd") { out = 3u; return {}; }  // optional NRD backend (PYXIS_WITH_NRD builds)
    return std::string{key} + ": expected \"dlss\" | \"builtin\" | \"off\" | \"nrd\" (got \""
         + value + "\")";
  }
  return ReadField(parent, key, out);
}

// Same shape as ReadDenoiserField for render.realTimeQuality.dlssExecMode
// ("auto"/"quality"/"balanced"/"performance"/"dlaa" or 0..4).
std::string ReadDlssExecModeField(const nlohmann::json& parent, const char* key, uint32_t& out) {
  const auto found = parent.find(key);
  if (found == parent.end() || found->is_null())
    return {};
  if (found->is_string())
  {
    const std::string value = found->get<std::string>();
    if (value == "auto") { out = 0u; return {}; }
    if (value == "quality") { out = 1u; return {}; }
    if (value == "balanced") { out = 2u; return {}; }
    if (value == "performance") { out = 3u; return {}; }
    if (value == "dlaa") { out = 4u; return {}; }
    return std::string{key}
         + ": expected \"auto\" | \"quality\" | \"balanced\" | \"performance\" | \"dlaa\" (got \""
         + value + "\")";
  }
  return ReadField(parent, key, out);
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
    if (failure.empty())
      failure = ReadField(*render, "exposure", target.render.exposure);
    if (failure.empty())
      failure = ReadField(*render, "autoExposure", target.render.autoExposure);
    if (failure.empty())
      failure = ReadField(*render, "autoExposureKey", target.render.autoExposureKey);
    // RTX-alignment design (rtx-realtime-alignment-design.md), Phase C.
    if (failure.empty())
      failure = ReadField(*render, "autoExposureHistogram", target.render.autoExposureHistogram);
    // RTX-alignment design, Phase B follow-up — headless temporal
    // convergence: render N frames back-to-back in ONE renderer session
    // (temporal denoiser + TAA history accumulate across them, RNG
    // decorrelates via frameIndex) and write the EXR after the last.
    // 1 = today's single-frame behaviour (byte-equal default).
    if (failure.empty())
      failure = ReadField(*render, "accumulationFrames", target.render.accumulationFrames);
    if (failure.empty())
      failure = ReadField(*render, "exposureMode", target.render.exposureMode);
    if (failure.empty())
      failure = ReadField(*render, "exposureResponsivity", target.render.exposureResponsivity);
    // RTX-alignment design, WP2-final — render.realTimeQuality.* (mirrors
    // pyxis::RenderSettings::RealTimeQuality; see Configuration.h).
    if (auto rtq = render->find("realTimeQuality");
        rtq != render->end() && rtq->is_object())
    {
      if (failure.empty())
        failure = ReadField(*rtq, "passMask", target.render.realTimeQuality.passMask);
      if (failure.empty())
        failure = ReadField(*rtq, "directSamples", target.render.realTimeQuality.directSamples);
      if (failure.empty())
        failure = ReadField(*rtq, "indirectSamples", target.render.realTimeQuality.indirectSamples);
      if (failure.empty())
        failure = ReadField(*rtq, "indirectMaxBounces",
                            target.render.realTimeQuality.indirectMaxBounces);
      if (failure.empty())
        failure =
            ReadField(*rtq, "reflectionSamples", target.render.realTimeQuality.reflectionSamples);
      if (failure.empty())
        failure = ReadField(*rtq, "reflectionMaxRoughness",
                            target.render.realTimeQuality.reflectionMaxRoughness);
      if (failure.empty())
        failure = ReadField(*rtq, "refractionMaxBounces",
                            target.render.realTimeQuality.refractionMaxBounces);
      if (failure.empty())
        failure = ReadField(*rtq, "aoRayLength", target.render.realTimeQuality.aoRayLength);
      if (failure.empty())
        failure = ReadField(*rtq, "maxRayIntensityDirect",
                            target.render.realTimeQuality.maxRayIntensityDirect);
      if (failure.empty())
        failure = ReadField(*rtq, "maxRayIntensityIndirect",
                            target.render.realTimeQuality.maxRayIntensityIndirect);
      if (failure.empty())
        failure = ReadField(*rtq, "maxRayIntensityReflections",
                            target.render.realTimeQuality.maxRayIntensityReflections);
      // RTX-alignment design (rtx-realtime-alignment-design.md), Phase C.
      if (failure.empty())
        failure = ReadField(*rtq, "tonemapOperator", target.render.realTimeQuality.tonemapOperator);
      if (failure.empty())
        failure = ReadField(*rtq, "physicalCameraFStop",
                            target.render.realTimeQuality.physicalCameraFStop);
      if (failure.empty())
        failure = ReadField(*rtq, "physicalCameraIso",
                            target.render.realTimeQuality.physicalCameraIso);
      if (failure.empty())
        failure = ReadField(*rtq, "physicalCameraExposureTimeSeconds",
                            target.render.realTimeQuality.physicalCameraExposureTimeSeconds);
      // DLSS Stage 1 (rtx-realtime-alignment-design.md).
      if (failure.empty())
        failure = ReadDenoiserField(*rtq, "denoiser", target.render.realTimeQuality.denoiser);
      if (failure.empty())
        failure = ReadDlssExecModeField(*rtq, "dlssExecMode",
                                        target.render.realTimeQuality.dlssExecMode);
      // RTX-alignment 2026-07-10 ("image not smooth") — optional post-tonemap
      // Gaussian sigma in pixels; 0 (default) disables the pass entirely.
      if (failure.empty())
        failure = ReadField(*rtq, "postSoftenSigma",
                            target.render.realTimeQuality.postSoftenSigma);
      // RTX-alignment 2026-07-11 ("window borders shadowed") — optional
      // veiling-bloom gain; 0 (default) disables the pass entirely.
      if (failure.empty())
        failure = ReadField(*rtq, "postBloomGain",
                            target.render.realTimeQuality.postBloomGain);
      // RTX-alignment 2026-07-11 (item 1, "windows different") — frame-wide
      // exposed-luminance cap; 0 (default) disables it.
      if (failure.empty())
        failure = ReadField(*rtq, "maxExposedLuminance",
                            target.render.realTimeQuality.maxExposedLuminance);
    }
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
  if (auto paths = document.find("paths"); paths != document.end() && paths->is_object())
  {
    if (failure.empty())
      failure = ReadField(*paths, "scene", target.paths.scene);
  }
  if (auto sceneNode = document.find("scene");
      sceneNode != document.end() && sceneNode->is_object())
  {
    if (failure.empty())
      failure = ReadField(*sceneNode, "camera", target.scene.camera);
  }
  if (auto appNode = document.find("app"); appNode != document.end() && appNode->is_object())
  {
    if (failure.empty())
      failure = ReadField(*appNode, "ingest", target.app.ingest);
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

// DLSS Stage 1 — inverse of ReadDenoiserField / ReadDlssExecModeField, used
// by WriteEffectiveConfig so the on-disk dump stays in the same
// human-readable string form the JSON loader accepts (rather than the
// harder-to-read raw integer every other realTimeQuality field uses).
std::string DenoiserToString(uint32_t value) noexcept {
  switch (value)
  {
    case 0u: return "dlss";
    case 1u: return "builtin";
    case 2u: return "off";
    case 3u: return "nrd";
    default: return "dlss";
  }
}

std::string DlssExecModeToString(uint32_t value) noexcept {
  switch (value)
  {
    case 0u: return "auto";
    case 1u: return "quality";
    case 2u: return "balanced";
    case 3u: return "performance";
    case 4u: return "dlaa";
    default: return "auto";
  }
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
  // RTX-alignment design (rtx-realtime-alignment-design.md), WP2-final —
  // --camera now really overrides scene.camera (StageWalker prefers the
  // SdfPath match over its boundCamera-hint / first-in-order fallback;
  // see IngestUsd -> StageWalker::WalkFile/WalkStage).
  if (!cli.cameraSdfPath.empty())
  {
    config.scene.camera = std::string{cli.cameraSdfPath};
    log.Info(log::APP, std::string{"--camera "} + config.scene.camera + " applied.");
  }
  if (!cli.profilePath.empty())
  {
    log.Info(log::APP, std::string{"--profile "} + std::string{cli.profilePath}
                           + " parsed but ignored (M11 profiling polish).");
  }
  if (!cli.ingest.empty())
  {
    config.app.ingest = std::string{cli.ingest};
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
  if (config.app.ingest != "hydra" && config.app.ingest != "usd_direct")
  {
    return std::unexpected{
        std::string{"app.ingest must be \"hydra\" or \"usd_direct\" (got \""}
        + config.app.ingest + "\")"};
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
  document["render"]["exposure"] = config.render.exposure;
  document["render"]["autoExposure"] = config.render.autoExposure;
  document["render"]["autoExposureKey"] = config.render.autoExposureKey;
  document["render"]["autoExposureHistogram"] = config.render.autoExposureHistogram;
  document["render"]["accumulationFrames"] = config.render.accumulationFrames;
  document["render"]["exposureMode"] = config.render.exposureMode;
  document["render"]["exposureResponsivity"] = config.render.exposureResponsivity;
  document["render"]["realTimeQuality"]["passMask"] = config.render.realTimeQuality.passMask;
  document["render"]["realTimeQuality"]["directSamples"] =
      config.render.realTimeQuality.directSamples;
  document["render"]["realTimeQuality"]["indirectSamples"] =
      config.render.realTimeQuality.indirectSamples;
  document["render"]["realTimeQuality"]["indirectMaxBounces"] =
      config.render.realTimeQuality.indirectMaxBounces;
  document["render"]["realTimeQuality"]["reflectionSamples"] =
      config.render.realTimeQuality.reflectionSamples;
  document["render"]["realTimeQuality"]["reflectionMaxRoughness"] =
      config.render.realTimeQuality.reflectionMaxRoughness;
  document["render"]["realTimeQuality"]["refractionMaxBounces"] =
      config.render.realTimeQuality.refractionMaxBounces;
  document["render"]["realTimeQuality"]["aoRayLength"] = config.render.realTimeQuality.aoRayLength;
  document["render"]["realTimeQuality"]["maxRayIntensityDirect"] =
      config.render.realTimeQuality.maxRayIntensityDirect;
  document["render"]["realTimeQuality"]["maxRayIntensityIndirect"] =
      config.render.realTimeQuality.maxRayIntensityIndirect;
  document["render"]["realTimeQuality"]["maxRayIntensityReflections"] =
      config.render.realTimeQuality.maxRayIntensityReflections;
  document["render"]["realTimeQuality"]["tonemapOperator"] =
      config.render.realTimeQuality.tonemapOperator;
  document["render"]["realTimeQuality"]["physicalCameraFStop"] =
      config.render.realTimeQuality.physicalCameraFStop;
  document["render"]["realTimeQuality"]["physicalCameraIso"] =
      config.render.realTimeQuality.physicalCameraIso;
  document["render"]["realTimeQuality"]["physicalCameraExposureTimeSeconds"] =
      config.render.realTimeQuality.physicalCameraExposureTimeSeconds;
  // DLSS Stage 1 (rtx-realtime-alignment-design.md) — dumped as strings
  // (not raw integers, unlike every other realTimeQuality field) so the
  // effective-config sidecar stays readable and round-trips through
  // ReadDenoiserField / ReadDlssExecModeField unchanged.
  document["render"]["realTimeQuality"]["denoiser"] =
      DenoiserToString(config.render.realTimeQuality.denoiser);
  document["render"]["realTimeQuality"]["dlssExecMode"] =
      DlssExecModeToString(config.render.realTimeQuality.dlssExecMode);
  document["render"]["realTimeQuality"]["postSoftenSigma"] =
      config.render.realTimeQuality.postSoftenSigma;
  document["render"]["realTimeQuality"]["postBloomGain"] =
      config.render.realTimeQuality.postBloomGain;
  document["render"]["realTimeQuality"]["maxExposedLuminance"] =
      config.render.realTimeQuality.maxExposedLuminance;
  document["output"]["image"] = config.output.image;
  document["output"]["ldr"] = config.output.ldr;
  document["output"]["effectiveConfig"] = config.output.effectiveConfig;
  document["diagnostics"]["validationLayer"] = config.diagnostics.validationLayer;
  document["diagnostics"]["aftermath"] = config.diagnostics.aftermath;
  document["limits"]["framesInFlight"] = config.limits.framesInFlight;
  document["paths"]["scene"] = config.paths.scene;
  document["scene"]["camera"] = config.scene.camera;
  document["app"]["ingest"] = config.app.ingest;

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
