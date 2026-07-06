// Pyxis renderer — IndirectDiffusePass, ONE shader file (RTX-alignment
// design, rtx-realtime-alignment-design.md, WP2-signals Phase A / Phase B
// estimator swap).
//
// Phase B: real 1 spp cosine-hemisphere path-traced bounce — replaces the
// Phase A deterministic dome-mip ambient approximation with actual GI
// color bleeding:
//   1. Cosine-importance-sample `gQuality.indirectSamples` bounce
//      direction(s) around the primary hit's shading normal.
//   2. At each bounce hit: evaluate the hit surface's material
//      (EvaluateSurfaceMaterial, shared), draw ONE NEE light sample
//      (uniform light pick, Dome excluded, same shape-aware point/
//      direction sampling direct_lighting.slang's RIS uses) + the hit's
//      own emission. On miss: sample the dome at FULL resolution (mip 0,
//      via the shared SampleDomeBackground — no mip-bias trick).
//   3. Average the `indirectSamples` returns, clamp to
//      `gQuality.maxRayIntensityIndirect`.
// Cosine-weighted importance sampling of a Lambertian lobe makes the
// BSDF/cosine/pdf terms cancel EXACTLY (contribution = albedo x Li), so
// the raw averaged bounce radiance IS the correctly-demodulated ambient
// signal this pass has always produced — no DemodulateByAlbedo divide
// needed (and, unlike Phase A's divide, this can't lose energy on
// near-zero-albedo/metal primary surfaces). CompositePass re-modulates by
// the PRIMARY surface's real (OpenPBR-complete) albedo, same as before.
//
// `gQuality.indirectMaxBounces` (default 2) IS now implemented: at the
// first bounce hit, ClosestHitMain's `BouncePayload.bounceDepth` (1 for
// this hit) is compared against `gQuality.indirectMaxBounces`; when it
// still has budget, the hit continues the path — one more cosine-
// hemisphere direction is sampled around THIS hit's own shading normal,
// traced, and shaded by the SAME recursive ClosestHitMain/MissMain
// estimator (direct-light NEE + emissive at the second hit, dome on
// miss). Unlike the primary surface's demodulated output, this SECOND
// hit's returned radiance arrives at the FIRST hit as real incoming
// light and is therefore explicitly weighted by the first hit's own
// diffuse albedo (`surf.baseColor`, the same value CompositePass uses to
// demodulate the primary signal) before folding into `payload.radiance`
// — see the continuation block in ClosestHitMain below. Recursion is
// hard-capped at bounce depth `MAX_INDIRECT_BOUNCE_DEPTH` (2) to match
// this pipeline's compiled `maxRecursionDepth` budget (IndirectDiffusePass.cpp);
// `indirectMaxBounces` values above 2 are clamped rather than silently
// mis-tracing — bounces beyond the second (and other missing transport)
// remain follow-up debt, see the design doc's Phase B section.
//
// "Interior illumination" follow-up (Phase B.2) — GI through glass: the
// bounce ray used to treat a transmissive hit (glass curtain wall) as an
// opaque surface, so a bounce aimed at a window shaded dark glass instead
// of reaching the sky behind it. AnyHitMain now lets the ray continue
// THROUGH transmissive surfaces (up to MAX_INDIRECT_TRANSMISSIVE_
// CONTINUATIONS), attenuating `BouncePayload::throughput` by each
// surface's transmission tint — see AnyHitMain's own doc comment.
//
// "Interior illumination" follow-up (Phase B.2 continued) — dome-NEE at
// bounce hits: ClosestHitMain now ALSO samples the dome directly (ONE
// cosine-hemisphere sample, budgeted) at every bounce hit, the same
// "IBL sampling as part of direct lighting" term direct_lighting.slang's
// ComputeDomeDirectDiffuse contributes at the PRIMARY hit. Before this, an
// interior second surface only received dome energy via a bounce ray that
// happened to escape to the sky — see ClosestHitMain's own doc comment for
// the full derivation and the recursion-budget analysis.
//
// Bindings — Set 0 (SceneBindings, shared verbatim with every other RT
// pass; declared transitively via shading.slang's include chain).
// Set 1 (this pass's own I/O):
//   set=1, binding=0 : StructuredBuffer<VisibilityGpu> (GBuffer pass output)
//   set=1, binding=1 : RWTexture2D<float4> gIndirectDiffuse (RGBA16F; .a = 1)

