// Pyxis renderer — RenderTargets POD.
// Plan §18.4 / §19.8. M1 ships only `color`; the AOV slots
// (depth/normal/albedo/motionVector/materialId/primId) come online
// at M5+.

#pragma once

#include <cstdint>

namespace nvrhi {
class ITexture;
class IBuffer;
}

namespace pyxis {

enum class AovFlag : uint32_t {
  None = 0,
  Color = 1u << 0,
  Depth = 1u << 1,
  Normal = 1u << 2,
  Albedo = 1u << 3,
  MotionVector = 1u << 4,
  MaterialId = 1u << 5,
  PrimId = 1u << 6,
};

struct RenderTargets {
  // NVRHI texture refs supplied by the caller (typically the
  // device-manager's current backbuffer). The renderer never allocates
  // these. Color is required; everything else is optional and gated
  // on the matching RenderSettings::enabledAovs bit (§19.8).
  nvrhi::ITexture* color = nullptr;  // required (RGBA16F or swapchain-format)
  // SSAA resolve destination (output resolution, UAV-capable). When
  // RenderSettings::ssaaFactor > 1, `color` is the super-res render
  // target and the renderer's SsaaResolvePass box-downsamples it into
  // this output-resolution texture. Null (or ssaaFactor == 1) = no
  // resolve; the caller consumes `color` directly. Caller-allocated.
  nvrhi::ITexture* colorResolved = nullptr;
  nvrhi::ITexture* depth = nullptr;
  nvrhi::ITexture* normal = nullptr;
  nvrhi::ITexture* albedo = nullptr;
  // RTX-alignment design (Phase A / WP1) — RaytracedLightingPass's
  // raygen now writes this: RG16F screen-space motion vector, in
  // PIXELS, of the primary hit (see viewZAov below for its denoiser-
  // guide sibling). Was declared but unwired since the M1-era draft
  // of this struct; still optional / caller-allocated like every AOV.
  nvrhi::ITexture* motionVector = nullptr;
  nvrhi::ITexture* materialId = nullptr;
  nvrhi::ITexture* primId = nullptr;

  // M7 follow-up — extra targets the AOV-inspector path writes.
  // These complement `color`: the BGRA8 `color` carries the post-
  // tonemap display output (whichever AOV the inspector picked) and
  // these carry the RAW per-AOV data so the picker / save-EXR can
  // pull untransformed values. All are caller-allocated and live
  // alongside `color` in AovTextures (pyxis_app private).
  //   colorHdr   : RGBA16F pre-tonemap radiance
  //   normalAov  : RGBA16F world-space normal (xyz, w unused)
  //   depthAov   : R32F    primary-ray hit distance (0 on miss)
  //   primIdAov  : R32_UINT InstanceID() at hit (~0u on miss) —
  //               Hydra's HdAovTokens->primId
  // Required for v1 viewer; passes can no-op if any are null.
  nvrhi::ITexture* colorHdr = nullptr;
  nvrhi::ITexture* normalAov = nullptr;
  nvrhi::ITexture* depthAov = nullptr;
  nvrhi::ITexture* primIdAov = nullptr;
  // Second AOV batch (M7 follow-up) — material id, baseColor,
  // world-position. Same caller-allocation contract; RaytracedLightingPass
  // binds 1×1 fallbacks when null.
  //   materialIdAov : R32_UINT (~0u on miss)
  //   baseColorAov  : RGBA16F  raw OpenPBR baseColor pre-shading
  //   worldPosAov   : RGBA32F  world-space hit point (precision)
  nvrhi::ITexture* materialIdAov = nullptr;
  nvrhi::ITexture* baseColorAov  = nullptr;
  nvrhi::ITexture* worldPosAov   = nullptr;
  // Tier 1 Hydra-canonical AOVs (every DCC delegate queries them).
  //   alphaAov       : R8_UNORM   1.0 on hit, 0.0 on miss
  //   elementIdAov   : R32_UINT   PrimitiveIndex() (per-face id, ~0u on miss)
  //   normalEyeAov   : RGBA16F    eye-space normal (Hydra's Neye)
  //   worldPosEyeAov : RGBA32F    eye-space hit position (Hydra's Peye)
  nvrhi::ITexture* alphaAov       = nullptr;
  nvrhi::ITexture* elementIdAov   = nullptr;
  nvrhi::ITexture* normalEyeAov   = nullptr;
  nvrhi::ITexture* worldPosEyeAov = nullptr;

  // RTX-alignment design (rtx-realtime-alignment-design.md), Phase A /
  // WP1 — denoiser-guide AOVs. New optional RenderTargets AOV slots
  // (§22.3 MINOR-additive). Caller-allocated + caller-owned like every
  // other AOV above; RaytracedLightingPass binds 1×1 fallbacks when null.
  //   viewZAov     : R32F  view-space Z of the primary hit — positive
  //                  distance along the camera's forward axis (0 on miss)
  //   motionVector : RG16F screen-space motion vector in PIXELS
  //                  (current pixel minus the previous frame's pixel
  //                  position of the same world point; (0,0) on miss
  //                  and on the first frame). This field has existed
  //                  since the M1-era draft of this struct but stayed
  //                  unwired until WP1.
  nvrhi::ITexture* viewZAov = nullptr;

  // 1-element RWStructuredBuffer<PickResult> the raygen writes when
  // the dispatched pixel matches RenderSettings::mousePixel{X,Y}.
  // Caller-owned; RaytracedLightingPass copies this into a staging buffer
  // each frame for one-frame-stale CPU readback. Null = no picker.
  nvrhi::IBuffer*  pickResult = nullptr;
  // CpuAccessMode::Read staging buffer the renderer copies pickResult
  // into at the end of each frame. The renderer maps this on the NEXT
  // call to read what the GPU wrote (one-frame stale). Null = no
  // readback (the picker still updates the device buffer for any
  // GPU-side consumer).
  nvrhi::IBuffer*  pickResultStaging = nullptr;
};

}  // namespace pyxis
