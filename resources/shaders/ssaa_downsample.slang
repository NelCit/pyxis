// Pyxis renderer — SSAA box-downsample compute shader.
//
// Viewer-mode deterministic supersampling resolve. The path tracer
// renders the color AOV at `factor`× the swapchain resolution per
// axis; this compute pass box-averages each factor×factor block down
// to one output texel and writes the backbuffer-resolution result.
//
// Color management: the source color AOV is the ACES-tonemapped color in LINEAR
// display space (raygen writes acesFilmic() with NO OETF — verified by readback:
// range [0,1], not raw HDR). This pass is the viewer's PRESENT-ENCODE step: it
// averages in linear light (correct — the source is already linear) and applies
// the sRGB OETF on OUTPUT so the value copied into the sRGB swapchain backbuffer
// displays correctly. This is the same single sRGB encode that pyxis.exe's PNG
// writer and the Kit pxr viewport (HdxColorCorrectionTask) apply, so the
// standalone viewer matches headless / Omniverse byte-for-byte. (It also runs at
// factor 1 as a pure LinearToSrgb passthrough — the non-supersampled present.)
//
// Deterministic by construction: fixed box kernel, no jitter, no
// accumulation. One dispatch per frame; thread per output texel.
// NOTE: this pass only runs for the standalone viewer (the only caller that binds
// colorResolved); Kit's PyxisEngine never binds it, so Omniverse output is
// unaffected by this shader.

#include "ShaderInterop.slang"

[[vk::binding(0, 0)]] Texture2D<float4>   gSource;   // super-res color (RGBA8 UNORM)
[[vk::binding(1, 0)]] RWTexture2D<float4> gDest;     // backbuffer-res output (RGBA8 UNORM)
[[vk::binding(2, 0)]] ConstantBuffer<SsaaDownsampleUniforms> gParams;

float LinearToSrgb(float val) {
    return (val <= 0.0031308f) ? (val * 12.92f)
                               : (1.055f * pow(val, 1.0f / 2.4f) - 0.055f);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
    if (tid.x >= gParams.destWidth || tid.y >= gParams.destHeight)
        return;

    const uint factor = max(1u, gParams.factor);
    const float invSamples = 1.0f / float(factor * factor);

    // Source is LINEAR (ACES-tonemapped, no OETF) — average it directly. At
    // factor 1 this is a single-sample passthrough.
    float3 accumLinear = float3(0.0f, 0.0f, 0.0f);
    float  accumAlpha  = 0.0f;
    for (uint subY = 0u; subY < factor; ++subY) {
        for (uint subX = 0u; subX < factor; ++subX) {
            const uint2 srcCoord = uint2(tid.x * factor + subX, tid.y * factor + subY);
            const float4 texel = gSource.Load(int3(int2(srcCoord), 0));
            accumLinear += texel.rgb;
            accumAlpha += texel.a;
        }
    }
    accumLinear *= invSamples;
    // Apply the sRGB OETF on output so the swapchain backbuffer displays correctly
    // (the single display encode, matching headless PNG / Kit viewport).
    const float3 outRgb = float3(LinearToSrgb(accumLinear.r),
                                 LinearToSrgb(accumLinear.g),
                                 LinearToSrgb(accumLinear.b));
    gDest[tid.xy] = float4(outRgb, accumAlpha * invSamples);
}
