// Pyxis renderer -- display-transform compute shader (TonemapPass).
//
// P3 of the pass-split design: the 11-way debug-view display branch extracted
// VERBATIM from the retired raygen.slang (exposure 2^stops + Narkowicz ACES on
// the COLOR path; the 10 debug encodings unchanged). Inputs are the fp32
// LINEAR radiance RaytracedLightingPass writes (gLinearColor, RGBA32F --
// bit-identical to the in-kernel payload.color the old inline branch consumed;
// NOT the RGBA16F colorHdr AOV, which would quantize the display path) plus
// the 10 raw debug-view AOV textures. Output is the BGRA8 display target (gOutput) the
// SSAA resolve / sRGB blit / headless writer / Kit export consume exactly as
// before.
//
// didHit is reconstructed from the depth AOV: gOutDepth stores hitT on a hit
// and 0.0 on a miss (raygen writes `(hitT > 0) ? hitT : 0`), so `depth > 0`
// is exactly the old `payload.hitT > 0.0f` -- primary hits always satisfy
// hitT >= TMin (0.001) > 0. The DEBUG_VIEW_* sentinel behaviours match:
// Normal's miss = mid-grey keys off didHit; PrimId's miss (~0u) hashes to
// black inside hashInstanceColor.
//
// Deterministic by construction: pure per-texel function of the inputs, no
// RNG, no neighbourhood reads. Thread per output texel.

#include "ShaderInterop.slang"

[[vk::binding(0, 0)]]  Texture2D<float4> gLinearColor;     // fp32 LINEAR radiance (RGBA32F)
// Raw debug-view AOVs (same formats the raygen UAV writes produce).
[[vk::binding(1, 0)]]  Texture2D<float4> gAovNormal;       // RGBA16F world normal
[[vk::binding(2, 0)]]  Texture2D<float>  gAovDepth;        // R32F hitT-or-0
[[vk::binding(3, 0)]]  Texture2D<uint>   gAovPrimId;       // R32U InstanceID (~0u miss)
[[vk::binding(4, 0)]]  Texture2D<uint>   gAovMaterialId;   // R32U material slot (~0u miss)
[[vk::binding(5, 0)]]  Texture2D<float4> gAovBaseColor;    // RGBA16F OpenPBR baseColor
[[vk::binding(6, 0)]]  Texture2D<float4> gAovWorldPos;     // RGBA32F world hit pos
[[vk::binding(7, 0)]]  Texture2D<float>  gAovAlpha;        // R8_UNORM 1.0 hit / 0.0 miss
[[vk::binding(8, 0)]]  Texture2D<uint>   gAovElementId;    // R32U PrimitiveIndex (~0u miss)
[[vk::binding(9, 0)]]  Texture2D<float4> gAovNormalEye;    // RGBA16F eye-space normal
[[vk::binding(10, 0)]] Texture2D<float4> gAovWorldPosEye;  // RGBA32F eye-space hit pos
[[vk::binding(11, 0)]] ConstantBuffer<TonemapUniforms> gParams;
[[vk::binding(12, 0)]] RWTexture2D<float4> gOutput;        // BGRA8 display output

// Narkowicz ACES Filmic -- moved verbatim from raygen.slang (P3 pass
// split). Same constants; the display branch for DEBUG_VIEW_COLOR
// calls it on the exposure-scaled radiance.
float3 acesFilmic(float3 x) {
    const float a = 2.51;
    const float b = 0.03;
    const float c = 2.43;
    const float d = 0.59;
    const float e = 0.14;
    return saturate((x * (a * x + b)) / (x * (c * x + d) + e));
}

