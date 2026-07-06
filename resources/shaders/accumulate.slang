// Pyxis renderer — AccumulationPass, ONE shader file, single compute entry
// (RTX-alignment design, rtx-realtime-alignment-design.md, "KEY FINDING
// (2026-07-06): no true accumulation buffer — top lever for <0.05").
//
// A true progressive average (running mean) of CompositePass's raw linear
// radiance across frames — see AccumulationPass.h's file header for the
// full design rationale (why this exists, why it accumulates the RAW
// pre-denoise signal, why no ping-pong is needed here unlike TaaPass /
// the denoiser chain).
//
// Per pixel, the exact closed-form running mean:
//     accum' = lerp(accum, current, 1/(sampleIndex+1))
// `sampleWeight` (== 1/(sampleIndex+1)) is precomputed CPU-side
// (AccumulationPass::Execute) so this shader is a single lerp — no
// per-pixel divide, no branch on sampleIndex. Weight 1.0 on the first
// accumulated sample (sampleIndex==0) makes this an exact overwrite,
// discarding whatever the freshly-(re)allocated `gAccumBuffer` held.
//
// No neighbor reads, no reprojection — each thread touches exactly one
// pixel address in BOTH gCurrentColor (read) and gAccumBuffer (read then
// write), so a single persistent buffer is safe (no cross-thread
// read/write hazard the way TaaPass's motion-compensated reprojection
// would have).
//
// A COMPUTE pass — no TraceRay, no SceneBindings (screen-space only).
// Bindings (single Set 0):
//   0 : Texture2D<float4>   gCurrentColor — CompositePass's linearColor
//   1 : RWTexture2D<float4> gAccumBuffer  — this pass's persistent running mean
//   2 : ConstantBuffer<AccumulationUniforms>

#include "ShaderInterop.slang"

[[vk::binding(0, 0)]] Texture2D<float4>   gCurrentColor;
[[vk::binding(1, 0)]] RWTexture2D<float4> gAccumBuffer;
[[vk::binding(2, 0)]] ConstantBuffer<AccumulationUniforms> gParams;

[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID) {
    const uint2 pixel = dispatchThreadId.xy;
    if (pixel.x >= gParams.destWidth || pixel.y >= gParams.destHeight)
        return;

    const float4 current = gCurrentColor[pixel];
    const float4 previous = gAccumBuffer[pixel];
    const float4 blended = lerp(previous, current, gParams.sampleWeight);
    gAccumBuffer[pixel] = blended;
}