#include "ShaderInterop.slang"
#include "camera_ray.slang"  // gCamera (0) + gQuality (15) + gSampling (16) + BuildCameraRay
#include "shading.slang"     // gTlas (1) + full Set-0 scene tables + EvaluateSurfaceMaterial
                             // + EvaluateOpenPBRDirect / OpenPBREmission; pulls in dome_sample.slang
                             // (gLights / gDomeEnvMap / gDomeSampler / SampleDomeBackground)
#include "rng.slang"         // Pcg32 + cosine-hemisphere sampling + MakeStreamId

[[vk::binding(0, 1)]] StructuredBuffer<VisibilityGpu> gVisibility;
[[vk::binding(1, 1)]] RWTexture2D<float4>              gIndirectDiffuse;

float IndirectLuminance(float3 c) {
    return dot(c, float3(0.2126, 0.7152, 0.0722));
}

// One payload type for BOTH ray types this pipeline issues (matches the
// existing single-payload-per-pipeline convention — see
// raytraced_lighting_anyhit.slang's doc comment): the bounce ray uses
// `radiance` as the returned incoming radiance; the NEE shadow ray fired
// FROM ClosestHitMain reuses the SAME struct, `radiance.x` doubling as the
// 0/1 visibility signal (pre-trace 0 = occluded, ShadowMissMain writes 1).
// `sampleIndex` is set by RayGen before the bounce-ray TraceRay so
// ClosestHitMain's own NEE draw can seed a stream decorrelated from the
// other bounce samples at the SAME pixel (plan §12.3 "no two bounces
// share a stream").
struct BouncePayload {
    float3 radiance;
    uint   sampleIndex;
    // RTX-alignment design (rtx-realtime-alignment-design.md), Phase B —
    // second indirect-diffuse bounce. Which bounce THIS ray/hit is: 1 for
    // the ray RayGen fires from the primary surface, 2 for the
    // continuation ray ClosestHitMain fires from the first bounce hit (see
    // the continuation block below). Doubles as the NEE-pick stream's
    // `bounceIndex` term (MakeStreamId) so each hit's own light draw uses
    // a distinct, stable stream — 1 at the first hit reproduces exactly
    // the stream this pass has always used there (byte-equal for
    // `indirectMaxBounces == 1`). Compared against
    // `gQuality.indirectMaxBounces` (clamped to MAX_INDIRECT_BOUNCE_DEPTH)
    // to decide whether a hit continues the path. Unused by the NEE
    // shadow ray reusing this struct — set for hygiene only, like
    // `throughput` below.
    uint   bounceDepth;
    // RTX-alignment design (rtx-realtime-alignment-design.md), Phase B
    // follow-up ("interior illumination" fix) — GI through glass.
    // Accumulated attenuation from every transmissive surface the BOUNCE
    // ray has passed through so far on its way to `radiance`'s hit/miss
    // point: AnyHitMain multiplies this by the hit's transmission tint and
    // `IgnoreHit()`s, letting the SAME TraceRay continue past the glass
    // instead of stopping at it. Starts at (1,1,1); ClosestHitMain /
    // MissMain multiply their own returned term by it before writing
    // `radiance`. Unused by the NEE shadow ray reusing this struct (see
    // the doc comment above) — left at whatever the caller set it to.
    float3 throughput;
    // Bounded continuation counter (mirrors translucency.slang's
    // MAX_TRANSPARENT_SEGMENTS cap philosophy, scaled down for this cheap
    // 1 spp estimate) — AnyHitMain stops ignoring transmissive hits once
    // this reaches MAX_INDIRECT_TRANSMISSIVE_CONTINUATIONS, so a stack of
    // many glass panes eventually resolves to an opaque shaded hit instead
    // of looping unbounded.
    uint transmissiveHitCount;
};

// Bounce-ray transmissive-continuation cap — see BouncePayload's doc
// comment. 4 (vs. translucency.slang's 16-segment MAX_TRANSPARENT_SEGMENTS)
// because this is a single 1 spp GI sample, not the primary transparency
// composite; a curtain wall's few glass panes fit comfortably under it.
static const uint MAX_INDIRECT_TRANSMISSIVE_CONTINUATIONS = 4u;

