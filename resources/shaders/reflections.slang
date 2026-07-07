// Pyxis renderer — ReflectionsPass, ONE shader file (RTX-alignment
// design, rtx-realtime-alignment-design.md, WP2-signals Phase A / Phase B
// estimator swap).
//
// RTX-RT single-bounce reflections. The PRIMARY surface's normal +
// roughness come straight from RaytracedGBufferPass's gNormalRoughness
// guide (Texture_SRV) — no material re-eval for the primary surface,
// only geometry. Surfaces with roughness < REFLECTION_TRACE_ROUGHNESS_CEILING
// (max'd against the live gQuality.reflectionMaxRoughness knob — see that
// constant's own doc comment below) get `gQuality.reflectionSamples`
// GGX-VNDF-importance-sampled TraceRay(s) (Phase B — Heitz 2018 "Sampling
// the GGX Distribution of Visible Normals"; Phase A traced exactly the
// deterministic mirror direction); rougher surfaces take a
// roughness-proportional dome-mip sample instead (the "rough fallback"
// analog of a roughness-cache term). At each reflection hit:
// EvaluateSurfaceMaterial (shared) + the shared direct-light loop WITHOUT
// shadow rays (EvaluateDirectLightSample, traceShadow = false —
// deliberately cheap) + an OCCLUSION-AWARE dome-ambient term (see
// ClosestHitMain's own doc comment — the "occlusion-aware ambient"
// follow-up, rtx-realtime-alignment-design.md, 2026-07-06) + emissive;
// misses sample the shared dome background (SampleDomeBackground). Phase B
// also clamps the averaged result to gQuality.maxRayIntensityReflections.
//
// Specular MODEL GAP fix (rtx-realtime-alignment-design.md, 2026-07-07) —
// OPTIONAL stochastic-GGX estimator, gated by RealTimeQuality::passMask
// bit 8 (PASS_MASK_STOCHASTIC_REFLECTIONS, ShaderInterop.slang) via the
// SPEC_ID_STOCHASTIC_REFLECTIONS spec constant (STOCHASTIC_REFLECTIONS_
// ENABLED below) ReflectionsPass.cpp folds at pipeline-creation time —
// default OFF, byte-identical to the pre-existing trace. Root cause: the
// default path's GGX-VNDF-sampled DIRECTION already exists (Phase B, see
// SampleGGXVNDF's own doc comment) but the traced radiance is averaged
// UNWEIGHTED and then multiplied by OpenPBRReflectionWeight's deterministic
// Fresnel(NdotV) x (1-r')^2 "glossy" fade downstream (CompositePass) — a
// fade that exists ONLY to compensate a single deterministic mirror sample
// for the lobe spread it doesn't simulate. That fade cannot reproduce a
// real GGX lobe integrating a bright environment (measured against ovrtx:
// id40 Paint_Satin 0.147 vs 0.324, id30 glass floor 0.225 vs 0.283) and
// darkens an already roughness-spread stochastic sample a SECOND time —
// Heitz 2018 VNDF sampling already draws directions proportional to the
// visible-normal density (D(h)*G1(v)*max(0,VdotH)/NdotV), so the plain
// multi-sample average IS the properly lobe-weighted mean incoming
// radiance; no additional per-sample BRDF term is needed on top (an
// earlier version of this fix DID fold a per-sample F(V,H)*G2(V,L)/G1(V)
// throughput in, reasoning that the VNDF pdf cancels the BRDF down to
// exactly that estimator — empirically WORSE, because CompositePass's
// `specWeight*reflections + (1-specWeight)*diffuseSpec` blend ALREADY
// applies its own Fresnel(NdotV) via specWeight, so the per-sample F(V,H)
// double-counted it; whole-frame RMSE regressed 0.176 -> 0.189 with id40/
// id30 getting DARKER. Reverted — see RayGenMain's trace-loop comment).
// When the bit is SET, RayGenMain instead ONLY changes the specWeight
// (gReflectionWeight) written for traced pixels: the SAME base dielectric/
// metal F82 Fresnel blend OpenPBRReflectionWeight uses, WITHOUT its
// (1-r')^2 term (see the specWeight block in RayGenMain) — that term
// existed purely to compensate the single-deterministic-sample path for
// the lobe spread it didn't simulate; the multi-sample VNDF average
// already supplies that spread. Convergence relies on TWO already-
// existing mechanisms, unchanged by this fix: RtxSamplingUniforms' per-
// FRAME-varying RNG seed (gSampling.frameIndex folds into SeedPcg32 below
// — every frame draws a fresh VNDF sample) and either AccumulationPass's
// N-frame running mean (PASS_MASK_ACCUMULATE) or an equal-length `render.
// accumulationFrames` re-run (both let the builtin denoiser's own EMA
// settle across the growing sample count). Measured BOTH candidate
// denoisers on the World Lobby guard_acc96 fixture (passMask 383 = 127 |
// 256, 96 frames): builtin -> whole-frame RMSE 0.17621 (baseline 0.17626,
// essentially at parity, with id40/id30 measurably closer to ovrtx); DLSS
// Ray Reconstruction -> 0.17842 (WORSE than both) — RR's own reconstruction
// over-brightened several unrelated materials (id3 Concrete, id6 Iron
// mullion) enough to erase the id40/id30 gains, the opposite of the
// original "RR should clean up stochastic noise better" hypothesis.
// Builtin is the recommended denoiser for this mode.
//
// Bindings — Set 0 (SceneBindings, shared verbatim with every other RT
// pass). Set 1 (this pass's own I/O):
//   set=1, binding=0 : StructuredBuffer<VisibilityGpu> (GBuffer pass output)
//   set=1, binding=1 : Texture2D<float4> gNormalRoughness (GBuffer guide, SRV)
//   set=1, binding=2 : RWTexture2D<float4> gReflections (RGBA16F; .a = hitT, miss/fallback = -1)
//   set=1, binding=3 : RWTexture2D<float4> gReflectionWeight (RGBA16F; .rgb = the
//                       deterministic Fresnel/spec-albedo mirror weight
//                       (OpenPBRReflectionWeight) at the PRIMARY surface — WP2-final,
//                       CompositePass's SpecWeight factor. Requires a full material
//                       re-eval at the primary hit (EvaluateSurfaceMaterial), unlike
//                       the geometry-only guide-texture read this pass otherwise uses —
//                       see RayGenMain.)
//
// Hit groups / miss shaders — TWO of each now (occlusion-aware ambient
// follow-up): "HitGroupDefault" (ClosestHitMain) + "MissMain" (index 0) for
// the PRIMARY reflection ray, unchanged; "HitGroupAo" (any-hit only,
// AoAnyHitMain) + "AoMissMain" (index 1) for the SHORT ambient-occlusion
// ray ClosestHitMain fires at the reflection hit (see below). The AO ray
// is a distinct hit group/miss pair (not a shared one, unlike
// indirect_diffuse.slang's single-payload trick) because it uses the
// shared `OcclusionPayload` type while the primary ray uses this file's
// own `ReflectionPayload` — DXR requires a shader's payload type to match
// its TraceRay call site's. ReflectionsPass.cpp's `maxRecursionDepth` is 2
// to cover this (RayGen's reflection TraceRay = level 1, ClosestHit's AO
// TraceRay = level 2).

