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
//   2. Variance-clip the history sample against the CURRENT frame's local
//      3x3 neighborhood mean/stddev (Salvi 2016 "An Excursion in Temporal
//      Supersampling"; supersedes Karis 2014's plain min/max box — see
//      work item 3's debugging notes below for why min/max specifically
//      doesn't work for this renderer's still-noisy composite input).
//   3. Blend the clipped history with the current frame.
// The camera jitter itself is applied upstream, once, in
// camera_ray.slang's BuildCameraRay (gSampling.jitterX/Y) — every pass
// that traces or reconstructs the primary ray already sees the SAME
// jittered ray for a given pixel+frame, so "gCurrentColor" here already
// reflects this frame's jittered sample.
//
// Noise-floor + vegetation spec (rtx-realtime-alignment-design.md,
// 2026-07-06), work item 3 — adaptive blend, two debugged iterations:
//   * The blend was a FIXED alpha = 0.1 (mostly history, every frame,
//     regardless of confidence). Now alpha is confidence-driven: the
//     history texture's alpha channel carries a running history LENGTH
//     (same 1/historyLength shape the ReLAX denoiser chain uses), so a
//     pixel that has reprojected cleanly for many frames converges alpha
//     down toward `gParams.alphaMin` (NRD-default-derived floor, ~1/32),
//     while a freshly-disoccluded/rejected pixel takes alpha = 1 (pure
//     current frame, no history contribution at all).
//   * DEBUGGING NOTE 1 (measured empirically): the first cut used Karis's
//     plain min/max box (8 neighbors) as BOTH the clamp and the rejection
//     signal — "how far the clamp had to pull the value back". That
//     measurably made the N=96/N=1 noise-proxy ratio WORSE (44% vs a 32%
//     baseline), because `gCurrentColor` here is CompositePass's
//     recombination of direct lighting that is STILL genuinely Monte-
//     Carlo-noisy per frame (RIS 2-sample estimator, SIGMA is spatial-
//     only, no temporal accumulation on that signal) — exactly the noise
//     TAA's temporal accumulation exists to remove. An 8-neighbor min/max
//     box from a SINGLE noisy frame is almost always narrower than the
//     legitimate frame-to-frame variance of that same signal, so nearly
//     every pixel's history "excursion" looked large relative to that
//     tiny box and got misclassified as disocclusion, collapsing alpha
//     back to 1 (no history) almost every frame — the opposite of the
//     intended effect. Fix: variance clipping (mean ± γσ, γ=3), which
//     uses the neighborhood's OWN spread as the yardstick instead of its
//     extremes — a much more forgiving/correct rejection threshold for
//     noisy input.
//   * DEBUGGING NOTE 2 (measured empirically): the SAME iteration also
//     added Karis 2014's follow-up "tone-compressed clamping"
//     (c' = c / (1 + luma(c)), inverted after the blend) to address the
//     "documented TAA debt" HDR-ringing note below — this is the
//     textbook fix for GAME-HDR content (luma typically 0-10ish). It
//     broke correctness here instead: `gCurrentColor` is this renderer's
//     UNEXPOSED physical linear radiance, which measured mean luma ~45
//     and max ~110 on the World Lobby test scene (exposure/tonemap apply
//     LATER, in TonemapPass) — c/(1+luma) crushes essentially the ENTIRE
//     image into the top ~2% of the [0,1) compressed range (luma=45 ->
//     compressed luma 0.978), where the expand function's derivative is
//     enormous, so ordinary fp32 rounding in the intervening lerp/clamp
//     arithmetic blew up into a near-total image blackout starting the
//     SECOND accumulated frame (measured: mean radiance dropped 10x from
//     frame 1 to frame 2 and stayed collapsed). Karis's compression needs
//     an exposure-scaled input to land in its useful range; retrofitting
//     that here is out of scope for this work item (TonemapPass's
//     exposure model would need to be threaded into this compute-only
//     pass). Fix: dropped tone-compression, clip/blend directly in linear
//     HDR space instead — see the "documented TAA debt" note below for
//     what's still owed.
//
// Documented TAA debt (unresolved, tracked forward): the design doc's
// "TAA raw-HDR clamp = ringing debt at emissive edges" note is NOT fixed
// by this work item — see debugging note 2 above for why the
// straightforward fix doesn't apply cleanly to this renderer's unexposed-
// radiance signal. An exposure-aware compression (thread TonemapPass's
// exposure scale into TaaUniforms, compress in STOPS/log space rather
// than raw radiance) is the correct follow-up.
//
// A COMPUTE pass — no TraceRay, no SceneBindings (screen-space only).
// Bindings (single Set 0):
//   0 : Texture2D<float4> gCurrentColor (SRV) — CompositePass's linearColor
//   1 : Texture2D<float4> gPrevHistory  (SRV) — TaaPass's own history[prev]
//                                                (.a = history length)
//   2 : Texture2D<float2> gMotionVector (SRV, GBuffer guide)
//   3 : RWTexture2D<float4> gOutHistory (UAV) — TaaPass's own history[curr]
//   4 : ConstantBuffer<TaaUniforms>

