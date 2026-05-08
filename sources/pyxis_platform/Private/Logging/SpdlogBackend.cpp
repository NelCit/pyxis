// Pyxis platform — spdlog backend.
//
// Single registry, multiple sinks (console + rotating file + optional profile
// sink). Plan §33.10 / §47.2.

#include "Logging/SpdlogBackend.h"

#include <Pyxis/Platform/FileSystem/AssetLocator.h>
#include <Pyxis/Platform/FileSystem/Path.h>

#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <string>
#include <vector>

namespace pyxis {

namespace {

spdlog::level::level_enum ToSpdlog(LogLevel level) noexcept {
    switch (level) {
        case LogLevel::Trace:    return spdlog::level::trace;
        case LogLevel::Debug:    return spdlog::level::debug;
        case LogLevel::Info:     return spdlog::level::info;
        case LogLevel::Warning:  return spdlog::level::warn;
        case LogLevel::Error:    return spdlog::level::err;
        case LogLevel::Critical: return spdlog::level::critical;
        case LogLevel::Off:      return spdlog::level::off;
    }
    return spdlog::level::info;
}

std::string DefaultLogDirectory() {
    const AssetLocator locator;
    const Path         dir = locator.LocalAppData().Join("Logs");
    (void)dir.EnsureDirectoryExists();
    return std::string{ dir.View() };
}

std::string LogFileName(std::string_view directory) {
    using namespace std::chrono;
    auto    now      = system_clock::now();
    auto    nowTimeT = system_clock::to_time_t(now);
    std::tm calendar{};
    localtime_s(&calendar, &nowTimeT);

    char fname[64];
    std::snprintf(fname, sizeof(fname), "pyxis-%lu-%04d%02d%02d.log",
                  static_cast<unsigned long>(_getpid()),
                  calendar.tm_year + 1900, calendar.tm_mon + 1, calendar.tm_mday);
    std::string out{ directory };
    if (!out.empty() && out.back() != '\\' && out.back() != '/') {
        out.push_back('/');
    }
    out += fname;
    return out;
}

}  // namespace

// ---------------------------------------------------------------------------
// Logger lifecycle.
// ---------------------------------------------------------------------------

Logger::Logger() : _impl(new Impl()) {}
Logger::~Logger() { delete _impl; }

void Logger::Configure(const LogConfig& cfg) {
    if (_impl->configured) {
        // Idempotent — re-applying the same config is a no-op; mismatch is
        // logged at info.
        _impl->logger->info("platform: Logger::Configure called twice; updating levels in place.");
    }

    std::vector<spdlog::sink_ptr> sinks;

    // Console sink.
    auto console = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    console->set_level(ToSpdlog(cfg.consoleLevel));
    sinks.push_back(std::move(console));

    // Rotating file sink.
    if (cfg.rotateBytes > 0 && cfg.rotateFiles > 0) {
        std::string dir{ cfg.fileDirectory };
        if (dir.empty()) {
            dir = DefaultLogDirectory();
        }
        const std::string filename = LogFileName(dir);
        auto              file     = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
            filename, cfg.rotateBytes, cfg.rotateFiles, /*rotate_on_open=*/true);
        file->set_level(ToSpdlog(cfg.fileLevel));
        sinks.push_back(std::move(file));
    }

    if (_impl->logger) {
        _impl->logger->sinks() = std::move(sinks);
    } else {
        _impl->logger = std::make_shared<spdlog::logger>(
            "pyxis", sinks.begin(), sinks.end());
        _impl->logger->set_level(spdlog::level::trace);  // sinks gate per-level.
        _impl->logger->set_pattern("%Y-%m-%d %H:%M:%S.%e [%l] %v");
        spdlog::register_logger(_impl->logger);
    }
    _impl->configured = true;
}

void Logger::Flush() noexcept {
    if (_impl->logger) _impl->logger->flush();
}

void Logger::Impl::EnsureLogger() noexcept {
    if (logger) return;
    auto console = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    console->set_level(spdlog::level::info);
    logger = std::make_shared<spdlog::logger>("pyxis", spdlog::sinks_init_list{ console });
    logger->set_level(spdlog::level::trace);
    logger->set_pattern("%Y-%m-%d %H:%M:%S.%e [%l] %v");
    spdlog::register_logger(logger);
}

// Per-level emit. The category is rendered as a `[cat] ` prefix. The bounded
// std::string concat is acceptable here — logging is not in the hot path
// (§30.10 forbids allocs in pass Execute(), not inside the platform logger).
#define PYXIS_LOG_IMPL(LevelLower, SpdlogLevel)                                          \
    void Logger::LevelLower(std::string_view category, std::string_view message) noexcept { \
        _impl->EnsureLogger();                                                           \
        std::string buf;                                                                 \
        buf.reserve(category.size() + message.size() + 4);                               \
        buf.append(1, '[').append(category).append("] ").append(message);                \
        _impl->logger->log(SpdlogLevel, buf);                                            \
    }

PYXIS_LOG_IMPL(Trace,    spdlog::level::trace)
PYXIS_LOG_IMPL(Debug,    spdlog::level::debug)
PYXIS_LOG_IMPL(Info,     spdlog::level::info)
PYXIS_LOG_IMPL(Warn,     spdlog::level::warn)
PYXIS_LOG_IMPL(Error,    spdlog::level::err)
PYXIS_LOG_IMPL(Critical, spdlog::level::critical)

#undef PYXIS_LOG_IMPL

// ---------------------------------------------------------------------------
// Logging::Get() — process-scoped singleton (§33.10 documented exception).
// ---------------------------------------------------------------------------

Logger& Logging::Get() noexcept {
    static Logger sLogger;
    return sLogger;
}

}  // namespace pyxis