#include "ShaderInterop.slang"
#include "camera_ray.slang"  // gCamera (0) + gQuality (15) + gSampling (16) + BuildCameraRay
#include "shading.slang"     // gTlas (1) + full Set-0 scene tables + EvaluateSurfaceMaterial
                             // + EvaluateDirectLightSample; pulls in dome_sample.slang
                             // (gLights / gDomeEnvMap / gDomeSampler / latLongUv /
                             // SampleDomeBackground) transitively.
#include "rng.slang"         // Pcg32 + BuildOrthonormalBasis + MakeStreamId
#include "radiance_cache.slang"  // SHARC world-space radiance cache (gQuality.giMode gate)

[[vk::binding(0, 1)]] StructuredBuffer<VisibilityGpu> gVisibility;
[[vk::binding(1, 1)]] Texture2D<float4>                gNormalRoughness;
[[vk::binding(2, 1)]] RWTexture2D<float4>              gReflections;
[[vk::binding(3, 1)]] RWTexture2D<float4>              gReflectionWeight;
// SHARC radiance cache (rtx-realtime-alignment-design.md, 2026-07-07) — reused
// from IndirectDiffusePass's update. Reflections only QUERIES (read-only): at a
// GLOSSY reflection hit, the cached full outgoing radiance supplies the
// indirect-GI term the cheap reflection-hit shade (direct + dome-ambient +
// emission) is missing — see ClosestHitMain's "One-bounce GI (NOT implemented)"
// note. Always bound; touched only when gQuality.giMode != 0. Owned by
// SharcResolvePass; hash + resolved suffice for a query (no accum/update here).
[[vk::binding(4, 1)]] RWByteAddressBuffer gShHash;      // 4 B/cell checksum table
[[vk::binding(5, 1)]] RWByteAddressBuffer gShResolved;  // 16 B/cell resolved radiance

// Camera world position (grid LoD origin) — view-space (0,0,0) → world.
float3 SharcCameraWorld() { return mul(gCamera.worldFromView, float4(0.0, 0.0, 0.0, 1.0)).xyz; }

// Specular MODEL GAP fix — folded at pipeline-creation time by
// ReflectionsPass.cpp from RealTimeQuality::passMask bit 8 (see this
// file's header). 0 (default) reproduces the pre-existing trace bit-for-
// bit; 1 selects the specWeight-without-glossy-fade branch below (traced
// pixels only — see the specWeight block's `willTrace` gate).
[[vk::constant_id(SPEC_ID_STOCHASTIC_REFLECTIONS)]] const uint STOCHASTIC_REFLECTIONS_ENABLED = 0u;

// RTX-alignment materials round 4 (floor/tables reflection-signal
// forensics, rtx-realtime-alignment-design.md, 2026-07-06) — floor/tables
// root-cause fix. World Lobby's indoor glossy surfaces above the OLD 0.3
// trace/fallback cutoff (floor materialId 30, table Paint_Satin materialId
// 40, roughness ~0.4-0.5) took the dome-mip "rough fallback" below, which
// reflects the scene's bright sky-dome (intensity 12000) instead of the
// dark interior they physically see: killing the fallback outright (black
// substitute) dropped the tables' brightness 73% (0.308 -> 0.083) and the
// floor's 35%, i.e. these surfaces' shaded result was dome-reflection-
// dominated, not interior-reflection-dominated — measured 2.1x too bright
// on the tables vs ovrtx (0.70 vs 0.33).
//
// ovrtx's own roughnessCacheThreshold (also 0.3, rtx-realtime-alignment-
// design.md's "Authoritative defaults") CACHES/approximates the
// reflection above that roughness — it does not substitute a completely
// different (sky) environment for it; ovrtx keeps reflecting real
// geometry either way. Pyxis has no reflection-probe/cache
// infrastructure to match that approximation (out of scope for this
// fix), so instead this raises how far up the roughness range the
// ALREADY-EXISTING traced path applies (SampleGGXVNDF below; alpha =
// roughness^2 already spreads the reflection lobe with roughness — no
// sampling-formula change needed, just more surfaces reach it) so these
// surfaces trace the real (dark) interior instead of the sky.
//
// This is a compile-time constant, not a new RenderSettings field:
// RenderSettings (the public POD, §18) is frozen during this phase for
// concurrent work — see CLAUDE.md. RayGenMain below takes
// `max(gQuality.reflectionMaxRoughness, REFLECTION_TRACE_ROUGHNESS_CEILING)`
// as the effective cutoff, so the live runtime knob still works (raising
// it even higher still takes effect) — only the plan-authored 0.3 default
// stops silently reverting these indoor glossy surfaces to the
// sky-fallback. Mirror-smooth surfaces (roughness well under the OLD 0.3
// cutoff — real mirrors, glass) are unaffected either way; only the
// 0.3-to-ceiling band changes behavior.
static const float REFLECTION_TRACE_ROUGHNESS_CEILING = 0.6;

// The reflection ray's payload — slimmer than HitInfo (this pass runs no
// shadow/AO/continuation rays from the hit, so it needs only the
// resulting radiance + hit distance).
struct ReflectionPayload {
    float3 color;
    float  hitT;       // RayTCurrent() at the hit; -1.0 on miss.
    float  roughness;  // primary (reflecting) surface roughness — SHARC cache gate
                       // (only glossy reflections use the voxel-blurred cache;
                       // sharp mirrors keep their traced sample). Set by RayGen.
};

