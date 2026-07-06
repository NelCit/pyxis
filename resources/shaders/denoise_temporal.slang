// Pyxis renderer — DenoiseTemporalPass, ONE shader file, single compute
// entry (RTX-alignment design, rtx-realtime-alignment-design.md, Phase B
// denoiser chain).
//
// ReLAX-class (NVIDIA NRD's RELAX_DIFFUSE_SPECULAR family) temporal
// reprojection + accumulation of the two Phase B Monte-Carlo signals:
// IndirectDiffusePass's gIndirectDiffuse (1 spp cosine bounce) and
// ReflectionsPass's gReflections (GGX VNDF sample) — "diffuse + specular
// in one dispatch (two texture pairs)" per the frozen spec.
//
// For each pixel: reproject via gMotionVector to find where this surface
// point was last frame (point-sampled — bilinear reprojection is a
// documented follow-up, see the design doc's drawbacks section), run a
// disocclusion test against the reprojected pixel's PREVIOUS normal/viewZ
// (snapshotted into gPrevNormalViewZ by the previous frame's dispatch of
// this SAME shader): viewZ relative plane-distance > 0.003 OR normal dot
// < 0.5 => disoccluded => history reset (frame 1 fallback: raw signal
// only, no blend). Otherwise: exponential cumulative-moving-average
// accumulation, historyLength capped at `maxAccumFrames` (NRD default
// target 30) — alpha = 1/historyLength gives fast initial convergence
// that slows toward a ~30-frame effective window.
//
// Noise-floor + vegetation spec (rtx-realtime-alignment-design.md,
// 2026-07-06), work item 2 — ReLAX DUAL history: alongside the SLOW
// accumulator above (cap `maxAccumFrames`, ~30), a second FAST
// accumulator (cap `fastHistoryFrames`, ~6) is maintained in lockstep
// (same reprojection/disocclusion test — geometry doesn't care which
// accumulator is reading it). The SLOW accumulator's LUMINANCE is then
// clamped against the FAST accumulator's luminance ± `clampSigma` (NRD
// default target ~2) local standard deviations, estimated from a 3x3
// spatial neighborhood of THIS frame's raw (pre-accumulation) signal —
// the standard SVGF/ReLAX anti-lag (fast reacts quickly to a real
// lighting change) + anti-flatten (slow's long memory doesn't let a
// stale value survive an actual change, nor drift away from the fast
// track's short-term truth) mechanism. Skipped on disocclusion (both
// accumulators already reset to the identical raw value that frame).
// DenoiseHistoryFixPass (work item 4) also reads the FAST accumulator
// directly as its preferred fallback at thin/chaotic geometry.
//
// Ping-pong is REQUIRED, not a style choice: this dispatch reads the
// PREVIOUS frame's data at a reprojected (generally different) pixel
// while writing THIS frame's data at the current pixel — a single shared
// buffer would race. DenoiserResources (Private/Passes/DenoiserResources.h)
// owns the four index-swapped texture sets (fast+slow x diffuse+specular)
// plus the shared normalViewZ set.
//
// A COMPUTE pass — no TraceRay, no SceneBindings (screen-space only).
// Bindings (single Set 0):
//   0  : Texture2D<float4> gRawDiffuse        (SRV, IndirectDiffusePass output)
//   1  : Texture2D<float4> gRawSpecular       (SRV, ReflectionsPass output)
//   2  : Texture2D<float2> gMotionVector      (SRV, GBuffer guide)
//   3  : Texture2D<float>  gViewZ             (SRV, GBuffer guide)
//   4  : Texture2D<float4> gNormalRoughness   (SRV, GBuffer guide)
//   5  : Texture2D<float4> gPrevDiffuseSlow   (SRV, DenoiserResources)
//   6  : Texture2D<float4> gPrevSpecularSlow  (SRV, DenoiserResources)
//   7  : Texture2D<float4> gPrevNormalViewZ   (SRV, DenoiserResources)
//   8  : Texture2D<float4> gPrevDiffuseFast   (SRV, DenoiserResources)
//   9  : Texture2D<float4> gPrevSpecularFast  (SRV, DenoiserResources)
//   10 : RWTexture2D<float4> gCurrDiffuseSlow  (UAV, DenoiserResources)
//   11 : RWTexture2D<float4> gCurrSpecularSlow (UAV, DenoiserResources)
//   12 : RWTexture2D<float4> gCurrNormalViewZ  (UAV, DenoiserResources)
//   13 : RWTexture2D<float4> gCurrDiffuseFast  (UAV, DenoiserResources)
//   14 : RWTexture2D<float4> gCurrSpecularFast (UAV, DenoiserResources)
//   15 : ConstantBuffer<DenoiseTemporalUniforms>

#include "ShaderInterop.slang"

