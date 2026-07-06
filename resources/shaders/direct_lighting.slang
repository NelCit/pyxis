// Pyxis renderer — DirectLightingPass, ONE shader file (RTX-alignment
// design, rtx-realtime-alignment-design.md, WP2-signals Phase A / Phase B
// estimator swap).
//
// Re-derives the primary hit's full material via the shared
// EvaluateSurfaceMaterial (bit-identical inputs to every other RT pass —
// same visibility-buffer reconstruction as raytraced_lighting_raygen.slang).
//
// Phase B: RIS (Resampled Importance Sampling, Talbot et al. 2005 /
// ReSTIR-DI lineage) sampled direct lighting — replaces the Phase A
// deterministic "loop every light, one shadow ray each" estimator with:
//   1. Draw `gQuality.directSamples` candidates, each a UNIFORM light pick
//      (Dome excluded — IndirectDiffusePass's domain) + a shape-correct
//      point/direction on that light (Rect: uniform area over the
//      axisU/axisV rhombus — this also covers Disk/Sphere UsdLux lights,
//      which StageWalker already folds into Kind::Rect at ingest, see
//      SampleLightCandidate's doc comment; Distant: the single authored
//      direction, no angular jitter — LightGpu carries no angular-size
//      data yet, tracked as debt; Cylinder/Geometry/Portal: point-at-
//      position, matching shading.slang's existing treatment — these
//      kinds are force-disabled by PackLightGpu today anyway).
//   2. Weighted-reservoir-select ONE winning candidate (probability
//      proportional to the candidate's unshadowed-contribution luminance
//      — see SampleLightCandidate's derivation comment for why
//      `weight = luminance(unshadowedContribution) * lightCount` is the
//      correct RIS resampling weight for this proposal distribution).
//   3. Fire exactly ONE shadow ray at the winner's sampled point — soft
//      shadows fall out of the area sampling (different pixels resolve
//      different random points on the light, so a partially-occluded
//      light produces a smooth per-pixel visibility gradient instead of a
//      binary hard edge).
//   4. Scale the winner's diffuse/specular by the RIS estimator ratio
//      (`runningSum / (M * winnerLuminance)`) and the traced visibility,
//      clamp the combined luminance to `gQuality.maxRayIntensityDirect`.
// `traceShadow=false`-style below-threshold fallback (LTC analytic
// centroid) is post-v1 per the frozen spec — every candidate here always
// resolves through the RIS + single shadow ray path.
//
// "Interior illumination" follow-up (rtx-realtime-alignment-design.md,
// Phase B.2) — the Dome is still excluded from the RIS reservoir above
// (it has no shape/area to sample a point on the way analytic lights do),
// but it is NO LONGER excluded from DIRECT lighting altogether: see
// ComputeDomeDirectDiffuse below for the separate cosine-hemisphere
// "Dome Light IBL sampling" estimator NVIDIA's own RTX Real-Time computes
// as part of this same pass. Without it, a dome's radiance only reached a
// directly-visible interior surface via IndirectDiffusePass's bounce-ray
// MISS path (an extra bounce away) — World Lobby's interior rendered as a
// night scene as a result.
//
// Bindings — Set 0 (SceneBindings, shared verbatim with every other RT
// pass). Set 1 (this pass's own I/O):
//   set=1, binding=0 : StructuredBuffer<VisibilityGpu> (GBuffer pass output)
//   set=1, binding=1 : RWTexture2D<float4> gDirectDiffuse  (RGBA16F, demodulated)
//   set=1, binding=2 : RWTexture2D<float4> gDirectSpecular (RGBA16F, NOT demodulated)

#include "ShaderInterop.slang"
#include "camera_ray.slang"  // gCamera (0) + gQuality (15) + gSampling (16) + BuildCameraRay
#include "shading.slang"     // gTlas (1) + full Set-0 scene tables + EvaluateSurfaceMaterial
                             // + EvaluateOpenPBRDirectSplit
