// Pyxis app — CLI parsing.

#include "CliArgs.h"

#include <Pyxis/Renderer/Version.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
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
      "  --width <int>           Override render.width.\n"
      "  --height <int>          Override render.height.\n"
      "  --samples <int>         Override render.samplesPerFrame.\n"
      "  --seed <int>            Override render.seed (must be non-zero in headless).\n"
      "  --output <path>         Override output.image (EXR).\n"
      "  --profile <path>        Override profiling output (M11+).\n"
      "\n"
      "Viewer extras:\n"
      "  --screenshot <path>     Run viewer briefly; write a PNG of the backbuffer.\n"
      "\n"
      "Help / version:\n"
      "  --version               Print version and exit.\n"
      "  -h, --help              Show this help and exit.\n",
      stdout);
}

void PrintVersion() noexcept {
  std::fprintf(stdout, "Pyxis %s\n", pyxis::GetVersionString());
}

}  // namespace pyxis::app
