// Pyxis renderer — shared camera-jitter sequence (plan §12.4).
//
// Extracted from SceneBindings.cpp (RTX-alignment design, Phase B) so DLSS
// Stage 2a's DlssPass can feed sl::Constants::jitterOffset the EXACT SAME
// per-frame pixel offset SceneBindings::Update uploads into the
// RtxSamplingUniforms cbuffer the raygen's BuildCameraRay reads — a
// divergence here (e.g. a second, slightly different formula) would mean
// Streamline reconstructs from a jitter offset that doesn't match what the
// G-buffer pass actually rendered with, corrupting the DLSS-SR result.
// One function, two consumers, cannot drift.

#pragma once

#include "ShaderInterop.slang"

#include <cstdint>

namespace pyxis {

namespace detail {

// Radical inverse in the given prime base — the core of the Halton
// low-discrepancy sequence. `index` is 1-based conventionally but 0-based
// works identically (both are just "some integer -> [0,1)" stream
// member); frameIndex is used directly.
inline float RadicalInverse(std::uint32_t index, std::uint32_t base) {
  float result = 0.0f;
  float invBase = 1.0f / static_cast<float>(base);
  float fraction = invBase;
  while (index > 0u) {
    result += static_cast<float>(index % base) * fraction;
    index /= base;
    fraction *= invBase;
  }
  return result;
}

}  // namespace detail

// Scrambled-Halton(2,3) camera jitter with a per-frame Cranley-Patterson
// rotation. Returns a sub-pixel offset in [-0.5, 0.5] PIXELS (the standard
// TAA/DLSS jitter amplitude — ProgrammingGuideDLSS.md 10.0: "Make sure
// that jitter offset values are in pixel space"). The "scramble" here is a
// fixed additive rotation derived from the default RNG seed (real
// per-seed variation is blocked on the same RenderSettings-freeze debt
// SceneBindings.cpp's DEFAULT_RNG_SEED originally documented) rather than
// a full digit-permutation scramble — deterministic, decorrelates the
// sequence's origin from frame 0 without needing a baked permutation
// table.
inline shaderinterop::float2 ComputeHaltonJitter(std::uint64_t frameIndex) {
  const auto index = static_cast<std::uint32_t>(frameIndex & 0xFFFFFFFFu) + 1u;
  float haltonX = detail::RadicalInverse(index, 2u);
  float haltonY = detail::RadicalInverse(index, 3u);
  // Cranley-Patterson rotation: shift by a fixed per-seed fraction, wrap
  // into [0, 1). std::fmod-free wrap since the shift is a compile-time
  // constant fraction in [0, 1).
  const float rotX = 0.171573f;  // derived from the default RNG seed via SplitMix-style mixing
  const float rotY = 0.837291f;
  haltonX += rotX;
  haltonY += rotY;
  haltonX -= static_cast<float>(static_cast<int>(haltonX));
  haltonY -= static_cast<float>(static_cast<int>(haltonY));
  return shaderinterop::float2{haltonX - 0.5f, haltonY - 0.5f};
}

}  // namespace pyxis