#include "rng.slang"         // Pcg32 + MakeStreamId (RNG_STREAM_DIRECT_LIGHTING)

[[vk::binding(0, 1)]] StructuredBuffer<VisibilityGpu> gVisibility;
[[vk::binding(1, 1)]] RWTexture2D<float4>              gDirectDiffuse;
[[vk::binding(2, 1)]] RWTexture2D<float4>              gDirectSpecular;

float Luminance(float3 c) {
    return dot(c, float3(0.2126, 0.7152, 0.0722));
}

// RTX-alignment design (rtx-realtime-alignment-design.md), Phase B follow-
// up ("interior illumination" fix) — the dome-light "IBL sampling"
// contribution NVIDIA's own RTX Real-Time computes as part of DIRECT
// lighting. Before this, the dome was excluded from this pass entirely
// (see SampleLightCandidate's doc comment / the RIS loop below, which
// still `continue`s on LIGHT_KIND_DOME — correctly, RIS is for the
// analytic-light reservoir only) and reached a directly-visible interior
// surface only via IndirectDiffusePass's bounce-ray MISS path, i.e. only
// after an extra bounce. World Lobby's interior therefore never received
// the dome's radiance on a primary hit — the "renders as a night scene"
// defect this function fixes.
//
// Draws `sampleCount` cosine-hemisphere directions around the shading
// normal (Malley's method, rng.slang) on a Pcg32 stream DISTINCT from the
// RIS candidate draws above (bounceIndex 1 vs 0 — rng.slang's "no two
// draws share a stream" convention), samples the dome equirect along each
// (SampleDomeBackground — the SAME lat-long + tint/scale/rotation math
// every other dome sample in this codebase uses), and fires one visibility
// ray per sample. The visibility ray reuses THIS FILE's existing
// OcclusionPayload / AnyHitMain / ShadowMissMain machinery UNCHANGED (the
// RIS winner's shadow ray, above) — a transmissive hit (glass) is
// `IgnoreHit()`-accepted (light passes through) exactly as it already is
// for analytic lights (commit eed2ad9), so a dome ray aimed at a glazed
// curtain wall isn't blocked by an opaque black hit.
//
// Cosine-weighted importance sampling of a Lambertian lobe makes BSDF x
// cos / pdf cancel to exactly 1 (the SAME argument indirect_diffuse.slang's
// own bounce estimator documents) — a sample's DEMODULATED contribution is
// simply the visibility-gated dome radiance itself; no baseColor divide
// needed. The caller adds the averaged, per-sample-clamped result directly
// into its own (separately demodulated) direct-diffuse signal.
//
// Gated on DomeLightExists() (dome_sample.slang) so scenes with no
// authored Dome light are byte-for-byte unchanged — see that function's
// doc comment for why SampleDomeBackground's own "no dome" fallback must
// NOT be sampled unconditionally here.
float3 ComputeDomeDirectDiffuse(float3 worldHit, float3 nWorld, uint sampleCount,
                                uint2 launchIndex, uint launchWidth) {
    if (!DomeLightExists())
        return float3(0.0, 0.0, 0.0);

    float3 tangent;
    float3 bitangent;
    BuildOrthonormalBasis(nWorld, tangent, bitangent);

    Pcg32 domeRng = SeedPcg32(gSampling.rngSeed, gSampling.frameIndex, launchIndex, launchWidth,
                              MakeStreamId(RNG_STREAM_DIRECT_LIGHTING, 0u, 1u));

    float3 sum = float3(0.0, 0.0, 0.0);
    [loop]
    for (uint s = 0u; s < sampleCount; ++s) {
        const float2 u = Pcg32NextFloat2(domeRng);
        const float3 dirLocal = CosineSampleHemisphere(u);
        const float3 dirWorld = LocalToWorld(dirLocal, tangent, bitangent, nWorld);

        RayDesc domeRay;
        domeRay.Origin    = worldHit + nWorld * 1e-3;
        domeRay.Direction = dirWorld;
        domeRay.TMin      = 1e-3;
        domeRay.TMax      = 10000.0;
        OcclusionPayload domePayload;
        domePayload.visible = 0.0;  // pre-trace = occluded
        domePayload.hitT    = 0.0;
        TraceRay(gTlas,
                 RAY_FLAG_SKIP_CLOSEST_HIT_SHADER | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH,
                 0xFFu,
                 /*RayContributionToHitGroupIndex*/ 0,
                 /*MultiplierForGeometryContributionToHitGroupIndex*/ 0,
                 /*MissShaderIndex*/ 1,
                 domeRay,
                 domePayload);

        float3 sampleValue = SampleDomeBackground(dirWorld) * domePayload.visible;
        // Phase B firefly clamp — PER SAMPLE (frozen spec), not just on
        // the averaged result, so one hot HDRI texel doesn't blow out the
        // whole pixel average.
        const float sampleLum = Luminance(sampleValue);
        if (sampleLum > gQuality.maxRayIntensityDirect && sampleLum > 1e-9)
            sampleValue *= gQuality.maxRayIntensityDirect / sampleLum;
        sum += sampleValue;
    }
    return sum / float(sampleCount);
}

