// Pyxis renderer — TranslucencyPass, ONE shader file (RTX-alignment
// design, rtx-realtime-alignment-design.md, WP2-signals Phase A).
//
// Ports raytraced_lighting_raygen.slang's front-to-back transparent-
// segment loop into its own signal. Re-shades the primary hit via the
// shared ShadeSurfaceHit (bit-identical inputs to every other RT pass)
// to get `shade.transmittance` — the SAME value the real composite
// gates on — then continues the IDENTICAL segment loop (same epsilons /
// break thresholds / MAX_TRANSPARENT_SEGMENTS cap) from segment 1
// onward, accumulating ONLY the radiance from BEHIND the primary
// surface (its own opaque lit/emitted term is DirectLighting /
// IndirectDiffuse / Reflections' job now). ClosestHitMain / MissMain /
// AnyHitMain are the existing megakernel machinery, reused verbatim
// (ShadeSurfaceHit fires its own shadow/AO/reflection sub-rays at every
// segment, exactly as raytraced_lighting_closesthit.slang's `main` does
// today) — the "pragmatic Phase A port" the WP2 contract table calls for.
//
// Bindings — Set 0 (SceneBindings, shared verbatim with every other RT
// pass). Set 1 (this pass's own I/O):
//   set=1, binding=0 : StructuredBuffer<VisibilityGpu> (GBuffer pass output)
//   set=1, binding=1 : RWTexture2D<float4> gTranslucency (RGBA16F;
//                       .rgb = premultiplied transmitted radiance,
//                       .a = coverage = 1 - max(transmittance channels)
//                       at the primary hit)

#include "ShaderInterop.slang"
#include "camera_ray.slang"  // gCamera (0) + BuildCameraRay
#include "shading.slang"     // gTlas (1) + full Set-0 scene tables + ShadeSurfaceHit

[[vk::binding(0, 1)]] StructuredBuffer<VisibilityGpu> gVisibility;
[[vk::binding(1, 1)]] RWTexture2D<float4>              gTranslucency;

// Same per-segment loop cap as raytraced_lighting_raygen.slang's own
// spec constant (identical numeric SPEC_ID — scoped per SPIR-V module,
// so this is an independent declaration, not a collision — see
// ShaderInterop.slang's ids comment). Left at the shared default (16u):
// this pass doesn't expose a runtime override.
[[vk::constant_id(SPEC_ID_MAX_TRANSPARENT_SEGMENTS)]] const uint MAX_TRANSPARENT_SEGMENTS = 16u;

[shader("raygeneration")]
void RayGenMain() {
    const uint2 launchIndex = DispatchRaysIndex().xy;
    const uint2 launchDim   = DispatchRaysDimensions().xy;

    float3 worldOriginXyz;
    float3 worldDirXyz;
    BuildCameraRay(launchIndex, launchDim, worldOriginXyz, worldDirXyz);

    const VisibilityGpu vis = gVisibility[launchIndex.y * launchDim.x + launchIndex.x];
    if (vis.hitT < 0.0f)
    {
        gTranslucency[launchIndex] = float4(0.0f, 0.0f, 0.0f, 0.0f);
        return;
    }

    const float2 visBary = float2(f16tof32(vis.baryPacked & 0xFFFFu),
                                   f16tof32(vis.baryPacked >> 16u));
    const float3 worldHit = worldOriginXyz + worldDirXyz * vis.hitT;
    const InstanceInfoGpu inst = gInstanceInfo[vis.instanceId];
    const float3x4 worldToObject = InvertAffine3x4(inst.objectToWorld);
    const SurfaceShadeResult shade = ShadeSurfaceHit(
        vis.instanceId, vis.primitiveIndex, visBary,
        worldHit, worldDirXyz, vis.hitT,
        inst.objectToWorld, worldToObject,
        /*recursionDepth*/ 0u);

    // Gate + throughput seed: EXACTLY the value
    // raytraced_lighting_raygen.slang's own front-to-back loop checks —
    // reused, not re-derived, so "is this surface transmissive" can never
    // drift from the real composite's answer.
    const float3 t0 = shade.transmittance;
    const float  maxT0 = max(max(t0.x, t0.y), t0.z);
    const float  coverage = saturate(1.0f - maxT0);

    if (maxT0 < 1e-3f)
    {
        // Opaque (or near-opaque) primary surface — nothing behind it
        // reaches the eye through it.
        gTranslucency[launchIndex] = float4(0.0f, 0.0f, 0.0f, coverage);
        return;
    }

    float3 radiance   = float3(0.0f, 0.0f, 0.0f);
    float3 throughput = t0;

    RayDesc segRay;
    segRay.Origin    = worldHit + worldDirXyz * 1e-3f;
    segRay.Direction = worldDirXyz;
    segRay.TMin      = 1e-3f;
    segRay.TMax      = 10000.0f;

    [loop]
    for (uint seg = 1u; seg < MAX_TRANSPARENT_SEGMENTS; ++seg)
    {
        HitInfo segPayload;
        segPayload.color           = float3(0.0f, 0.0f, 0.0f);
        segPayload.hitT            = -1.0f;
        segPayload.normalWorld     = float3(0.0f, 0.0f, 0.0f);
        segPayload.instanceId      = 0xFFFFFFFFu;
        segPayload.baseColor       = float3(0.0f, 0.0f, 0.0f);
        segPayload.materialId      = 0xFFFFFFFFu;
        segPayload.worldHit        = float3(0.0f, 0.0f, 0.0f);
        segPayload.elementId       = 0xFFFFFFFFu;
        segPayload.recursionDepth  = 0;  // primary depth — matches the real loop.
        segPayload.transmittance   = float3(0.0f, 0.0f, 0.0f);

        TraceRay(gTlas,
                 RAY_FLAG_NONE,
                 0xFFu,
                 /*RayContributionToHitGroupIndex*/ 0,
                 /*MultiplierForGeometryContributionToHitGroupIndex*/ 0,
                 /*MissShaderIndex*/ 0,
                 segRay,
                 segPayload);

        radiance += throughput * segPayload.color;

        if (segPayload.hitT < 0.0f)
            break;  // background — nothing more lies behind it.

        const float3 t = segPayload.transmittance;
        if (max(max(t.x, t.y), t.z) < 1e-3f)
            break;  // opaque (or near-opaque) surface — stop.

        throughput *= t;

        const float3 hitPos = segRay.Origin + segRay.Direction * segPayload.hitT;
        segRay.Origin = hitPos + segRay.Direction * 1e-3f;
        segRay.TMin   = 1e-3f;
    }

    gTranslucency[launchIndex] = float4(radiance, coverage);
}

