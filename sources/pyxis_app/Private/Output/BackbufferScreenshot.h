// Pyxis app — viewer screenshot helper.
//
// Capture the current backbuffer to PNG via TextureReadback +
// stb_image_write. Used by the M1 --screenshot CLI path. Synchronous:
// closes/executes the supplied open command list, waitForIdles, maps,
// swizzles BGRA8 -> RGBA8, writes PNG. The caller must have rendered
// into the backbuffer and left it in nvrhi::ResourceStates::RenderTarget
// (or any state copyTexture can transition from); the helper returns
// the backbuffer to ResourceStates::Present so the next swapchain
// acquire sees a well-defined layout.
//
// Split out of ViewerMode.cpp during the file-size audit — the helper
// is self-contained (only depends on TextureReadback + stb_image_write
// + spdlog) so it lives next to its peers in Output/.

#pragma once

#include <string_view>

namespace nvrhi {
class ICommandList;
class IDevice;
class ITexture;
}  // namespace nvrhi

namespace pyxis::app {

// Returns true on success; false on any failure (null inputs, empty
// path, readback / map / write failure). Logs the success line + the
// "all-black" sanity warning on its own.
[[nodiscard]] bool CaptureBackbufferToPng(nvrhi::IDevice* device,
                                          nvrhi::ICommandList* commandList,
                                          nvrhi::ITexture* backbuffer,
                                          std::string_view pngPath) noexcept;

}  // namespace pyxis::app
