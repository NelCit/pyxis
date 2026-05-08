// Pyxis app — Configuration loader.

#include "Config/Configuration.h"

#include "CliArgs.h"

#include <Pyxis/Platform/Logging/Log.h>
#include <Pyxis/Platform/Logging/LogCategories.h>

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <ios>
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
    if (found == parent.end() || found->is_null()) return {};
    if constexpr (std::is_same_v<T, std::string>) {
        if (!found->is_string()) {
            return std::string{key} + ": expected string";
        }
        out = found->get<std::string>();
    } else if constexpr (std::is_same_v<T, bool>) {
        if (!found->is_boolean()) {
            return std::string{key} + ": expected bool";
        }
        out = found->get<bool>();
    } else {  // numeric (uint32_t etc.)
        if (!found->is_number_unsigned() && !found->is_number_integer()) {
            return std::string{key} + ": expected number";
        }
        out = found->get<T>();
    }
    return {};
}

}  // namespace

std::expected<Configuration, std::string>
ParseConfiguration(std::string_view jsonText) noexcept {
    // allow_exceptions=false keeps us /EHs-c- clean; on parse error
    // nlohmann returns json::value_t::discarded which is_discarded()=true.
    const nlohmann::json document =
        nlohmann::json::parse(jsonText.begin(), jsonText.end(),
                              /*cb*/        nullptr,
                              /*throw*/     false,
                              /*ignoreCmts*/true);
    if (document.is_discarded()) {
        return std::unexpected{std::string{"parameters.json: invalid JSON"}};
    }
    if (!document.is_object()) {
        return std::unexpected{std::string{"parameters.json: top-level must be an object"}};
    }

    Configuration config{};
    std::string failure;

    if (auto render = document.find("render"); render != document.end() && render->is_object()) {
        if (failure.empty()) failure = ReadField(*render, "width",           config.render.width);
        if (failure.empty()) failure = ReadField(*render, "height",          config.render.height);
        if (failure.empty()) failure = ReadField(*render, "samplesPerFrame", config.render.samplesPerFrame);
        if (failure.empty()) failure = ReadField(*render, "seed",            config.render.seed);
    }
    if (auto output = document.find("output"); output != document.end() && output->is_object()) {
        if (failure.empty()) failure = ReadField(*output, "image",            config.output.image);
        if (failure.empty()) failure = ReadField(*output, "ldr",              config.output.ldr);
        if (failure.empty()) failure = ReadField(*output, "effectiveConfig",  config.output.effectiveConfig);
    }
    if (auto diag = document.find("diagnostics"); diag != document.end() && diag->is_object()) {
        if (failure.empty()) failure = ReadField(*diag, "validationLayer", config.diagnostics.validationLayer);
        if (failure.empty()) failure = ReadField(*diag, "aftermath",       config.diagnostics.aftermath);
    }
    if (auto limits = document.find("limits"); limits != document.end() && limits->is_object()) {
        if (failure.empty()) failure = ReadField(*limits, "framesInFlight", config.limits.framesInFlight);
    }
    if (!failure.empty()) {
        return std::unexpected{"parameters.json: " + failure};
    }
    return config;
}

void ApplyCliOverrides(Configuration& config, const CliArgs& cli) noexcept {
    // CLI args win over JSON per §27 "CLI overrides: each CLI arg maps
    // to a JSON pointer; applied after parse, before validate."
    if (cli.adapterIndex >= 0) {
        // M3+ wires adapter into config.adapter; no-op for now.
    }
    if (cli.enableValidation) {
        config.diagnostics.validationLayer = true;
    }
    if (cli.width  != 0) config.render.width  = cli.width;
    if (cli.height != 0) config.render.height = cli.height;
    if (cli.samples != 0) config.render.samplesPerFrame = cli.samples;
    if (cli.seed   != 0) config.render.seed = cli.seed;
    if (!cli.outputPath.empty()) config.output.image = std::string{cli.outputPath};
}

std::expected<void, std::string>
ValidateForHeadless(const Configuration& config) noexcept {
    if (config.output.image.empty()) {
        return std::unexpected{
            std::string{"headless requires output.image (or --output <path>)"}};
    }
    if (config.render.seed == 0) {
        // §33.7: zero seed defeats byte-identical EXR.
        return std::unexpected{
            std::string{"render.seed must be non-zero (§33.7 determinism contract)"}};
    }
    if (config.render.width == 0 || config.render.height == 0) {
        return std::unexpected{std::string{"render.width / render.height must be > 0"}};
    }
    return {};
}

bool WriteEffectiveConfig(const Configuration& config) noexcept {
    if (config.output.effectiveConfig.empty()) return false;
    auto& log = Logging::Get();

    // mkdir -p the parent dir per §27 ("missing directories must never
    // abort a 30-minute Moana render"). Failure logged, non-fatal.
    const fs::path imagePath{config.output.effectiveConfig};
    if (imagePath.has_parent_path()) {
        std::error_code errorCode;
        fs::create_directories(imagePath.parent_path(), errorCode);
        // Ignore errorCode — directory may already exist.
    }

    nlohmann::json document;
    document["render"]["width"]            = config.render.width;
    document["render"]["height"]           = config.render.height;
    document["render"]["samplesPerFrame"]  = config.render.samplesPerFrame;
    document["render"]["seed"]             = config.render.seed;
    document["output"]["image"]            = config.output.image;
    document["output"]["ldr"]              = config.output.ldr;
    document["output"]["effectiveConfig"]  = config.output.effectiveConfig;
    document["diagnostics"]["validationLayer"] = config.diagnostics.validationLayer;
    document["diagnostics"]["aftermath"]   = config.diagnostics.aftermath;
    document["limits"]["framesInFlight"]   = config.limits.framesInFlight;

    std::ofstream stream(config.output.effectiveConfig, std::ios::binary | std::ios::trunc);
    if (!stream.is_open()) {
        log.Warn(log::APP, "WriteEffectiveConfig: could not open " + config.output.effectiveConfig);
        return false;
    }
    const std::string text = document.dump(2);
    stream.write(text.data(), static_cast<std::streamsize>(text.size()));
    return stream.good();
}

}  // namespace pyxis::app
