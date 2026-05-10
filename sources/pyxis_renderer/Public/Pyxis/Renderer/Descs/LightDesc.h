// Pyxis renderer — public light descriptor.
//
// Plan §18.4. Input to GpuScene::AddLight / UpdateLight.
//
// Three light kinds in v1 (§7 / §11): Distant, Dome, Rect. Fields
// not relevant to the chosen kind are ignored — e.g. `direction` only
// drives Distant, `envMap` only drives Dome, `position` / `axisU` /
// `axisV` / `doubleSided` only drive Rect.
//
// M3 ships the Distant kind only (one hardcoded sun for the path-
// trace box); Dome lands at M7 with the dome importance-sampling
// path and Rect joins it. The byte-frozen layout is locked here so
// adding M7 features is additive — never change order, never narrow
// types.

#pragma once

#include <Pyxis/Renderer/Forward.h>
#include <Pyxis/Renderer/RendererApi.h>

#include <cstdint>
#include <hlsl++.h>

namespace pyxis {

struct LightDesc {
  enum class Kind : uint8_t {
    Distant,
    Dome,
    Rect,
    // §22 MINOR-additive enum-tail expansion. Loaded by the M8a
    // UsdLux ingest pass so the data survives the ingest → GpuScene
    // boundary; the M7-simple closesthit treats them as inert
    // (PackLightGpu zeroes intensity for non-renderable kinds).
    Cylinder,    // UsdLuxCylinderLight — fluorescent tube
    Geometry,    // UsdLuxGeometryLight — emission bound to a mesh
    Portal,      // UsdLuxPortalLight — dome-sampling variance hint
  };

  // UsdLuxDomeLight texture mapping. Most production scenes ship
  // LatLong (equirectangular); MirroredBall / Angular are legacy
  // formats but USD authorises them. New public type — see version
  // notes for the §22 MINOR-version bump where it landed.
  enum class DomeFormat : uint8_t {
    LatLong      = 0,
    MirroredBall = 1,
    Angular      = 2,
  };

  // ========================================================================
  // ORIGINAL FIELDS — byte-frozen layout (§18.4). Order, type, and
  // alignment of every field down to the `_reserved` tail are part of
  // the public ABI contract: never reorder, never narrow, never insert
  // between. New fields go in the §22.3 trailing block below and
  // consume slots out of `_reserved`.
  // ========================================================================
  Kind kind = Kind::Distant;
  hlslpp::float3 color = {1.0f, 1.0f, 1.0f};
  float intensity = 1.0f;
  hlslpp::float3 direction = {0.0f, -1.0f, 0.0f};
  TextureHandle envMap = TextureHandle::Invalid;
  hlslpp::float3 position = {0.0f, 0.0f, 0.0f};
  hlslpp::float3 axisU = {1.0f, 0.0f, 0.0f};
  hlslpp::float3 axisV = {0.0f, 1.0f, 0.0f};
  bool doubleSided = false;

  // ========================================================================
  // §22.3 MINOR-additive trailing block. Every field below was
  // appended after v0.0.1 and consumed slots out of the original
  // `_reserved[8]` budget. Order WITHIN this block is also part of
  // the contract once published — newest fields go LAST. The
  // remaining `_reserved[N]` slot count tells you how many MINOR
  // additions are still budgeted before a MAJOR break.
  // ========================================================================

  // M8a — `UsdLuxDomeLight::texture:format` (latlong / mirroredBall /
  // angular). Closesthit's miss shader currently assumes lat-long;
  // M9 polish wires the other UV mappings.
  DomeFormat domeFormat = DomeFormat::LatLong;
  // M8a — `UsdLuxLightAPI::normalize`. When true, intensity was
  // authored as TOTAL emitted power; load path skips the per-area
  // multiply that the default normalize=false path applies. Stored
  // for diagnostics + the M9 area-resampling pass.
  bool normalize = false;
  // M8a — `UsdLuxLightAPI::diffuse` / `specular` per-contribution
  // multipliers. Default 1.0 = no-op. M7-simple ignores both; the
  // M9 BSDF split picks them up on the diffuse / specular lobes.
  float diffuse = 1.0f;
  float specular = 1.0f;
  // M8a — `UsdLuxDistantLight::angle`. Sun apparent angular diameter
  // in degrees (default 0.53° = real sun). Drives shadow softness
  // in M7-full's shadow-ray pass; M7-simple ignores.
  float distantAngle = 0.53f;
  // M8a — `UsdLuxShapingAPI` cone + focus + focusTint. shapingConeAngle
  // < 90° turns any light into a spotlight; softness is a 0..1
  // cosine-falloff width. Stored for M9's spot pass; M7-simple
  // ignores all four.
  float shapingConeAngle = 90.0f;
  float shapingConeSoftness = 0.0f;
  float shapingFocus = 0.0f;
  hlslpp::float3 shapingFocusTint = {1.0f, 1.0f, 1.0f};

  // §22.3 reserved padding. Started at `_reserved[8]` in v0.0.1; the
  // M8a additions above consumed 6 uint32-equivalent slots
  // (DomeFormat + normalize packs into 1, diffuse/specular/
  // distantAngle/shapingConeAngle/shapingConeSoftness/shapingFocus =
  // 6 floats, shapingFocusTint = 16-byte hlslpp::float3 = 4 slots).
  // §43.1 reserves a `lightLinkSet` slot for the M9 light-linking
  // pass; remaining capacity is for IES-profile handles, geometry-
  // light mesh refs, and portal-volume extents. Naming is §22.3 /
  // §43 convention; see OpenPBRMaterialDesc.h for the §30.2 NOLINT
  // rationale.
  // NOLINTNEXTLINE(readability-identifier-naming)
  uint32_t _reserved[4] = {};
};

}  // namespace pyxis
