// Pyxis renderer — DenoiseAoPass, ONE shader file, single compute entry
// (noise-floor + vegetation spec, rtx-realtime-alignment-design.md,
// 2026-07-06, work item 1).
//
// Owner diagnosis: AmbientOcclusionPass's gAo signal already carries the
// right occlusion structure (dark plant interiors, no backface culling on
// occlusion rays) but was consumed RAW by CompositePass — noisy N=2
// stochastic samples per pixel with no temporal or spatial smoothing at
// all, unlike every other signal (which gets the SIGMA/ReLAX chain).
// Cheapest ReBLUR-occlusion-style fix, folded into ONE dispatch:
//   1. A small edge-aware spatial pre-filter of the RAW AO neighborhood
//      (radius `spatialRadiusPixels`, guided by gNormalRoughness/gViewZ —
//      the same normal-power + plane-distance edge stops the rest of the
//      chain uses) — cuts the highest-frequency speckle before temporal
//      accumulation ever sees it.
//   2. Reprojection via gMotionVector + a disocclusion test against this
//      pass's OWN previous-frame normal/viewZ snapshot (thresholds match
//      denoise_temporal.slang: plane-distance 0.003, normal dot 0.5),
//      then exponential accumulation capped at `maxAccumFrames` (NRD-
//      ReBLUR-occlusion-default-derived: 32) — alpha = 1/historyLength.
// Packed into a float4 ping-pong texture (r = accumulated AO, g = history
// length, b/g unused) — CompositePass/composite.slang only ever reads
// `.r`, matching AmbientOcclusionPass's own output convention.
//
// Self-contained: this pass owns its OWN tiny ping-pong (not
// DenoiserResources — that class is documented as exclusive to
// DenoiseTemporalPass's 3-pair diffuse/specular/normalViewZ history; AO
// only needs 2 small pairs, so a dedicated pair kept directly on
// DenoiseAoPass is cheaper and avoids widening DenoiserResources'
// ownership contract for one unrelated signal).
//
// A COMPUTE pass — no TraceRay, no SceneBindings (screen-space only).
// Bindings (single Set 0):
//   0 : Texture2D<float4> gRawAo          (SRV, AmbientOcclusionPass output; .r used)
//   1 : Texture2D<float4> gNormalRoughness (SRV, GBuffer guide)
//   2 : Texture2D<float>  gViewZ           (SRV, GBuffer guide)
//   3 : Texture2D<float2> gMotionVector    (SRV, GBuffer guide)
//   4 : Texture2D<float4> gPrevAo          (SRV, own ping-pong: r=ao, g=historyLength)
//   5 : Texture2D<float4> gPrevGuide       (SRV, own ping-pong: xyz=normal, w=viewZ)
//   6 : RWTexture2D<float4> gCurrAo        (UAV)
//   7 : RWTexture2D<float4> gCurrGuide     (UAV)
//   8 : ConstantBuffer<DenoiseAoUniforms>

#include "ShaderInterop.slang"

[[vk::binding(0, 0)]] Texture2D<float4>   gRawAo;
[[vk::binding(1, 0)]] Texture2D<float4>   gNormalRoughness;
[[vk::binding(2, 0)]] Texture2D<float>    gViewZ;
[[vk::binding(3, 0)]] Texture2D<float2>   gMotionVector;
[[vk::binding(4, 0)]] Texture2D<float4>   gPrevAo;
[[vk::binding(5, 0)]] Texture2D<float4>   gPrevGuide;
[[vk::binding(6, 0)]] RWTexture2D<float4> gCurrAo;
[[vk::binding(7, 0)]] RWTexture2D<float4> gCurrGuide;
[[vk::binding(8, 0)]] ConstantBuffer<DenoiseAoUniforms> gParams;