float ReflectionsLuminance(float3 c) {
    return dot(c, float3(0.2126, 0.7152, 0.0722));
}

// GGX Visible-Normal-Distribution-Function sample (Heitz 2018, "Sampling
// the GGX Distribution of Visible Normals") — `viewLocal` is the view
// direction in the LOCAL shading frame (z = normal), `alpha` is the
// isotropic GGX roughness parameter (roughness^2, the codebase's standard
// convention — see openpbr_material.slang's `alpha = roughness*roughness`).
// Returns the sampled half-vector in the SAME local frame; the caller
// reflects the local view direction around it and transforms back to
// world space (see RayGenMain). Phase B: used purely to pick a more
// representative reflection RAY DIRECTION for roughness < the mirror/
// rough-fallback cutoff — the resulting radiance is scaled by
// gReflectionWeight (OpenPBRReflectionWeight, computed once at the
// primary surface) in CompositePass, exactly as the Phase A deterministic
// mirror direction was; no additional VNDF/BSDF weighting term is needed
// here — VNDF sampling already draws directions proportional to the
// visible-normal density, so a plain average over samples is the properly
// lobe-weighted mean (see this file's header point 2 for why an explicit
// per-sample F(V,H)*G2/G1 term was tried and reverted: it double-counts
// the Fresnel term composite.slang's specWeight blend already applies).
float3 SampleGGXVNDF(float3 viewLocal, float alpha, float2 u) {
    const float3 vh = normalize(float3(alpha * viewLocal.x, alpha * viewLocal.y, viewLocal.z));
    const float lensq = vh.x * vh.x + vh.y * vh.y;
    const float3 basisT1 =
        (lensq > 0.0) ? float3(-vh.y, vh.x, 0.0) * rsqrt(lensq) : float3(1.0, 0.0, 0.0);
    const float3 basisT2 = cross(vh, basisT1);
    const float r = sqrt(u.x);
    const float phi = 6.283185307179586 * u.y;
    float t1 = r * cos(phi);
    float t2 = r * sin(phi);
    const float s = 0.5 * (1.0 + vh.z);
    t2 = (1.0 - s) * sqrt(max(0.0, 1.0 - t1 * t1)) + s * t2;
    const float3 nh = t1 * basisT1 + t2 * basisT2 + sqrt(max(0.0, 1.0 - t1 * t1 - t2 * t2)) * vh;
    return normalize(float3(alpha * nh.x, alpha * nh.y, max(0.0, nh.z)));
}

// Roughness-proportional dome sample — the "rough fallback" this pass
// takes for surfaces at/above REFLECTION_MAX_ROUGHNESS. Duplicates the
// dome-light-finding loop dome_sample.slang's SampleDomeBackground /
// shading.slang's EvaluateDomeAmbient both already have their own copy
// of (each tied to a different mip policy: 0 for the sharp background,
// ~6 for the diffuse-IBL approximation) — this one's mip scales with
// roughness, approximating a prefiltered-mip environment convolution
// without an actual mip chain built for that purpose.
//
// RTX-alignment materials round 3 (materialId 40 forensics,
// rtx-realtime-alignment-design.md) — sqrt(roughness), not roughness
// itself, drives the mip pick (the standard split-sum / Karis-2013
// perceptual-roughness convention every prefiltered-IBL implementation
// uses, because a GGX lobe's angular width grows much faster than
// linearly with roughness). A pure LINEAR roughness->mip map left
// "just above REFLECTION_MAX_ROUGHNESS" materials (satin/semi-gloss,
// roughness ~0.3-0.5 — e.g. the World Lobby's rh_table_short
// Paint_Satin, measured roughness ~0.40) sampling a nearly-unblurred
// mip. The scene's default (untextured) dome is a procedural sky with
// a bright horizon band / sun disk (max ~76000 vs a ~0.75 frame
// average — resources/scenes/default_sky.exr); a low-height camera
// viewing floor-level satin furniture at a near-grazing angle
// reflects toward that SAME near-horizon band, and the barely-blurred
// linear-mip sample was punching that hotspot straight through as a
// bright, un-satin-like sheen (measured 2.1x too bright vs ovrtx on
// materialId 40, whose own diffuse albedo already matches — this is a
// reflections-signal magnitude gap, not a translation one). sqrt(0.40)
// = 0.63, i.e. ~58% more mip levels of blur for exactly this
// roughness band, which properly averages the hotspot into the
// surrounding sky instead of resolving it.
float3 SampleDomeRoughnessFallback(float3 dir, float roughness) {
    float3 domeTint = float3(0.55, 0.65, 0.85);
    float  domeScale = 1.0;
    float  domeRotationY = 0.0;
    bool   domeFound = false;

    uint lightCount;
    uint _stride;
    gLights.GetDimensions(lightCount, _stride);
    for (uint i = 0; i < lightCount; ++i) {
        const LightGpu L = gLights[i];
        if (L.intensity <= 0.0) continue;
        if (L.kind != LIGHT_KIND_DOME) continue;
        domeTint      = float3(L.colorR, L.colorG, L.colorB);
        domeScale     = L.intensity;
        domeRotationY = L.domeRotationYRadians;
        domeFound     = true;
        break;
    }

    const float3 dirRaw = normalize(dir);
    float3 d = dirRaw;
    if (domeFound && abs(domeRotationY) > 1e-4) {
        const float cosR = cos(-domeRotationY);
        const float sinR = sin(-domeRotationY);
        d = float3(cosR * dirRaw.x + sinR * dirRaw.z,
                   dirRaw.y,
                   -sinR * dirRaw.x + cosR * dirRaw.z);
    }

    const float2 uv = latLongUv(d);
    float domeWidth;
    float domeHeight;
    float domeMipLevels;
    gDomeEnvMap.GetDimensions(0, domeWidth, domeHeight, domeMipLevels);
    const float maxMip = max(0.0, domeMipLevels - 1.0);
    const float targetMip = sqrt(saturate(roughness)) * maxMip;
    const float3 hdri = gDomeEnvMap.SampleLevel(gDomeSampler, uv, targetMip).rgb;

    return domeFound ? (hdri * domeTint * domeScale) : domeTint;
}

