// Pyxis renderer — lighting-pass closest-hit shader (the retained
// megakernel, P4 pass split).
//
// Since the visibility-buffer split this closesthit serves ONLY
// SECONDARY rays — the transparency-continuation segments the lighting
// raygen re-traces front-to-back, and the deterministic mirror-
// reflection rays (both at missIndex 0 with the full HitInfo payload).
// The PRIMARY hit is shaded by raytraced_lighting_raygen.slang from the
// visibility buffer instead.
//
// The entire shading body lives in shading.slang (ShadeSurfaceHit) and
// is shared with the lighting raygen's first-hit path — calling the
// SAME function from both stages is what guarantees first-hit and
// secondary shading stay in lockstep (§33.7). This entry forwards the
// hit-stage intrinsics as the trace-dependent parameters:
//   InstanceID() / PrimitiveIndex() / attribs.barycentrics /
//   RayTCurrent() / WorldRayDirection() / ObjectToWorld3x4() /
//   WorldToObject3x4() / payload.recursionDepth.
//
// Bindings — match pyxis::RaytracedLightingPass's NVRHI BindingLayout;
// declared by the shared modules (camera_ray.slang: gCamera at 0;
// dome_sample.slang: gLights 5 / gDomeEnvMap 9 / gDomeSampler 33;
// shading.slang: gTlas 1, gMaterials 3, gInstanceInfo 4, gMeshInfo 6,
// gMeshFaceNormals 7, gBindlessSampler 10, gMeshUvs 24, gMeshIndices 26,
// gBindlessTextures 28, gMeshVertexAttribs 29).

#include "ShaderInterop.slang"
#include "shading.slang"

// Slang emits the SPIR-V `OpEntryPoint` name as `"main"` regardless
// of the source-side function name; the C++ side passes `"main"` to
// NVRHI's createShader. See the matching note in
// raytraced_lighting_raygen.slang.
[shader("closesthit")]
void main(inout HitInfo payload, in BuiltInTriangleIntersectionAttributes attribs) {
    // Same value the pre-split kernel computed for hitW / hitWorld /
    // payload.worldHit (the compiler CSE'd the duplicate expressions
    // into one); hoisted so the shared function and the payload write
    // consume the identical float3.
    const float3 worldHit = WorldRayOrigin() + RayTCurrent() * WorldRayDirection();

    const SurfaceShadeResult shade = ShadeSurfaceHit(
        InstanceID(), PrimitiveIndex(), attribs.barycentrics,
        worldHit, WorldRayDirection(), RayTCurrent(),
        ObjectToWorld3x4(), WorldToObject3x4(),
        payload.recursionDepth);

    payload.color = shade.emitted;
    payload.transmittance = shade.transmittance;
    payload.hitT  = RayTCurrent();
    // M7 follow-up — fields the raygen-side AOV inspector reads.
    // Normal is the world-space shading normal; instanceId is the
    // per-instance slot the shading branched on for material lookup.
    // baseColor / materialId / worldHit feed the BaseColor /
    // MaterialID / WorldPos AOVs respectively.
    payload.normalWorld = shade.normalWorld;
    payload.instanceId  = InstanceID();
    payload.baseColor   = shade.baseColor;
    payload.materialId  = shade.materialId;
    payload.worldHit    = worldHit;
    // PrimitiveIndex() is the per-face id within the BLAS — Hydra's
    // HdAovTokens->elementId. ~0u is the miss sentinel raygen sets
    // before TraceRay; we overwrite with the real face here.
    payload.elementId   = PrimitiveIndex();
}
