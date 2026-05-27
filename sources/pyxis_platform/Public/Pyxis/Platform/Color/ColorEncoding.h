// Pyxis Platform — color/half encoding helpers.
//
// Small, dependency-free inline conversions shared by the Hydra delegate's
// host-AOV composite, the headless harnesses, and anywhere a tonemapped color
// buffer is read back / written. Consolidates copies that previously lived in
// each consumer. Header-only (inline) so no link dependency.
//
// NOTE: the standalone app's PNG/EXR writers (pyxis_app) keep their own 8-bit
// LUT / writer-specific code — this header is the canonical float path.

#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>

namespace pyxis::color {

// IEEE half (uint16 bit pattern) -> float. Handles subnormals + inf/nan.
[[nodiscard]] inline float HalfToFloat(uint16_t half) noexcept {
  const uint32_t sign = (half >> 15) & 0x1u, exp = (half >> 10) & 0x1Fu, mant = half & 0x3FFu;
  uint32_t bits;
  if (exp == 0) {
    if (mant == 0) {
      bits = sign << 31;
    } else {
      int shift = -1;
      uint32_t norm = mant;
      do {
        ++shift;
        norm <<= 1;
      } while ((norm & 0x400u) == 0);
      bits = (sign << 31) | (static_cast<uint32_t>(127 - 15 - shift) << 23) | ((norm & 0x3FFu) << 13);
    }
  } else if (exp == 0x1F) {
    bits = (sign << 31) | (0xFFu << 23) | (mant << 13);
  } else {
    bits = (sign << 31) | ((exp - 15 + 127) << 23) | (mant << 13);
  }
  float out;
  std::memcpy(&out, &bits, sizeof(out));
  return out;
}

// float -> IEEE half bit pattern. Truncating (round-to-nearest-even not needed
// for display-color output); flushes underflow to +/-0, overflow to inf.
[[nodiscard]] inline uint16_t FloatToHalf(float value) noexcept {
  uint32_t bits;
  std::memcpy(&bits, &value, sizeof(bits));
  const uint32_t sign = (bits >> 16) & 0x8000u;
  const int32_t expo = static_cast<int32_t>((bits >> 23) & 0xFFu) - 127 + 15;
  const uint32_t mant = bits & 0x7FFFFFu;
  if (expo <= 0)
    return static_cast<uint16_t>(sign);
  if (expo >= 0x1F)
    return static_cast<uint16_t>(sign | (0x1Fu << 10));
  return static_cast<uint16_t>(sign | (static_cast<uint32_t>(expo) << 10) | (mant >> 13));
}

// Standard sRGB OETF (linear display value in [0,1] -> sRGB-encoded [0,1]).
[[nodiscard]] inline float LinearToSrgb(float linear) noexcept {
  linear = linear < 0.0f ? 0.0f : (linear > 1.0f ? 1.0f : linear);
  return linear <= 0.0031308f ? linear * 12.92f
                              : 1.055f * std::pow(linear, 1.0f / 2.4f) - 0.055f;
}

// Standard sRGB EOTF (sRGB-encoded [0,1] -> linear [0,1]).
[[nodiscard]] inline float SrgbToLinear(float srgb) noexcept {
  srgb = srgb < 0.0f ? 0.0f : (srgb > 1.0f ? 1.0f : srgb);
  return srgb <= 0.04045f ? srgb / 12.92f : std::pow((srgb + 0.055f) / 1.055f, 2.4f);
}

}  // namespace pyxis::color
