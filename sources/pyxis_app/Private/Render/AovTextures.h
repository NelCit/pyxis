// Pyxis app — caller-side AOV texture ownership.
//
// Plan §18.4 / §19.8: `RenderTargets` is caller-allocated. "NVRHI
// texture refs supplied by the caller (Hydra Bprims, swapchain
// target, or headless writer). Renderer never allocates these." This
// file is the pyxis-driven home for the three modes pyxis.exe owns:
// viewer, headless, and the USD-direct ingest path. The Hydra render
// delegate owns its AOV textures separately as Bprims (mandated by
// the Hydra delegate API).
//
// Shape: a public-fields POD that mirrors `RenderTargets`. Each AOV
// slot is one TextureHandle so callers can populate
// `RenderTargets{ .color = aovs.color.Get(), .depth = ... }` without
// going through accessors. M2 only allocates `color`; the other six
// slots from §18.4 are reserved here as commented-out future fields
// and land alongside their respective passes:
//   depth/normal/albedo at M5 (UsdPreviewSurface→OpenPBR)
//   motionVector       at M11 polish
//   materialId / instanceId at M6 alongside instance AOVs
//
// Format choice: SBGRA8_UNORM matches both the viewer swapchain and
// the §33.7 byte-identical EXR contract that M2 ships today. The
// public RenderTargets descriptor pins `color` at RGBA16F as the
// long-term target; promotion lands at M5 alongside the path tracer
// (where HDR fidelity actually matters and tinyexr's float scanlines
// stop being a wasted promotion).
//
// Lifetime: created on demand by the caller (HeadlessMode at startup,
// ViewerMode on swapchain rebuild). Destroyed strictly before the
// device manager so NVRHI's deferred-destruction queue can drain
// against the still-live VkDevice — the standard NVRHI lifetime
// discipline. The TextureHandle (RefCountPtr) fields are reference-
// counted, so callers can take a borrowed pointer via `.Get()` and
// hand it to RenderTargets without lifetime concerns as long as the
// owning AovTextures outlives the RenderFrame call.

#pragma once

#include <nvrhi/nvrhi.h>

#include <cstdint>
#include <expected>
#include <string>

namespace pyxis::app {

struct AovTextures {
    // §18.4 slots.
    nvrhi::TextureHandle color;
    // M5+ — uncomment + populate in Create() alongside their consumer
    // passes. Kept here so the §18.4 slot list is visible at the point
    // a future contributor goes looking for it:
    //   nvrhi::TextureHandle depth;          // M5  R32F
    //   nvrhi::TextureHandle normal;         // M5  RGB16F
    //   nvrhi::TextureHandle albedo;         // M5  RGBA16F
    //   nvrhi::TextureHandle motionVector;   // M11 RG16F
    //   nvrhi::TextureHandle materialId;     // M6  R32_UINT
    //   nvrhi::TextureHandle instanceId;     // M6  R32_UINT

    uint32_t width  = 0;
    uint32_t height = 0;

    // Allocate the M2 active set (color only). Returns the unexpected
    // branch with a human-readable reason on null device, zero dims, or
    // an NVRHI createTexture failure.
    [[nodiscard]] static std::expected<AovTextures, std::string>
    Create(nvrhi::IDevice* device, uint32_t width, uint32_t height) noexcept;
};

}  // namespace pyxis::app
