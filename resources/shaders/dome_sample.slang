// Pyxis renderer — shared dome / background sampling (P4 pass split).
//
// Extracted VERBATIM from the retired miss.slang so the Lighting miss
// shader (raytraced_lighting_miss.slang — secondary / continuation rays)
// and the Lighting raygen (raytraced_lighting_raygen.slang — primary-miss
// pixels resolved from the visibility buffer) compute bit-identical
// backgrounds (§33.7).
//
// Lat-long mapping (right-handed Y-up, USD convention):
//   u = atan2(dir.x, dir.z) / (2π) + 0.5    // azimuth, wraps in U
//   v = acos(dir.y) / π                     // elevation, [0, 1]
// +Z is at u = 0.5 (image centre column), +Y at v = 0 (top row),
// -Y at v = 1 (bottom row). Matches Poly Haven HDRI authoring.
//
// **Still no IBL importance sampling** — that's the M7-full closesthit
// replacement (alias-table over the env-map per §7). The background here
// is just visual — the hit-shading dome term (shading.slang) continues
// to use the fast-diffuse-IBL mip-bias approximation.
//
// Declares the light table + dome HDRI + dome sampler — RTX-alignment
// design WP2 rewrite: binding numbers (5 / 10 / 11) are the canonical
// SceneBindings Set-0 slots (Private/Passes/SceneBindings.h), shared
// verbatim by every RT pipeline that includes this module.

#ifndef PYXIS_DOME_SAMPLE_SLANG
#define PYXIS_DOME_SAMPLE_SLANG

#include "ShaderInterop.slang"

[[vk::binding(5, 0)]]  StructuredBuffer<LightGpu> gLights;
[[vk::binding(10, 0)]] Texture2D<float4>          gDomeEnvMap;
// M9-fidelity per-role samplers: dome at binding 11 (Wrap-Clamp-Wrap).
// Binding 12 is the material sampler (Wrap-Wrap-Wrap), used by shading.slang.
[[vk::binding(11, 0)]] SamplerState               gDomeSampler;

static const float PYXIS_PI         = 3.14159265358979323846;
static const float PYXIS_TWO_PI     = 6.28318530717958647692;

// Convert a unit-length world-space direction to equirectangular
// lat-long UV. Defensively clamps cos-theta input so acos() doesn't
// produce NaN at the poles when the direction's y is slightly out
// of [-1, 1] from float-precision drift.
float2 latLongUv(float3 dir) {
    const float u = atan2(dir.x, dir.z) / PYXIS_TWO_PI + 0.5;
    const float v = acos(clamp(dir.y, -1.0, 1.0)) / PYXIS_PI;
    return float2(u, v);
}

// Background radiance for a ray that escaped the scene. `rayDir` is the
// world-space ray direction EXACTLY as it was passed to TraceRay (the
// miss shader passes WorldRayDirection(); the lighting raygen passes the
// BuildCameraRay output, which is the same value for the primary ray) —
// the normalize() below then reproduces the old miss shader bit-for-bit.
float3 SampleDomeBackground(float3 rayDir) {
    // Find the first authored Dome light — its color × intensity
    // tints / scales the HDRI sample. The fallback (no dome found)
    // keeps the M3-era soft sky-blue.
    float3 domeTint        = float3(0.55, 0.65, 0.85);
    float  domeScale       = 1.0;
    float  domeRotationY   = 0.0;
    bool   domeFound       = false;

    uint lightCount;
    uint _stride;
    gLights.GetDimensions(lightCount, _stride);
    for (uint i = 0; i < lightCount; ++i) {
        const LightGpu L = gLights[i];
        if (L.intensity <= 0.0) continue;
        if (L.kind != LIGHT_KIND_DOME) continue;
        domeTint      = float3(L.colorR, L.colorG, L.colorB);
        domeScale     = L.intensity;
        domeRotationY = L.domeRotationYRadians;
        domeFound     = true;
        break;
    }

    // M9-fidelity per-prim dome rotation. Apply the inverse of the
    // dome's authored Y rotation to the sample direction so a
    // positive xformOp:rotateY = 30° spins the HDRI horizon 30° to
    // the right (matches usdview / Storm convention). Single-axis
    // rotation matrix: rotates around world-Y, leaves Y unchanged.
    const float3 dirRaw = normalize(rayDir);
    float3 dir = dirRaw;
    if (domeFound && abs(domeRotationY) > 1e-4) {
        const float cosR = cos(-domeRotationY);
        const float sinR = sin(-domeRotationY);
        dir = float3(cosR * dirRaw.x + sinR * dirRaw.z,
                     dirRaw.y,
                    -sinR * dirRaw.x + cosR * dirRaw.z);
    }

    // Sample the HDRI at the (possibly-rotated) miss direction.
    // RaytracedLightingPass's 1×1 WHITE fallback covers the no-HDRI
    // case (color-only dome OR failed EXR decode) — sample returns
    // (1,1,1) so `hdri × tint × scale` collapses to `tint × scale`,
    // i.e. the dome's authored colour.
    const float2 uv  = latLongUv(dir);
    const float3 hdri = gDomeEnvMap.SampleLevel(gDomeSampler, uv, 0).rgb;

    // No-dome scenes (M3 cube / M5 m5_three_materials / M6 m6_
    // instanced_grid — all author no UsdLuxDomeLight) keep the
    // M3-era soft sky-blue. Dome-authored scenes use the HDRI
    // sample modulated by tint × scale.
    const float3 background = domeFound
        ? hdri * domeTint * domeScale
        : domeTint;

    return background;
}

// RTX-alignment design (rtx-realtime-alignment-design.md), Phase B follow-
// up ("interior illumination" fix) — true iff the scene authors at least
// one ENABLED (intensity > 0) Dome light. DirectLightingPass's new dome-IBL
// direct-diffuse term (direct_lighting.slang) gates on this so scenes with
// NO dome light are byte-for-byte unchanged: SampleDomeBackground's own
// "no dome" fallback above (the soft sky-blue tint) is a MISS-ray visual
// convenience, not a light source — sampling it unconditionally as a
// DIRECT contribution would fabricate light in dome-less fixtures (furnace
// tests, M3-M6 scenes) and silently change their goldens.
bool DomeLightExists() {
    uint lightCount;
    uint _stride;
    gLights.GetDimensions(lightCount, _stride);
    for (uint i = 0; i < lightCount; ++i) {
        if (gLights[i].intensity > 0.0 && gLights[i].kind == LIGHT_KIND_DOME)
            return true;
    }
    return false;
}

#endif  // PYXIS_DOME_SAMPLE_SLANG
