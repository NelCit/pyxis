// Pyxis renderer — AmbientOcclusionPass, ONE shader file (RTX-alignment
// design, rtx-realtime-alignment-design.md, WP2-signals Phase A / Phase B
// estimator swap).
//
// RTAO: `AO_SAMPLE_COUNT` (2) stochastic cosine-hemisphere visibility rays
// per pixel around the PRIMARY hit's shading normal (read from
// RaytracedGBufferPass's gNormalRoughness guide, NOT re-evaluated — RTAO
// needs only geometry, not material). TMax = gQuality.aoRayLength
// (RealTimeQualityUniforms). gAo.r is the fraction of the N rays that
// escaped the hemisphere — a smooth [0, 1] value (Phase A's single
// deterministic ray along the normal only ever produced a binary 0/1;
// N=2 stochastic cosine-weighted samples give real penumbra gradation on
// partially-occluded pixels, matching ovrtx's "long-radius hemispherical
// occlusion, not contact-only" RTAO). `AO_SAMPLE_COUNT` is a fixed
// compile-time constant (not runtime-configurable) — RealTimeQuality has
// no sample-count field for AO yet (only aoRayLength).
//
// Ray flags match "today's" shadow/AO ray convention (shading.slang's
// dome-ambient AO ray + per-light shadow ray): RAY_FLAG_SKIP_CLOSEST_
// HIT_SHADER | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH, with the SAME
// shared any-hit gate (alpha-tested geometry is invisible to the ray;
// transmissive geometry doesn't occlude either — raytraced_anyhit.slang's
// AnyHitIsAlphaTested / AnyHitIsTransmissive, reused verbatim).
//
// Bindings — Set 0 (SceneBindings, shared verbatim with every other RT
// pass): gCamera (0, camera_ray.slang) for the primary-ray reconstruction
// that recovers worldHit from the visibility buffer's hitT; gTlas (1,
// declared here directly — this module does NOT include shading.slang,
// keeping it independent of the OpenPBR closure / UV pipeline);
// gMaterials (2) / gInstanceInfo (3) via raytraced_anyhit.slang, for the
// any-hit alpha-test/transmissive gate only.
// Set 1 (this pass's own I/O):
//   set=1, binding=0 : StructuredBuffer<VisibilityGpu> (GBuffer pass output)
//   set=1, binding=1 : Texture2D<float4> gNormalRoughness (GBuffer guide)
//   set=1, binding=2 : RWTexture2D<float4> gAo (RGBA16F; .r used, .gba = 0,0,1)

#include "ShaderInterop.slang"
#include "camera_ray.slang"       // gCamera (0) + gQuality (15) + gSampling (16) + BuildCameraRay
#include "raytraced_anyhit.slang" // gMaterials (2) + gInstanceInfo (3) + alpha-test helpers
#include "rng.slang"              // Pcg32 + cosine-hemisphere sampling + MakeStreamId

// Phase B — fixed stochastic sample count (see file header: no runtime
// RealTimeQuality field for this yet).
static const uint AO_SAMPLE_COUNT = 2u;

[[vk::binding(1, 0)]] RaytracingAccelerationStructure gTlas;

[[vk::binding(0, 1)]] StructuredBuffer<VisibilityGpu> gVisibility;
[[vk::binding(1, 1)]] Texture2D<float4>               gNormalRoughness;
[[vk::binding(2, 1)]] RWTexture2D<float4>             gAo;

[shader("raygeneration")]
void RayGenMain() {
    const uint2 launchIndex = DispatchRaysIndex().xy;
    const uint2 launchDim   = DispatchRaysDimensions().xy;

    const VisibilityGpu vis = gVisibility[launchIndex.y * launchDim.x + launchIndex.x];
    if (vis.hitT < 0.0f)
    {
        // Primary miss — nothing to occlude; clear (matches the "no
        // surface, no shadow" convention every other signal uses).
        gAo[launchIndex] = float4(1.0f, 0.0f, 0.0f, 1.0f);
        return;
    }

    float3 worldOriginXyz;
    float3 worldDirXyz;
    BuildCameraRay(launchIndex, launchDim, worldOriginXyz, worldDirXyz);
    const float3 worldHit = worldOriginXyz + worldDirXyz * vis.hitT;
    const float3 nWorld = gNormalRoughness[launchIndex].xyz;

    float3 tangent;
    float3 bitangent;
    BuildOrthonormalBasis(nWorld, tangent, bitangent);

    Pcg32 rng = SeedPcg32(gSampling.rngSeed, gSampling.frameIndex, launchIndex, launchDim.x,
                          MakeStreamId(RNG_STREAM_AMBIENT_OCCLUSION, 0u, 0u));

    // Phase B — N stochastic cosine-hemisphere rays average into a smooth
    // [0, 1] occlusion fraction (Phase A traced exactly one deterministic
    // ray along the normal, producing a binary 0/1 result).
    float visibleSum = 0.0f;
    [loop]
    for (uint s = 0u; s < AO_SAMPLE_COUNT; ++s) {
        const float2 u = Pcg32NextFloat2(rng);
        const float3 dirLocal = CosineSampleHemisphere(u);
        const float3 dirWorld = LocalToWorld(dirLocal, tangent, bitangent, nWorld);

        RayDesc aoRay;
        aoRay.Origin    = worldHit + nWorld * 1e-3f;
        aoRay.Direction = dirWorld;
        aoRay.TMin      = 1e-3f;
        // WP2-final — runtime RealTimeQuality.aoRayLength (SceneBindings
        // gQuality, binding 15 of Set 0) replaces the AO_RAY_LENGTH compile-time
        // constant; ShaderInterop.slang's AO_RAY_LENGTH literal now only backs
        // the C++-side RenderSettings::RealTimeQuality::aoRayLength default.
        aoRay.TMax      = gQuality.aoRayLength;

        OcclusionPayload payload;
        payload.visible = 0.0f;  // pre-trace = occluded
        payload.hitT    = 0.0f;

        TraceRay(gTlas,
                 RAY_FLAG_SKIP_CLOSEST_HIT_SHADER | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH,
                 0xFFu,
                 /*RayContributionToHitGroupIndex*/ 0,
                 /*MultiplierForGeometryContributionToHitGroupIndex*/ 0,
                 /*MissShaderIndex*/ 0,
                 aoRay,
                 payload);
        visibleSum += payload.visible;
    }

    gAo[launchIndex] = float4(visibleSum / float(AO_SAMPLE_COUNT), 0.0f, 0.0f, 1.0f);
}

[shader("miss")]
void MissMain(inout OcclusionPayload payload) {
    payload.visible = 1.0f;  // escaped the hemisphere — clear.
}

[shader("anyhit")]
void AnyHitMain(inout OcclusionPayload payload, in BuiltInTriangleIntersectionAttributes attribs) {
    if (AnyHitIsAlphaTested()) {
        IgnoreHit();
        return;
    }
    // This ray always carries RAY_FLAG_SKIP_CLOSEST_HIT_SHADER — matches
    // the shadow-ray convention that lets transmissive (glass-like)
    // surfaces not occlude (raytraced_lighting_anyhit.slang's identical
    // gate).
    if (AnyHitIsTransmissive()) {
        IgnoreHit();
        return;
    }
    payload.hitT = RayTCurrent();  // accepted occluder — dominant-light guide (unused here).
}