// Hard cap on `BouncePayload::bounceDepth` continuation (see its doc
// comment) — matches this pipeline's compiled `maxRecursionDepth` budget
// (IndirectDiffusePass.cpp: raygen bounce (1) + this pass's own
// second-bounce continuation (2) + the second hit's NEE shadow ray (3)).
// `gQuality.indirectMaxBounces` is clamped to this rather than trusted
// verbatim, so a misconfigured value above 2 degrades to the 2-bounce
// estimator instead of under-tracing the recursion budget.
static const uint MAX_INDIRECT_BOUNCE_DEPTH = 2u;

[shader("raygeneration")]
void RayGenMain() {
    const uint2 launchIndex = DispatchRaysIndex().xy;
    const uint2 launchDim   = DispatchRaysDimensions().xy;

    const VisibilityGpu vis = gVisibility[launchIndex.y * launchDim.x + launchIndex.x];
    if (vis.hitT < 0.0f)
    {
        gIndirectDiffuse[launchIndex] = float4(0.0f, 0.0f, 0.0f, 1.0f);
        return;
    }

    float3 worldOriginXyz;
    float3 worldDirXyz;
    BuildCameraRay(launchIndex, launchDim, worldOriginXyz, worldDirXyz);
    const float3 worldHit = worldOriginXyz + worldDirXyz * vis.hitT;
    const float2 visBary = float2(f16tof32(vis.baryPacked & 0xFFFFu),
                                   f16tof32(vis.baryPacked >> 16u));
    const InstanceInfoGpu inst = gInstanceInfo[vis.instanceId];
    const float3x4 worldToObject = InvertAffine3x4(inst.objectToWorld);
    const SurfaceMaterialEval surf = EvaluateSurfaceMaterial(
        vis.instanceId, vis.primitiveIndex, visBary,
        worldHit, worldDirXyz, vis.hitT,
        inst.objectToWorld, worldToObject);

    float3 tangent;
    float3 bitangent;
    BuildOrthonormalBasis(surf.normalWorld, tangent, bitangent);

    Pcg32 rng = SeedPcg32(gSampling.rngSeed, gSampling.frameIndex, launchIndex, launchDim.x,
                          MakeStreamId(RNG_STREAM_INDIRECT_DIFFUSE, 0u, 0u));

    const uint sampleCount = max(gQuality.indirectSamples, 1u);
    float3 radianceSum = float3(0.0, 0.0, 0.0);

    [loop]
    for (uint s = 0u; s < sampleCount; ++s) {
        const float2 u = Pcg32NextFloat2(rng);
        const float3 dirLocal = CosineSampleHemisphere(u);
        const float3 dirWorld = LocalToWorld(dirLocal, tangent, bitangent, surf.normalWorld);

        RayDesc bounceRay;
        bounceRay.Origin    = worldHit + surf.normalWorld * 1e-3;
        bounceRay.Direction = dirWorld;
        bounceRay.TMin      = 1e-3;
        bounceRay.TMax      = 10000.0;

        BouncePayload payload;
        payload.radiance             = float3(0.0, 0.0, 0.0);
        payload.sampleIndex          = s;
        payload.bounceDepth          = 1u;  // first bounce — see BouncePayload's doc comment.
        // GI-through-glass (see BouncePayload's doc comment) — starts
        // fully clear; AnyHitMain attenuates as the ray crosses glass.
        payload.throughput           = float3(1.0, 1.0, 1.0);
        payload.transmissiveHitCount = 0u;
        TraceRay(gTlas, RAY_FLAG_NONE, 0xFFu, 0, 0, /*MissShaderIndex*/ 0, bounceRay, payload);
        radianceSum += payload.radiance;
    }

    float3 meanRadiance = radianceSum / float(sampleCount);
    const float lum = IndirectLuminance(meanRadiance);
    if (lum > gQuality.maxRayIntensityIndirect && lum > 1e-9) {
        meanRadiance *= gQuality.maxRayIntensityIndirect / lum;
    }

    gIndirectDiffuse[launchIndex] = float4(meanRadiance, 1.0f);
}

