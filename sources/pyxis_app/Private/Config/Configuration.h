// Pyxis app — Configuration POD + loader.
//
// Plan §26 / §27. Mirrors the parameters.json schema 1:1; all fields
// the application code consumes flow through this struct. M2 ships the
// minimum subset needed by HeadlessMode (render dims + seed, output
// paths, diagnostics, limits.framesInFlight); M3+ grows the
// scene / textures / geometry / hydra / profiling sections in lockstep
// with the corresponding subsystems.
//
// The loader is exception-free: nlohmann::json's allow_exceptions=false
// parse mode keeps us compatible with /EHs-c- across pyxis_app.

#pragma once

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>

namespace pyxis::app {

struct CliArgs;  // CliArgs.h forward — the loader applies overrides.

// ----- §27.render --------------------------------------------------------
struct RenderConfig {
    uint32_t width           = 1920;
    uint32_t height          = 1080;
    uint32_t samplesPerFrame = 1;
    // RNG seed (§12 PCG32 + §33.7 determinism). Required to be non-zero
    // for headless EXR — a zero seed defeats the byte-identical contract
    // and is rejected at validation time.
    uint32_t seed            = 1;
    // M3+ extensions: maxBounces, enableAccumulation, exposure, toneMap,
    // debugView, accumulationFrameLimit, russianRouletteStartBounce,
    // fireflyClampLuminance, lowDiscrepancySampling, aovs, ...
};

// ----- §27.output --------------------------------------------------------
struct OutputConfig {
    std::string image;              // EXR path (required when headless).
    std::string ldr;                // Optional PNG path; empty = skip.
    std::string effectiveConfig;    // Resolved-config JSON dump path.
};

// ----- §27.diagnostics ---------------------------------------------------
struct DiagnosticsConfig {
    bool validationLayer = false;
    bool aftermath       = false;
};

// ----- §27.limits --------------------------------------------------------
struct LimitsConfig {
    // §33.1 cap is 3; the headless EXR path raises this from M2's default
    // back to 3 per §33.7 (byte-identical pinning).
    uint32_t framesInFlight = 1;
};

// ----- The whole tree ----------------------------------------------------
struct Configuration {
    RenderConfig      render;
    OutputConfig      output;
    DiagnosticsConfig diagnostics;
    LimitsConfig      limits;
    // M3+ sections (app.ingest, scene, textures, geometry, hydra,
    // profiling) land alongside the systems that consume them.
};

// Parse a parameters.json document into Configuration. Defaults are
// applied for missing fields; an unparseable document or a schema-level
// error returns the unexpected branch with a human-readable message.
[[nodiscard]] std::expected<Configuration, std::string>
ParseConfiguration(std::string_view jsonText) noexcept;

// Overlay CLI args onto a parsed Configuration. Applied AFTER parse, so
// CLI values take precedence (per §27 "applied after parse, before
// validate"). Mutates `config` in place.
void ApplyCliOverrides(Configuration& config, const CliArgs& cli) noexcept;

// Validate a fully-resolved Configuration (after CLI overrides). Headless
// invocations must carry a non-zero seed and a non-empty output.image.
[[nodiscard]] std::expected<void, std::string>
ValidateForHeadless(const Configuration& config) noexcept;

// Write the resolved Configuration back to disk at
// output.effectiveConfig. The parent directory is created first; failure
// is non-fatal (logged, returns false).
[[nodiscard]] bool WriteEffectiveConfig(const Configuration& config) noexcept;

}  // namespace pyxis::app