// --------------------------------------------------------------------
// ClosestHitMain — the retained megakernel, reused verbatim (identical
// body to raytraced_lighting_closesthit.slang's `main`): calls the
// shared ShadeSurfaceHit for every segment hit this pass's RayGen traces.
// --------------------------------------------------------------------
[shader("closesthit")]
void ClosestHitMain(inout HitInfo payload, in BuiltInTriangleIntersectionAttributes attribs) {
    const float3 worldHit = WorldRayOrigin() + RayTCurrent() * WorldRayDirection();

    const SurfaceShadeResult shade = ShadeSurfaceHit(
        InstanceID(), PrimitiveIndex(), attribs.barycentrics,
        worldHit, WorldRayDirection(), RayTCurrent(),
        ObjectToWorld3x4(), WorldToObject3x4(),
        payload.recursionDepth);

    payload.color          = shade.emitted;
    payload.transmittance  = shade.transmittance;
    payload.hitT           = RayTCurrent();
    payload.normalWorld    = shade.normalWorld;
    payload.instanceId     = InstanceID();
    payload.baseColor      = shade.baseColor;
    payload.materialId     = shade.materialId;
    payload.worldHit       = worldHit;
    payload.elementId      = PrimitiveIndex();
}

// --------------------------------------------------------------------
// ShadowMissMain — required because the reused ShadeSurfaceHit
// (shading.slang) fires its OWN per-light shadow ray and dome-ambient
// AO ray internally, both hardcoded to MissShaderIndex = 1 (see
// shading.slang's TraceRay calls) — the SAME contract
// raytraced_lighting_shadow_miss.slang satisfies for RaytracedLightingPass.
// Without this slot at index 1, those rays index out of bounds into this
// pipeline's shader binding table (undefined behavior / GPU crash) —
// discovered via bisection while verifying this pass. Identical body to
// raytraced_lighting_shadow_miss.slang's `main`.
// --------------------------------------------------------------------
[shader("miss")]
void ShadowMissMain(inout HitInfo payload) {
    payload.color = float3(1.0f, 1.0f, 1.0f);
}

// --------------------------------------------------------------------
// MissMain — identical to raytraced_lighting_miss.slang's `main`.
// --------------------------------------------------------------------
[shader("miss")]
void MissMain(inout HitInfo payload) {
    payload.color        = SampleDomeBackground(WorldRayDirection());
    payload.hitT         = -1.0f;
    payload.normalWorld  = float3(0.0f, 0.0f, 0.0f);
    payload.instanceId   = 0xFFFFFFFFu;
    payload.baseColor    = float3(0.0f, 0.0f, 0.0f);
    payload.materialId   = 0xFFFFFFFFu;
    payload.worldHit     = float3(0.0f, 0.0f, 0.0f);
    payload.elementId    = 0xFFFFFFFFu;
}

// --------------------------------------------------------------------
// AnyHitMain — identical to raytraced_lighting_anyhit.slang's `main`.
// --------------------------------------------------------------------
[shader("anyhit")]
void AnyHitMain(inout HitInfo payload, in BuiltInTriangleIntersectionAttributes attribs) {
    if (AnyHitIsAlphaTested()) {
        IgnoreHit();
        return;
    }
    if ((RayFlags() & RAY_FLAG_SKIP_CLOSEST_HIT_SHADER) != 0u
        && AnyHitIsTransmissive()) {
        IgnoreHit();
        return;
    }
}
