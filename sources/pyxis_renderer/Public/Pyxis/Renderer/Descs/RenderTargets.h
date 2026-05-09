// Pyxis renderer — RenderTargets POD.
// Plan §18.4 / §19.8. M1 ships only `color`; the AOV slots
// (depth/normal/albedo/motionVector/materialId/instanceId) come online
// at M5+.

#pragma once

#include <cstdint>

namespace nvrhi {
class ITexture;
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
};

}  // namespace pyxis