// One RIS candidate's evaluated (unshadowed) contribution + the data the
// winner needs to fire its shadow ray. `diffuseOut`/`specularOut` already
// carry the full geometry term (area/solid-angle -> radiance conversion;
// see the derivation below) — NOT yet divided by any pdf; the caller
// applies the RIS resampling weight and (for the winner only) shadow
// visibility.
struct LightCandidate {
    float3 diffuse;
    float3 specular;
    float3 sampleDirection;  // FROM surface TO the sampled point (unit)
    float  shadowRayTMax;    // distance to the sampled point; 1e30 for Distant
};

// Sample one light candidate + evaluate its UNSHADOWED contribution.
// `L` is a uniformly-picked light (Dome / disabled already filtered by the
// caller). Returns false when the candidate contributes nothing (back-
// facing, degenerate geometry, or a shaping-cone miss) — the caller skips
// it in the reservoir (weight 0).
//
// Geometry-term derivation (RIS resampling-weight correctness, see the
// file header): for an AREA light sampled uniformly over its shape, the
// rendering-equation integrand in the AREA domain is
//   f(x) = BSDF(L)·NdotL·Le·cosLight / distSq
// with sampling pdf p(x) = (1/lightCount)·(1/Area). The per-candidate
// value this function returns is `EvaluateOpenPBRDirectSplit(...)` (BSDF
// × NdotL × Le) scaled by `cosLight·Area/distSq` — i.e. f(x)·Area. Multiply
// by `lightCount` (done by the caller, RIS_WeightOf) to get the full RIS
// weight `f(x)/p(x) = f(x)·lightCount·Area`. For a RECT/DISK/SPHERE light
// (all folded into Kind::Rect at ingest — see StageWalker.cpp; a true
// per-shape solid-angle sampler for Disk/Sphere needs a shape
// discriminator LightGpu doesn't carry yet, tracked as debt) this is
// genuine uniform-area sampling. For DISTANT (a delta direction, no area)
// `cosLight·Area/distSq` collapses to the constant 1 (p(x)=1, a
// deterministic "candidate"), which is the correct degenerate case.
bool SampleLightCandidate(LightGpu L, float3 hitWorld, float3 nWorld, float3 viewDir,
                          OpenPBRSurfaceParams pbr, inout Pcg32 rng,
                          out LightCandidate candidate) {
    candidate.diffuse = float3(0.0, 0.0, 0.0);
    candidate.specular = float3(0.0, 0.0, 0.0);
    candidate.sampleDirection = float3(0.0, 1.0, 0.0);
    candidate.shadowRayTMax = 1e30;

    float3 lightDir;
    float3 lightColor = float3(L.colorR, L.colorG, L.colorB) * L.intensity;
    float geometryTerm;

    if (L.kind == LIGHT_KIND_RECT) {
        // Uniform area sample over the rhombus spanned by axisU/axisV
        // (StageWalker.cpp: axisU/axisV are HALF-extent vectors, so the
        // full parallelogram is position +/- axisU +/- axisV).
        const float2 uv = Pcg32NextFloat2(rng) * 2.0 - 1.0;
        const float3 axisU = float3(L.axisUx, L.axisUy, L.axisUz);
        const float3 axisV = float3(L.axisVx, L.axisVy, L.axisVz);
        const float3 samplePos = float3(L.posX, L.posY, L.posZ) + uv.x * axisU + uv.y * axisV;
        const float3 toLight = samplePos - hitWorld;
        const float distSq = dot(toLight, toLight);
        if (distSq < 1e-8)
            return false;
        const float dist = sqrt(distSq);
        lightDir = toLight / dist;
        candidate.sampleDirection = lightDir;
        candidate.shadowRayTMax = dist;

        // Rect facing test. `cross(axisU, axisV)` is the light's LOCAL +Z
        // axis carried into world space (StageWalker.cpp), but
        // UsdLuxRectLight emits toward LOCAL -Z ("away from the front
        // face" — see EvaluateDirectLightSample's identical derivation in
        // shading.slang) — i.e. the emission direction is
        // -cross(axisU,axisV), not +cross(axisU,axisV). `lightDir` here
        // points FROM the surface TO the light, so the direction FROM the
        // light TO the surface is `-lightDir`; the surface is on the lit
        // side exactly when that vector has a positive component along
        // the emission direction: dot(-lightDir, -lightNormal) > 0, i.e.
        // dot(lightDir, lightNormal) > 0. (An earlier draft used
        // dot(lightNormal, -lightDir) — the textbook formula for a light
        // whose normal points INTO the illuminated hemisphere, which is
        // the opposite of USD's rect-light convention — and always
        // evaluated <= 0 for a normally-authored single-sided RectLight,
        // silently zeroing every rect light's contribution.)
        const float3 lightNormal = normalize(cross(axisU, axisV));
        float cosLight = dot(lightNormal, lightDir);
        cosLight = (L.doubleSided != 0u) ? abs(cosLight) : max(cosLight, 0.0);
        if (cosLight <= 1e-6)
            return false;
        const float area = 4.0 * length(axisU) * length(axisV);
        geometryTerm = cosLight * area / distSq;
    } else if (L.kind == LIGHT_KIND_DISTANT) {
        // Parallel rays, no angular jitter (see file header debt note).
        lightDir = -float3(L.dirX, L.dirY, L.dirZ);
        candidate.sampleDirection = lightDir;
        candidate.shadowRayTMax = 1e30;
        geometryTerm = 1.0;
    } else {
        // Cylinder / Geometry / Portal — point-at-position, matching
        // shading.slang's existing EvaluateDirectLightSample treatment.
        // PackLightGpu force-disables these (intensity=0) today, so this
        // branch is currently unreachable in practice; kept for
        // forward-compatibility (§22 MINOR-additive kinds).
        const float3 toLight = float3(L.posX, L.posY, L.posZ) - hitWorld;
        const float distSq = dot(toLight, toLight);
        const float dist = sqrt(max(distSq, 1e-8));
        lightDir = toLight / dist;
        candidate.sampleDirection = lightDir;
        candidate.shadowRayTMax = dist;
        geometryTerm = 1.0 / max(distSq, 0.01);
    }

    // UsdLuxShapingAPI cone gate — same formula as
    // EvaluateDirectLightSample (shading.slang), reproduced here because
    // that helper always treats a light as a point at its centroid (no
    // per-candidate sampled point) and doesn't return split diffuse/spec.
    if (L.shapingConeCosOuter > 1e-4) {
        float3 coneAxis;
        if (L.kind == LIGHT_KIND_RECT) {
            const float3 axisU = float3(L.axisUx, L.axisUy, L.axisUz);
            const float3 axisV = float3(L.axisVx, L.axisVy, L.axisVz);
            coneAxis = normalize(cross(axisU, axisV));
        } else {
            coneAxis = float3(L.dirX, L.dirY, L.dirZ);
        }
        const float3 lightToSurface =
            (L.kind == LIGHT_KIND_DISTANT) ? float3(L.dirX, L.dirY, L.dirZ) : -lightDir;
        const float cosAngle = dot(normalize(lightToSurface), coneAxis);
        const float coneAttenuation =
            smoothstep(L.shapingConeCosOuter, L.shapingConeCosInner, cosAngle);
        if (coneAttenuation <= 0.0)
            return false;
        geometryTerm *= coneAttenuation;
    }

    if (dot(nWorld, lightDir) <= 0.0)
        return false;

    EvaluateOpenPBRDirectSplit(pbr, nWorld, viewDir, lightDir, lightColor,
                              candidate.diffuse, candidate.specular);
    candidate.diffuse *= geometryTerm;
    candidate.specular *= geometryTerm;
    return true;
}

