// Pyxis app — OpenPBR feature-mask bits (app-local mirror).
//
// Q3 OpenPBR-complete (openpbr-complete-design.md "Control surface").
// App-local mirrors of the shaderinterop::OPENPBR_FEATURE_* bits that
// compose RenderSettings::openPbrFeatureMask. pyxis_app does not
// include ShaderInterop.slang (the dual-language interop header needs
// hlslpp, which the app doesn't link), so the six bits are restated
// here and pinned: the static_assert below ties the composed ALL to
// RenderSettings::openPbrFeatureMask's default, and PyxisRenderer.cpp
// ties THAT default to the interop constant — so a drift anywhere in
// the chain fails to compile.
//
// Consumers: ImGuiHost (editor checkboxes -> mask), CliArgs
// (--openpbr-mask clamp).

#pragma once

#include <Pyxis/Renderer/Descs/RenderSettings.h>

#include <cstdint>

namespace pyxis::app {

inline constexpr uint32_t OPENPBR_FEATURE_COAT         = 1u << 0;
inline constexpr uint32_t OPENPBR_FEATURE_FUZZ         = 1u << 1;
inline constexpr uint32_t OPENPBR_FEATURE_TRANSMISSION = 1u << 2;
inline constexpr uint32_t OPENPBR_FEATURE_SUBSURFACE   = 1u << 3;
inline constexpr uint32_t OPENPBR_FEATURE_ANISOTROPY   = 1u << 4;
inline constexpr uint32_t OPENPBR_FEATURE_EON_DIFFUSE  = 1u << 5;
inline constexpr uint32_t OPENPBR_FEATURES_ALL =
    OPENPBR_FEATURE_COAT | OPENPBR_FEATURE_FUZZ | OPENPBR_FEATURE_TRANSMISSION
    | OPENPBR_FEATURE_SUBSURFACE | OPENPBR_FEATURE_ANISOTROPY | OPENPBR_FEATURE_EON_DIFFUSE;

static_assert(OPENPBR_FEATURES_ALL == RenderSettings{}.openPbrFeatureMask,
              "App-side OPENPBR_FEATURE_* bits drifted from RenderSettings::openPbrFeatureMask's "
              "default — keep this mirror in lockstep with ShaderInterop.slang.");

}  // namespace pyxis::app
