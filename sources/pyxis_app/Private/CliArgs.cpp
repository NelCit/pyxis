// Pyxis app — CLI parsing.

#include "CliArgs.h"

#include <Pyxis/Renderer/Version.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>

namespace pyxis::app {

namespace {

bool Equals(const char* lhs, const char* rhs) noexcept {
  return std::strcmp(lhs, rhs) == 0;
}

bool ParseInt32(const char* str, int32_t& out) noexcept {
  if (str == nullptr || *str == '\0')
    return false;
  char* end = nullptr;
  const long val = std::strtol(str, &end, 10);
  if (end == str || *end != '\0')
    return false;
  out = static_cast<int32_t>(val);
  return true;
}

bool ParseUInt32(const char* str, uint32_t& out) noexcept {
  if (str == nullptr || *str == '\0')
    return false;
  char* end = nullptr;
  const unsigned long val = std::strtoul(str, &end, 10);
  if (end == str || *end != '\0')
    return false;
  out = static_cast<uint32_t>(val);
  return true;
}

// Helper: consume the next argv slot as a value for --flag. Returns
// false on missing value, in which case the caller marks invalid + bails.
bool TakeValue(int argc, char** argv, int& index, std::string_view& out) noexcept {
  if (index + 1 >= argc)
    return false;
  out = argv[index + 1];
  ++index;
  return true;
}

}  // namespace

CliArgs Parse(int argc, char** argv) noexcept {
  CliArgs out;
  for (int i = 1; i < argc; ++i)
  {
    const char* arg = argv[i];

    // ---- Mode + adapter ---------------------------------------------
    if (Equals(arg, "--headless"))
    {
      out.headless = true;
    }
    else if (Equals(arg, "--vk-validation"))
    {
      out.enableValidation = true;
    }
    else if (Equals(arg, "--adapter"))
    {
      if (i + 1 >= argc || !ParseInt32(argv[i + 1], out.adapterIndex))
      {
        out.invalid = true;
        out.invalidArg = arg;
        return out;
      }
      ++i;

      // ---- Help / version ---------------------------------------------
    }
    else if (Equals(arg, "--help") || Equals(arg, "-h"))
    {
      out.showHelp = true;
    }
    else if (Equals(arg, "--version"))
    {
      out.showVersion = true;
    }
    else if (Equals(arg, "--print-default-scene-path"))
    {
      out.printDefaultScenePath = true;

      // ---- §26 config + scene -----------------------------------------
    }
    else if (Equals(arg, "--config"))
    {
      if (!TakeValue(argc, argv, i, out.configPath))
      {
        out.invalid = true;
        out.invalidArg = arg;
        return out;
      }
    }
    else if (Equals(arg, "--scene"))
    {
      if (!TakeValue(argc, argv, i, out.scenePath))
      {
        out.invalid = true;
        out.invalidArg = arg;
        return out;
      }
    }
    else if (Equals(arg, "--camera"))
    {
      if (!TakeValue(argc, argv, i, out.cameraSdfPath))
      {
        out.invalid = true;
        out.invalidArg = arg;
        return out;
      }
    }
    else if (Equals(arg, "--ingest"))
    {
      if (!TakeValue(argc, argv, i, out.ingest))
      {
        out.invalid = true;
        out.invalidArg = arg;
        return out;
      }

      // ---- §26 render / output overrides ------------------------------
    }
    else if (Equals(arg, "--width"))
    {
      if (i + 1 >= argc || !ParseUInt32(argv[i + 1], out.width))
      {
        out.invalid = true;
        out.invalidArg = arg;
        return out;
      }
      ++i;
    }
    else if (Equals(arg, "--height"))
    {
      if (i + 1 >= argc || !ParseUInt32(argv[i + 1], out.height))
      {
        out.invalid = true;
        out.invalidArg = arg;
        return out;
      }
      ++i;
    }
    else if (Equals(arg, "--samples"))
    {
      if (i + 1 >= argc || !ParseUInt32(argv[i + 1], out.samples))
      {
        out.invalid = true;
        out.invalidArg = arg;
        return out;
      }
      ++i;
    }
    else if (Equals(arg, "--seed"))
    {
      if (i + 1 >= argc || !ParseUInt32(argv[i + 1], out.seed))
      {
        out.invalid = true;
        out.invalidArg = arg;
        return out;
      }
      ++i;
    }
    else if (Equals(arg, "--output"))
    {
      if (!TakeValue(argc, argv, i, out.outputPath))
      {
        out.invalid = true;
        out.invalidArg = arg;
        return out;
      }
    }
    else if (Equals(arg, "--profile"))
    {
      if (!TakeValue(argc, argv, i, out.profilePath))
      {
        out.invalid = true;
        out.invalidArg = arg;
        return out;
      }

      // ---- M1 viewer extras -------------------------------------------
    }
    else if (Equals(arg, "--screenshot"))
    {
      if (!TakeValue(argc, argv, i, out.screenshotPath))
      {
        out.invalid = true;
        out.invalidArg = arg;
        return out;
      }
    }
    else if (Equals(arg, "--save-aov"))
    {
      if (!TakeValue(argc, argv, i, out.saveAov))
      {
        out.invalid = true;
        out.invalidArg = arg;
        return out;
      }
    }
    else if (Equals(arg, "--shader-rebuild-dir"))
    {
      if (!TakeValue(argc, argv, i, out.shaderRebuildDir))
      {
        out.invalid = true;
        out.invalidArg = arg;
        return out;
      }
    }
    else if (Equals(arg, "--bench-frames"))
    {
      if (i + 1 >= argc || !ParseUInt32(argv[i + 1], out.benchFrames))
      {
        out.invalid = true;
        out.invalidArg = arg;
        return out;
      }
      ++i;
    }
    else if (Equals(arg, "--frame"))
    {
      if (i + 1 >= argc || !ParseInt32(argv[i + 1], out.frameNumber))
      {
        out.invalid = true;
        out.invalidArg = arg;
        return out;
      }
      ++i;
    }
    else if (Equals(arg, "--frame-range"))
    {
      // Format: BEGIN..END[:STEP] — e.g. 0..100 or 0..100:5.
      // Parse by string-search since we don't want a third-party
      // tokenizer for one CLI flag.
      if (i + 1 >= argc)
      {
        out.invalid = true;
        out.invalidArg = arg;
        return out;
      }
      const std::string spec{argv[i + 1]};
      const std::size_t dotPos = spec.find("..");
      if (dotPos == std::string::npos)
      {
        out.invalid = true;
        out.invalidArg = arg;
        return out;
      }
      const std::string beginStr = spec.substr(0, dotPos);
      const std::string tail = spec.substr(dotPos + 2);
      std::string endStr = tail;
      std::string stepStr = "1";
      if (const std::size_t colonPos = tail.find(':');
          colonPos != std::string::npos)
      {
        endStr  = tail.substr(0, colonPos);
        stepStr = tail.substr(colonPos + 1);
      }
      if (!ParseInt32(beginStr.c_str(), out.frameRangeBegin)
          || !ParseInt32(endStr.c_str(),   out.frameRangeEnd)
          || !ParseInt32(stepStr.c_str(),  out.frameRangeStep)
          || out.frameRangeStep <= 0
          || out.frameRangeEnd < out.frameRangeBegin)
      {
        out.invalid = true;
        out.invalidArg = arg;
        return out;
      }
      ++i;
    }
    else if (Equals(arg, "--load-mode"))
    {
      if (!TakeValue(argc, argv, i, out.loadMode))
      {
        out.invalid = true;
        out.invalidArg = arg;
        return out;
      }
    }
    else if (Equals(arg, "--population-mask"))
    {
      if (!TakeValue(argc, argv, i, out.populationMask))
      {
        out.invalid = true;
        out.invalidArg = arg;
        return out;
      }
    }
    else if (Equals(arg, "--variant"))
    {
      // V2.A.2 (M12). Comma-separated <primPath>:<setName>=<value>
      // triples applied to the stage's session layer post Open.
      // Parsing of the spec into structured records lives in
      // VariantParser.{h,cpp} so the unit test can exercise it
      // without dragging USD into the test TU.
      if (!TakeValue(argc, argv, i, out.variantSelections))
      {
        out.invalid = true;
        out.invalidArg = arg;
        return out;
      }
    }
    else
    {
      out.invalid = true;
      out.invalidArg = arg;
      return out;
    }
  }
  return out;
}

void PrintUsage() noexcept {
  std::fputs(
      "Usage: pyxis [options]\n"
      "\n"
      "Mode:\n"
      "  --headless              Run with VkDeviceManagerHeadless (no window).\n"
      "  --adapter <i>           Force adapter index (default: highest-VRAM RT-capable).\n"
      "  --vk-validation         Enable Vulkan validation layers.\n"
      "\n"
      "Configuration (§26 / §27):\n"
      "  --config <path>         Load parameters.json. Overlay order:\n"
      "                          embedded-defaults -> exe-dir -> user -> --config.\n"
      "  --scene <path>          Override scene.path.\n"
      "  --camera <sdfPath>      Override scene.camera.\n"
      "  --ingest <hydra|usd_direct>  Override app.ingest (§3 / §25.O).\n"
      "  --width <int>           Override render.width.\n"
      "  --height <int>          Override render.height.\n"
      "  --samples <int>         Override render.samplesPerFrame.\n"
      "  --seed <int>            Override render.seed (must be non-zero in headless).\n"
      "  --output <path>         Override output.image (EXR).\n"
      "  --profile <path>        Headless: write a profile JSON sidecar to <path>\n"
      "                          describing GPU / driver / scene / per-pass\n"
      "                          percentiles (M10 §35). Consumed by\n"
      "                          _tools/run_regression.py to build the rolling\n"
      "                          per-test KPI CSV. With --bench-frames > 0 the\n"
      "                          passes[] block is populated from the steady-state\n"
      "                          measurement window; otherwise an empty bench.\n"
      "  --save-aov <list>       Headless: write extra EXRs alongside --output for the\n"
      "                          comma-separated raw AOV names. Recognised:\n"
      "                          color,normal,depth,instanceId,materialId,baseColor,\n"
      "                          worldPos,all. Each AOV writes <prefix>_<aov>.exr where\n"
      "                          <prefix> is --output stripped of its .exr extension.\n"
      "  --shader-rebuild-dir <path>\n"
      "                          Viewer: CMake build dir the Reload Shaders button\n"
      "                          spawns ShaderMake against (overrides the cwd walk-up\n"
      "                          heuristic). Use when the binary lives outside the\n"
      "                          build tree.\n"
      "  --bench-frames <int>    Headless: after the regular EXR write, run N warm-up\n"
      "                          frames + N measurement frames; print a §34 KPI table\n"
      "                          (pass.PathTrace, frame.cpu.commitResources, p50/p99/max).\n"
      "                          0 = disabled (default).\n"
      "  --frame <int>           Time-varying USD frame to evaluate (V2.A.13).\n"
      "                          -1 = default time.\n"
      "  --frame-range B..E[:S]  Multi-frame headless render (V2.A.4). Loops the\n"
      "                          render for frames B, B+S, ..., <= E and writes one\n"
      "                          numbered EXR per frame (out.0000.exr, out.0001.exr,\n"
      "                          ...). Ignores --frame. Step defaults to 1.\n"
      "  --load-mode <mode>      USD composition load mode (V2.A.15):\n"
      "                            all       UsdStage::InitialLoadSet::LoadAll (default).\n"
      "                            none      LoadNone — open with all payloads unloaded.\n"
      "                            metadata  alias for 'none'.\n"
      "                          Unknown values warn + fall back to all.\n"
      "  --population-mask <p>   Comma-separated SdfPath prefixes for partial-stage load.\n"
      "                          Triggers UsdStage::OpenMasked instead of Open.\n"
      "  --variant <spec>        Comma-separated <primPath>:<setName>=<value> triples\n"
      "                          applied via the stage's session layer after Open,\n"
      "                          overriding any authored variantSelection (V2.A.2).\n"
      "                          Example: --variant /World/Hero:lod=high,/World/Cup:season=winter\n"
      "\n"
      "Viewer extras:\n"
      "  --screenshot <path>     Run viewer briefly; write a PNG of the backbuffer.\n"
      "\n"
      "Help / version:\n"
      "  --version               Print version and exit.\n"
      "  -h, --help              Show this help and exit.\n"
      "\n"
      "Tooling:\n"
      "  --print-default-scene-path  Print <exe-dir>/Resources/scenes/default.usd\n"
      "                              (the §29.4.a bundled default) and exit.\n",
      stdout);
}

void PrintVersion() noexcept {
  std::fprintf(stdout, "Pyxis %s\n", pyxis::GetVersionString());
}

}  // namespace pyxis::app