#include "ShaderInterop.slang"

[[vk::binding(0, 0)]]  Texture2D<float4>   gCurrentColor;
[[vk::binding(1, 0)]]  Texture2D<float4>   gPrevHistory;
[[vk::binding(2, 0)]]  Texture2D<float2>   gMotionVector;
[[vk::binding(3, 0)]]  RWTexture2D<float4> gOutHistory;
[[vk::binding(4, 0)]]  ConstantBuffer<TaaUniforms> gParams;

// NRD-default-derived: the history-length cap alpha=1/historyLength
// converges against (same order of magnitude as ReLAX's own slow-history
// window). GAMMA is the variance-clip width in local standard deviations
// (Salvi 2016 default range 1-3; 3 is the permissive end, chosen so
// ordinary per-frame Monte-Carlo noise on the still-noisy direct-lighting
// term doesn't get misread as disocclusion — see debugging note 1 above).
// REJECT_THRESHOLD is the fraction of that ±GAMMA*sigma band a sample
// must overshoot before it counts as rejected.
static const float TAA_HISTORY_MAX = 32.0;
static const float TAA_VARIANCE_CLIP_GAMMA = 3.0;
static const float TAA_REJECT_THRESHOLD = 0.9;

[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID) {
    const uint2 pixel = dispatchThreadId.xy;
    if (pixel.x >= gParams.destWidth || pixel.y >= gParams.destHeight)
        return;

    const float3 current = gCurrentColor[pixel].rgb;

    float3 blended = current;
    float outHistoryLen = 1.0;

    if (gParams.hasHistory != 0u) {
        const float2 motionVector = gMotionVector[pixel];
        const float2 prevPixelF = float2(pixel) + 0.5 - motionVector;
        const int2 prevPixel = int2(floor(prevPixelF));
        const bool insideBounds = prevPixel.x >= 0 && prevPixel.y >= 0
                                && prevPixel.x < int(gParams.destWidth)
                                && prevPixel.y < int(gParams.destHeight);
        if (insideBounds) {
            const float4 prevSample = gPrevHistory[uint2(prevPixel)];
            const float3 historyColor = prevSample.rgb;
            const float prevHistoryLen = prevSample.a;

            // 3x3 neighborhood mean/variance (linear HDR), including the
            // center pixel — the statistics the variance clip and the
            // rejection score below are both derived from.
            float3 sum = float3(0.0, 0.0, 0.0);
            float3 sumSq = float3(0.0, 0.0, 0.0);
            float count = 0.0;
            [loop]
            for (int dy = -1; dy <= 1; ++dy) {
                [loop]
                for (int dx = -1; dx <= 1; ++dx) {
                    const int2 neighborSigned = int2(pixel) + int2(dx, dy);
                    if (neighborSigned.x < 0 || neighborSigned.y < 0
                        || neighborSigned.x >= int(gParams.destWidth)
                        || neighborSigned.y >= int(gParams.destHeight))
                        continue;
                    const float3 neighborColor = gCurrentColor[uint2(neighborSigned)].rgb;
                    sum += neighborColor;
                    sumSq += neighborColor * neighborColor;
                    count += 1.0;
                }
            }
            const float3 mean = sum / max(count, 1.0);
            const float3 variance = max(sumSq / max(count, 1.0) - mean * mean, 0.0);
            const float3 stddev = sqrt(variance);

            const float3 boxHalfRange = max(TAA_VARIANCE_CLIP_GAMMA * stddev, 1e-4);
            const float3 boxMin = mean - boxHalfRange;
            const float3 boxMax = mean + boxHalfRange;
            const float3 historyClamped = clamp(historyColor, boxMin, boxMax);

            // Confidence-driven alpha (work item 3): the fraction of the
            // variance-clip band a sample had to be pulled back by IS the
            // reprojection-error / disocclusion signal — reused here as an
            // explicit rejection score instead of only silently clipping.
            const float3 clampExcess = abs(historyColor - historyClamped);
            const float rejection = saturate(max(
                max(clampExcess.r / boxHalfRange.r, clampExcess.g / boxHalfRange.g),
                clampExcess.b / boxHalfRange.b));
            const bool rejected = rejection > TAA_REJECT_THRESHOLD;

            const float historyLen = rejected ? 1.0 : min(prevHistoryLen + 1.0, TAA_HISTORY_MAX);
            const float alpha = rejected ? 1.0 : max(1.0 / historyLen, gParams.alphaMin);

            blended = lerp(historyClamped, current, alpha);
            outHistoryLen = historyLen;
        }
    }

    gOutHistory[pixel] = float4(blended, outHistoryLen);
}
