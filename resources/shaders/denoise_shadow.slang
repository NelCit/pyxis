// Pyxis renderer — DenoiseShadowPass, ONE shader file, single compute
// entry dispatched TWICE per frame (RTX-alignment design,
// rtx-realtime-alignment-design.md, Phase B denoiser chain).
//
// SIGMA-class (NVIDIA NRD's shadow/penumbra denoiser family) spatial-only
// filter for the direct-lighting signal (gDirectDiffuse / gDirectSpecular
// — both noisy from Phase B's RIS single-shadow-ray-per-pixel estimator).
// A cross-bilateral blur guided by:
//   * the occluder hitT carried in gDirectDiffuse.a (DirectLightingPass's
//     "future SIGMA guide" — see ShaderInterop.slang's OcclusionPayload
//     doc comment): neighbors whose occluder distance differs a lot from
//     the center pixel's are down-weighted, so the filter stops AT
//     penumbra/umbra boundaries instead of blurring across them.
//   * the G-buffer normal (gNormalRoughness.xyz): stops at geometric
//     silhouette edges.
//   * the G-buffer material id (gMaterialId, RTX-alignment halo fix,
//     2026-07-05): a HARD reject (weight *= 0) when the sample's material
//     slot differs from the center pixel's. This is the channel
//     DirectLightingPass demodulates (divides by the surface's own albedo
//     — see shading.slang's DemodulateByAlbedo), so a tap that crosses a
//     silhouette into a different material carries a value scaled by a
//     COMPLETELY different albedo's inverse; normal + occluder-hitT alone
//     don't catch this (two coplanar-ish surfaces of very different
//     material can share both). materialId is exact and needs no phi
//     tuning — see the design doc's "denoiser chain" section for the
//     white-halo forensics this fixes. ~0u (miss) never matches a real
//     material slot, so a hit pixel's kernel never blends in a miss
//     pixel's (meaningless) value either.
// Two dispatches (DenoiseShadowPass::Execute), narrow then wide radius —
// the "2-dispatch spatial" shape the frozen spec calls for (a simplified
// stand-in for SIGMA's true wavelet/Poisson-disc kernel).
//
// A COMPUTE pass — no TraceRay, no SceneBindings (screen-space only).
// Bindings (single Set 0):
//   0 : Texture2D<float4> gInDiffuse   (SRV) — dispatch 0: raw gDirectDiffuse;
//                                                dispatch 1: dispatch 0's output
//   1 : Texture2D<float4> gInSpecular  (SRV) — same shape, gDirectSpecular
//   2 : Texture2D<float4> gNormalRoughness (SRV, GBuffer guide)
//   3 : RWTexture2D<float4> gOutDiffuse  (UAV)
//   4 : RWTexture2D<float4> gOutSpecular (UAV)
//   5 : ConstantBuffer<DenoiseShadowUniforms>
//   6 : Texture2D<uint>    gMaterialId  (SRV, GBuffer guide; ~0u on miss —
//                                          1x1 fallback of 0 when the
//                                          caller doesn't wire materialIdAov,
//                                          which reads back as 0 for every
//                                          out-of-bounds pixel and is
//                                          therefore a behaviour-preserving
//                                          no-op gate, see DenoiseShadowPass.cpp)

#include "ShaderInterop.slang"

[[vk::binding(0, 0)]] Texture2D<float4>   gInDiffuse;
[[vk::binding(1, 0)]] Texture2D<float4>   gInSpecular;
[[vk::binding(2, 0)]] Texture2D<float4>   gNormalRoughness;
[[vk::binding(3, 0)]] RWTexture2D<float4> gOutDiffuse;
[[vk::binding(4, 0)]] RWTexture2D<float4> gOutSpecular;
[[vk::binding(5, 0)]] ConstantBuffer<DenoiseShadowUniforms> gParams;
[[vk::binding(6, 0)]] Texture2D<uint>     gMaterialId;

float ShadowGaussian(float distSq, float sigma) {
    return exp(-distSq / max(2.0 * sigma * sigma, 1e-6));
}

[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID) {
    const uint2 pixel = dispatchThreadId.xy;
    if (pixel.x >= gParams.destWidth || pixel.y >= gParams.destHeight)
        return;

    const float4 centerDiffuse = gInDiffuse[pixel];
    const float4 centerSpecular = gInSpecular[pixel];
    const float3 centerNormal = gNormalRoughness[pixel].xyz;
    const float centerOccluderHitT = centerDiffuse.a;
    const uint centerMaterialId = gMaterialId[pixel];

    float3 diffuseSum = float3(0.0, 0.0, 0.0);
    float3 specularSum = float3(0.0, 0.0, 0.0);
    float weightSum = 0.0;

    const int radius = int(ceil(gParams.radiusPixels));
    const float sigma = max(gParams.radiusPixels, 0.5);

    [loop]
    for (int dy = -radius; dy <= radius; ++dy) {
        [loop]
        for (int dx = -radius; dx <= radius; ++dx) {
            const int2 samplePixelSigned = int2(pixel) + int2(dx, dy);
            if (samplePixelSigned.x < 0 || samplePixelSigned.y < 0
                || samplePixelSigned.x >= int(gParams.destWidth)
                || samplePixelSigned.y >= int(gParams.destHeight))
                continue;
            const uint2 samplePixel = uint2(samplePixelSigned);

            const float4 sampleDiffuse = gInDiffuse[samplePixel];
            const float4 sampleSpecular = gInSpecular[samplePixel];
            const float3 sampleNormal = gNormalRoughness[samplePixel].xyz;

            const float spatialDistSq = float(dx * dx + dy * dy);
            const float spatialWeight = ShadowGaussian(spatialDistSq, sigma);
            // Edge-stop on occluder-distance divergence — the core SIGMA
            // idea: two pixels with very different shadow-ray occluder
            // distances likely straddle a penumbra/umbra boundary and
            // should NOT be averaged together.
            const float occluderDiff = abs(sampleDiffuse.a - centerOccluderHitT);
            const float occluderWeight = exp(-occluderDiff * 2.0);
            const float normalWeight = pow(saturate(dot(centerNormal, sampleNormal)), 8.0);
            // Halo fix (see file header) — hard reject across a material
            // boundary; DemodulateByAlbedo's divisor changes discontinuously
            // there, which occluder-hitT/normal similarity alone can't catch.
            const float materialWeight =
                (gMaterialId[samplePixel] == centerMaterialId) ? 1.0 : 0.0;

            const float weight = spatialWeight * occluderWeight * normalWeight * materialWeight;
            diffuseSum += sampleDiffuse.rgb * weight;
            specularSum += sampleSpecular.rgb * weight;
            weightSum += weight;
        }
    }

    const float3 filteredDiffuse =
        (weightSum > 1e-6) ? (diffuseSum / weightSum) : centerDiffuse.rgb;
    const float3 filteredSpecular =
        (weightSum > 1e-6) ? (specularSum / weightSum) : centerSpecular.rgb;

    // Occluder hitT / alpha pass through unfiltered — only the radiance
    // is spatially smoothed; the guide itself stays exact for whichever
    // downstream consumer wants it.
    gOutDiffuse[pixel] = float4(filteredDiffuse, centerDiffuse.a);
    gOutSpecular[pixel] = float4(filteredSpecular, centerSpecular.a);
}
