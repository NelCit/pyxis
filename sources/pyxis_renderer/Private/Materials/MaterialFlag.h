// Pyxis renderer — MaterialFlag bitmask (§11.6).
//
// Mirrored in resources/shaders/ShaderInterop.slang so the
// closesthit's branchless dispatch reads the same bits the C++
// MaterialTable writes. Each bit corresponds to one OpenPBR lobe or
// texture-presence signal the closesthit can short-circuit on
// without a follow-up handle-vs-Invalid check.

#pragma once

#include <cstdint>

namespace pyxis {

enum class MaterialFlag : uint32_t {
  None                = 0,
  DoubleSided         = 1u << 0,
  HasBaseColorMap     = 1u << 1,
  HasNormalMap        = 1u << 2,
  HasMetallicMap      = 1u << 3,
  HasRoughnessMap     = 1u << 4,
  HasEmissionMap      = 1u << 5,
  HasOpacityMap       = 1u << 6,
  HasTransmissionMap  = 1u << 7,
  HasCoatRoughnessMap = 1u << 8,
  AlphaTested         = 1u << 9,   // opacityThreshold > 0
  CoatEnabled         = 1u << 10,  // coatWeight > 0
  TransmissionEnabled = 1u << 11,  // transmissionWeight > 0
  Emissive            = 1u << 12,  // emissionLuminance > 0 OR emissionMap valid
};

constexpr uint32_t operator|(MaterialFlag lhs, MaterialFlag rhs) noexcept {
  return static_cast<uint32_t>(lhs) | static_cast<uint32_t>(rhs);
}
constexpr uint32_t operator|(uint32_t lhs, MaterialFlag rhs) noexcept {
  return lhs | static_cast<uint32_t>(rhs);
}
constexpr uint32_t operator&(uint32_t lhs, MaterialFlag rhs) noexcept {
  return lhs & static_cast<uint32_t>(rhs);
}

}  // namespace pyxis