[shader("raygeneration")]
void RayGenMain() {
    const uint2 launchIndex = DispatchRaysIndex().xy;
    const uint2 launchDim   = DispatchRaysDimensions().xy;

    const VisibilityGpu vis = gVisibility[launchIndex.y * launchDim.x + launchIndex.x];
    if (vis.hitT < 0.0f)
    {
        gReflections[launchIndex]      = float4(0.0f, 0.0f, 0.0f, -1.0f);
        gReflectionWeight[launchIndex] = float4(0.0f, 0.0f, 0.0f, 1.0f);
        return;
    }

    float3 worldOriginXyz;
    float3 worldDirXyz;
    BuildCameraRay(launchIndex, launchDim, worldOriginXyz, worldDirXyz);
    const float3 worldHit = worldOriginXyz + worldDirXyz * vis.hitT;

    const float4 guide = gNormalRoughness[launchIndex];
    const float3 nWorld = guide.xyz;
    const float  roughness = guide.w;
    const float3 reflDir = normalize(reflect(worldDirXyz, nWorld));

    // WP2-final — CompositePass's SpecWeight factor: the SAME deterministic
    // Fresnel/spec-albedo mirror weight (OpenPBRReflectionWeight,
    // openpbr_material.slang) the retired raytraced_lighting megakernel
    // applied to its own reflection contribution (shading.slang's
    // ShadeSurfaceHit: `reflWeightOut = OpenPBRReflectionWeight(pbr, nWorld,
    // viewDir)`). Needs a full material re-eval at the PRIMARY surface —
    // unlike the rest of this raygen, which only reads geometry
    // (gNormalRoughness) — because the weight depends on specular_ior /
    // specular_color / metalness / coat / fuzz, none of which live in the
    // G-buffer guide textures. Written for every hit pixel regardless of
    // the rough-fallback branch below, so CompositePass always has a valid
    // multiplier for whatever gReflections holds (traced or dome fallback).
    const float2 visBary = float2(f16tof32(vis.baryPacked & 0xFFFFu),
                                  f16tof32(vis.baryPacked >> 16u));
    const InstanceInfoGpu inst = gInstanceInfo[vis.instanceId];
    const float3x4 worldToObject = InvertAffine3x4(inst.objectToWorld);
    const SurfaceMaterialEval surf = EvaluateSurfaceMaterial(
        vis.instanceId, vis.primitiveIndex, visBary,
        worldHit, worldDirXyz, vis.hitT,
        inst.objectToWorld, worldToObject);
    const float3 viewDir = normalize(-worldDirXyz);

    // WP2-final — runtime RealTimeQuality.reflectionMaxRoughness (SceneBindings
    // gQuality) replaces the REFLECTION_MAX_ROUGHNESS compile-time constant;
    // ShaderInterop.slang's literal now only backs the C++-side
    // RenderSettings::RealTimeQuality::reflectionMaxRoughness default.
    //
    // Materials round 4 — max'd against REFLECTION_TRACE_ROUGHNESS_CEILING
    // (this file's own doc comment, above the binding declarations) so the
    // default 0.3 knob no longer sends World Lobby's indoor glossy floor/
    // table surfaces to the sky-dome fallback below. Specular MODEL GAP fix
    // (2026-07-07) — hoisted ABOVE the specWeight block (was computed just
    // before the rough-fallback branch) so that block can gate the
    // stochastic specWeight override on `willTrace`: a pixel taking the
    // rough-fallback path below NEVER runs the multi-sample GGX-VNDF trace
    // (its gReflections sample is the SAME deterministic dome-mip fallback
    // either way), so it must keep OpenPBRReflectionWeight's (1-r')^2
    // glossy fade regardless of the stochastic bit — dropping it there
    // would over-brighten every ROUGH material (concrete, stucco, fabric)
    // with no corresponding fix to the signal it's weighting.
    const float traceRoughnessCeiling =
        max(gQuality.reflectionMaxRoughness, REFLECTION_TRACE_ROUGHNESS_CEILING);
    const bool willTrace = roughness < traceRoughnessCeiling;

    // Specular MODEL GAP fix (2026-07-07, this file's header) — the SAME
    // modulated IOR OpenPBRReflectionWeight derives internally
    // (openpbr_material.slang's own etaPrime), computed once here so the
    // stochastic branch below (this specWeight override AND the per-sample
    // trace-loop throughput) doesn't re-derive it twice. Only the
    // stochastic branch consumes it; `xiFsUnused` is discarded exactly as
    // OpenPBRReflectionWeight's own local does.
    float xiFsUnused;
    const float etaPrimeStochastic = OpenPBRModulatedIor(
        surf.pbr.specularIor, surf.pbr.specularWeight, surf.pbr.coatIor,
        OpenPBRFeature(OPENPBR_FEATURE_COAT) ? saturate(surf.pbr.coatWeight) : 0.0, xiFsUnused);

    float3 specWeight;
    if (STOCHASTIC_REFLECTIONS_ENABLED != 0u && willTrace) {
        // Stochastic specWeight — the composite blend factor
        // (composite.slang: `lit = specWeight*reflections +
        // (1-specWeight)*diffuseSpec`) still needs a Fresnel-based split
        // between "energy that left via the specular lobe" and "energy
        // that left via diffuse", but must NOT ALSO layer
        // OpenPBRReflectionWeight's (1-r')^2 glossy fade on top — that fade
        // exists ONLY to compensate the DETERMINISTIC single-mirror-sample
        // path for the lobe spread it doesn't simulate; the trace loop's
        // multi-sample GGX-VNDF average (unchanged below — see this file's
        // header for why an explicit per-sample BRDF re-weighting was tried
        // and reverted) already supplies that spread, so re-applying the
        // fade here would darken the now-correctly-averaged signal a second
        // time (the exact failure mode three prior scalar-reflection-weight
        // tuning attempts hit — see this file's header). Uses the SAME base
        // dielectric/metal F82 Fresnel blend OpenPBRReflectionWeight itself
        // starts from (NdotV, no coat/fuzz layering — a documented
        // simplification for this mode; coat/fuzz materials keep the
        // deterministic path's full closure when this bit is clear).
        const float ndotVPrimary = max(1e-4, dot(surf.normalWorld, viewDir));
        const float3 fDielectricPrimary = saturate(surf.pbr.specularColor)
                                        * FresnelDielectricExact(ndotVPrimary, etaPrimeStochastic);
        const float3 fMetalPrimary = OpenPBRConductorFresnel(
            ndotVPrimary, surf.pbr.baseColor, surf.pbr.baseWeight,
            surf.pbr.specularColor, surf.pbr.specularWeight);
        specWeight = saturate(lerp(fDielectricPrimary, fMetalPrimary, saturate(surf.pbr.baseMetalness)));
    } else {
        specWeight = OpenPBRReflectionWeight(surf.pbr, surf.normalWorld, viewDir);
    }

    // RTX-alignment window forensics (rtx-realtime-alignment-design.md,
    // 2026-07-06) — two corrections to the PRIMARY surface's specWeight,
    // measured against ovrtx on the World Lobby's window facade (mullion
    // materialId 6, glass materialId 2). Applied HERE (gReflectionWeight
    // is CompositePass's own SpecWeight input, consumed nowhere else) and
    // NOT inside OpenPBRReflectionWeight itself: that closure is also
    // called by shading.slang's ShadeSurfaceHit for TranslucencyPass's
    // front-to-back segments (whatever is visible THROUGH a transmissive
    // surface, e.g. a mullion glimpsed through an adjacent glass pane) —
    // an earlier version of this fix lived in the shared closure and
    // measurably regressed materialId 2/30 (both partly-glass) by
    // dimming metal content seen *through* them, even though those
    // materials' OWN primary-surface weight was untouched (metalness/
    // thin-walled gates were 0 for them). Scoping to this raygen's own
    // gReflectionWeight write keeps the fix confined to the primary-
    // surface reflection blend CompositePass performs.
    //
    // (1) Metalness-gated extra glossy damping. World Lobby's "Iron"
    // mullion is a genuine PARTIAL metal (OmniPBR metallic_constant =
    // 0.5, reflection_roughness_constant = 0.5) — an unusual middle-of-
    // the-road case most materials don't hit. Phase B's GGX-VNDF trace +
    // 96-frame accumulation already converges gReflections to the true
    // solid-angle-weighted average of whatever the lobe sees (dark
    // interior most of the time, this scene's flat untextured 12000-
    // intensity dome light on the facade orientations that catch sky —
    // see REFLECTION_TRACE_ROUGHNESS_CEILING's own doc comment above for
    // that dome's "no HDRI, flat colour-only" status, itself scene/
    // ingest debt outside this module's reach). Measured: on the sky-
    // catching facade segments OpenPBRReflectionWeight's (1-r)² glossy
    // fade let ~2x too much of that flat, converged-bright sample
    // through (0.29 vs ovrtx's 0.13-0.15 local mean at the affected
    // NdotV band) while the SAME-roughness dielectric floor/table
    // materials (Paint_Satin, metalness 0) the reflection-ambient commit
    // just tuned are NOT over-bright — i.e. the excess is specifically
    // the metal fraction's grazing rise, not a generic roughness-damping
    // shortfall. An EXTRA (1-r')^(2·M) factor (M = base_metalness)
    // compounds with the existing (1-r')² to give an effective (1-r')^
    // (2+2M) curve — a no-op at metalness 0 (every dielectric: floor,
    // tables, this same glass, is untouched), while a partial-to-full
    // metal gets a steeper falloff at the SAME roughness (net exponent
    // 3 at M=0.5, 4 at M=1). Gated on `metalness > 0` rather than
    // relying on pow(x, 0) == 1 for the no-op case: a FULLY ROUGH
    // dielectric (roughness == 1, common for matte materials — concrete,
    // stucco, fabric) makes the pow() base (1 - r') exactly 0, and
    // GPU pow()'s exp2(y * log2(x)) implementation computes
    // 0 * log2(0) = 0 * (-inf) = NaN even at y = 0 (IEEE754 "0 * inf"
    // is NaN, not the mathematically-expected pow(0,0) = 1) — an actual
    // NaN measured on several fully-rough materials (materialId 15/16/
    // 17/21/26 in the World Lobby) that corrupted their whole composite.
    // Skipping the multiply entirely for metalness == 0 sidesteps the
    // edge case exactly where it would fire, at zero cost (it was a
    // no-op there anyway).
    if (surf.pbr.baseMetalness > 0.0) {
        const float rPrimeForDamping = OpenPBRCoatRoughenedRoughness(surf.pbr);
        const float extraMetalExponent = 2.0 * saturate(surf.pbr.baseMetalness);
        specWeight *= pow(saturate(1.0 - rPrimeForDamping), extraMetalExponent);
    }

    // (2) Thin-walled second-interface reflectance. `p.thinWalled` (set
    // for the World Lobby's Clear_Glass OmniGlass, ior 1.52) is carried
    // in OpenPBRSurfaceParams but OpenPBRReflectionWeight never consumes
    // it. A real thin sheet of glass has TWO parallel dielectric
    // interfaces at the same visual depth; a viewer sees BOTH surfaces'
    // reflections layered together (the familiar "double image" ghost
    // reflection in a windowpane) — measured against ovrtx, the World
    // Lobby window glass's structural (non-glare) reflection lobe was
    // ~2.4x too dark (0.30 vs 0.73).
    //
    // RTX-alignment glass forensics follow-up (2026-07-06) — the ORIGINAL
    // fix here used the non-absorbing Airy-doubling closed form
    // R_thin = 2R/(1+R), which implicitly assumes the medium between the
    // two interfaces is CLEAR (single-pass transmittance T = 1): true for
    // this facade's Clear_Glass (glass_color ~= white) but NOT for the
    // World Lobby's second thin-walled glass surface, materialId 30's
    // Tinted_Glass (transmission_color 0.35 — a strongly absorptive
    // medium). Applying the same non-absorbing doubling to BOTH measurably
    // regressed id30 (local RMSE 0.3110 -> 0.3152 at the largest blend
    // that didn't regress the whole-frame number) because a tinted pane's
    // second-interface light has to cross the tint TWICE (round trip)
    // before it can re-emerge, so it should be damped by transmission_
    // color², not passed through at full (T=1) strength. Switched to
    // OpenPBRThinFilmSum (openpbr_material.slang) — the general absorbing-
    // medium incoherent thin-film sum this non-absorbing formula is the
    // T=1 special case of — parameterized by the material's OWN
    // transmissionColor: Clear_Glass (T ~= 1) still gets ~the full
    // doubling; Tinted_Glass (T = 0.35, T² ~= 0.12) gets most of the boost
    // naturally suppressed, without a per-material special case and
    // without touching any non-thin-walled material (mullion, floor,
    // tables, mirrors all have thinWalled = 0 and take the untouched R
    // path).
    //
    // OpenPBRTransmittance is deliberately NOT switched to this helper's
    // T_thin (see that function's own doc comment): ground-truthed
    // against the World Lobby's actual MDL source
    // (Templates/GlassWithVolume.mdl), ovrtx's own transmittance for
    // BOTH these materials is the plain single-interface
    // transmission_color·(1−R) this renderer already had — MDL's
    // `fresnel_layer` never applies a two-interface term to either lobe.
    // The R_thin reflectance boost here is this renderer's own pre-
    // existing (Airy-doubling) approximation for the "double image"
    // ghost reflection a real viewer sees in a windowpane, generalized
    // to also account for absorption; it is not itself claimed to be
    // what ovrtx's MDL evaluator does, only tuned to move id2 measurably
    // closer without disturbing any other material (confirmed by a full
    // per-material RMSE sweep: only materialId 30 changed at all before
    // vs after this fix, id2/id6 bit-identical).
    //
    // Still blended at a FRACTION (0.15, the previously-measured-safe
    // strength) rather than applied at full weight: id30's OWN traced
    // gReflections carries more per-pixel Monte-Carlo variance than id2's
    // does, and even the now-much-smaller absorption-damped delta is
    // safer left partial than risking a repeat of the variance-driven
    // RMSE regression the flat non-absorbing version caused.
    if (surf.pbr.thinWalled) {
        float3 rThin;
        float3 tThinUnused;
        OpenPBRThinFilmSum(specWeight, saturate(surf.pbr.transmissionColor), rThin, tThinUnused);
        specWeight = lerp(specWeight, rThin, 0.15);
    }

    gReflectionWeight[launchIndex] = float4(specWeight, 1.0f);

    // traceRoughnessCeiling / willTrace computed above (before the
    // specWeight block) — see that computation's own doc comment.
    if (!willTrace)
    {
        // Rough fallback — no trace. Phase B firefly clamp applies HERE
        // too: an un-mip-biased-enough dome/HDRI sample (bright sun disk,
        // hot practical lights) can exceed maxRayIntensityReflections just
        // as easily as a traced hit can — this branch must not bypass the
        // clamp the traced branch below applies (an earlier version did
        // exactly that, `return`ing before ever reaching the clamp code).
        float3 fallback = SampleDomeRoughnessFallback(reflDir, roughness);
        const float fallbackLum = ReflectionsLuminance(fallback);
        if (fallbackLum > gQuality.maxRayIntensityReflections && fallbackLum > 1e-9) {
            fallback *= gQuality.maxRayIntensityReflections / fallbackLum;
        }
        gReflections[launchIndex] = float4(fallback, -1.0f);
        return;
    }

    // Phase B — GGX VNDF importance-sampled reflection direction(s)
    // instead of the pure deterministic mirror. Below
    // traceRoughnessCeiling the VNDF sample (alpha = roughness^2 below)
    // spreads the reflected direction proportional to roughness — a
    // representative glossy-reflection direction rather than the single
    // sharp mirror ray Phase A always traced, and (materials round 4)
    // wide enough to cover World Lobby's satin/semi-gloss furniture that
    // used to fall to the sky-dome fallback below.
    float3 tangent;
    float3 bitangent;
    BuildOrthonormalBasis(nWorld, tangent, bitangent);
    const float3 viewLocal =
        float3(dot(viewDir, tangent), dot(viewDir, bitangent), dot(viewDir, nWorld));
    const float alpha = max(0.001, roughness * roughness);

    Pcg32 rng = SeedPcg32(gSampling.rngSeed, gSampling.frameIndex, launchIndex, launchDim.x,
                          MakeStreamId(RNG_STREAM_REFLECTIONS, 0u, 0u));

    const uint sampleCount = max(gQuality.reflectionSamples, 1u);
    float3 colorSum = float3(0.0f, 0.0f, 0.0f);
    float  hitTSum = 0.0f;
    uint   hitCount = 0u;

    [loop]
    for (uint s = 0u; s < sampleCount; ++s) {
        const float2 u = Pcg32NextFloat2(rng);
        const float3 halfLocal = SampleGGXVNDF(viewLocal, alpha, u);
        const float3 halfWorld = LocalToWorld(halfLocal, tangent, bitangent, nWorld);
        const float3 sampleDir = normalize(reflect(worldDirXyz, halfWorld));

        RayDesc reflRay;
        reflRay.Origin    = worldHit + nWorld * 1e-3f;
        reflRay.Direction = sampleDir;
        reflRay.TMin      = 1e-3f;
        reflRay.TMax      = 10000.0f;

        ReflectionPayload payload;
        payload.color = float3(0.0f, 0.0f, 0.0f);
        payload.hitT  = -1.0f;
        payload.roughness = roughness;  // SHARC cache gate at the hit (glossy only)
        TraceRay(gTlas, RAY_FLAG_NONE, 0xFFu, 0, 0, 0, reflRay, payload);

        // Heitz 2018 "Sampling the GGX Distribution of Visible Normals" —
        // sampling H from the VNDF (SampleGGXVNDF above, unconditional,
        // both modes) already draws directions distributed proportional to
        // D(h)*G1(v)*max(0,VdotH)/NdotV — the visible-normal density — so a
        // PLAIN average of the traced samples is already the properly
        // lobe-weighted mean incoming radiance the GGX lobe integrates
        // over, no additional per-sample F(V,H)*G2/G1 term needed here.
        // (An earlier version of this fix DID fold F(V,H)*G2/G1 into the
        // per-sample throughput AND relied on it in the specWeight block
        // above — empirically WORSE: whole-frame RMSE regressed 0.176 ->
        // 0.189 because composite.slang's `specWeight*reflections +
        // (1-specWeight)*diffuseSpec` LERP already applies its own
        // Fresnel(NdotV) via specWeight, so folding F(V,H) in here double-
        // counted it. G2/G1 alone, without F, was a smaller version of the
        // same over-correction: 0.176 -> 0.176 [wash] but left id40/id30
        // under-corrected. The fix that actually reproduces the target
        // lobe-integration behavior is REMOVING OpenPBRReflectionWeight's
        // (1-r')^2 glossy fade in the specWeight block above — see that
        // block's doc comment — and leaving this trace loop exactly as the
        // pre-existing unweighted average; the multi-sample/multi-frame
        // VNDF average alone (no ad hoc single-sample compensation term)
        // supplies the lobe integration ovrtx's real GGX-Smith BRDF
        // produces.)
        colorSum += payload.color;
        if (payload.hitT >= 0.0f) {
            hitTSum += payload.hitT;
            hitCount += 1u;
        }
    }

    float3 meanColor = colorSum / float(sampleCount);
    const float lum = ReflectionsLuminance(meanColor);
    if (lum > gQuality.maxRayIntensityReflections && lum > 1e-9) {
        meanColor *= gQuality.maxRayIntensityReflections / lum;
    }
    // .a = mean hitT of the samples that actually hit geometry (NRD-style
    // normalized later); -1 when every sample missed (matches the
    // single-sample miss sentinel every other branch of this pass uses).
    const float meanHitT = (hitCount > 0u) ? (hitTSum / float(hitCount)) : -1.0f;

    gReflections[launchIndex] = float4(meanColor, meanHitT);
}

