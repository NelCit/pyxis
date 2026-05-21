// Pyxis renderer — public camera descriptor.
//
// Plan §18.4. Input to GpuScene::SetCamera. Both matrices are
// row-major + column-vector (§10): `posView = mul(viewFromWorld, posWorld)`,
// `posClip = mul(projFromView, posView)`.
//
// `apertureFStop = 0.0f` is the pinhole sentinel — DOF stays off (§43.3
// reserves the runtime path for M9+). `focusDistance` is in scene
// units (meters by default). `nearClip` / `farClip` follow the usual
// reverse-Z-friendly convention: keep `near` small and positive.

#pragma once

#include <Pyxis/Renderer/RendererApi.h>

#include <hlsl++.h>

namespace pyxis {

struct CameraDesc {
  hlslpp::float4x4 viewFromWorld{};
  hlslpp::float4x4 projFromView{};
  float focalLengthMm = 35.0f;
  float apertureFStop = 0.0f;  // 0 = pinhole
  float focusDistance = 1.0f;
  float nearClip = 0.01f;
  float farClip = 10000.0f;

  // Photographic exposure adjustment in STOPS. Multiplies scene
  // radiance by 2^exposure before the tonemap fires. Negative
  // darkens, positive brightens. Mirrors UsdGeomCamera's `exposure`
  // attribute. Lives on the camera (not the light) because it's a
  // film-sensitivity / display-space concept, not a radiometric
  // emission scale — the light's `inputs:exposure` adjusts the
  // EMITTED radiance separately. 0.0 = ×1 = no adjustment.
  float exposure = 0.0f;

  // V2.A.20 — projection mode. 0 = perspective (the v1 default;
  // raygen builds rays from camera origin through clip-space NDC),
  // 1 = orthographic (raygen treats clip-space xy as the per-pixel
  // ray origin in view space + uses (0,0,-1) as the view-space ray
  // direction). Mirrors UsdGeomCamera.projection. Consumes one of
  // the `_reserved` floats by reinterpreting it as uint32 — float
  // + uint32 share alignment (4 bytes) so the layout stays
  // byte-stable per §22.3.
  uint32_t projectionMode = 0u;  // 0 = perspective, 1 = orthographic

  // §22.3 reserved padding. §43.2 reserves shutterOpen / shutterClose
  // for motion blur (M11), §43.3 reserves room for DoF tuning. Naming
  // is §22.3 / §43 convention; see OpenPBRMaterialDesc.h for the
  // §30.2 NOLINT rationale. Two slots consumed by `exposure` +
  // `projectionMode`; six remain.
  // NOLINTNEXTLINE(readability-identifier-naming)
  float _reserved[6] = {};
};

}  // namespace pyxis
