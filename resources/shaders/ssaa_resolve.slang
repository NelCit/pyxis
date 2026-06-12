// Pyxis renderer -- SSAA box-downsample compute shader.
//
// Viewer-mode deterministic supersampling resolve. The path tracer renders the
// color AOV at `factor`x the swapchain resolution per axis; this compute pass
// box-averages each factor x factor block down to one output texel.
//
// Single responsibility: DOWNSAMPLE ONLY, staying in LINEAR throughout. The source
// color AOV is the ACES-tonemapped color in LINEAR display space (TonemapPass
// writes acesFilmic() with NO OETF -- verified by readback: range [0,1], not raw
// HDR), and averaging in linear is the correct way to downsample (the sRGB curve
// is convex; a naive average of encoded values biases dark). The sRGB DISPLAY
// encode is a separate pass (BlitToSrgbPass / blit_to_srgb.slang) -- this pass is
// box-downsample, nothing else. Output is base-res LINEAR.
//
// Deterministic by construction: fixed box kernel, no jitter, no accumulation.
// One dispatch per frame; thread per output texel. Only the standalone viewer
// binds the resolve target and only at factor > 1; Kit's PyxisEngine never binds
// it, so Omniverse output is unaffected.

#include "ShaderInterop.slang"

[[vk::binding(0, 0)]] Texture2D<float4>   gSource;   // super-res LINEAR color
[[vk::binding(1, 0)]] RWTexture2D<float4> gDest;     // base-res LINEAR output
[[vk::binding(2, 0)]] ConstantBuffer<SsaaDownsampleUniforms> gParams;

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
    if (tid.x >= gParams.destWidth || tid.y >= gParams.destHeight)
        return;

    const uint factor = max(1u, gParams.factor);
    const float invSamples = 1.0f / float(factor * factor);

    // Source is LINEAR -- average it directly and write LINEAR (no OETF here).
    float4 accum = float4(0.0f, 0.0f, 0.0f, 0.0f);
    for (uint subY = 0u; subY < factor; ++subY) {
        for (uint subX = 0u; subX < factor; ++subX) {
            const uint2 srcCoord = uint2(tid.x * factor + subX, tid.y * factor + subY);
            accum += gSource.Load(int3(int2(srcCoord), 0));
        }
    }
    gDest[tid.xy] = accum * invSamples;
}
