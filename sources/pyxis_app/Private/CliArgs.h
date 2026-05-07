// Pyxis app — minimal CLI parsing for M0.
//
// Plan §26 enumerates the eventual CLI surface (--config, --headless,
// --scene, --camera, ...). M0 only needs:
//   --headless           run with VkDeviceManagerHeadless instead of VkDeviceManager
//   --adapter <i>        force a specific adapter index
//   --vk-validation      enable validation layers
//   --version            print PYXIS_VERSION_STRING + git SHA and exit 0
//   --help / -h          print usage and exit 0
//
// Anything else is rejected with exit code 3 (config fail per §41 M0).

#pragma once

#include <cstdint>
#include <string_view>

namespace pyxis::app {

struct CliArgs {
    bool        headless        = false;
    bool        enableValidation = false;
    int32_t     adapterIndex    = -1;        // -1 = pick highest-VRAM RT-capable.
    bool        showHelp        = false;
    bool        showVersion     = false;

    // True if `parse` saw an unknown flag. Application maps to exit 3.
    bool        invalid         = false;
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
