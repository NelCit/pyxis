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
//   materialId / primId at M6 alongside instance AOVs
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
  nvrhi::TextureHandle color;          // BGRA8_UNORM display target

  // M7 follow-up — AOV inspector + picker raw outputs. The raygen
  // writes all four every frame from the same TraceRay payload so
  // the inspector / Save EXR / pick buffer can read RAW data
  // without a re-trace. Formats are storage-capable per Vulkan's
  // VkFormatFeatureFlagBits VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT:
  //   colorHdr      RGBA16_FLOAT (pre-tonemap radiance)
  //   normal        RGBA16_FLOAT (world normal in xyz, w unused)
  //   depth         R32_FLOAT    (primary-ray distance, 0 on miss)
  //   primId        R32_UINT     per-instance slot — Hydra's HdAovTokens->primId
  //                              (~0u on miss; the per-FACE id is `elementId`
  //                              below in the Tier 1 batch)
  nvrhi::TextureHandle colorHdr;
  nvrhi::TextureHandle normal;
  nvrhi::TextureHandle depth;
  nvrhi::TextureHandle primId;
  // Second AOV batch (M7 follow-up).
  //   materialId   R32_UINT     material slot (~0u on miss)
  //   baseColor    RGBA16_FLOAT raw OpenPBR baseColor pre-shading
  //   worldPos     RGBA32_FLOAT world-space hit position (precision)
  nvrhi::TextureHandle materialId;
  nvrhi::TextureHandle baseColor;
  nvrhi::TextureHandle worldPos;
  // Tier 1 Hydra-canonical AOVs (every DCC delegate queries them).
  //   alpha       R8_UNORM    1.0 on hit, 0.0 on miss (binary today;
  //                           future-proofed for transmission / coverage)
  //   elementId   R32_UINT    per-face id within a BLAS (~0u on miss)
  //   normalEye   RGBA16_FLOAT eye-space normal (Hydra's Neye)
  //   worldPosEye RGBA32_FLOAT eye-space hit position (Hydra's Peye)
  nvrhi::TextureHandle alpha;
  nvrhi::TextureHandle elementId;
  nvrhi::TextureHandle normalEye;
  nvrhi::TextureHandle worldPosEye;

  // 1-element RWStructuredBuffer<PickResult> + a host-readable
  // staging buffer for one-frame-stale CPU readback. PathTracePass
  // copies device→staging at the end of each Execute(); the viewer
  // maps the staging buffer the NEXT frame to read what the GPU
  // wrote (mapping the same frame would block on a fence).
  nvrhi::BufferHandle  pickResult;
  nvrhi::BufferHandle  pickResultStaging;

  uint32_t width = 0;
  uint32_t height = 0;

  // Allocate every owned resource (display color + 4 AOVs + pick
  // buffer pair). Returns the unexpected branch with a human-readable
  // reason on null device, zero dims, or any createTexture / Buffer
  // failure.
  [[nodiscard]] static std::expected<AovTextures, std::string> Create(nvrhi::IDevice* device,
                                                                      uint32_t width,
                                                                      uint32_t height) noexcept;
};

}  // namespace pyxis::app
