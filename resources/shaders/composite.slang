// Pyxis renderer — CompositePass, ONE shader file, single compute entry
// (RTX-alignment design, rtx-realtime-alignment-design.md, WP2-final).
//
// Recombines the five Phase A signal passes' outputs into the final linear
// radiance, replacing the retired raytraced_lighting_raygen.slang
// megakernel's in-shader shading:
//
//   diffuseSpec.rgb = gAlbedo.rgb * (gDirectDiffuse.rgb + gIndirectDiffuse.rgb * gAo.r)
//                   + gDirectSpecular.rgb                      (already un-demodulated)
//   final.rgb = gReflectionWeight.rgb * gReflections.rgb
//             + (1 - gReflectionWeight.rgb) * diffuseSpec.rgb
//             + gEmissive.rgb
//   final.rgb = final.rgb + gTranslucency.rgb             (see parity note below)
//
// Phase B energy split (rtx-realtime-alignment-design.md "Phase B
// implementation spec" / composite energy split fix): this MIRRORS the
// retired megakernel's actual combination in ShadeSurfaceHit
// (shading.slang) — `emitted = reflWeight·reflColor + (1-reflWeight)·lit +
// emission` — rather than the Phase A interim formula (`lit +
// reflections·specWeight`, a plain ADD that double-counted energy at the
// primary surface for pixels with a non-trivial reflection weight: glass /
// metal / dielectric-Fresnel surfaces read too bright because their own
// diffuse+specular term was never attenuated by (1-reflWeight)). `lit`
// there was the COMBINED diffuse+specular closure
// (EvaluateOpenPBRDirect); the signal-pass split keeps them separate, so
// `diffuseSpec` above is the split reconstruction of that same `lit`
// value (see DirectLightingPass's header for why gDirectDiffuse is
// demodulated and gDirectSpecular is not).
//
// Miss pixels (vis.hitT < 0): the dome background, computed EXACTLY as the
// old lighting raygen's primary-miss branch did (SampleDomeBackground) —
// NOT the signal recombination above (none of the five signal textures
// carry a meaningful value for a miss pixel).
//
// ---- Parity notes (documented deviations from the retired megakernel) ---
//
// SpecWeight: the retired ShadeSurfaceHit (shading.slang) computed its
// mirror-reflection weight as `OpenPBRReflectionWeight(pbr, nWorld, viewDir)`
// evaluated on the FULL material closure at the primary hit, then multiplied
// it against a RECURSIVE mirror-reflection trace (up to REFL_MAX_SEGMENTS
// glass/mirror bounces layered through translucent surfaces). ReflectionsPass
// (reflections.slang) computes the SAME OpenPBRReflectionWeight helper at the
// SAME primary hit — a full material re-eval it wouldn't otherwise need,
// added specifically so this composite can multiply by the SAME deterministic
// Fresnel/spec-albedo factor the old code used — but the traced reflection
// itself is now single-bounce direct-lighting-at-the-hit (RTX-RT
// maxReflectionBounces = 1 semantics), not a recursive composite. So the
// WEIGHT matches exactly; the radiance it's weighting is a documented,
// intentional look change (matches the design's RTX-Real-Time target, not
// the old renderer's recursive mirror).
//
// Translucency: the retired raygen's front-to-back transparency loop
// computed `radiance = primary_emitted + throughput_1 * behindColor_1 +
// throughput_1*throughput_2 * behindColor_2 + ...` — i.e. the primary
// surface's own emitted radiance ALWAYS contributes in full, with whatever
// lies behind a transmissive surface ADDED on top (never blended against
// the primary term). TranslucencyPass (translucency.slang) already computes
// that exact running-throughput sum for segments 1+ into `gTranslucency.rgb`
// (zero for opaque primary surfaces, by construction — see its ClosestHit/
// RayGen). Composing them is therefore a plain ADDITION, not a
// `foreground-over-background` alpha blend: `gTranslucency.a` is coded as
// COVERAGE (1 = fully opaque primary, 0 = fully see-through — see
// translucency.slang's doc comment), so a naive
// `gTranslucency.rgb + (1 - gTranslucency.a) * lit` blend would multiply
// the opaque-surface term by (1 - 1) = 0 and black out every non-
// transmissive pixel (the vast majority of any scene). Plain addition
// reproduces the retired megakernel's total exactly (gTranslucency.rgb is
// already zero for opaque pixels and already throughput-weighted for
// transmissive ones), so that's what this shader does.
//
// Everything else (the primary surface's own diffuse/specular/indirect/
// emissive decomposition) is close-to-exact: DirectLightingPass /
// IndirectDiffusePass / AmbientOcclusionPass each re-derive the primary hit
// via the SAME EvaluateSurfaceMaterial / EvaluateDomeAmbient / dome-mip
// helpers ShadeSurfaceHit used inline, just demodulated/split into separate
// signals and recombined here instead of accumulated in one function body —
// the WP2 rewrite's documented f16-barycentric visibility-buffer precision
// drop (sub-golden-tolerance) is the only other source of drift.
//
// A COMPUTE pass (not RT): no TraceRay of its own. Binds the shared Set-0
// SceneBindings layout (camera + lights + dome, for the miss-pixel
// background) alongside its own Set-1 signal-texture I/O.
//
// Halo-fix note (rtx-realtime-alignment-design.md, "denoiser chain" /
// white-fringe forensics, 2026-07-05): gDirectDiffuse is DEMODULATED by
// DirectLightingPass via shading.slang's DemodulateByAlbedo, which now
// floors its divisor at MIN_DEMODULATION_ALBEDO instead of only guarding
// near-zero (see that function's doc comment for the full mechanism —
// TL;DR: dividing by a dark-but-nonzero real albedo, e.g. the World Lobby
// black vase's ~0.02, amplified this signal's Monte-Carlo noise into a
// white/bright halo wherever a spatial denoiser tap crossed the
// silhouette). The re-modulation below MIRRORS that exact floor
// (kMinDemodulationAlbedo, same literal — keep in sync with
// shading.slang's MIN_DEMODULATION_ALBEDO) so the denoise-off round trip
// is unaffected: signal/max(albedo,floor) * max(albedo,floor) == signal,
// for ANY floor value. gIndirectDiffuse is NOT demodulated by division
// (see indirect_diffuse.slang's file header — its bounce estimator never
// multiplies by the PRIMARY surface's albedo to begin with), so it keeps
// being re-modulated by the real, unfloored `albedo` below.
//
// Bindings — Set 0 (SceneBindings, shared verbatim with every other RT/
// compute pass in this graph). Set 1 (this pass's own I/O):
//   set=1, binding=0  : StructuredBuffer<VisibilityGpu> (GBuffer pass output)
//   set=1, binding=1  : Texture2D<float4> gAlbedo
//   set=1, binding=2  : Texture2D<float4> gEmissive
//   set=1, binding=3  : Texture2D<float4> gAo
//   set=1, binding=4  : Texture2D<float4> gDirectDiffuse
//   set=1, binding=5  : Texture2D<float4> gDirectSpecular
//   set=1, binding=6  : Texture2D<float4> gIndirectDiffuse
//   set=1, binding=7  : Texture2D<float4> gReflections
//   set=1, binding=8  : Texture2D<float4> gReflectionWeight
//   set=1, binding=9  : Texture2D<float4> gTranslucency
//   set=1, binding=10 : RWTexture2D<float4> gLinearColor (RGBA32F)
//   set=1, binding=11 : RWTexture2D<float4> gOutColorHdr (RGBA16F)
//   set=1, binding=12 : RWTexture2D<float>  gOutAlpha    (R8_UNORM)