[shader("raygeneration")]
void RayGenMain() {
    const uint2 launchIndex = DispatchRaysIndex().xy;
    const uint2 launchDim   = DispatchRaysDimensions().xy;

    const VisibilityGpu vis = gVisibility[launchIndex.y * launchDim.x + launchIndex.x];
    if (vis.hitT < 0.0f)
    {
        gDirectDiffuse[launchIndex]  = float4(0.0f, 0.0f, 0.0f, 1.0f);
        gDirectSpecular[launchIndex] = float4(0.0f, 0.0f, 0.0f, 0.0f);
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

    const float3 viewDir = normalize(-worldDirXyz);

    uint lightCount;
    uint _stride;
    gLights.GetDimensions(lightCount, _stride);

    Pcg32 rng = SeedPcg32(gSampling.rngSeed, gSampling.frameIndex, launchIndex, launchDim.x,
                          MakeStreamId(RNG_STREAM_DIRECT_LIGHTING, 0u, 0u));

    const uint candidateCount = max(gQuality.directSamples, 1u);
    float runningSum = 0.0;
    float winnerLuminance = 0.0;
    bool haveWinner = false;
    LightCandidate winner;
    winner.diffuse = float3(0.0, 0.0, 0.0);
    winner.specular = float3(0.0, 0.0, 0.0);
    winner.sampleDirection = float3(0.0, 1.0, 0.0);
    winner.shadowRayTMax = 1e30;

    if (lightCount > 0u) {
        [loop]
        for (uint s = 0u; s < candidateCount; ++s) {
            const uint lightIndex = min(uint(Pcg32NextFloat(rng) * float(lightCount)),
                                        lightCount - 1u);
            const LightGpu L = gLights[lightIndex];
            if (L.intensity <= 0.0 || L.kind == LIGHT_KIND_DOME)
                continue;  // Dome / disabled — IndirectDiffusePass's domain.

            LightCandidate candidate;
            if (!SampleLightCandidate(L, worldHit, surf.normalWorld, viewDir, surf.pbr, rng,
                                      candidate))
                continue;

            // RIS resampling weight: f(x)/p(x) = (candidate value, which
            // already carries the ×Area/×cosLight/×1 geometry term) ×
            // lightCount — see SampleLightCandidate's derivation comment.
            const float candidateLuminance =
                Luminance(candidate.diffuse + candidate.specular);
            const float weight = candidateLuminance * float(lightCount);
            if (weight <= 0.0)
                continue;
            runningSum += weight;
            if (Pcg32NextFloat(rng) * runningSum < weight) {
                winner = candidate;
                winnerLuminance = candidateLuminance;
                haveWinner = true;
            }
        }
    }

    float3 diffuseSum = float3(0.0, 0.0, 0.0);
    float3 specularSum = float3(0.0, 0.0, 0.0);
    float  dominantOccluderHitT = 0.0;

    if (haveWinner && winnerLuminance > 1e-9 && runningSum > 0.0) {
        const float finalScale = runningSum / (float(candidateCount) * winnerLuminance);

        // Single shadow ray to the RIS winner's sampled point — soft
        // shadows fall out of the per-pixel random point choice.
        RayDesc shadowRay;
        shadowRay.Origin    = worldHit + surf.normalWorld * 1e-3;
        shadowRay.Direction = winner.sampleDirection;
        shadowRay.TMin      = 1e-3;
        shadowRay.TMax      = max(1e-3, winner.shadowRayTMax - 1e-3);
        OcclusionPayload shadowPayload;
        shadowPayload.visible = 0.0;  // pre-trace = occluded
        shadowPayload.hitT    = 0.0;
        TraceRay(gTlas,
                 RAY_FLAG_SKIP_CLOSEST_HIT_SHADER | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH,
                 0xFFu,
                 /*RayContributionToHitGroupIndex*/ 0,
                 /*MultiplierForGeometryContributionToHitGroupIndex*/ 0,
                 /*MissShaderIndex*/ 1,
                 shadowRay,
                 shadowPayload);

        diffuseSum  = winner.diffuse * finalScale * shadowPayload.visible;
        specularSum = winner.specular * finalScale * shadowPayload.visible;
        dominantOccluderHitT = shadowPayload.hitT;
    }

    // Phase B firefly clamp — scale the COMBINED diffuse+specular down to
    // maxRayIntensityDirect if it exceeds it, preserving the diffuse/
    // specular ratio (per-sample clamp per the frozen spec).
    const float combinedLuminance = Luminance(diffuseSum + specularSum);
    if (combinedLuminance > gQuality.maxRayIntensityDirect && combinedLuminance > 1e-9) {
        const float clampScale = gQuality.maxRayIntensityDirect / combinedLuminance;
        diffuseSum *= clampScale;
        specularSum *= clampScale;
    }

    // "Interior illumination" follow-up — dome-light IBL sampling as part
    // of DIRECT lighting (see ComputeDomeDirectDiffuse's doc comment).
    // Already demodulated + per-sample-clamped; added straight into the
    // demodulated analytic-light diffuse below (NOT run back through
    // DemodulateByAlbedo, which would incorrectly divide it by baseColor
    // a second time).
    const float3 domeDirectDiffuse =
        ComputeDomeDirectDiffuse(worldHit, surf.normalWorld, candidateCount, launchIndex,
                                 launchDim.x);

    gDirectDiffuse[launchIndex] = float4(DemodulateByAlbedo(diffuseSum, surf.baseColor)
                                             + domeDirectDiffuse,
                                         dominantOccluderHitT);
    // NOT demodulated (documented Phase A simplification, still true in
    // Phase B — see this pass's C++ header): no clean per-channel specular
    // albedo exists to divide by.
    gDirectSpecular[launchIndex] = float4(specularSum, 1.0f);
}

// --------------------------------------------------------------------
// ShadowMissMain — the per-candidate shadow ray's miss shader. Same
// "unoccluded" convention as raytraced_lighting_shadow_miss.slang.
// --------------------------------------------------------------------
[shader("miss")]
void ShadowMissMain(inout OcclusionPayload payload) {
    payload.visible = 1.0f;
}

// --------------------------------------------------------------------
// AnyHitMain — the shadow ray's alpha-test + transmissive-passthrough
// gate, identical to raytraced_lighting_anyhit.slang's shadow-ray branch.
// Writes the occluder's hitT (§"future SIGMA guide") when accepting a hit.
// --------------------------------------------------------------------
[shader("anyhit")]
void AnyHitMain(inout OcclusionPayload payload, in BuiltInTriangleIntersectionAttributes attribs) {
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
