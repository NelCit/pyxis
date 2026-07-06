// Pyxis renderer — DenoiseHistoryFixPass, ONE shader file, single compute
// entry (RTX-alignment design, rtx-realtime-alignment-design.md, Phase B
// denoiser chain).
//
// For pixels whose DenoiseTemporalPass-accumulated history length (the
// `.a` channel of its diffuse/specular output) is below
// `HISTORY_FIX_THRESHOLD` (3 frames — NRD's historyFixFrameNum default
// target), widen the spatial sample stride to pull in more neighboring
// values and fill disocclusions with spatial information before the
// à-trous pass runs. Pixels already past the threshold pass through
// unchanged — DenoiseTemporalPass's own temporal accumulation is already
// doing the smoothing for those. Diffuse and specular are fixed
// independently (their history lengths can differ after a partial
// disocclusion).
//
// Halo-fix note (rtx-realtime-alignment-design.md, "denoiser chain" /
// white-fringe forensics, 2026-07-05): `WidenedBoxAverage` used to be a
// plain, unguarded box average — the widest tap radius in the whole
// denoiser chain (stride up to HISTORY_FIX_MAX_STRIDE=14) with NO edge
// stop of any kind (not even normal/depth). Combined with camera jitter,
// pixels sitting exactly on a silhouette (e.g. the World Lobby's black
// vase against the bright floor) alternate which surface they hit
// frame-to-frame — DenoiseTemporalPass correctly disoccludes them every
// single frame (different normal/viewZ each time), which means THIS pass'
// wide, previously-unguarded box blur ran on them EVERY frame of a
// multi-hundred-frame accumulation, freely averaging a dark object's
// signal into a bright neighbor's (or vice versa) up to 14 pixels away —
// the dominant contributor to the reported white/bright halo. Gated now
// on the G-buffer material id (gMaterialId): a tap whose material slot
// differs from the center pixel's contributes nothing.
//
// A COMPUTE pass — no TraceRay, no SceneBindings (screen-space only).
// Bindings (single Set 0):
//   0 : Texture2D<float4> gInDiffuse   (SRV, DenoiseTemporalPass output)
//   1 : Texture2D<float4> gInSpecular  (SRV, DenoiseTemporalPass output)
//   2 : RWTexture2D<float4> gOutDiffuse  (UAV)
//   3 : RWTexture2D<float4> gOutSpecular (UAV)
//   4 : ConstantBuffer<DenoiseHistoryFixUniforms>
//   5 : Texture2D<uint> gMaterialId (SRV, GBuffer guide; ~0u on miss — 1x1
//       zero-filled fallback when the caller doesn't wire materialIdAov,
//       see DenoiseHistoryFixPass.cpp)

#include "ShaderInterop.slang"

[[vk::binding(0, 0)]] Texture2D<float4>   gInDiffuse;
[[vk::binding(1, 0)]] Texture2D<float4>   gInSpecular;
[[vk::binding(2, 0)]] RWTexture2D<float4> gOutDiffuse;
[[vk::binding(3, 0)]] RWTexture2D<float4> gOutSpecular;
[[vk::binding(4, 0)]] ConstantBuffer<DenoiseHistoryFixUniforms> gParams;
[[vk::binding(5, 0)]] Texture2D<uint>     gMaterialId;

static const float HISTORY_FIX_THRESHOLD = 3.0;
static const float HISTORY_FIX_MAX_STRIDE = 14.0;
static const float HISTORY_FIX_MIN_STRIDE = 1.0;

// A small 3x3 sparse-tap box average at the given pixel stride — cheap
// spatial fill for a disoccluded (low-history) pixel. `channel` selects
// which texture to sample (0 = diffuse, 1 = specular) so both channels
// share this one helper.
//
// Halo fix (file header) — `centerMaterialId` gates every tap: a stride-14
// box average with no material discrimination is exactly how a dark
// object's signal (WHICH, for the direct-diffuse channel upstream, is
// itself already amplified by DemodulateByAlbedo's divide-by-albedo — see
// shading.slang) spreads across a silhouette into a bright neighbor 14
// pixels away. Skipped taps are excluded from the average entirely (not
// zero-weighted-but-counted), same shape as the existing bounds check.
float3 WidenedBoxAverage(Texture2D<float4> tex, uint2 centerPixel, float stride, uint2 dims,
                         uint centerMaterialId) {
    float3 sum = float3(0.0, 0.0, 0.0);
    float weight = 0.0;
    const int step = max(1, int(round(stride)));
    [loop]
    for (int dy = -1; dy <= 1; ++dy) {
        [loop]
        for (int dx = -1; dx <= 1; ++dx) {
            const int2 offsetPixel = int2(centerPixel) + int2(dx, dy) * step;
            if (offsetPixel.x < 0 || offsetPixel.y < 0 || offsetPixel.x >= int(dims.x)
                || offsetPixel.y >= int(dims.y))
                continue;
            const uint2 samplePixel = uint2(offsetPixel);
            if (gMaterialId[samplePixel] != centerMaterialId)
                continue;
            sum += tex[samplePixel].rgb;
            weight += 1.0;
        }
    }
    return (weight > 0.0) ? (sum / weight) : tex[centerPixel].rgb;
}

[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID) {
    const uint2 pixel = dispatchThreadId.xy;
    if (pixel.x >= gParams.destWidth || pixel.y >= gParams.destHeight)
        return;

    const uint2 dims = uint2(gParams.destWidth, gParams.destHeight);
    const float4 centerDiffuse = gInDiffuse[pixel];
    const float4 centerSpecular = gInSpecular[pixel];
    const uint centerMaterialId = gMaterialId[pixel];

    float3 outDiffuseColor = centerDiffuse.rgb;
    if (centerDiffuse.a < HISTORY_FIX_THRESHOLD) {
        const float t = saturate(centerDiffuse.a / HISTORY_FIX_THRESHOLD);
        const float stride = lerp(HISTORY_FIX_MAX_STRIDE, HISTORY_FIX_MIN_STRIDE, t);
        outDiffuseColor = WidenedBoxAverage(gInDiffuse, pixel, stride, dims, centerMaterialId);
    }

    float3 outSpecularColor = centerSpecular.rgb;
    if (centerSpecular.a < HISTORY_FIX_THRESHOLD) {
        const float t = saturate(centerSpecular.a / HISTORY_FIX_THRESHOLD);
        const float stride = lerp(HISTORY_FIX_MAX_STRIDE, HISTORY_FIX_MIN_STRIDE, t);
        outSpecularColor = WidenedBoxAverage(gInSpecular, pixel, stride, dims, centerMaterialId);
    }

    // History length passes through unchanged — only the radiance gets
    // spatially filled; DenoiseAtrousPass's edge-stopping reads it next.
    gOutDiffuse[pixel] = float4(outDiffuseColor, centerDiffuse.a);
    gOutSpecular[pixel] = float4(outSpecularColor, centerSpecular.a);
}