// Small edge-aware spatial pre-filter of the raw AO neighborhood — the
// "spatial blur" half of the frozen spec's item 1. Guided by normal power
// + relative viewZ plane-distance only (no materialId: foliage shares one
// material slot, so that edge-stop gives zero intra-plant protection
// anyway — see the design doc's diagnosis; normal/viewZ are the only
// signals that actually vary at blade scale).
float SpatialPrefilterAo(uint2 centerPixel, uint2 dims) {
    const float4 centerRaw = gRawAo[centerPixel];
    const float3 centerNormal = gNormalRoughness[centerPixel].xyz;
    const float centerViewZ = gViewZ[centerPixel];

    const int radius = int(ceil(gParams.spatialRadiusPixels));
    const float sigma = max(gParams.spatialRadiusPixels, 0.5);

    float sum = 0.0;
    float weightSum = 0.0;
    [loop]
    for (int dy = -radius; dy <= radius; ++dy) {
        [loop]
        for (int dx = -radius; dx <= radius; ++dx) {
            const int2 samplePixelSigned = int2(centerPixel) + int2(dx, dy);
            if (samplePixelSigned.x < 0 || samplePixelSigned.y < 0
                || samplePixelSigned.x >= int(dims.x) || samplePixelSigned.y >= int(dims.y))
                continue;
            const uint2 samplePixel = uint2(samplePixelSigned);

            const float spatialDistSq = float(dx * dx + dy * dy);
            const float spatialWeight = exp(-spatialDistSq / max(2.0 * sigma * sigma, 1e-6));
            const float3 sampleNormal = gNormalRoughness[samplePixel].xyz;
            const float normalWeight = pow(saturate(dot(centerNormal, sampleNormal)), 8.0);
            const float sampleViewZ = gViewZ[samplePixel];
            const float planeDistance = abs(sampleViewZ - centerViewZ) / max(centerViewZ, 1e-3);
            const float viewZWeight = exp(-planeDistance * 64.0);

            const float weight = spatialWeight * normalWeight * viewZWeight;
            sum += gRawAo[samplePixel].r * weight;
            weightSum += weight;
        }
    }
    return (weightSum > 1e-6) ? (sum / weightSum) : centerRaw.r;
}

[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID) {
    const uint2 pixel = dispatchThreadId.xy;
    if (pixel.x >= gParams.destWidth || pixel.y >= gParams.destHeight)
        return;
    const uint2 dims = uint2(gParams.destWidth, gParams.destHeight);

    const float spatialAo = SpatialPrefilterAo(pixel, dims);
    const float3 normal = gNormalRoughness[pixel].xyz;
    const float viewZ = gViewZ[pixel];
    const float2 motionVector = gMotionVector[pixel];

    // Reprojection — same convention as denoise_temporal.slang: motionVector
    // = currentPixel - previousPixel (pixels).
    const float2 prevPixelF = float2(pixel) + 0.5 - motionVector;
    const int2 prevPixel = int2(floor(prevPixelF));
    const bool insideBounds = prevPixel.x >= 0 && prevPixel.y >= 0
                            && prevPixel.x < int(gParams.destWidth)
                            && prevPixel.y < int(gParams.destHeight);

    bool disoccluded = true;
    float prevAo = 0.0;
    float prevHistoryLen = 0.0;
    if (gParams.hasHistory != 0u && insideBounds) {
        const uint2 prevPixelU = uint2(prevPixel);
        const float4 prevGuide = gPrevGuide[prevPixelU];
        const float3 prevNormal = prevGuide.xyz;
        const float prevViewZ = prevGuide.w;
        // Same disocclusion thresholds as the ReLAX diffuse/specular chain
        // (denoise_temporal.slang): viewZ plane-distance 0.003 + normal
        // dot 0.5 — the chaotic per-frame normal/viewZ at blade-scale
        // foliage correctly triggers frequent disocclusion here rather
        // than smearing AO across silhouettes.
        const float planeDistance = abs(prevViewZ - viewZ) / max(viewZ, 1e-3);
        const float normalDot = dot(normal, prevNormal);
        if (planeDistance < 0.003 && normalDot > 0.5) {
            disoccluded = false;
            const float4 prevSample = gPrevAo[prevPixelU];
            prevAo = prevSample.r;
            prevHistoryLen = prevSample.g;
        }
    }

    const float maxAccum = float(max(gParams.maxAccumFrames, 1u));
    const float newHistoryLen = disoccluded ? 1.0 : min(prevHistoryLen + 1.0, maxAccum);
    const float alpha = 1.0 / newHistoryLen;
    const float outAo = disoccluded ? spatialAo : lerp(prevAo, spatialAo, alpha);

    gCurrAo[pixel] = float4(outAo, newHistoryLen, 0.0, 1.0);
    gCurrGuide[pixel] = float4(normal, viewZ);
}