// --------------------------------------------------------------------
// ClosestHitMain — evaluates the reflection hit's material + an
// UNSHADOWED direct-light loop + an OCCLUSION-AWARE dome-ambient term +
// emissive. Still deliberately cheap (no per-light shadow rays — Phase A
// cost cap; see the file header) but now fires ONE short AO ray to gate
// the ambient term.
//
// RTX-alignment materials round 3 (materialId 30 forensics,
// rtx-realtime-alignment-design.md) — the direct-light loop above
// skips LIGHT_KIND_DOME entries (EvaluateDirectLightSample is the
// point/rect/distant NdotL path only; dome contribution is a SEPARATE
// evaluation everywhere else in this codebase — see
// EvaluateDomeAmbient's own doc comment). Every OTHER pass that
// shades a surface (IndirectDiffusePass, the retired ShadeSurfaceHit
// megakernel TranslucencyPass still reuses) adds that dome term; this
// pass never did, so a reflected surface whose OWN lighting is
// indirect-dominated (a dome/ambient-lit wall or ceiling, not a
// direct-light-lit one) reflected back at near-zero brightness
// regardless of how reflective the PRIMARY surface actually is —
// measured on the World Lobby's floor-level glossy glass inset
// (materialId 30) and, after the traced-rough-reflections fix above
// raised the trace ceiling to 0.6, on the indoor satin tables
// (materialId 40, mean 0.099 vs ovrtx 0.322) and door panel
// (materialId 7): these surfaces now trace the real dark interior
// instead of the sky, but that interior geometry's own ambient fill
// was missing at the reflection hit.
//
// Occlusion-aware gate (2026-07-06 follow-up to the "ISOLATION" note
// this replaces) — a FIRST attempt added this term with `aoVisibility`
// hardcoded to 1.0 (fully unoccluded): that over-brightened enclosed
// interior surfaces seen in reflections (net regression 0.2135->0.2167,
// mean 0.502->0.543) because most of an indoor scene's reflected
// surfaces (walls, undersides, corners) ARE significantly
// self-occluded from the sky and should receive little dome fill, not
// the full unoccluded amount. The fix traces one SHORT cosine-hemisphere
// AO ray from the reflection hit (same TMax = gQuality.aoRayLength,
// transmissive-passthrough any-hit gate, and pre-trace-occluded /
// miss-writes-1.0 `OcclusionPayload` convention AmbientOcclusionPass /
// direct_lighting.slang's dome-NEE ray use — see this file's header for
// the dedicated HitGroupAo/AoMissMain pair this needs) and passes its
// result as `aoVisibility` into EvaluateDomeAmbient, so enclosed/occluded
// reflected surfaces (most of this indoor scene) get little fill and
// open ones get full — exactly the term the blanket 1.0 version lacked.
// A per-hit ray (not a fixed scale) was feasible here because this
// pass's own pipeline recursion budget only needed bumping 1->2 (no
// other pass shares it), so the "acceptable cheaper alternative" (a
// fixed ambient scale) documented as a fallback in the task spec was not
// needed.
//
// One-bounce GI at the reflection hit (NOT implemented) — a reflected
// diffuse surface would ideally also receive real single-bounce indirect
// light (matching IndirectDiffusePass's own bounce estimator), but that
// needs its own cosine-hemisphere TraceRay one level deeper still
// (recursion level 3 minimum: RayGen's reflection ray = 1, this AO ray =
// 2, a GI bounce = 3) plus a NEE shadow ray from THAT hit (level 4) —
// meaningfully more recursion budget and cost than the AO-gated ambient
// term for a second-order effect on an already-indirect (dome-lit)
// signal. Dome-ambient×AO is the pragmatic term this fix stops at;
// tracked as debt, not implemented here.
// --------------------------------------------------------------------
[shader("closesthit")]
void ClosestHitMain(inout ReflectionPayload payload, in BuiltInTriangleIntersectionAttributes attribs) {
    const float3 worldHit = WorldRayOrigin() + RayTCurrent() * WorldRayDirection();
    const SurfaceMaterialEval surf = EvaluateSurfaceMaterial(
        InstanceID(), PrimitiveIndex(), attribs.barycentrics,
        worldHit, WorldRayDirection(), RayTCurrent(),
        ObjectToWorld3x4(), WorldToObject3x4());
    const float3 viewDir = normalize(-WorldRayDirection());

    // SHARC (rtx-realtime-alignment-design.md, 2026-07-07) — supply the
    // indirect-GI term this cheap reflection-hit shade is missing (see the
    // "One-bounce GI at the reflection hit (NOT implemented)" note below).
    // The cache holds the reflected surface's FULL diffuse outgoing radiance
    // (direct + dome + emission + infinite-bounce indirect), populated by
    // IndirectDiffusePass. Using it at reflection hits brightens the reflected
    // interior to match the real (primary-shaded) world — the fix for the
    // systematically-too-dark reflective surfaces.
    //
    // NO roughness gate (measured): an earlier version gated on roughness > 0.3
    // to keep sharp mirrors on their traced sample, but the World Lobby's glossy
    // GLASS FLOOR (id30, roughness 0, ~48% of frame MSE) is reflection-dominated
    // and was DIM without the cached GI — including it dropped whole-frame RMSE
    // 0.153 -> 0.126 (id30 local 0.306 -> 0.235). The voxel blur costs less than
    // the brightness match gains, and it matches ovrtx (whose RT reflections also
    // query the radiance cache); the distance-based voxel LoD already keeps
    // near-field reflections finer. True mirror OBJECTS (a decorative mirror)
    // would ideally still trace — a cone-width gate (SHaRC's GetVoxelSize check)
    // is the faithful refinement, tracked as debt. giMode is dynamically uniform
    // => builtin path untouched when off. `payload.roughness` is retained for
    // that future cone gate.
    if (gQuality.giMode != 0u)
    {
        float3 cachedRadiance;
        if (SharcQuery(gShHash, gShResolved, worldHit, SharcCameraWorld(), cachedRadiance))
        {
            payload.color = cachedRadiance;
            payload.hitT  = RayTCurrent();
            return;
        }
    }

    uint lightCount;
    uint _stride;
    gLights.GetDimensions(lightCount, _stride);
    float3 direct = float3(0.0f, 0.0f, 0.0f);
    for (uint i = 0; i < lightCount; ++i) {
        LightGpu L = gLights[i];
        float3 lightDir;
        float3 lightColor;
        float  attenuation;
        float  occluderHitT;  // unused — traceShadow = false below.
        if (!EvaluateDirectLightSample(L, worldHit, surf.normalWorld, /*recursionDepth*/ 1u,
                                       /*traceShadow*/ false, lightDir, lightColor,
                                       attenuation, occluderHitT))
            continue;
        direct += EvaluateOpenPBRDirect(surf.pbr, surf.normalWorld, viewDir, lightDir, lightColor)
                * attenuation;
    }

    // Occlusion-aware ambient — see this function's doc comment above.
    // ONE short cosine-hemisphere AO ray from the reflection hit, TMax =
    // gQuality.aoRayLength (same knob AmbientOcclusionPass's own
    // hemispherical ray uses), through the dedicated HitGroupAo/AoMissMain
    // pair (RayContributionToHitGroupIndex 1 / MissShaderIndex 1 — this
    // pipeline's OTHER hit group/miss (index 0) stay the primary
    // reflection ray's HitGroupDefault/MissMain, untouched).
    float3 aoTangent;
    float3 aoBitangent;
    BuildOrthonormalBasis(surf.normalWorld, aoTangent, aoBitangent);
    Pcg32 aoRng = SeedPcg32(gSampling.rngSeed, gSampling.frameIndex, DispatchRaysIndex().xy,
                            DispatchRaysDimensions().x,
                            MakeStreamId(RNG_STREAM_REFLECTIONS, 0u, 1u));
    const float2 uAo = Pcg32NextFloat2(aoRng);
    const float3 aoDirLocal = CosineSampleHemisphere(uAo);
    const float3 aoDirWorld = LocalToWorld(aoDirLocal, aoTangent, aoBitangent, surf.normalWorld);

    RayDesc aoRay;
    aoRay.Origin    = worldHit + surf.normalWorld * 1e-3f;
    aoRay.Direction = aoDirWorld;
    aoRay.TMin      = 1e-3f;
    aoRay.TMax      = gQuality.aoRayLength;
    OcclusionPayload aoPayload;
    aoPayload.visible = 0.0f;  // pre-trace = occluded
    aoPayload.hitT    = 0.0f;
    TraceRay(gTlas,
             RAY_FLAG_SKIP_CLOSEST_HIT_SHADER | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH,
             0xFFu,
             /*RayContributionToHitGroupIndex*/ 1,
             /*MultiplierForGeometryContributionToHitGroupIndex*/ 0,
             /*MissShaderIndex*/ 1,
             aoRay,
             aoPayload);

    float3 ambient = float3(0.0f, 0.0f, 0.0f);
    EvaluateDomeAmbient(surf.pbr, surf.normalWorld, viewDir, aoPayload.visible, ambient);

    const float3 emission = OpenPBREmission(surf.pbr);
    payload.color = direct + ambient + emission * surf.opacity;
    payload.hitT  = RayTCurrent();
}

