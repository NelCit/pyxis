// Pyxis renderer — DlssPass utility: in-place RGB exposure scale
// (RTX-alignment 2026-07-11, DLSS-RR pre-exposure).
//
// DLSSDOptions carries NO useAutoExposure/kBufferTypeExposure hook (unlike
// the SR path), so Ray Reconstruction always normalizes raw HDR input with
// its own internal tone handling — and the World Lobby's 0–12000-nit
// linear range sits far outside its trained domain (measured: broad
// +0.02..+0.10 luma inflation across materials, whole-frame 0.09294 vs the
// builtin 0.07958). ovrtx feeds its denoisers PRE-EXPOSED buffers (its
// maxRayUnexposedIntensity setting divides by the exposure scale — the
// buffers themselves are exposed), so DlssPass does the same for RR:
// multiply colorIn's RGB by ComputeEffectiveExposureScale before
// slEvaluateFeature, divide the reconstructed output by it after
// (TonemapPass downstream still applies the same gain itself — net
// identity outside RR's nonlinearity).
//
// In-place is safe: each thread reads and writes only its own texel, and
// the texture (CompositePass/AccumulationPass's linearColor, or DlssPass's
// own output) is fully re-produced by its owning pass every frame.
//
// Second utility shader owned by DlssPass (alongside dlss_depth_convert
// .slang) — the "one shader per pass" convention reads as "per dispatch
// purpose" here, same as the depth converter.
//
// Bindings — single Set 0:
//   0 : RWTexture2D<float4>                gColor (in-place RGB scale)
//   1 : ConstantBuffer<DlssExposeParams>   gParams

#include "ShaderInterop.slang"

// Byte-for-byte lockstep with DlssPass.cpp's local `struct DlssExposeParams`
// (16 bytes).
struct DlssExposeParams
{
    uint  width;
    uint  height;
    float scale;   // exposureGain, or 1/exposureGain for the inverse dispatch
    float _pad0;
};

[[vk::binding(0, 0)]] RWTexture2D<float4>              gColor;
[[vk::binding(1, 0)]] ConstantBuffer<DlssExposeParams> gParams;

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (dispatchThreadId.x >= gParams.width || dispatchThreadId.y >= gParams.height)
        return;
    const int2 pixel = int2(dispatchThreadId.xy);
    const float4 c = gColor[pixel];
    gColor[pixel] = float4(c.rgb * gParams.scale, c.a);
}
