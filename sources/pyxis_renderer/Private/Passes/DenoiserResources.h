// Pyxis renderer — DenoiserResources (RTX-alignment design,
// rtx-realtime-alignment-design.md, Phase B denoiser chain).
//
// The ReLAX-class temporal accumulation's cross-FRAME ping-ponged history:
// two full sets of {diffuse, specular, normal+viewZ} textures, index-
// swapped every frame via Advance(). Owned exclusively by
// DenoiseTemporalPass (composition, not shared with any other pass) —
// pulled into its own small class per the design doc's "History textures
// ping-pong owned by a small DenoiserResources helper" instruction, kept
// separate from DenoiseTemporalPass itself for the same
// single-responsibility reason SceneBindings is its own class rather than
// living inside a specific pass.
//
// Layout, per index:
//   diffuse[i]     RGBA16F — {indirect-diffuse rgb, historyLength}
//   specular[i]    RGBA16F — {reflections rgb, historyLength}
//   normalViewZ[i] RGBA16F — {world normal xyz, viewZ} — snapshot of the
//     G-buffer guide at the time that index was written, read back next
//     frame as the disocclusion test's "previous" normal/depth (plan
//     §"denoise_temporal.slang" thresholds: viewZ plane-distance 0.003 +
//     normal dot 0.5).
//
// Ping-pong is REQUIRED (not a style choice): the temporal pass reads the
// previous frame's data at a REPROJECTED (generally different) pixel
// location while writing this frame's data at the CURRENT pixel location,
// in the same dispatch — a single shared buffer would race (thread A's
// read of a neighbor pixel racing thread B's write to that same pixel,
// order undefined within a dispatch).

#pragma once

#include <nvrhi/nvrhi.h>

#include <array>
#include <cstdint>

namespace pyxis {

class DenoiserResources {
 public:
  explicit DenoiserResources(nvrhi::IDevice* device) noexcept : _device(device) {}
  ~DenoiserResources() = default;
  DenoiserResources(const DenoiserResources&) = delete;
  DenoiserResources& operator=(const DenoiserResources&) = delete;

  // (Re)creates both ping-pong sets at width x height when the size
  // changed (or on first call). Resets HasHistory() to false — the
  // freshly (re)allocated textures hold no meaningful previous-frame data.
  // Returns true iff both sets exist and are the requested size (whether
  // freshly created or already current); false on a createTexture
  // failure (logged), in which case the PREVIOUS good allocation (if any)
  // is left untouched so the caller can keep running at the old size
  // rather than crash on null textures.
  [[nodiscard]] bool Ensure(uint32_t width, uint32_t height);

  [[nodiscard]] nvrhi::ITexture* PrevDiffuse() const noexcept {
    return _diffuse[_prevIndex].Get();
  }
  [[nodiscard]] nvrhi::ITexture* CurrDiffuse() const noexcept {
    return _diffuse[_currIndex].Get();
  }
  [[nodiscard]] nvrhi::ITexture* PrevSpecular() const noexcept {
    return _specular[_prevIndex].Get();
  }
  [[nodiscard]] nvrhi::ITexture* CurrSpecular() const noexcept {
    return _specular[_currIndex].Get();
  }
  [[nodiscard]] nvrhi::ITexture* PrevNormalViewZ() const noexcept {
    return _normalViewZ[_prevIndex].Get();
  }
  [[nodiscard]] nvrhi::ITexture* CurrNormalViewZ() const noexcept {
    return _normalViewZ[_currIndex].Get();
  }

  // Swap prev/curr for the NEXT frame — call once per frame, AFTER
  // DenoiseTemporalPass::Execute has finished writing into the CurrX()
  // textures. Also flips HasHistory() to true (a no-op if already true).
  void Advance() noexcept {
    _prevIndex ^= 1u;
    _currIndex ^= 1u;
    _hasHistory = true;
  }

  // False on the first Execute() after construction or a resize (the
  // ping-pong buffers hold no previous-frame data yet — DenoiseTemporalPass
  // must not blend against them); true from the first Advance() onward.
  [[nodiscard]] bool HasHistory() const noexcept { return _hasHistory; }

  [[nodiscard]] uint32_t Width() const noexcept { return _width; }
  [[nodiscard]] uint32_t Height() const noexcept { return _height; }

 private:
  nvrhi::IDevice* _device = nullptr;
  std::array<nvrhi::TextureHandle, 2> _diffuse;
  std::array<nvrhi::TextureHandle, 2> _specular;
  std::array<nvrhi::TextureHandle, 2> _normalViewZ;
  uint32_t _width = 0;
  uint32_t _height = 0;
  uint32_t _prevIndex = 0;
  uint32_t _currIndex = 1;
  bool _hasHistory = false;
};

}  // namespace pyxis
