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

#include <hlsl++.h>

#include <cstdint>

namespace pyxis {

struct LightDesc {
    enum class Kind : uint8_t {
        Distant,
        Dome,
        Rect,
    };

    Kind             kind      = Kind::Distant;
    hlslpp::float3   color     = { 1.0f, 1.0f, 1.0f };
    float            intensity = 1.0f;

    // Distant — direction the light is travelling (i.e. away from
    // the surface), in world space. Normalised by the renderer.
    hlslpp::float3   direction = { 0.0f, -1.0f, 0.0f };

    // Dome — lat-long EXR fed through TextureCache. Invalid means a
    // procedural greyscale fallback (§7) which is enough for the M3
    // smoke render but won't match a USD `UsdLuxDomeLight` reference.
    TextureHandle    envMap = TextureHandle::Invalid;

    // Rect — local-frame quad, world-positioned via `position`. The
    // emitting face is the +N direction defined by axisU × axisV.
    hlslpp::float3   position    = { 0.0f, 0.0f, 0.0f };
    hlslpp::float3   axisU       = { 1.0f, 0.0f, 0.0f };
    hlslpp::float3   axisV       = { 0.0f, 1.0f, 0.0f };
    bool             doubleSided = false;
};

}  // namespace pyxis
