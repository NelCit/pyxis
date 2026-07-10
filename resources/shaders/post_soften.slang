// Pyxis renderer — PostSoftenPass, ONE shader file, single compute entry
// (RTX-alignment 2026-07-10, "image not smooth" — ovrtx output-character
// match).
//
// A small display-space Gaussian (radius 2, separable weights folded into
// a single 5x5 dispatch) applied to the post-tonemap display color when
// RealTimeQuality::postSoftenSigma > 0. ovrtx's reference output is
// DLSS-processed and measurably softer than Pyxis's raw-sharp converged
// frames at the texture-detail frequency band; this reproduces that
// character (measured on the World Lobby vs the ovrtx RT-wide capture:
// sigma 0.5 takes whole-frame honest-sRGB RMSE 0.08240 -> 0.07956 and
// lands the wall high-frequency statistic at 0.0925 vs ovrtx's 0.0959,
// from 0.1051 unsoftened). sigma = 0 disables the pass entirely
// (PostSoftenPass::Execute early-outs before any GPU work) — the default,
// byte-identical output.
//
// The 1D weights arrive PRE-NORMALIZED from the CPU (PostSoftenPass
// computes w[i] = exp(-i^2 / (2 sigma^2)), i in 0..2, normalized so the
// 5-tap sum is 1) — the shader only forms the separable products, so the
// kernel always sums to exactly 1 regardless of sigma (no energy gain or
// loss). Edge pixels clamp their taps to the image rect and renormalize
// by the actually-summed weight, matching the numpy measurement's 'same'
// convolution behaviour closely enough at radius 2.
//
// Bindings — single Set 0:
//   0 : Texture2D<float4>              gColorIn  (SRV — the display color)
//   1 : RWTexture2D<float4>            gColorOut (UAV — the owned temp;
//        the pass copyTexture's it back over the display color)
//   2 : ConstantBuffer<PostSoftenParams> gParams

#include "ShaderInterop.slang"

// Hand-rolled cbuffer layout, kept in byte-for-byte lockstep with
// PostSoftenPass.cpp's own local `struct PostSoftenParams` (16 bytes —
// §10/§23 default cbuffer alignment satisfied by construction).
struct PostSoftenParams
{
    uint  width;
    uint  height;
    float weight0;  // centre tap
    float weight1;  // +-1 tap
};
// weight2 (+-2 tap) is derived in-shader: the 5-tap normalization means
// w0 + 2 w1 + 2 w2 == 1, so w2 = (1 - w0 - 2 w1) / 2 — keeps the cbuffer
// at exactly 16 bytes with no padding games.

[[vk::binding(0, 0)]] Texture2D<float4>              gColorIn;
[[vk::binding(1, 0)]] RWTexture2D<float4>            gColorOut;
[[vk::binding(2, 0)]] ConstantBuffer<PostSoftenParams> gParams;

// The blur runs in sRGB-ENCODED space — encode each tap, average, decode
// back to the display-linear values this target stores. The reference
// character (ovrtx's DLSS output) and the tuning measurement both live in
// the final sRGB image; blurring display-linear instead over-weights
// bright pixels through the encode and measurably gives back half the
// gain (in-engine World Lobby, sigma 0.5: linear-space blur 0.08095 vs
// sRGB-space blur matching the numpy-measured 0.0796 band). Exact
// piecewise IEC 61966-2-1 pair, matching the metric script's own OETF.
float3 SrgbEncode(float3 c)
{
    c = clamp(c, 0.0, 1.0);
    const float3 lo = c * 12.92;
    const float3 hi = 1.055 * pow(c, 1.0 / 2.4) - 0.055;
    return select(c <= 0.0031308, lo, hi);
}

float3 SrgbDecode(float3 s)
{
    const float3 lo = s / 12.92;
    const float3 hi = pow((s + 0.055) / 1.055, 2.4);
    return select(s <= 0.04045, lo, hi);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (dispatchThreadId.x >= gParams.width || dispatchThreadId.y >= gParams.height)
        return;

    const int2 pixel = int2(dispatchThreadId.xy);
    const float weight2 = max(0.0, (1.0 - gParams.weight0 - 2.0 * gParams.weight1) * 0.5);
    const float weights[5] = {weight2, gParams.weight1, gParams.weight0, gParams.weight1, weight2};

    float3 sum = float3(0.0, 0.0, 0.0);
    float alphaSum = 0.0;
    float weightSum = 0.0;
    [unroll]
    for (int dy = -2; dy <= 2; ++dy)
    {
        [unroll]
        for (int dx = -2; dx <= 2; ++dx)
        {
            const int2 tap = pixel + int2(dx, dy);
            if (tap.x < 0 || tap.y < 0 || tap.x >= int(gParams.width)
                || tap.y >= int(gParams.height))
                continue;
            const float w = weights[dx + 2] * weights[dy + 2];
            const float4 c = gColorIn[tap];
            sum += w * SrgbEncode(c.rgb);
            alphaSum += w * c.a;
            weightSum += w;
        }
    }

    gColorOut[pixel] = (weightSum > 1e-6)
        ? float4(SrgbDecode(sum / weightSum), alphaSum / weightSum)
        : gColorIn[pixel];
}
