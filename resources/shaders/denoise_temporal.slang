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
// Simplification vs. true NRD RELAX (tracked as debt): this is a SINGLE
// accumulator per signal, not NRD's parallel fast+slow dual-history blend
// (`fastHistoryFrames`, DenoiseTemporalUniforms, is plumbed but not yet
// read by this shader) or its luminance-variance-based clamp against a
// fast history. History-length-driven alpha alone already removes most
// of the Phase B estimators' per-frame noise across a static camera/scene.
//
// Ping-pong is REQUIRED, not a style choice: this dispatch reads the
// PREVIOUS frame's data at a reprojected (generally different) pixel
// while writing THIS frame's data at the current pixel — a single shared
// buffer would race. DenoiserResources (Private/Passes/DenoiserResources.h)
// owns the two index-swapped texture sets.
//
// A COMPUTE pass — no TraceRay, no SceneBindings (screen-space only).
// Bindings (single Set 0):
//   0  : Texture2D<float4> gRawDiffuse       (SRV, IndirectDiffusePass output)
//   1  : Texture2D<float4> gRawSpecular      (SRV, ReflectionsPass output)
//   2  : Texture2D<float2> gMotionVector     (SRV, GBuffer guide)
//   3  : Texture2D<float>  gViewZ            (SRV, GBuffer guide)
//   4  : Texture2D<float4> gNormalRoughness  (SRV, GBuffer guide)
//   5  : Texture2D<float4> gPrevDiffuse      (SRV, DenoiserResources)
//   6  : Texture2D<float4> gPrevSpecular     (SRV, DenoiserResources)
//   7  : Texture2D<float4> gPrevNormalViewZ  (SRV, DenoiserResources)
//   8  : RWTexture2D<float4> gCurrDiffuse    (UAV, DenoiserResources)
//   9  : RWTexture2D<float4> gCurrSpecular   (UAV, DenoiserResources)
//   10 : RWTexture2D<float4> gCurrNormalViewZ(UAV, DenoiserResources)
//   11 : ConstantBuffer<DenoiseTemporalUniforms>

#include "ShaderInterop.slang"

[[vk::binding(0, 0)]]  Texture2D<float4>   gRawDiffuse;
[[vk::binding(1, 0)]]  Texture2D<float4>   gRawSpecular;
[[vk::binding(2, 0)]]  Texture2D<float2>   gMotionVector;
[[vk::binding(3, 0)]]  Texture2D<float>    gViewZ;
[[vk::binding(4, 0)]]  Texture2D<float4>   gNormalRoughness;
[[vk::binding(5, 0)]]  Texture2D<float4>   gPrevDiffuse;
[[vk::binding(6, 0)]]  Texture2D<float4>   gPrevSpecular;
[[vk::binding(7, 0)]]  Texture2D<float4>   gPrevNormalViewZ;
[[vk::binding(8, 0)]]  RWTexture2D<float4> gCurrDiffuse;
[[vk::binding(9, 0)]]  RWTexture2D<float4> gCurrSpecular;
[[vk::binding(10, 0)]] RWTexture2D<float4> gCurrNormalViewZ;
[[vk::binding(11, 0)]] ConstantBuffer<DenoiseTemporalUniforms> gParams;

[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID) {
    const uint2 pixel = dispatchThreadId.xy;
    if (pixel.x >= gParams.destWidth || pixel.y >= gParams.destHeight)
        return;

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
    float3 prevDiffuseColor = float3(0.0, 0.0, 0.0);
    float3 prevSpecularColor = float3(0.0, 0.0, 0.0);
    float prevHistoryLenDiffuse = 0.0;
    float prevHistoryLenSpecular = 0.0;

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
            const float4 prevD = gPrevDiffuse[prevPixelU];
            const float4 prevS = gPrevSpecular[prevPixelU];
            prevDiffuseColor = prevD.rgb;
            prevHistoryLenDiffuse = prevD.a;
            prevSpecularColor = prevS.rgb;
            prevHistoryLenSpecular = prevS.a;
        }
    }

    const float maxAccum = float(max(gParams.maxAccumFrames, 1u));
    const float newHistoryLenDiffuse =
        disoccluded ? 1.0 : min(prevHistoryLenDiffuse + 1.0, maxAccum);
    const float newHistoryLenSpecular =
        disoccluded ? 1.0 : min(prevHistoryLenSpecular + 1.0, maxAccum);

    const float alphaDiffuse = 1.0 / newHistoryLenDiffuse;
    const float alphaSpecular = 1.0 / newHistoryLenSpecular;

    const float3 outDiffuse =
        disoccluded ? rawDiffuse.rgb : lerp(prevDiffuseColor, rawDiffuse.rgb, alphaDiffuse);
    const float3 outSpecular =
        disoccluded ? rawSpecular.rgb : lerp(prevSpecularColor, rawSpecular.rgb, alphaSpecular);

    gCurrDiffuse[pixel] = float4(outDiffuse, newHistoryLenDiffuse);
    gCurrSpecular[pixel] = float4(outSpecular, newHistoryLenSpecular);
    gCurrNormalViewZ[pixel] = float4(normal, viewZ);
}
