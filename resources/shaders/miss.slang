// Pyxis renderer — miss shader.
//
// Plan §41 M7-IBL. Samples the dome HDRI bound at binding 9 via the
// ray direction's lat-long (equirectangular) UV, modulates by the
// FIRST authored Dome light's color × intensity, and emits that as
// the background. Replaces the M3-era hardcoded sky-blue (`0.55,
// 0.65, 0.85`) and the M7-simple "authored-color-only" fallback.
//
// Lat-long mapping (right-handed Y-up, USD convention):
//   u = atan2(dir.x, dir.z) / (2π) + 0.5    // azimuth, wraps in U
//   v = acos(dir.y) / π                     // elevation, [0, 1]
// +Z is at u = 0.5 (image centre column), +Y at v = 0 (top row),
// -Y at v = 1 (bottom row). Matches Poly Haven HDRI authoring.
//
// **Still no IBL importance sampling** — that's the M7-full closesthit
// replacement (alias-table over the env-map per §7). The miss-shader
// background here is just visual — the closesthit's dome term
// continues to use the simple Lambert-against-up approximation
// until M7-full lands.

#include "ShaderInterop.slang"

[[vk::binding(5, 0)]]  StructuredBuffer<LightGpu> gLights;
[[vk::binding(9, 0)]]  Texture2D<float4>          gDomeEnvMap;
[[vk::binding(10, 0)]] SamplerState               gDomeSampler;

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

// Slang emits the SPIR-V `OpEntryPoint` name as `"main"` regardless
// of the source-side function name; the C++ side passes `"main"` to
// NVRHI's createShader. See the matching note in raygen.slang.
[shader("miss")]
void main(inout HitInfo payload) {
    // Find the first authored Dome light — its color × intensity
    // tints / scales the HDRI sample. The fallback (no dome found)
    // keeps the M3-era soft sky-blue.
    float3 domeTint     = float3(0.55, 0.65, 0.85);
    float  domeScale    = 1.0;
    bool   domeFound    = false;

    uint lightCount;
    uint _stride;
    gLights.GetDimensions(lightCount, _stride);
    for (uint i = 0; i < lightCount; ++i) {
        const LightGpu L = gLights[i];
        if (L.intensity <= 0.0) continue;
        if (L.kind != LIGHT_KIND_DOME) continue;
        domeTint  = float3(L.colorR, L.colorG, L.colorB);
        domeScale = L.intensity;
        domeFound = true;
        break;
    }

    // Sample the HDRI at the primary ray's miss direction. The
    // PathTracePass's 1×1 WHITE fallback covers the no-HDRI case
    // (color-only dome OR failed EXR decode) — sample returns
    // (1,1,1) so `hdri × tint × scale` collapses to `tint × scale`,
    // i.e. the dome's authored colour. With a real HDRI bound, the
    // hdri value carries the radiance distribution; tint × scale
    // lets the .usd modulate it without re-baking the EXR.
    const float3 dir = normalize(WorldRayDirection());
    const float2 uv  = latLongUv(dir);
    const float3 hdri = gDomeEnvMap.SampleLevel(gDomeSampler, uv, 0).rgb;

    // No-dome scenes (M3 cube / M5 m5_three_materials / M6 m6_
    // instanced_grid — all author no UsdLuxDomeLight) keep the
    // M3-era soft sky-blue. Dome-authored scenes use the HDRI
    // sample modulated by tint × scale.
    const float3 background = domeFound
        ? hdri * domeTint * domeScale
        : domeTint;

    payload.color = background;
    payload.hitT  = -1.0f;  // miss sentinel — raygen branches on this.
}
