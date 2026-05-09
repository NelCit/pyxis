// Pyxis app — EXR writer (tinyexr).
//
// Plan §41 M2 image-output, §35 image-regression artefact. Headless's
// readback path hands us a host-mapped BGRA8 staging texture; this
// module swizzles + normalises to RGBA float and writes the final EXR
// via tinyexr's SaveEXR. M3+'s path-tracer already produces float
// data — when that lands the BGRA8 path becomes a fallback.
//
// Compression is tinyexr's default ZIP (deterministic across runs of
// the same binary, which is what §33.7 byte-identical EXR requires).
// `mkdir -p` of the parent dir mirrors §27 ("missing directories must
// never abort a 30-minute Moana render").

#pragma once

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>

namespace pyxis::app {

// Write a BGRA8 buffer (host-mapped staging texture) to <path> as a
// 4-channel float EXR. Source is `width * height` pixels with the row
// stride supplied separately (NVRHI's mapStagingTexture returns this).
//
// Returns the unexpected branch with a human-readable message on
// any failure (path-create, channel-conversion, tinyexr write).
[[nodiscard]] std::expected<void, std::string> WriteExrBgra8(std::string_view filePath,
                                                             uint32_t width, uint32_t height,
                                                             const void* bgra8Pixels,
                                                             std::size_t rowPitchBytes) noexcept;

}  // namespace pyxis::app