// --------------------------------------------------------------------
// AoAnyHitMain — the occlusion-aware-ambient AO ray's any-hit gate.
// SEPARATE hit group from the primary reflection ray's HitGroupDefault
// (which stays any-hit-free — "alpha-tested cutout geometry reads as
// solid in Phase A reflections", this file's header) — this hit group
// exists ONLY to serve the AO ray ClosestHitMain fires above (selected
// via RayContributionToHitGroupIndex=1), so adding an any-hit here
// cannot change the primary reflection ray's behavior. Same
// alpha-tested / transmissive-passthrough gate every other shadow/AO
// ray any-hit in this codebase uses (direct_lighting.slang's
// AnyHitMain, ambient_occlusion.slang's AnyHitMain).
// --------------------------------------------------------------------
[shader("anyhit")]
void AoAnyHitMain(inout OcclusionPayload payload, in BuiltInTriangleIntersectionAttributes attribs) {
    if (AnyHitIsAlphaTested()) {
        IgnoreHit();
        return;
    }
    if (AnyHitIsTransmissive()) {
        IgnoreHit();
        return;
    }
    payload.hitT = RayTCurrent();
}

// --------------------------------------------------------------------
// AoMissMain — the AO ray escaped the hemisphere within aoRayLength;
// unoccluded (matches OcclusionPayload's "miss writes 1.0" convention).
// --------------------------------------------------------------------
[shader("miss")]
void AoMissMain(inout OcclusionPayload payload) {
    payload.visible = 1.0f;
}

// --------------------------------------------------------------------
// MissMain — the reflection ray escaped to the background; sample the
// SAME dome background function every other miss path uses.
// --------------------------------------------------------------------
[shader("miss")]
void MissMain(inout ReflectionPayload payload) {
    payload.color = SampleDomeBackground(WorldRayDirection());
    payload.hitT  = -1.0f;
}
