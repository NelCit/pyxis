// Pyxis app — CLI parsing.
//
// Plan §26 enumerates the full CLI surface; this struct grows in
// lockstep. Each new flag wires through Application::Run → the
// Configuration loader's CLI-overlay path (§27 "applied after parse,
// before validate"), which is the only place CLI values reach the
// rendering code.
//
// Anything not listed below is rejected with exit code 3 (config fail
// per §41 exit codes).

#pragma once

#include <cstdint>
#include <string_view>

namespace pyxis::app {

struct CliArgs {
  // ---- Mode + adapter -------------------------------------------------
  bool headless = false;
  bool enableValidation = false;  // --vk-validation
  int32_t adapterIndex = -1;      // -1 = pick highest-VRAM RT-capable.

  // ---- Help / version -------------------------------------------------
  bool showHelp = false;
  bool showVersion = false;

  // ---- Tooling / introspection ---------------------------------------
  // §29.4.a: print the absolute path of the bundled default scene
  // (<exe-dir>/Resources/scenes/default.usd) and exit. Useful for
  // tooling that wants to open the file in usdview / a text editor
  // without grepping the install layout.
  bool printDefaultScenePath = false;

  // ---- §26 config + scene --------------------------------------------
  std::string_view configPath;     // --config <path>
  std::string_view scenePath;      // --scene <path>  (M3.5 wires through SceneResolver)
  std::string_view cameraSdfPath;  // --camera <sdfPath>  (M2: stored)
  std::string_view ingest;         // --ingest hydra|usd_direct  (overrides app.ingest)

  // ---- §26 render / output overrides ---------------------------------
  // Zero means "no override; defer to JSON or default". Non-zero
  // overrides the corresponding parameters.json field at validate time.
  uint32_t width = 0;            // --width <int>
  uint32_t height = 0;           // --height <int>
  uint32_t samples = 0;          // --samples <int>
  uint32_t seed = 0;             // --seed <int>
  std::string_view outputPath;   // --output <path>  (overrides output.image)
  std::string_view profilePath;  // --profile <path>  (M11+ wires)

  // ---- M7 follow-up: AOV save -----------------------------------------
  // --save-aov <list>  Comma-separated list of raw AOVs to dump
  // alongside the regular `--output` BGRA8 EXR. The path stem of
  // `--output` becomes the prefix; per-AOV files are written as
  // `<prefix>_<aov>.exr` (RGBA32F). Headless-only — viewer mode has
  // its own per-frame Save button. Recognised names:
  //   color, normal, depth, instanceId, materialId, baseColor,
  //   worldPos, all
  // The "all" alias expands to every AOV in one shot.
  // Empty = no AOV save (current behaviour).
  std::string_view saveAov;

  // --shader-rebuild-dir <path>: explicit CMake build directory the
  // editor's "Reload shaders" button passes to ShaderMake. Empty =
  // walk-up heuristic from cwd looking for CMakeCache.txt (covers
  // the typical bin/Release/pyxis.exe -> build/dev layout). Useful
  // when the binary lives outside the build tree (packaged install
  // demo, dev launching from a separate working dir, etc.) so the
  // heuristic can't find a CMakeCache.txt anywhere upstream.
  std::string_view shaderRebuildDir;

  // ---- M1 viewer extras ----------------------------------------------
  // --screenshot <path>: run the viewer for a few warmup frames, copy
  // the backbuffer to PNG at the given path, then exit. Plan §35
  // image-regression artefact for the M1 viewer (M2's --headless
  // --output is the proper EXR pipeline). Empty = no screenshot.
  std::string_view screenshotPath;

  // ---- Parse error reporting -----------------------------------------
  bool invalid = false;  // True if `parse` saw an unknown flag.
  std::string_view invalidArg;
};

// Parses `argv[1..argc-1]`. Never allocates beyond the implicit
// std::string_view inputs — works under /EHs-c-.
CliArgs Parse(int argc, char** argv) noexcept;

// Writes a usage string to stdout. Stays small enough to fit in one screen.
void PrintUsage() noexcept;

// Writes the version line ("Pyxis <version>+<sha>") to stdout.
void PrintVersion() noexcept;

}  // namespace pyxis::app