#include "ShaderInterop.slang"
#include "camera_ray.slang"  // gCamera (0) + gQuality (15) + BuildCameraRay
#include "dome_sample.slang" // gLights (5) + gDomeEnvMap (10) + gDomeSampler (11) + SampleDomeBackground

[[vk::binding(0, 1)]]  StructuredBuffer<VisibilityGpu> gVisibility;
[[vk::binding(1, 1)]]  Texture2D<float4>                gAlbedo;
[[vk::binding(2, 1)]]  Texture2D<float4>                gEmissive;
[[vk::binding(3, 1)]]  Texture2D<float4>                gAo;
[[vk::binding(4, 1)]]  Texture2D<float4>                gDirectDiffuse;
[[vk::binding(5, 1)]]  Texture2D<float4>                gDirectSpecular;
[[vk::binding(6, 1)]]  Texture2D<float4>                gIndirectDiffuse;
[[vk::binding(7, 1)]]  Texture2D<float4>                gReflections;
[[vk::binding(8, 1)]]  Texture2D<float4>                gReflectionWeight;
[[vk::binding(9, 1)]]  Texture2D<float4>                gTranslucency;
[[vk::binding(10, 1)]] RWTexture2D<float4>              gLinearColor;   // RGBA32F radiance
[[vk::binding(11, 1)]] RWTexture2D<float4>              gOutColorHdr;   // RGBA16F radiance
[[vk::binding(12, 1)]] RWTexture2D<float>               gOutAlpha;      // R8_UNORM 1.0 hit / 0.0 miss

