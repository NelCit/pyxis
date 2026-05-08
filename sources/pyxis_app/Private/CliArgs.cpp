// Pyxis app — minimal CLI parsing for M0.

#include "CliArgs.h"

#include <Pyxis/Renderer/Version.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string_view>

namespace pyxis::app {

namespace {

bool Equals(const char* a, const char* b) noexcept {
    return std::strcmp(a, b) == 0;
}

bool ParseInt(const char* s, int32_t& out) noexcept {
    if (s == nullptr || *s == '\0') return false;
    char*    end   = nullptr;
    const long val = std::strtol(s, &end, 10);
    if (end == s || *end != '\0') return false;
    out = static_cast<int32_t>(val);
    return true;
}

}  // namespace

CliArgs Parse(int argc, char** argv) noexcept {
    CliArgs out;
    for (int i = 1; i < argc; ++i) {
        const char* arg = argv[i];
        if (Equals(arg, "--headless")) {
            out.headless = true;
        } else if (Equals(arg, "--vk-validation")) {
            out.enableValidation = true;
        } else if (Equals(arg, "--adapter")) {
            if (i + 1 >= argc || !ParseInt(argv[i + 1], out.adapterIndex)) {
                out.invalid    = true;
                out.invalidArg = arg;
                return out;
            }
            ++i;
        } else if (Equals(arg, "--screenshot")) {
            if (i + 1 >= argc) {
                out.invalid    = true;
                out.invalidArg = arg;
                return out;
            }
            out.screenshotPath = argv[i + 1];
            ++i;
        } else if (Equals(arg, "--help") || Equals(arg, "-h")) {
            out.showHelp = true;
        } else if (Equals(arg, "--version")) {
            out.showVersion = true;
        } else {
            out.invalid    = true;
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
        "Options:\n"
        "  --headless              Run with VkDeviceManagerHeadless (no window).\n"
        "  --adapter <i>           Force adapter index (default: highest-VRAM RT-capable).\n"
        "  --vk-validation         Enable Vulkan validation layers.\n"
        "  --screenshot <path>     Run the viewer briefly, write the backbuffer\n"
        "                          as PNG to <path>, then exit.\n"
        "  --version               Print version and exit.\n"
        "  -h, --help              Show this help and exit.\n"
        "\n"
        "Plan reference: §26 (full CLI surface lands at M2+).\n",
        stdout);
}

void PrintVersion() noexcept {
    std::fprintf(stdout, "Pyxis %s\n", pyxis::GetVersionString());
}

}  // namespace pyxis::app