[[vk::binding(0, 0)]]  Texture2D<float4>   gRawDiffuse;
[[vk::binding(1, 0)]]  Texture2D<float4>   gRawSpecular;
[[vk::binding(2, 0)]]  Texture2D<float2>   gMotionVector;
[[vk::binding(3, 0)]]  Texture2D<float>    gViewZ;
[[vk::binding(4, 0)]]  Texture2D<float4>   gNormalRoughness;
[[vk::binding(5, 0)]]  Texture2D<float4>   gPrevDiffuseSlow;
[[vk::binding(6, 0)]]  Texture2D<float4>   gPrevSpecularSlow;
[[vk::binding(7, 0)]]  Texture2D<float4>   gPrevNormalViewZ;
[[vk::binding(8, 0)]]  Texture2D<float4>   gPrevDiffuseFast;
[[vk::binding(9, 0)]]  Texture2D<float4>   gPrevSpecularFast;
[[vk::binding(10, 0)]] RWTexture2D<float4> gCurrDiffuseSlow;
[[vk::binding(11, 0)]] RWTexture2D<float4> gCurrSpecularSlow;
[[vk::binding(12, 0)]] RWTexture2D<float4> gCurrNormalViewZ;
[[vk::binding(13, 0)]] RWTexture2D<float4> gCurrDiffuseFast;
[[vk::binding(14, 0)]] RWTexture2D<float4> gCurrSpecularFast;
[[vk::binding(15, 0)]] ConstantBuffer<DenoiseTemporalUniforms> gParams;

float Luma(float3 c) {
    return dot(c, float3(0.2126, 0.7152, 0.0722));
}

// Cheap per-pixel local standard deviation of the RAW (pre-accumulation)
// signal's luminance, from its immediate 3x3 neighborhood — the spatial
// proxy for "how noisy/volatile is this pixel right now" that the
// fast-history luminance clamp uses in place of a tracked temporal
// variance (which this simplified chain doesn't carry — see file header).
float LocalRawLumaStdDev(Texture2D<float4> rawTex, uint2 centerPixel, uint2 dims) {
    float sum = 0.0;
    float sumSq = 0.0;
    float count = 0.0;
    [loop]
    for (int dy = -1; dy <= 1; ++dy) {
        [loop]
        for (int dx = -1; dx <= 1; ++dx) {
            const int2 samplePixelSigned = int2(centerPixel) + int2(dx, dy);
            if (samplePixelSigned.x < 0 || samplePixelSigned.y < 0
                || samplePixelSigned.x >= int(dims.x) || samplePixelSigned.y >= int(dims.y))
                continue;
            const float luma = Luma(rawTex[uint2(samplePixelSigned)].rgb);
            sum += luma;
            sumSq += luma * luma;
            count += 1.0;
        }
    }
    if (count < 2.0)
        return 0.0;
    const float mean = sum / count;
    const float variance = max(sumSq / count - mean * mean, 0.0);
    return sqrt(variance);
}

