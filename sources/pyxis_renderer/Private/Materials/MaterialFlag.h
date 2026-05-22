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
  // V2.A.24 — normal-map tangent-handedness flips (OmniPBR
  // `flip_tangent_u` / `flip_tangent_v`). The DirectX-vs-OpenGL normal
  // convention: Omniverse content commonly authors flip_tangent_v=1
  // (green channel points down), so without flipping, bump lighting
  // is inverted on the V axis. Closesthit negates the matching
  // tangent-space normal component when set.
  FlipTangentU        = 1u << 13,
  FlipTangentV        = 1u << 14,
  // V2.A.24 — a non-identity UV transform (scale / rotate / translate)
  // is authored; closesthit applies it to the interpolated UV before
  // every texture sample. Gates the transform math so identity-UV
  // materials pay nothing.
  HasUvTransform      = 1u << 15,
  // RFC 0005 — planar projection: derive UV from the hit position with the
  // texture U axis along up, so directional detail reads consistently
  // regardless of the asset's per-mesh unwrap. WorldProjection projects in
  // world space; ObjectProjection transforms the hit by WorldToObject first
  // so the projection follows the instance (OmniPBR world_or_object=1). The
  // C++ flag-packing site sets exactly one from `projectionMode`.
  WorldProjection     = 1u << 16,
  ObjectProjection    = 1u << 17,
};

constexpr uint32_t operator|(MaterialFlag lhs, MaterialFlag rhs) noexcept {
  return static_cast<uint32_t>(lhs) | static_cast<uint32_t>(rhs);
}
constexpr uint32_t operator|(uint32_t lhs, MaterialFlag rhs) noexcept {
  return lhs | static_cast<uint32_t>(rhs);
}
constexpr uint32_t& operator|=(uint32_t& lhs, MaterialFlag rhs) noexcept {
  lhs = lhs | static_cast<uint32_t>(rhs);
  return lhs;
}
constexpr uint32_t operator&(uint32_t lhs, MaterialFlag rhs) noexcept {
  return lhs & static_cast<uint32_t>(rhs);
}

}  // namespace pyxis
