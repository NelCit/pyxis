// Pyxis platform — logging category prefix table.
//
// Plan §31 / §34: a single, dotted-prefix scheme so a Tracy/spdlog capture
// stays readable across viewer/headless. Categories are *strings*, not
// enum values — the routing happens in spdlog by sink filter, not in the
// public API. Listing them here gives reviewers something concrete to
// check against.

#pragma once

namespace pyxis::log {

// Top-level component prefixes. New categories should always start with
// one of these, optionally with a sub-component (e.g. "render.tlas.build").
inline constexpr const char* INGEST_HYDRA = "ingest.hydra";
inline constexpr const char* INGEST_USD = "ingest.usd";
inline constexpr const char* INGEST_SHARED = "ingest.shared";
inline constexpr const char* ASSETS = "assets";
inline constexpr const char* RENDER = "render";
inline constexpr const char* APP = "app";
inline constexpr const char* PLATFORM = "platform";

}  // namespace pyxis::log