// --------------------------------------------------------------------
// ClosestHitMain — the bounce hit's shading: one NEE light sample (single
// uniform light pick, shape-aware point/direction — same technique
// direct_lighting.slang's RIS candidate sampler uses, without the
// multi-candidate reservoir since this is a single-sample draw) +
// emission. Fires its own shadow ray (SAME BouncePayload type, reused as
// a visibility scalar — see the struct's doc comment).
// --------------------------------------------------------------------
[shader("closesthit")]
void ClosestHitMain(inout BouncePayload payload, in BuiltInTriangleIntersectionAttributes attribs) {
    const float3 worldHit = WorldRayOrigin() + RayTCurrent() * WorldRayDirection();
    const SurfaceMaterialEval surf = EvaluateSurfaceMaterial(
        InstanceID(), PrimitiveIndex(), attribs.barycentrics,
        worldHit, WorldRayDirection(), RayTCurrent(),
        ObjectToWorld3x4(), WorldToObject3x4());
    const float3 viewDir = normalize(-WorldRayDirection());

    uint lightCount;
    uint _stride;
    gLights.GetDimensions(lightCount, _stride);

    float3 directContribution = float3(0.0, 0.0, 0.0);
    if (lightCount > 0u) {
        Pcg32 hitRng = SeedPcg32(gSampling.rngSeed, gSampling.frameIndex, DispatchRaysIndex().xy,
                                 DispatchRaysDimensions().x,
                                 MakeStreamId(RNG_STREAM_INDIRECT_DIFFUSE, payload.sampleIndex,
                                              payload.bounceDepth));
        const uint lightIndex =
            min(uint(Pcg32NextFloat(hitRng) * float(lightCount)), lightCount - 1u);
        const LightGpu L = gLights[lightIndex];

        if (L.intensity > 0.0 && L.kind != LIGHT_KIND_DOME) {
            const float3 lightColor = float3(L.colorR, L.colorG, L.colorB) * L.intensity;
            float3 lightDir;
            float  shadowTMax;
            float  geometryTerm;

            if (L.kind == LIGHT_KIND_RECT) {
                const float2 uv = Pcg32NextFloat2(hitRng) * 2.0 - 1.0;
                const float3 axisU = float3(L.axisUx, L.axisUy, L.axisUz);
                const float3 axisV = float3(L.axisVx, L.axisVy, L.axisVz);
                const float3 samplePos =
                    float3(L.posX, L.posY, L.posZ) + uv.x * axisU + uv.y * axisV;
                const float3 toLight = samplePos - worldHit;
                const float distSq = dot(toLight, toLight);
                shadowTMax = sqrt(max(distSq, 1e-8));
                lightDir = toLight / shadowTMax;
                // Rect facing test — see direct_lighting.slang's
                // SampleLightCandidate for the full sign derivation
                // (UsdLuxRectLight emits toward LOCAL -Z, i.e. away from
                // +cross(axisU,axisV); the correct facing dot product
                // uses +lightDir, not -lightDir).
                const float3 lightNormal = normalize(cross(axisU, axisV));
                float cosLight = dot(lightNormal, lightDir);
                cosLight = (L.doubleSided != 0u) ? abs(cosLight) : max(cosLight, 0.0);
                const float area = 4.0 * length(axisU) * length(axisV);
                geometryTerm = (cosLight > 1e-6 && distSq > 1e-8) ? cosLight * area / distSq : 0.0;
            } else if (L.kind == LIGHT_KIND_DISTANT) {
                lightDir = -float3(L.dirX, L.dirY, L.dirZ);
                shadowTMax = 1e30;
                geometryTerm = 1.0;
            } else {
                const float3 toLight = float3(L.posX, L.posY, L.posZ) - worldHit;
                const float distSq = dot(toLight, toLight);
                shadowTMax = sqrt(max(distSq, 1e-8));
                lightDir = toLight / shadowTMax;
                geometryTerm = 1.0 / max(distSq, 0.01);
            }

            if (geometryTerm > 0.0 && dot(surf.normalWorld, lightDir) > 0.0) {
                float3 contribution =
                    EvaluateOpenPBRDirect(surf.pbr, surf.normalWorld, viewDir, lightDir, lightColor)
                    * geometryTerm;

                RayDesc shadowRay;
                shadowRay.Origin    = worldHit + surf.normalWorld * 1e-3;
                shadowRay.Direction = lightDir;
                shadowRay.TMin      = 1e-3;
                shadowRay.TMax      = max(1e-3, shadowTMax - 1e-3);
                BouncePayload shadowPayload;
                shadowPayload.radiance             = float3(0.0, 0.0, 0.0);  // pre-trace = occluded
                shadowPayload.sampleIndex          = 0u;
                shadowPayload.bounceDepth          = 0u;
                // Unused by the shadow-ray AnyHitMain branch (see
                // BouncePayload's doc comment) — initialised for hygiene only.
                shadowPayload.throughput           = float3(1.0, 1.0, 1.0);
                shadowPayload.transmissiveHitCount = 0u;
                TraceRay(gTlas,
                         RAY_FLAG_SKIP_CLOSEST_HIT_SHADER
                           | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH,
                         0xFFu, 0, 0, /*MissShaderIndex*/ 1, shadowRay, shadowPayload);

                // Single uniform light pick over `lightCount` lights is an
                // unbiased estimator of the SUM over all lights when
                // scaled by lightCount (standard one-sample NEE).
                directContribution =
                    contribution * shadowPayload.radiance.x * float(lightCount);
            }
        }
    }

    const float3 emission = OpenPBREmission(surf.pbr) * surf.opacity;

    // "Interior illumination" follow-up (Phase B.2 continued) — dome-light
    // IBL sampling at BOUNCE hits, this file's equivalent of
    // direct_lighting.slang's ComputeDomeDirectDiffuse for the PRIMARY hit.
    // Before this, an interior second surface (a bounce hit off a wall, say)
    // only received the dome's radiance when the NEXT bounce ray happened to
    // escape to the sky (MissMain) — an extra bounce away and comparatively
    // rare — which is why enabling the second indirect bounce only lifted
    // the World Lobby image mean 0.295 -> 0.304 instead of the larger jump
    // expected from a dome-lit interior. ONE dome sample (budget) per hit,
    // gated on DomeLightExists() so dome-less fixtures stay byte-for-byte
    // unchanged, on a PCG32 stream distinct from both this hit's NEE pick
    // (stream tag == bounceDepth) and the second-bounce continuation
    // direction (stream tag == 10 + bounceDepth, below) — see the +20 offset.
    // Reuses THIS FILE's own BouncePayload-as-shadow-ray convention
    // (radiance.x as 0/1 visibility, MissShaderIndex 1 == ShadowMissMain),
    // NOT direct_lighting.slang's OcclusionPayload, which this pipeline's
    // miss-shader table doesn't wire up. Folded into `payload.radiance`
    // below exactly like `directContribution` — both are demodulated
    // incoming light arriving at this hit.
    //
    // Recursion budget: this fires from ClosestHitMain at EITHER bounce
    // depth (1 or 2). At depth 1 (the raygen bounce ray's hit, itself
    // running at ray-recursion level 1) this TraceRay lands at level 2 —
    // the SAME level as this hit's own NEE shadow ray and its second-bounce
    // continuation ray, already covered by maxRecursionDepth=3
    // (IndirectDiffusePass.cpp). At depth 2 (the continuation hit, running
    // at level 2) this TraceRay lands at level 3 — the SAME level as that
    // hit's own NEE shadow ray, ALSO already covered. Sibling TraceRay
    // calls at an already-reached depth don't deepen the required
    // maxRecursionDepth (only the longest nesting CHAIN does), so no bump
    // is needed here.
    float3 domeContribution = float3(0.0, 0.0, 0.0);
    if (DomeLightExists()) {
        float3 domeTangent;
        float3 domeBitangent;
        BuildOrthonormalBasis(surf.normalWorld, domeTangent, domeBitangent);

        Pcg32 domeRng = SeedPcg32(gSampling.rngSeed, gSampling.frameIndex, DispatchRaysIndex().xy,
                                  DispatchRaysDimensions().x,
                                  MakeStreamId(RNG_STREAM_INDIRECT_DIFFUSE, payload.sampleIndex,
                                               20u + payload.bounceDepth));
        const float2 uDome = Pcg32NextFloat2(domeRng);
        const float3 domeDirLocal = CosineSampleHemisphere(uDome);
        const float3 domeDirWorld =
            LocalToWorld(domeDirLocal, domeTangent, domeBitangent, surf.normalWorld);

        RayDesc domeRay;
        domeRay.Origin    = worldHit + surf.normalWorld * 1e-3;
        domeRay.Direction = domeDirWorld;
        domeRay.TMin      = 1e-3;
        domeRay.TMax      = 10000.0;
        BouncePayload domeShadowPayload;
        domeShadowPayload.radiance             = float3(0.0, 0.0, 0.0);  // pre-trace = occluded
        domeShadowPayload.sampleIndex          = 0u;
        domeShadowPayload.bounceDepth          = 0u;
        // Unused by the shadow-ray AnyHitMain branch (see BouncePayload's
        // doc comment) — initialised for hygiene only.
        domeShadowPayload.throughput           = float3(1.0, 1.0, 1.0);
        domeShadowPayload.transmissiveHitCount = 0u;
        TraceRay(gTlas,
                 RAY_FLAG_SKIP_CLOSEST_HIT_SHADER | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH,
                 0xFFu, 0, 0, /*MissShaderIndex*/ 1, domeRay, domeShadowPayload);

        // Cosine-weighted importance sampling cancels BSDF x cos / pdf
        // exactly (same argument as RayGen's own bounce estimator, file
        // header) — the visibility-gated dome radiance IS the demodulated
        // contribution, no further weighting needed.
        float3 domeValue = SampleDomeBackground(domeDirWorld) * domeShadowPayload.radiance.x;
        const float domeLum = IndirectLuminance(domeValue);
        if (domeLum > gQuality.maxRayIntensityIndirect && domeLum > 1e-9)
            domeValue *= gQuality.maxRayIntensityIndirect / domeLum;
        domeContribution = domeValue;
    }

    // RTX-alignment design, Phase B — second indirect-diffuse bounce
    // (see this file's header). When this hit still has bounce budget,
    // continue the path: sample ONE more cosine-hemisphere direction
    // around THIS hit's own shading normal (identical Malley's-method
    // importance sampling to RayGen's first-bounce sample, so the
    // BSDF/cosine/pdf cancellation is exact here too), trace it, and let
    // the SAME ClosestHitMain/MissMain estimator shade the second hit
    // (direct-light NEE + emissive, or dome on miss — recursion, not a
    // duplicated code path). `gQuality.indirectMaxBounces == 1` makes
    // this condition false at bounceDepth 1, reproducing today's
    // single-bounce output byte-for-bit (secondBounceRadiance stays the
    // zero-initialized float3 below, and x + 0 is exact in IEEE 754).
    float3 secondBounceRadiance = float3(0.0, 0.0, 0.0);
    if (payload.bounceDepth < min(gQuality.indirectMaxBounces, MAX_INDIRECT_BOUNCE_DEPTH)) {
        float3 bounceTangent;
        float3 bounceBitangent;
        BuildOrthonormalBasis(surf.normalWorld, bounceTangent, bounceBitangent);

        // New stream id for the second-bounce direction — offset by +10
        // so it can never collide with this (or any) hit's own NEE-pick
        // stream (bounceIndex == bounceDepth, i.e. 1 or 2 today); see
        // BouncePayload's doc comment.
        Pcg32 dirRng = SeedPcg32(gSampling.rngSeed, gSampling.frameIndex, DispatchRaysIndex().xy,
                                 DispatchRaysDimensions().x,
                                 MakeStreamId(RNG_STREAM_INDIRECT_DIFFUSE, payload.sampleIndex,
                                              10u + payload.bounceDepth));
        const float2 u2 = Pcg32NextFloat2(dirRng);
        const float3 dirLocal2 = CosineSampleHemisphere(u2);
        const float3 dirWorld2 =
            LocalToWorld(dirLocal2, bounceTangent, bounceBitangent, surf.normalWorld);

        RayDesc secondBounceRay;
        secondBounceRay.Origin    = worldHit + surf.normalWorld * 1e-3;
        secondBounceRay.Direction = dirWorld2;
        secondBounceRay.TMin      = 1e-3;
        secondBounceRay.TMax      = 10000.0;

        BouncePayload nextPayload;
        nextPayload.radiance             = float3(0.0, 0.0, 0.0);
        nextPayload.sampleIndex          = payload.sampleIndex;
        nextPayload.bounceDepth          = payload.bounceDepth + 1u;
        // GI-through-glass — fresh budget for this new ray, same as the
        // first bounce ray gets in RayGen.
        nextPayload.throughput           = float3(1.0, 1.0, 1.0);
        nextPayload.transmissiveHitCount = 0u;
        TraceRay(gTlas, RAY_FLAG_NONE, 0xFFu, 0, 0, /*MissShaderIndex*/ 0, secondBounceRay,
                 nextPayload);

        // Cancellation (file header): the returned radiance is what
        // arrives at THIS hit as incoming light, so — unlike the primary
        // surface's demodulated output, which CompositePass re-modulates
        // downstream — it must be explicitly weighted by THIS hit's own
        // diffuse albedo before folding into `payload.radiance`.
        // `surf.baseColor` is the same post-texture albedo CompositePass
        // uses to demodulate the primary signal (gAlbedo); reused here
        // for the internal bounce vertex since nothing downstream
        // re-modulates it for us.
        secondBounceRadiance = nextPayload.radiance * surf.baseColor;
    }

    // GI-through-glass (RTX-alignment design Phase B follow-up) — this hit
    // point may lie BEHIND one or more transmissive surfaces the bounce ray
    // already passed through (AnyHitMain); `payload.throughput` carries
    // their accumulated tint attenuation and is (1,1,1) in the common case
    // (no glass crossed), so this multiply is a no-op then.
    payload.radiance =
        (directContribution + domeContribution + emission + secondBounceRadiance)
        * payload.throughput;
}

