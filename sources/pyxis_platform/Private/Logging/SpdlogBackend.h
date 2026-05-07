// Pyxis platform — spdlog-backed Logger implementation.
//
// This header is Private/, so spdlog stays out of the Public/ surface
// (plan §33.10).

#pragma once

#include <Pyxis/Platform/Logging/Log.h>

#include <spdlog/spdlog.h>

#include <memory>

namespace pyxis {

struct Logger::Impl {
    std::shared_ptr<spdlog::logger> logger;
    bool                            configured = false;
};

}  // namespace pyxis
