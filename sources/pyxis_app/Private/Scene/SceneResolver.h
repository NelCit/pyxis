// Pyxis app — default-startup-scene resolution chain.
//
// Plan §29.4.a. Decides which scene file `pyxis.exe` should hand to
// the (M4+) ingest layer at startup. Chain (highest precedence first):
//
//   1. --scene <path>         (CLI override)
//   2. paths.scene            (parameters.json field)
//   3. recent_scenes.json     (deferred to M4+ — needs scene-load
//                              telemetry that doesn't exist yet)
//   4. <exe-dir>/Resources/scenes/default.usd  (bundled default)
//
// At M3.5 the resolved path is just *emitted* via a one-line spdlog
// (`scene.resolved.source = ...`) and stored alongside the
// Configuration; nothing parses USD yet. M4+ feeds the resolved path
// into IngestUsd() (which dispatches by `config.app.ingest` to the
// hydra / usd_direct adapter) and renders it for real.

#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace pyxis::app {

struct CliArgs;
struct Configuration;

enum class SceneSource : uint8_t {
  Cli,      // --scene <path>
  Config,   // parameters.json paths.scene
  Recent,   // %LOCALAPPDATA%/Pyxis/recent_scenes.json (M4+)
  Bundled,  // <exe-dir>/Resources/scenes/default.usd
  None,     // chain exhausted (the bundled default is missing too —
            // installation is broken)
};

struct ResolvedScene {
  // Absolute path to the resolved scene file, or empty if `source ==
  // SceneSource::None`. UTF-8.
  std::string path;
  SceneSource source = SceneSource::None;
};

// Run the §29.4.a chain. Side effect: if a higher-priority step
// supplies a non-empty path that doesn't exist on disk, logs a Warn
// and falls through to the next step — user typos route to the
// bundled default rather than producing a hard error, matching the
// §29.4.a "must produce a renderable image" contract.
[[nodiscard]] ResolvedScene ResolveScene(const CliArgs& cli,
                                         const Configuration& config) noexcept;

// Human-readable label matching the spdlog format documented in
// §29.4.a (`scene.resolved.source = cli | config | recent | bundled`).
[[nodiscard]] std::string_view SceneSourceLabel(SceneSource source) noexcept;

// Absolute path of the bundled default scene
// (<exe-dir>/Resources/scenes/default.usd). Returns the empty string
// if the file is missing (broken install). Used by both the resolver
// itself and the `--print-default-scene-path` exit-mode CLI flag.
[[nodiscard]] std::string BundledDefaultScenePath() noexcept;

}  // namespace pyxis::app