// --------------------------------------------------------------------
// MissMain — the bounce ray escaped to the background; sample the dome at
// FULL resolution (mip 0, via the shared SampleDomeBackground) — replaces
// Phase A's mip-biased "fast diffuse IBL" approximation with the real
// path-traced miss term the frozen spec calls for. Attenuated by
// `payload.throughput` for the same GI-through-glass reason as
// ClosestHitMain above (the sky seen THROUGH a tinted curtain wall keeps
// that tint, rather than reading full-bright).
// --------------------------------------------------------------------
[shader("miss")]
void MissMain(inout BouncePayload payload) {
    payload.radiance = SampleDomeBackground(WorldRayDirection()) * payload.throughput;
}

// --------------------------------------------------------------------
// ShadowMissMain — the NEE shadow ray's miss shader (unoccluded = 1.0 in
// `radiance.x`, the same reused-payload convention as the megakernel's
// HitInfo.color.x visibility trick — see BouncePayload's doc comment).
// --------------------------------------------------------------------
[shader("miss")]
void ShadowMissMain(inout BouncePayload payload) {
    payload.radiance = float3(1.0, 0.0, 0.0);
}

// --------------------------------------------------------------------
// AnyHitMain — ONE any-hit shader serving BOTH ray types (matches
// raytraced_lighting_anyhit.slang's established pattern): alpha-tested
// geometry is always ignored for both ray types.
//
// SHADOW ray (RAY_FLAG_SKIP_CLOSEST_HIT_SHADER set): transmissive geometry
// passes through unconditionally (glass shouldn't cast an opaque shadow
// onto the GI estimate) — the SAME binary convention every other shadow
// ray in this codebase uses (commit eed2ad9), unchanged.
//
// BOUNCE ray (RTX-alignment design Phase B follow-up, "interior
// illumination" fix — GI through glass): a transmissive hit no longer
// stops the ray as an opaque wall. It attenuates `payload.throughput` by
// the material's transmission tint (AnyHitTransmissionTint,
// raytraced_anyhit.slang) and `IgnoreHit()`s, letting the SAME TraceRay
// keep going — so a GI ray aimed at a glazed curtain wall can reach the
// sky/dome behind it instead of shading the glass pane as if it were a
// diffuse opaque surface. Bounded by
// MAX_INDIRECT_TRANSMISSIVE_CONTINUATIONS (mirrors translucency.slang's
// segment-cap philosophy): once exhausted, the hit is accepted as opaque
// (no IgnoreHit) and ClosestHitMain shades it normally — the pre-existing
// Phase B simplification, now only reachable past the cap.
// --------------------------------------------------------------------
[shader("anyhit")]
void AnyHitMain(inout BouncePayload payload, in BuiltInTriangleIntersectionAttributes attribs) {
    if (AnyHitIsAlphaTested()) {
        IgnoreHit();
        return;
    }
    if ((RayFlags() & RAY_FLAG_SKIP_CLOSEST_HIT_SHADER) != 0u) {
        if (AnyHitIsTransmissive()) {
            IgnoreHit();
        }
        return;
    }
    if (AnyHitIsTransmissive()
        && payload.transmissiveHitCount < MAX_INDIRECT_TRANSMISSIVE_CONTINUATIONS) {
        payload.throughput *= AnyHitTransmissionTint();
        payload.transmissiveHitCount += 1u;
        IgnoreHit();
    }
}
