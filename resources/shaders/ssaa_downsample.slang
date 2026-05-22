// Pyxis renderer — SSAA box-downsample compute shader.
//
// Viewer-mode deterministic supersampling resolve. The path tracer
// renders the color AOV at `factor`× the swapchain resolution per
// axis; this compute pass box-averages each factor×factor block down
// to one output texel and writes the backbuffer-resolution result.
//
// Gamma-aware: the source color AOV is the post-tonemap display-
// referred image (sRGB-encoded BGRA8 semantics, but stored in the
// AOV's RGBA8 texture). We average in approximately-linear light
// (decode → average → re-encode) so edges don't darken — the sRGB
// curve is convex, so a naive average of encoded values biases dark.
//
// Deterministic by construction: fixed box kernel, no jitter, no
// accumulation. One dispatch per frame; thread per output texel.

#include "ShaderInterop.slang"

[[vk::binding(0, 0)]] Texture2D<float4>   gSource;   // super-res color (RGBA8 UNORM)
[[vk::binding(1, 0)]] RWTexture2D<float4> gDest;     // backbuffer-res output (RGBA8 UNORM)
[[vk::binding(2, 0)]] ConstantBuffer<SsaaDownsampleUniforms> gParams;

float SrgbToLinear(float val) {
    return (val <= 0.04045f) ? (val / 12.92f)
                             : pow((val + 0.055f) / 1.055f, 2.4f);
}
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

    float3 accumLinear = float3(0.0f, 0.0f, 0.0f);
    float  accumAlpha  = 0.0f;
    for (uint subY = 0u; subY < factor; ++subY) {
        for (uint subX = 0u; subX < factor; ++subX) {
            const uint2 srcCoord = uint2(tid.x * factor + subX, tid.y * factor + subY);
            const float4 texel = gSource.Load(int3(int2(srcCoord), 0));
            accumLinear += float3(SrgbToLinear(texel.r),
                                  SrgbToLinear(texel.g),
                                  SrgbToLinear(texel.b));
            accumAlpha += texel.a;
        }
    }
    accumLinear *= invSamples;
    const float3 outRgb = float3(LinearToSrgb(accumLinear.r),
                                 LinearToSrgb(accumLinear.g),
                                 LinearToSrgb(accumLinear.b));
    gDest[tid.xy] = float4(outRgb, accumAlpha * invSamples);
}