[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID) {
    uint destWidth;
    uint destHeight;
    gLinearColor.GetDimensions(destWidth, destHeight);
    if (dispatchThreadId.x >= destWidth || dispatchThreadId.y >= destHeight)
        return;

    const uint2 launchIndex = dispatchThreadId.xy;
    const VisibilityGpu vis = gVisibility[launchIndex.y * destWidth + launchIndex.x];

    float3 color;
    float  alpha;

    if (vis.hitT < 0.0f)
    {
        // Primary miss — the SAME background the retired lighting raygen
        // computed for a miss pixel (SampleDomeBackground on the primary
        // camera ray direction), NOT the signal recombination below: none
        // of the five signal textures carry a meaningful value here.
        float3 worldOriginXyz;
        float3 worldDirXyz;
        BuildCameraRay(launchIndex, uint2(destWidth, destHeight), worldOriginXyz, worldDirXyz);
        color = SampleDomeBackground(worldDirXyz);
        alpha = 0.0f;
    }
    else
    {
        const float3 albedo           = gAlbedo[launchIndex].rgb;
        const float3 emissive         = gEmissive[launchIndex].rgb;
        const float  ambientOcclusion = gAo[launchIndex].r;
        const float3 directDiffuse    = gDirectDiffuse[launchIndex].rgb;
        const float3 directSpecular   = gDirectSpecular[launchIndex].rgb;
        const float3 indirectDiffuse  = gIndirectDiffuse[launchIndex].rgb;
        const float3 reflections      = gReflections[launchIndex].rgb;
        const float3 specWeight       = gReflectionWeight[launchIndex].rgb;

        // The primary surface's own diffuse+specular closure — re-modulates
        // the demodulated diffuse signals by albedo, adds the
        // un-demodulated specular directly (mirrors the retired
        // megakernel's combined `lit` term — see this file's header).
        //
        // Halo-fix (see this file's header): gDirectDiffuse must be
        // re-modulated by the SAME floored albedo DirectLightingPass
        // divided by (shading.slang's MIN_DEMODULATION_ALBEDO), not the
        // real albedo, so the floor's denoise-off round trip is exact.
        // gIndirectDiffuse keeps the real, unfloored albedo (it was never
        // divided by albedo upstream).
        const float kMinDemodulationAlbedo = 0.04;  // keep in sync with shading.slang
        const float3 flooredAlbedo = max(albedo, float3(kMinDemodulationAlbedo,
                                                         kMinDemodulationAlbedo,
                                                         kMinDemodulationAlbedo));
        const float3 diffuseSpec = flooredAlbedo * directDiffuse
                                 + albedo * indirectDiffuse * ambientOcclusion
                                 + directSpecular;

        // Phase B energy split (composite energy split fix): reflections
        // are layered ON TOP of the diffuse+specular term with an
        // energy-conserving (1 - specWeight) attenuation of the latter —
        // the SAME split ShadeSurfaceHit's `reflWeight·refl +
        // (1-reflWeight)·lit` combination used, instead of Phase A's
        // plain addition (which double-counted energy on
        // high-specular-weight surfaces).
        const float3 oneMinusSpecWeight = float3(1.0f, 1.0f, 1.0f) - specWeight;
        const float3 lit = specWeight * reflections
                         + oneMinusSpecWeight * diffuseSpec
                         + emissive;

        // Translucency — plain addition; see this file's header "Parity
        // notes" for why an alpha-blend against gTranslucency.a would be
        // wrong (gTranslucency.a is CODED as coverage, not the translucent
        // layer's own opacity, and gTranslucency.rgb is already zero for
        // opaque pixels / already throughput-weighted for transmissive ones).
        const float4 translucency = gTranslucency[launchIndex];
        color = lit + translucency.rgb;
        alpha = 1.0f;
    }

    // colorHdr + alpha AOVs move here from the retired lighting pass (WP2-
    // final contract): transparency-composited coverage / radiance is only
    // fully known once this pass has combined every signal.
    gOutColorHdr[launchIndex] = float4(color, 1.0f);
    gOutAlpha[launchIndex]    = alpha;

    // Full fp32-precision linear radiance for TonemapPass / AutoExposurePass —
    // the same value as gOutColorHdr above, just undequantized.
    gLinearColor[launchIndex] = float4(color, 1.0f);
}
