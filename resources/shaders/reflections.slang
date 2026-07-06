// Pyxis renderer — ReflectionsPass, ONE shader file (RTX-alignment
// design, rtx-realtime-alignment-design.md, WP2-signals Phase A / Phase B
// estimator swap).
//
// RTX-RT single-bounce reflections. The PRIMARY surface's normal +
// roughness come straight from RaytracedGBufferPass's gNormalRoughness
// guide (Texture_SRV) — no material re-eval for the primary surface,
// only geometry. Surfaces with roughness < gQuality.reflectionMaxRoughness
// get `gQuality.reflectionSamples` GGX-VNDF-importance-sampled TraceRay(s)
// (Phase B — Heitz 2018 "Sampling the GGX Distribution of Visible
// Normals"; Phase A traced exactly the deterministic mirror direction);
// rougher surfaces take a roughness-proportional dome-mip sample instead
// (the "rough fallback" analog of a roughness-cache term, unchanged). At
// each reflection hit: EvaluateSurfaceMaterial (shared) + the shared
// direct-light loop WITHOUT shadow rays (EvaluateDirectLightSample,
// traceShadow = false — deliberately cheap) + emissive; misses sample the
// shared dome background (SampleDomeBackground). Phase B also clamps the
// averaged result to gQuality.maxRayIntensityReflections.
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

#include "ShaderInterop.slang"
#include "camera_ray.slang"  // gCamera (0) + gQuality (15) + gSampling (16) + BuildCameraRay
#include "shading.slang"     // gTlas (1) + full Set-0 scene tables + EvaluateSurfaceMaterial
                             // + EvaluateDirectLightSample; pulls in dome_sample.slang
                             // (gLights / gDomeEnvMap / gDomeSampler / latLongUv /
                             // SampleDomeBackground) transitively.
#include "rng.slang"         // Pcg32 + BuildOrthonormalBasis + MakeStreamId

[[vk::binding(0, 1)]] StructuredBuffer<VisibilityGpu> gVisibility;
[[vk::binding(1, 1)]] Texture2D<float4>                gNormalRoughness;
[[vk::binding(2, 1)]] RWTexture2D<float4>              gReflections;
[[vk::binding(3, 1)]] RWTexture2D<float4>              gReflectionWeight;

// The reflection ray's payload — slimmer than HitInfo (this pass runs no
// shadow/AO/continuation rays from the hit, so it needs only the
// resulting radiance + hit distance).
struct ReflectionPayload {
    float3 color;
    float  hitT;  // RayTCurrent() at the hit; -1.0 on miss.
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
// here (single-sample-per-pixel, no accumulation).
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
// ~6 for the diffuse-IBL approximation) — this one's mip scales linearly
// with roughness, approximating a prefiltered-mip environment convolution
// without an actual mip chain built for that purpose.
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
    const float targetMip = saturate(roughness) * maxMip;
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
    const float3 specWeight = OpenPBRReflectionWeight(surf.pbr, surf.normalWorld, viewDir);
    gReflectionWeight[launchIndex] = float4(specWeight, 1.0f);

    // WP2-final — runtime RealTimeQuality.reflectionMaxRoughness (SceneBindings
    // gQuality) replaces the REFLECTION_MAX_ROUGHNESS compile-time constant;
    // ShaderInterop.slang's literal now only backs the C++-side
    // RenderSettings::RealTimeQuality::reflectionMaxRoughness default.
    if (roughness >= gQuality.reflectionMaxRoughness)
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
    // gQuality.reflectionMaxRoughness the microfacet lobe is narrow
    // enough that a handful of VNDF samples resolve a representative
    // glossy-reflection direction rather than the single sharp mirror
    // ray Phase A always traced.
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
        TraceRay(gTlas, RAY_FLAG_NONE, 0xFFu, 0, 0, 0, reflRay, payload);

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
// UNSHADOWED direct-light loop + emissive. Deliberately cheap (no
// shadow rays here — Phase A cost cap; see the file header).
// --------------------------------------------------------------------
[shader("closesthit")]
void ClosestHitMain(inout ReflectionPayload payload, in BuiltInTriangleIntersectionAttributes attribs) {
    const float3 worldHit = WorldRayOrigin() + RayTCurrent() * WorldRayDirection();
    const SurfaceMaterialEval surf = EvaluateSurfaceMaterial(
        InstanceID(), PrimitiveIndex(), attribs.barycentrics,
        worldHit, WorldRayDirection(), RayTCurrent(),
        ObjectToWorld3x4(), WorldToObject3x4());
    const float3 viewDir = normalize(-WorldRayDirection());

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

    const float3 emission = OpenPBREmission(surf.pbr);
    payload.color = direct + emission * surf.opacity;
    payload.hitT  = RayTCurrent();
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
