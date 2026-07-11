// Pyxis renderer — DenoiseAtrousPass, ONE shader file, single compute
// entry dispatched 5 TIMES per frame, ping-ponging between two scratch
// buffers (RTX-alignment design, rtx-realtime-alignment-design.md, Phase B
// denoiser chain).
//
// Edge-stopping à-trous wavelet filter (Dammertz et al. 2010, the
// technique SVGF/ReLAX/REBLUR's spatial stage is built on) — the FINAL
// stage of the ReLAX-class chain, consuming DenoiseHistoryFixPass's
// output. Each of the 5 iterations dilates its 3x3 tap pattern by
// `stepSize` (1, 2, 4, 8, 16 — the classic à-trous "holes" progression,
// approximating a much larger true kernel with a constant tap count).
// Edge-stopping weights (per the frozen spec):
//   * luminance: exp(-|lumCenter - lumSample| / phi), phi = diffPhiLuminance
//     (2.0) for the diffuse channel, specPhiLuminance (1.0) for specular —
//     NRD default targets.
//   * normal: pow(saturate(dot(Ncenter, Nsample)), normalPower), power in
//     the spec's ~8-32 range; the specular channel's power is
//     ROUGHNESS-AWARE (lower power = wider tolerance on rough surfaces,
//     where the reflection lobe itself is broad) per "Roughness-aware
//     kernel for specular".
//   * depth: exp(-|viewZcenter - viewZsample| / (0.003 * viewZcenter)) —
//     same 0.003 relative plane-distance threshold DenoiseTemporalPass
//     uses for disocclusion.
// Simplification vs. full SVGF (tracked as debt): no per-pixel VARIANCE
// estimate scales phi — DenoiseTemporalPass's simplified single-
// accumulator (see its file header) doesn't track the 2nd luminance
// moment SVGF needs for that; the fixed phi constants above are the
// frozen spec's own literal targets, used directly.
//
// Halo-fix note (rtx-realtime-alignment-design.md, "denoiser chain" /
// white-fringe forensics, 2026-07-05): normal + depth + luminance
// edge-stopping isn't sufficient on its own — two surfaces of very
// different material can be near-coplanar (similar normal/depth) at a
// silhouette, and this filter's dilation grows to a total footprint radius
// of 1+2+4+8+16=31 px by the last iteration. Added a HARD material-id
// reject (gMaterialId) alongside the existing weights.
//
// A COMPUTE pass — no TraceRay, no SceneBindings (screen-space only).
// Bindings (single Set 0):
//   0 : Texture2D<float4> gInDiffuse   (SRV) — ping-ponged input
//   1 : Texture2D<float4> gInSpecular  (SRV) — ping-ponged input
//   2 : Texture2D<float4> gNormalRoughness (SRV, GBuffer guide)
//   3 : Texture2D<float>  gViewZ       (SRV, GBuffer guide)
//   4 : RWTexture2D<float4> gOutDiffuse  (UAV) — ping-ponged output
//   5 : RWTexture2D<float4> gOutSpecular (UAV) — ping-ponged output
//   6 : ConstantBuffer<DenoiseAtrousUniforms>
//   7 : Texture2D<uint>    gMaterialId  (SRV, GBuffer guide; ~0u on miss —
//                                          1x1 zero-filled fallback when
//                                          the caller doesn't wire
//                                          materialIdAov, see
//                                          DenoiseAtrousPass.cpp)

#include "ShaderInterop.slang"

[[vk::binding(0, 0)]] Texture2D<float4>   gInDiffuse;
[[vk::binding(1, 0)]] Texture2D<float4>   gInSpecular;
[[vk::binding(2, 0)]] Texture2D<float4>   gNormalRoughness;
[[vk::binding(3, 0)]] Texture2D<float>    gViewZ;
[[vk::binding(4, 0)]] RWTexture2D<float4> gOutDiffuse;
[[vk::binding(5, 0)]] RWTexture2D<float4> gOutSpecular;
[[vk::binding(6, 0)]] ConstantBuffer<DenoiseAtrousUniforms> gParams;
[[vk::binding(7, 0)]] Texture2D<uint>     gMaterialId;

float AtrousLuminance(float3 c) {
    return dot(c, float3(0.2126, 0.7152, 0.0722));
}

// 3x3 binomial-ish kernel weight, indexed by tap offset in {-1,0,1}^2.
float AtrousKernelWeight(int dx, int dy) {
    const float wx = (dx == 0) ? 2.0 : 1.0;
    const float wy = (dy == 0) ? 2.0 : 1.0;
    return wx * wy;  // center=4, edge-mid=2, corner=1 — sums to 16.
}

