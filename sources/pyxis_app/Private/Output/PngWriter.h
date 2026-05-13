// Pyxis app — PNG writer (stb_image_write).
//
// Sibling of ExrWriter for the golden-test path. PNG is 8-bit per
// channel + lossless + universally viewable in standard image
// viewers + diff-friendly on GitHub PRs. The trade against EXR is
// loss of sub-LSB precision — fine for the v1 visual-regression
// suite (a path-trace BSDF drift that tonemaps to the same 8-bit
// pixel doesn't matter for "did the picture change").
//
// Determinism: stb_image_write's deflate is deterministic for a
// pinned compression level + filter. We pin both at first call.
// Same RGBA input → same PNG bytes across runs — that's the
// §33.7 byte-identical contract restated for 8-bit output.
//
// Colourspace: the renderer's headless RT is BGRA8_UNORM (linear).
// We apply a linear→sRGB encode (256-entry LUT) before writing so
// the resulting PNG renders correctly in any standard viewer.
// Same transform applied every run → same bytes.

#pragma once

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>

namespace pyxis::app {

// Write a BGRA8_UNORM (linear) buffer to <path> as a deterministic
// sRGB-encoded RGBA PNG. Source is `width * height` pixels with the
// row stride supplied separately (NVRHI's mapStagingTexture returns
// this).
//
// Returns the unexpected branch with a human-readable message on
// any failure (path-create, channel-conversion, stb write).
[[nodiscard]] std::expected<void, std::string> WritePngBgra8(std::string_view filePath,
                                                             uint32_t width, uint32_t height,
                                                             const void* bgra8Pixels,
                                                             std::size_t rowPitchBytes) noexcept;

}  // namespace pyxis::app
