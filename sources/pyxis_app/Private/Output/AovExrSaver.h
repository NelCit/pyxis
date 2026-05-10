// Pyxis app — AOV-to-EXR saver.
//
// Plan §35. Synchronous: dispatches a TextureReadback against the
// supplied AOV texture, maps the staging buffer, repacks the per-AOV
// native format (RGBA16_FLOAT / RGBA32_FLOAT / R32_FLOAT / R32_UINT)
// into the interleaved RGBA float32 layout WriteExrRgba32f wants,
// and writes the EXR. Same per-format conversion table the
// viewer's "Save current AOV..." button uses; lives here so the
// headless `--save-aov` path doesn't need to duplicate it.
//
// Stalls the caller (waits the GPU idle to retire the readback copy)
// — that's intentional for a one-click human action / a one-shot
// headless write. Not for the per-frame hot path.

#pragma once

#include <expected>
#include <string>
#include <string_view>

namespace nvrhi {
class ICommandList;
class IDevice;
class ITexture;
}  // namespace nvrhi

namespace pyxis::app {

// Save `aov` to `filePath` as a 4-channel float32 EXR. `commandList`
// must NOT be open on entry; this function opens it, records the
// copy-to-staging, closes + executes + waitForIdle, maps the staging,
// converts to RGBA32F, and calls WriteExrRgba32f. `debugName` is the
// label propagated to the staging texture for tooling visibility.
//
// Returns the unexpected branch on any failure (null inputs, read-
// back creation, map, EXR write, unsupported source format).
[[nodiscard]] std::expected<void, std::string> SaveAovAsExr(
    nvrhi::IDevice* device,
    nvrhi::ICommandList* commandList,
    nvrhi::ITexture* aov,
    std::string_view debugName,
    std::string_view filePath) noexcept;

}  // namespace pyxis::app