[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID) {
    const uint2 pixel = dispatchThreadId.xy;
    if (pixel.x >= gParams.destWidth || pixel.y >= gParams.destHeight)
        return;

    const float4 centerDiffuse = gInDiffuse[pixel];
    const float4 centerSpecular = gInSpecular[pixel];
    const float4 centerGuide = gNormalRoughness[pixel];
    const float3 centerNormal = centerGuide.xyz;
    const float centerRoughness = centerGuide.w;
    const float centerViewZ = gViewZ[pixel];
    const float centerLumDiffuse = AtrousLuminance(centerDiffuse.rgb);
    const float centerLumSpecular = AtrousLuminance(centerSpecular.rgb);
    const uint centerMaterialId = gMaterialId[pixel];

    // Roughness-aware specular normal power: smooth (roughness -> 0)
    // surfaces get a tight, near-mirror-exact kernel (power -> 32); rough
    // surfaces tolerate more normal deviation (power -> 4), matching how
    // a broad GGX lobe is itself insensitive to small normal changes.
    const float specularNormalPower = lerp(32.0, 4.0, saturate(centerRoughness));
    // MEASURED NON-LEVER (RTX-alignment 2026-07-11, "scene too noisy"
    // forensics — full trail in the design doc): loosening this to 1.0 was
    // metric-neutral (0.07958 -> 0.07956) AND visually neutral on the wall
    // mottle, because the mottle is converged bump-lighting response with
    // the SAME band energy as ovrtx at every scale (1-32px) — the gap is
    // PHASE COHERENCE (their detail is structured, ours random-phase),
    // which no energy-domain edge-stop can alter. Do not re-tune this
    // expecting smoothness; the source-side fix is Toksvig/LEAN-style
    // normal-map filtering (flatten + roughness-compensate at distance).
    const float diffuseNormalPower = 32.0;

    const int step = int(max(gParams.stepSize, 1u));
    float3 diffuseSum = float3(0.0, 0.0, 0.0);
    float3 specularSum = float3(0.0, 0.0, 0.0);
    float diffuseWeightSum = 0.0;
    float specularWeightSum = 0.0;

    [loop]
    for (int dy = -1; dy <= 1; ++dy) {
        [loop]
        for (int dx = -1; dx <= 1; ++dx) {
            const int2 samplePixelSigned = int2(pixel) + int2(dx, dy) * step;
            if (samplePixelSigned.x < 0 || samplePixelSigned.y < 0
                || samplePixelSigned.x >= int(gParams.destWidth)
                || samplePixelSigned.y >= int(gParams.destHeight))
                continue;
            const uint2 samplePixel = uint2(samplePixelSigned);

            const float4 sampleGuide = gNormalRoughness[samplePixel];
            const float3 sampleNormal = sampleGuide.xyz;
            const float sampleViewZ = gViewZ[samplePixel];
            const float kernelWeight = AtrousKernelWeight(dx, dy);
            const float depthWeight =
                exp(-abs(sampleViewZ - centerViewZ) / max(0.003 * centerViewZ, 1e-4));
            // Halo fix (file header) — hard reject across a material
            // boundary; normal/depth/luminance similarity alone can't
            // catch a near-coplanar silhouette between two materials.
            const float materialWeight =
                (gMaterialId[samplePixel] == centerMaterialId) ? 1.0 : 0.0;

            const float4 sampleDiffuse = gInDiffuse[samplePixel];
            const float diffuseLumWeight =
                exp(-abs(AtrousLuminance(sampleDiffuse.rgb) - centerLumDiffuse)
                    / max(gParams.diffPhiLuminance, 1e-3));
            const float diffuseNormalWeight =
                pow(saturate(dot(centerNormal, sampleNormal)), diffuseNormalPower);
            const float diffuseWeight =
                kernelWeight * depthWeight * diffuseLumWeight * diffuseNormalWeight * materialWeight;
            diffuseSum += sampleDiffuse.rgb * diffuseWeight;
            diffuseWeightSum += diffuseWeight;

            const float4 sampleSpecular = gInSpecular[samplePixel];
            const float specularLumWeight =
                exp(-abs(AtrousLuminance(sampleSpecular.rgb) - centerLumSpecular)
                    / max(gParams.specPhiLuminance, 1e-3));
            const float specularNormalWeight =
                pow(saturate(dot(centerNormal, sampleNormal)), specularNormalPower);
            const float specularWeight =
                kernelWeight * depthWeight * specularLumWeight * specularNormalWeight
                    * materialWeight;
            specularSum += sampleSpecular.rgb * specularWeight;
            specularWeightSum += specularWeight;
        }
    }

    const float3 filteredDiffuse =
        (diffuseWeightSum > 1e-6) ? (diffuseSum / diffuseWeightSum) : centerDiffuse.rgb;
    const float3 filteredSpecular =
        (specularWeightSum > 1e-6) ? (specularSum / specularWeightSum) : centerSpecular.rgb;

    gOutDiffuse[pixel] = float4(filteredDiffuse, centerDiffuse.a);
    gOutSpecular[pixel] = float4(filteredSpecular, centerSpecular.a);
}
