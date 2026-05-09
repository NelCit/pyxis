// Pyxis renderer — RenderTargets POD.
// Plan §18.4 / §19.8. M1 ships only `color`; the AOV slots
// (depth/normal/albedo/motionVector/materialId/instanceId) come online
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
  InstanceId = 1u << 6,
};

struct RenderTargets {
  // NVRHI texture refs supplied by the caller (typically the
  // device-manager's current backbuffer). The renderer never allocates
  // these. Color is required; everything else is optional and gated
  // on the matching RenderSettings::enabledAovs bit (§19.8).
  nvrhi::ITexture* color = nullptr;  // required (RGBA16F or swapchain-format)
  nvrhi::ITexture* depth = nullptr;
  nvrhi::ITexture* normal = nullptr;
  nvrhi::ITexture* albedo = nullptr;
  nvrhi::ITexture* motionVector = nullptr;
  nvrhi::ITexture* materialId = nullptr;
  nvrhi::ITexture* instanceId = nullptr;

  // M7 follow-up — extra targets the AOV-inspector path writes.
  // These complement `color`: the BGRA8 `color` carries the post-
  // tonemap display output (whichever AOV the inspector picked) and
  // these carry the RAW per-AOV data so the picker / save-EXR can
  // pull untransformed values. All are caller-allocated and live
  // alongside `color` in AovTextures (pyxis_app private).
  //   colorHdr   : RGBA16F pre-tonemap radiance
  //   normalAov  : RGBA16F world-space normal (xyz, w unused)
  //   depthAov   : R32F    primary-ray hit distance (0 on miss)
  //   instanceIdAov : R32_UINT InstanceID() at hit (~0u on miss)
  // Required for v1 viewer; passes can no-op if any are null.
  nvrhi::ITexture* colorHdr = nullptr;
  nvrhi::ITexture* normalAov = nullptr;
  nvrhi::ITexture* depthAov = nullptr;
  nvrhi::ITexture* instanceIdAov = nullptr;

  // 1-element RWStructuredBuffer<PickResult> the raygen writes when
  // the dispatched pixel matches RenderSettings::mousePixel{X,Y}.
  // Caller-owned; PathTracePass copies this into a staging buffer
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
