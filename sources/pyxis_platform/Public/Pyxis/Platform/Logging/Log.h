// Pyxis platform — logging facade.
//
// Plan §33.10 — there is exactly *one* spdlog registry in the process.
// pyxis_platform.dll links spdlog SHARED and exposes the single Logger&
// through pyxis::Logging::Get(). Other DLLs reach the logger through this
// accessor — they MUST NOT call spdlog::default_logger() / SPDLOG_INFO()
// directly. The reviewer checklist (§30.9 + this rule) rejects PRs that do.
//
// spdlog itself never appears in this Public/ header — see SpdlogBackend
// in Private/ for the implementation. Consumers receive a thin pyxis::Logger
// reference and call Trace / Debug / Info / Warn / Error / Critical, all
// of which take std::string_view + a tiny POD payload.

#pragma once

#include <Pyxis/Platform/Logging/LogConfig.h>
#include <Pyxis/Platform/PlatformApi.h>

#include <cstdarg>
#include <string_view>

namespace pyxis {

class PYXIS_PLATFORM_API Logger final {
public:
    // ------------------------------------------------------------------
    // Apply a new configuration. Idempotent; safe to call multiple times.
    // The first call lazy-creates the rotating file sink.
    // ------------------------------------------------------------------
    void Configure(const LogConfig& cfg);

    // ------------------------------------------------------------------
    // Per-level emit. `category` follows the §31 dotted-prefix convention.
    // The message buffer is bounded; overlong messages are truncated with
    // a trailing ellipsis.
    // ------------------------------------------------------------------
    void Trace   (std::string_view category, std::string_view message) noexcept;
    void Debug   (std::string_view category, std::string_view message) noexcept;
    void Info    (std::string_view category, std::string_view message) noexcept;
    void Warn    (std::string_view category, std::string_view message) noexcept;
    void Error   (std::string_view category, std::string_view message) noexcept;
    void Critical(std::string_view category, std::string_view message) noexcept;

    // ------------------------------------------------------------------
    // Drain every sink. Called at process exit and from PYXIS_FATAL.
    // ------------------------------------------------------------------
    void Flush() noexcept;

    // Disable copy/move; the logger is process-scoped through Logging::Get().
    Logger(const Logger&)            = delete;
    Logger& operator=(const Logger&) = delete;

private:
    friend class Logging;
    Logger();
    ~Logger();

    struct Impl;
    Impl* _impl = nullptr;   // owned; private to keep spdlog out of Public/.
};

class PYXIS_PLATFORM_API Logging final {
public:
    // ------------------------------------------------------------------
    // Process-scoped accessor. Plan §33.10 — the documented exception to
    // the "no singletons" rule (§30.9) along with Tracy's client.
    // ------------------------------------------------------------------
    [[nodiscard]] static Logger& Get() noexcept;

private:
    Logging() = delete;
};

}  // namespace pyxis
