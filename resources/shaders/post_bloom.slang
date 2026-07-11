// Pyxis renderer — PostBloomPass, ONE shader file, single compute entry
// dispatched twice per frame (RTX-alignment 2026-07-11, "window borders
// shadowed" — ovrtx veiling-halo match).
//
// ovrtx's reference output carries a wide veiling halo around blown-out
// highlights (its denoiser/DLSS-RR bleeds bright-window energy ~10-30px
// into adjacent geometry; ring-profiled on the World Lobby: pyx sits
// -0.055 luma below ovrtx 13-24px from blown sky while matching at the
// clipped edge). This pass reproduces it as a separable Gaussian bloom on
// the post-tonemap display-linear color:
//
//   halo = GaussianBlur_sigma24( color where luminance > 0.8 )
//   out  = color + gain * halo
//
// THRESHOLD (0.8, display-linear luminance) and SIGMA (24 px) are fixed
// constants below — the measured optimum band (numpy prototype sweep,
// design doc 2026-07-11: gain 0.10 takes whole-frame honest-sRGB RMSE
// 0.07958 -> 0.07877 on top of postSoftenSigma 0.5 and halves the
// window-ring deficit). RealTimeQuality::postBloomGain is the one dial;
// 0 (default) disables the pass entirely (PostBloomPass::Execute
// early-outs — byte-identical output, goldens untouched).
//
// Two dispatches of this one entry, selected by gParams.mode:
//   mode 0 (EXTRACT_BLUR_H): read gSrcColor (the display color), write
//     gOut = horizontally-blurred thresholded highlights.
//   mode 1 (BLUR_V_COMPOSITE): read gSrcColor (= the mode-0 output) for
//     the vertical blur leg, read gBaseColor (the display color), write
//     gOut = base + gain * blurredHalo. The pass then copyTexture's gOut
//     back over the display color (same owned-temp shape PostSoftenPass
//     uses — downstream consumers keep reading targets->color).
//
// Edge handling matches the numpy prototype exactly: out-of-bounds taps
// contribute zero but the kernel normalization stays FULL-kernel (np
// 'same' convolution semantics) — the halo legitimately dims at the
// frame border instead of renormalizing.
//
// Bindings — single Set 0:
//   0 : Texture2D<float4>   gSrcColor  (SRV — blur source for this leg)
//   1 : Texture2D<float4>   gBaseColor (SRV — display color; mode 1 only)
//   2 : RWTexture2D<float4> gOut       (UAV — owned temp)
//   3 : ConstantBuffer<PostBloomParams> gParams

#include "ShaderInterop.slang"

static const float BLOOM_SIGMA = 24.0;
static const int   BLOOM_RADIUS = 72;          // 3 sigma
static const float BLOOM_THRESHOLD = 0.8;      // display-linear luminance

// Byte-for-byte lockstep with PostBloomPass.cpp's local mirror (16 bytes).
struct PostBloomParams
{
    uint  width;
    uint  height;
    float gain;   // composite gain (mode 1); unused in mode 0
    uint  mode;   // 0 = extract + horizontal blur, 1 = vertical blur + composite
};

[[vk::binding(0, 0)]] Texture2D<float4>              gSrcColor;
[[vk::binding(1, 0)]] Texture2D<float4>              gBaseColor;
[[vk::binding(2, 0)]] RWTexture2D<float4>            gOut;
[[vk::binding(3, 0)]] ConstantBuffer<PostBloomParams> gParams;

float Luminance(float3 c)
{
    return dot(c, float3(0.2126, 0.7152, 0.0722));
}

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (dispatchThreadId.x >= gParams.width || dispatchThreadId.y >= gParams.height)
        return;
    const int2 pixel = int2(dispatchThreadId.xy);
    const int2 dir = (gParams.mode == 0u) ? int2(1, 0) : int2(0, 1);

    // Full-kernel normalization computed inline (145 exp() per pixel is
    // cheap next to the 145 texture loads, and it keeps the weights
    // byte-equivalent to the numpy prototype's discretely-normalized
    // kernel regardless of sigma/radius edits).
    float weightSum = 0.0;
    [loop]
    for (int i = -BLOOM_RADIUS; i <= BLOOM_RADIUS; ++i)
        weightSum += exp(-float(i * i) / (2.0 * BLOOM_SIGMA * BLOOM_SIGMA));

    float3 sum = float3(0.0, 0.0, 0.0);
    [loop]
    for (int i = -BLOOM_RADIUS; i <= BLOOM_RADIUS; ++i)
    {
        const int2 tap = pixel + dir * i;
        if (tap.x < 0 || tap.y < 0 || tap.x >= int(gParams.width)
            || tap.y >= int(gParams.height))
            continue;   // zero-pad ('same' convolution) — no renormalize.
        const float w = exp(-float(i * i) / (2.0 * BLOOM_SIGMA * BLOOM_SIGMA));
        float3 v = gSrcColor[tap].rgb;
        if (gParams.mode == 0u)
            v = (Luminance(v) > BLOOM_THRESHOLD) ? v : float3(0.0, 0.0, 0.0);
        sum += w * v;
    }
    sum /= weightSum;

    if (gParams.mode == 0u)
    {
        gOut[pixel] = float4(sum, 0.0);
    }
    else
    {
        const float4 base = gBaseColor[pixel];
        gOut[pixel] = float4(base.rgb + gParams.gain * sum, base.a);
    }
}
