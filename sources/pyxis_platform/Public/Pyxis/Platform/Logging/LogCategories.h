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
inline constexpr const char* kIngestHydra    = "ingest.hydra";
inline constexpr const char* kIngestUsd      = "ingest.usd";
inline constexpr const char* kIngestShared   = "ingest.shared";
inline constexpr const char* kAssets         = "assets";
inline constexpr const char* kRender         = "render";
inline constexpr const char* kApp            = "app";
inline constexpr const char* kPlatform       = "platform";

}  // namespace pyxis::log
