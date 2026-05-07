// Pyxis app — Application entry point.
//
// Plan §1 / §41. M0: parses CLI, configures logging, dispatches to
// viewer or headless driver, returns the process exit code. Anything
// requiring config files or stages lands later (M2+).

#pragma once

namespace pyxis::app {

// argv is owned by the runtime (Win32 main / wWinMain shim in Main.cpp);
// Application borrows it.
int Run(int argc, char** argv) noexcept;

}  // namespace pyxis::app
