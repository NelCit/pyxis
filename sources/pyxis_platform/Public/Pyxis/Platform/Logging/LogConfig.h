// Pyxis platform — runtime log configuration.
//
// Mutated by the Application from parameters.json + CLI; consumed once at
// pyxis::Logging::Get().Configure(...) time. Plan §47.2 (rotation) +
// §33.10 (single-registry contract).

#pragma once

#include <Pyxis/Platform/PlatformApi.h>

#include <cstdint>
#include <string_view>

namespace pyxis {

enum class LogLevel : uint8_t {
  Trace = 0,
  Debug = 1,
  Info = 2,
  Warning = 3,
  Error = 4,
  Critical = 5,
  Off = 6,
};

struct LogConfig {
  // Default-constructed values match the v1 floor:
  //   - Info-level to console + the rotating file sink.
  //   - %LOCALAPPDATA%/Pyxis/Logs/pyxis-<pid>-YYYYMMDD.log, 64 MiB / file,
  //     10 files retained per pid (≤ 640 MiB).  Plan §47.2.
  LogLevel consoleLevel = LogLevel::Info;
  LogLevel fileLevel = LogLevel::Debug;

  // Rotation (§47.2). 0 disables rotation (file sink off).
  uint64_t rotateBytes = 64ull * 1024ull * 1024ull;
  uint32_t rotateFiles = 10;

  // When non-empty, used as the directory for the rotating sink instead
  // of %LOCALAPPDATA%/Pyxis/Logs.
  std::string_view fileDirectory = {};

  // Headless --profile mode adds an unrotated sink (cap 100 MiB, then
  // ErrorKind::FileQuotaExceeded — §47.2). Empty disables.
  std::string_view profileSinkPath = {};
};

}  // namespace pyxis