[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID) {
    const uint2 pixel = dispatchThreadId.xy;
    if (pixel.x >= gParams.destWidth || pixel.y >= gParams.destHeight)
        return;
    const uint2 dims = uint2(gParams.destWidth, gParams.destHeight);

    const float4 rawDiffuse = gRawDiffuse[pixel];
    const float4 rawSpecular = gRawSpecular[pixel];
    const float3 normal = gNormalRoughness[pixel].xyz;
    const float viewZ = gViewZ[pixel];
    const float2 motionVector = gMotionVector[pixel];

    // Reprojection: motionVector = currentPixel - previousPixel (pixels;
    // raytraced_gbuffer.slang's convention), so previousPixel =
    // currentPixel - motionVector. Point-sampled (nearest) — see file
    // header debt note.
    const float2 prevPixelF = float2(pixel) + 0.5 - motionVector;
    const int2 prevPixel = int2(floor(prevPixelF));
    const bool insideBounds = prevPixel.x >= 0 && prevPixel.y >= 0
                            && prevPixel.x < int(gParams.destWidth)
                            && prevPixel.y < int(gParams.destHeight);

    bool disoccluded = true;
    float3 prevDiffuseSlow = float3(0.0, 0.0, 0.0);
    float3 prevSpecularSlow = float3(0.0, 0.0, 0.0);
    float prevHistLenDiffuseSlow = 0.0;
    float prevHistLenSpecularSlow = 0.0;
    float3 prevDiffuseFast = float3(0.0, 0.0, 0.0);
    float3 prevSpecularFast = float3(0.0, 0.0, 0.0);
    float prevHistLenDiffuseFast = 0.0;
    float prevHistLenSpecularFast = 0.0;

    if (gParams.hasHistory != 0u && insideBounds) {
        const uint2 prevPixelU = uint2(prevPixel);
        const float4 prevNormalViewZ = gPrevNormalViewZ[prevPixelU];
        const float3 prevNormal = prevNormalViewZ.xyz;
        const float prevViewZ = prevNormalViewZ.w;
        // Disocclusion thresholds per the frozen spec: viewZ plane-
        // distance 0.003 (relative) + normal dot 0.5.
        const float planeDistance = abs(prevViewZ - viewZ) / max(viewZ, 1e-3);
        const float normalDot = dot(normal, prevNormal);
        if (planeDistance < 0.003 && normalDot > 0.5) {
            disoccluded = false;
            const float4 prevDSlow = gPrevDiffuseSlow[prevPixelU];
            const float4 prevSSlow = gPrevSpecularSlow[prevPixelU];
            prevDiffuseSlow = prevDSlow.rgb;
            prevHistLenDiffuseSlow = prevDSlow.a;
            prevSpecularSlow = prevSSlow.rgb;
            prevHistLenSpecularSlow = prevSSlow.a;
            const float4 prevDFast = gPrevDiffuseFast[prevPixelU];
            const float4 prevSFast = gPrevSpecularFast[prevPixelU];
            prevDiffuseFast = prevDFast.rgb;
            prevHistLenDiffuseFast = prevDFast.a;
            prevSpecularFast = prevSFast.rgb;
            prevHistLenSpecularFast = prevSFast.a;
        }
    }

    const float maxAccumSlow = float(max(gParams.maxAccumFrames, 1u));
    const float maxAccumFast = float(max(gParams.fastHistoryFrames, 1u));

    // SLOW accumulator (long memory).
    const float newHistLenDiffuseSlow =
        disoccluded ? 1.0 : min(prevHistLenDiffuseSlow + 1.0, maxAccumSlow);
    const float newHistLenSpecularSlow =
        disoccluded ? 1.0 : min(prevHistLenSpecularSlow + 1.0, maxAccumSlow);
    const float alphaDiffuseSlow = 1.0 / newHistLenDiffuseSlow;
    const float alphaSpecularSlow = 1.0 / newHistLenSpecularSlow;
    float3 outDiffuseSlow =
        disoccluded ? rawDiffuse.rgb : lerp(prevDiffuseSlow, rawDiffuse.rgb, alphaDiffuseSlow);
    float3 outSpecularSlow =
        disoccluded ? rawSpecular.rgb : lerp(prevSpecularSlow, rawSpecular.rgb, alphaSpecularSlow);

    // FAST accumulator (short memory) — work item 2.
    const float newHistLenDiffuseFast =
        disoccluded ? 1.0 : min(prevHistLenDiffuseFast + 1.0, maxAccumFast);
    const float newHistLenSpecularFast =
        disoccluded ? 1.0 : min(prevHistLenSpecularFast + 1.0, maxAccumFast);
    const float alphaDiffuseFast = 1.0 / newHistLenDiffuseFast;
    const float alphaSpecularFast = 1.0 / newHistLenSpecularFast;
    const float3 outDiffuseFast =
        disoccluded ? rawDiffuse.rgb : lerp(prevDiffuseFast, rawDiffuse.rgb, alphaDiffuseFast);
    const float3 outSpecularFast =
        disoccluded ? rawSpecular.rgb : lerp(prevSpecularFast, rawSpecular.rgb, alphaSpecularFast);

    // Fast-history luminance clamp of the slow accumulator (work item 2) —
    // skipped on disocclusion (both accumulators already equal the raw
    // value this frame, nothing to reconcile).
    if (!disoccluded) {
        const float sigmaDiffuse = LocalRawLumaStdDev(gRawDiffuse, pixel, dims);
        const float lumFastDiffuse = Luma(outDiffuseFast);
        const float lumSlowDiffuseRaw = Luma(outDiffuseSlow);
        const float loDiffuse = lumFastDiffuse - gParams.clampSigma * sigmaDiffuse;
        const float hiDiffuse = lumFastDiffuse + gParams.clampSigma * sigmaDiffuse;
        const float lumDiffuseClamped = clamp(lumSlowDiffuseRaw, loDiffuse, hiDiffuse);
        if (lumSlowDiffuseRaw > 1e-5)
            outDiffuseSlow *= (lumDiffuseClamped / lumSlowDiffuseRaw);

        const float sigmaSpecular = LocalRawLumaStdDev(gRawSpecular, pixel, dims);
        const float lumFastSpecular = Luma(outSpecularFast);
        const float lumSlowSpecularRaw = Luma(outSpecularSlow);
        const float loSpecular = lumFastSpecular - gParams.clampSigma * sigmaSpecular;
        const float hiSpecular = lumFastSpecular + gParams.clampSigma * sigmaSpecular;
        const float lumSpecularClamped = clamp(lumSlowSpecularRaw, loSpecular, hiSpecular);
        if (lumSlowSpecularRaw > 1e-5)
            outSpecularSlow *= (lumSpecularClamped / lumSlowSpecularRaw);
    }

    gCurrDiffuseSlow[pixel] = float4(outDiffuseSlow, newHistLenDiffuseSlow);
    gCurrSpecularSlow[pixel] = float4(outSpecularSlow, newHistLenSpecularSlow);
    gCurrNormalViewZ[pixel] = float4(normal, viewZ);
    gCurrDiffuseFast[pixel] = float4(outDiffuseFast, newHistLenDiffuseFast);
    gCurrSpecularFast[pixel] = float4(outSpecularFast, newHistLenSpecularFast);
}
