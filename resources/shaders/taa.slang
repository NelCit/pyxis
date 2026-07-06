// Pyxis renderer — TaaPass, ONE shader file, single compute entry
// (RTX-alignment design, rtx-realtime-alignment-design.md, Phase B).
//
// Jittered-camera temporal anti-aliasing on the LINEAR pre-tonemap color
// (runs between AutoExposurePass and TonemapPass — "before Tonemap" per
// the frozen spec, matching NRD guidance to run TAA on linear values).
// Per pixel:
//   1. Reproject via gMotionVector to the previous frame's pixel
//      (point-sampled — see the file's debt note, same simplification the
//      denoiser chain's reprojection uses).
//   2. 3x3 neighborhood min/max clamp of the history sample against the
//      CURRENT frame's local color range (Karis 2014 "High Quality
//      Temporal Supersampling") — the standard ghosting/disocclusion
//      mitigation that lets TAA skip a separate depth/normal test.
//   3. Blend the clamped history with the current frame at a fixed
//      alpha = 0.1 (the frozen spec's default — mostly history, slow
//      convergence, matches the jittered-camera-sequence's intent of
//      accumulating sub-pixel offsets over many frames).
// The camera jitter itself is applied upstream, once, in
// camera_ray.slang's BuildCameraRay (gSampling.jitterX/Y) — every pass
// that traces or reconstructs the primary ray already sees the SAME
// jittered ray for a given pixel+frame, so "gCurrentColor" here already
// reflects this frame's jittered sample.
//
// A COMPUTE pass — no TraceRay, no SceneBindings (screen-space only).
// Bindings (single Set 0):
//   0 : Texture2D<float4> gCurrentColor (SRV) — CompositePass's linearColor
//   1 : Texture2D<float4> gPrevHistory  (SRV) — TaaPass's own history[prev]
//   2 : Texture2D<float2> gMotionVector (SRV, GBuffer guide)
//   3 : RWTexture2D<float4> gOutHistory (UAV) — TaaPass's own history[curr]
//   4 : ConstantBuffer<TaaUniforms>

#include "ShaderInterop.slang"

[[vk::binding(0, 0)]] Texture2D<float4>   gCurrentColor;
[[vk::binding(1, 0)]] Texture2D<float4>   gPrevHistory;
[[vk::binding(2, 0)]] Texture2D<float2>   gMotionVector;
[[vk::binding(3, 0)]] RWTexture2D<float4> gOutHistory;
[[vk::binding(4, 0)]] ConstantBuffer<TaaUniforms> gParams;

[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID) {
    const uint2 pixel = dispatchThreadId.xy;
    if (pixel.x >= gParams.destWidth || pixel.y >= gParams.destHeight)
        return;

    const float3 current = gCurrentColor[pixel].rgb;

    float3 blended = current;
    if (gParams.hasHistory != 0u) {
        const float2 motionVector = gMotionVector[pixel];
        const float2 prevPixelF = float2(pixel) + 0.5 - motionVector;
        const int2 prevPixel = int2(floor(prevPixelF));
        const bool insideBounds = prevPixel.x >= 0 && prevPixel.y >= 0
                                && prevPixel.x < int(gParams.destWidth)
                                && prevPixel.y < int(gParams.destHeight);
        if (insideBounds) {
            const float3 historyColor = gPrevHistory[uint2(prevPixel)].rgb;

            // 3x3 neighborhood clamp — bounds the history sample to the
            // CURRENT frame's local color range so a disoccluded/ghosted
            // history value can't survive the blend.
            float3 neighborMin = current;
            float3 neighborMax = current;
            [loop]
            for (int dy = -1; dy <= 1; ++dy) {
                [loop]
                for (int dx = -1; dx <= 1; ++dx) {
                    if (dx == 0 && dy == 0)
                        continue;
                    const int2 neighborSigned = int2(pixel) + int2(dx, dy);
                    if (neighborSigned.x < 0 || neighborSigned.y < 0
                        || neighborSigned.x >= int(gParams.destWidth)
                        || neighborSigned.y >= int(gParams.destHeight))
                        continue;
                    const float3 neighborColor = gCurrentColor[uint2(neighborSigned)].rgb;
                    neighborMin = min(neighborMin, neighborColor);
                    neighborMax = max(neighborMax, neighborColor);
                }
            }
            const float3 historyClamped = clamp(historyColor, neighborMin, neighborMax);
            blended = lerp(historyClamped, current, gParams.blendAlpha);
        }
    }

    gOutHistory[pixel] = float4(blended, 1.0);
}
