// RFC 0004 — minimal stand-in for NVIDIA's UNSHIPPED carb/imaging/IImaging.h.
//
// Kit 110's public C++ SDK omits this header, yet omni.ui's ImageProvider.h
// (pulled by ImageWithProvider / DynamicTextureProvider) includes it. We supply
// the two symbols ImageProvider.h actually needs:
//
//   * carb::imaging::DisplayWindowRect — a BY-VALUE member of ImageProvider
//     (m_imageDisplayWindow), so its size/layout MUST match the one omni.ui.dll
//     was compiled with. It is a normalized display-window rectangle; the
//     header's own initializer `{ 0.0f, 0.0f, 1.0f, 1.0f }` proves it is exactly
//     four floats. We never read or write this member — it exists only so the
//     surrounding members keep their correct offsets (ABI parity).
//   * carb::imaging::IMetadata — used only through a pointer parameter, so a
//     forward declaration is sufficient and carries zero ABI risk.
//
// If a future Kit ships the real header, drop this stub from the include path.

#pragma once

#include <cstdint>

namespace carb
{
namespace imaging
{

// 4 floats — matches ImageProvider.h's `{0,0,1,1}` initializer. Field names are
// irrelevant to ABI; only sizeof/alignof (16 / 4) must match.
struct DisplayWindowRect
{
    float left = 0.0f;
    float top = 0.0f;
    float right = 1.0f;
    float bottom = 1.0f;
};

// Pointer-only in ImageProvider.h — forward declaration is enough.
class IMetadata;

} // namespace imaging
} // namespace carb