// Cheap integer hash → unit-interval float, used to recolor instance
// IDs into a visually-distinct palette for the InstanceID debug view.
// Murmur-style mix; avalanches enough that adjacent IDs land on
// different hues without a lookup table. Moved verbatim from
// raygen.slang (P3 pass split); the ~0u miss sentinel maps to black.
float3 hashInstanceColor(uint id) {
    if (id == 0xFFFFFFFFu) return float3(0.0, 0.0, 0.0);
    uint x = id * 2654435761u;
    x ^= x >> 16;
    x *= 2246822519u;
    x ^= x >> 13;
    x *= 3266489917u;
    x ^= x >> 16;
    const uint r = (x      ) & 0xFFu;
    const uint g = (x >>  8) & 0xFFu;
    const uint b = (x >> 16) & 0xFFu;
    return float3(float(r), float(g), float(b)) * (1.0 / 255.0);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
    if (tid.x >= gParams.destWidth || tid.y >= gParams.destHeight)
        return;
    const uint2 launchIndex = tid.xy;
    const int3  texel = int3(int2(launchIndex), 0);

    // Reconstruct the old raygen `didHit` (= payload.hitT > 0.0f) from the
    // depth AOV -- see the header comment for the sentinel equivalence.
    const float depth  = gAovDepth.Load(texel);
    const bool  didHit = depth > 0.0f;

    // ----- Display output (BGRA8 backbuffer) ----------------------
    // ACES tonemap on the Color path; (n*0.5+0.5) on Normal; 1/depth
    // grayscale on Depth; hash-coloured palette on InstanceID. Moved
    // verbatim from raygen.slang's inline branch (the M9 "ToneMapPass
    // takeover" TODO, discharged by the P3 pass split).
    float3 displayColor = float3(0.0, 0.0, 0.0);
    if (gParams.debugViewMode == DEBUG_VIEW_COLOR)
    {
        // Photographic exposure: scale scene radiance by 2^exposure
        // before tonemapping. Mirrors UsdGeomCamera.exposure semantics
        // (negative darkens, positive brightens). World Lobby et al.
        // calibrate light intensities for a strongly-negative camera
        // exposure (~-10 stops); without this scale the dome blows out.
        const float exposureGain = exp2(gParams.exposure);
        const float3 radiance = gLinearColor.Load(texel).xyz;
        displayColor = acesFilmic(radiance * exposureGain);
    }
    else if (gParams.debugViewMode == DEBUG_VIEW_NORMAL)
    {
        // Miss is (0,0,0) in the Normal AOV; display as middle grey
        // so the background isn't visually identical to a Z-aligned
        // surface. Hit normals: standard half-encode.
        displayColor = didHit
            ? (gAovNormal.Load(texel).xyz * 0.5f + 0.5f)
            : float3(0.5f, 0.5f, 0.5f);
    }
    else if (gParams.debugViewMode == DEBUG_VIEW_DEPTH)
    {
        // Inverse-depth grayscale: surfaces closer to the camera
        // appear brighter, miss (= 0 sentinel) renders black.
        // The 1/(1+d) curve avoids the divide-by-zero at d=0 and
        // gives a smooth response across the typical [0.1, 50]
        // viewer range without a configured min/max range.
        const float d = depth;
        const float intensity = (d > 0.0f) ? (1.0f / (1.0f + d * 0.1f)) : 0.0f;
        displayColor = float3(intensity, intensity, intensity);
    }
    else if (gParams.debugViewMode == DEBUG_VIEW_PRIM_ID)
    {
        displayColor = hashInstanceColor(gAovPrimId.Load(texel));
    }
    else if (gParams.debugViewMode == DEBUG_VIEW_MATERIAL_ID)
    {
        // Same hash function as InstanceID — different identifier
        // domains so the colour palette is independent. Reuses
        // hashInstanceColor since the math is identity (uint -> hue).
        displayColor = hashInstanceColor(gAovMaterialId.Load(texel));
    }
    else if (gParams.debugViewMode == DEBUG_VIEW_BASE_COLOR)
    {
        // Raw OpenPBR baseColor straight off the material table —
        // pre-shading albedo. saturate() clamps in case a (post-v1)
        // emissive material's baseColor exceeds [0,1].
        displayColor = saturate(gAovBaseColor.Load(texel).xyz);
    }
    else if (gParams.debugViewMode == DEBUG_VIEW_WORLD_POS)
    {
        // Visualise world-space hit position via a SMOOTH periodic
        // encoding — sin remap to [0,1] with a configurable period.
        // The sin encoding is C-infinity smooth so no discontinuities
        // exist anywhere in world space (the raw worldPos values stay
        // unmodified in gOutWorldPos / the picker readout).
        //
        // Period comes from the editor's WorldPos period slider via
        // TonemapUniforms; defaults to 10 m for human-scale scenes.
        // Guard against 0 (would divide by zero).
        const float period = max(gParams.worldPosPeriod, 1e-3);
        const float WORLD_POS_TAU = 6.28318530717958647692;
        const float omega = WORLD_POS_TAU / period;
        displayColor = didHit
            ? (sin(gAovWorldPos.Load(texel).xyz * omega) * 0.5f + 0.5f)
            : float3(0.0, 0.0, 0.0);
    }
    // Tier 1 Hydra-canonical display branches.
    else if (gParams.debugViewMode == DEBUG_VIEW_ALPHA)
    {
        // Binary today; future-proofed for transmission / coverage.
        // White = hit, black = miss. The alpha AOV stores exactly the
        // old `didHit ? 1.0f : 0.0f` encode (R8_UNORM holds 0.0 / 1.0
        // exactly), so reading it back reproduces the raygen branch.
        const float a = gAovAlpha.Load(texel);
        displayColor = float3(a, a, a);
    }
    else if (gParams.debugViewMode == DEBUG_VIEW_ELEMENT_ID)
    {
        // Same hash palette as InstanceID — different identifier
        // domain (per-face vs per-instance) so adjacent faces land
        // on visually-distinct hues. Miss → black.
        displayColor = hashInstanceColor(gAovElementId.Load(texel));
    }
    else if (gParams.debugViewMode == DEBUG_VIEW_NORMAL_EYE)
    {
        // Standard half-encode like the world-normal branch; miss
        // renders middle grey so the background isn't visually
        // identical to a Z-aligned eye-space surface.
        displayColor = didHit
            ? (gAovNormalEye.Load(texel).xyz * 0.5f + 0.5f)
            : float3(0.5f, 0.5f, 0.5f);
    }
    else if (gParams.debugViewMode == DEBUG_VIEW_WORLD_POS_EYE)
    {
        // Same sin-encoded periodic visualisation as WORLD_POS, but
        // sampled in eye space. Useful for verifying that a Hydra
        // delegate's Peye matches the renderer's view transform.
        const float period = max(gParams.worldPosPeriod, 1e-3);
        const float WORLD_POS_TAU_EYE = 6.28318530717958647692;
        const float omegaEye = WORLD_POS_TAU_EYE / period;
        displayColor = didHit
            ? (sin(gAovWorldPosEye.Load(texel).xyz * omegaEye) * 0.5f + 0.5f)
            : float3(0.0, 0.0, 0.0);
    }
    gOutput[launchIndex] = float4(displayColor, 1.0f);
}
