// Pyxis renderer — shared any-hit alpha-test logic (P4 pass split).
//
// M9: invisibility-as-translucency stub. The DXR / Vulkan ray-tracing
// pipeline fires the anyhit on every triangle hit before the
// closesthit decision; calling IgnoreHit() makes traversal skip the
// hit and continue to the next intersection. We use that to make
// alpha-tested materials INVISIBLE — the ray passes through and we
// shade whatever opaque surface is behind. Proper transmission BSDF +
// alpha-from-texture lookups are M11+ work; this is the visually-
// passable stub that stops translucent geometry from blocking light
// incorrectly.
//
// The BLAS build path drops the `Opaque` flag on every geometry so
// the anyhit fires unconditionally. For opaque materials this
// returns immediately (no IgnoreHit); the cost is one extra anyhit
// invocation per opaque hit, well under the §34.3 KPI budget.
//
// P4: this file is a MODULE, not an entry point. The DXR/Slang
// constraint is that an anyhit's payload type must match the
// pipeline's TraceRay payload type — the GBuffer pipeline traces the
// thin VisibilityPayload, the Lighting pipeline the full HitInfo — so
// ONE compiled anyhit can't serve both. The shared decision lives here
// as a function; two tiny entry files
// (raytraced_gbuffer_anyhit.slang / raytraced_lighting_anyhit.slang)
// include it and apply the identical IgnoreHit gate against their own
// payload type, keeping visibility semantics bit-identical across the
// two pipelines (§33.7).
//
// Both pipelines place the two tables this reads at the same slots —
// RTX-alignment design WP2 rewrite: gMaterials at binding 2,
// gInstanceInfo at binding 3 (set 0 / SceneBindings — see
// Private/Passes/SceneBindings.h for the full canonical slot map).

#ifndef PYXIS_RAYTRACED_ANYHIT_SLANG
#define PYXIS_RAYTRACED_ANYHIT_SLANG

#include "ShaderInterop.slang"

[[vk::binding(2, 0)]] StructuredBuffer<OpenPBRMaterialGPU> gMaterials;
// P2 packing — the packed per-instance record (materialSlot half) replaces the
// old gInstanceMaterial uint table at the same binding.
[[vk::binding(3, 0)]] StructuredBuffer<InstanceInfoGpu>    gInstanceInfo;

// V2.B follow-up — only IgnoreHit for true alpha cutout
// (`MATERIAL_FLAG_ALPHA_TESTED`). Translucent + transmission
// materials are accepted by the anyhit so the closesthit can
// shade them with proper alpha continuation (opacity < 1) and
// Snell refraction (TRANSMISSION_ENABLED). Without accepting,
// those materials were rendering as completely invisible — the
// ray would skip them and shade whatever opaque surface was
// behind, indistinguishable from a hole in the model.
//
bool AnyHitIsAlphaTested() {
    const uint instanceId = InstanceID();
    const uint materialId = gInstanceInfo[instanceId].materialSlot;
    const OpenPBRMaterialGPU mat = gMaterials[materialId];
    return (mat.flags & MATERIAL_FLAG_ALPHA_TESTED) != 0u;
}

// True iff the hit material is transmissive (glass-like). The lighting anyhit
// uses this — gated on RayFlags() SKIP_CLOSEST_HIT_SHADER, i.e. ONLY for
// shadow / AO rays — to let light pass THROUGH transmissive surfaces instead of
// casting an opaque shadow (physically, light goes through glass). Composite /
// reflection rays (closesthit runs) still hit transmissive surfaces so the
// front-to-back transparency loop can shade them. This is a binary
// approximation (transmissive ⇒ no shadow); colored/attenuated transparent
// shadows are a follow-up.
bool AnyHitIsTransmissive() {
    const uint instanceId = InstanceID();
    const uint materialId = gInstanceInfo[instanceId].materialSlot;
    const OpenPBRMaterialGPU mat = gMaterials[materialId];
    return (mat.flags & MATERIAL_FLAG_TRANSMISSION_ENABLED) != 0u;
}

// RTX-alignment design (rtx-realtime-alignment-design.md), Phase B follow-
// up ("interior illumination" fix) — the hit material's RAW transmission
// tint (transmissionColor x transmissionWeight, saturated), a cheap
// material-table lookup with NO UV/texture evaluation (anyhit has no
// derivatives to sample with). Used by IndirectDiffusePass's bounce ray
// (indirect_diffuse.slang) to attenuate its throughput when continuing
// THROUGH a transmissive surface instead of shading it as an opaque wall,
// so a GI ray aimed at a glazed curtain wall can still reach the sky. This
// is a cheap tinted-passthrough approximation, not the full
// OpenPBRTransmittance formula (fresnel/opacity/metalness terms) the real
// closesthit transparency loop (translucency.slang) uses — same "binary-
// approximation, colored attenuation is a follow-up" spirit as
// AnyHitIsTransmissive's shadow-ray passthrough above (commit eed2ad9),
// just one step further (tinted instead of flat 1.0).
float3 AnyHitTransmissionTint() {
    const uint instanceId = InstanceID();
    const uint materialId = gInstanceInfo[instanceId].materialSlot;
    const OpenPBRMaterialGPU mat = gMaterials[materialId];
    return saturate(float3(mat.transmissionColorR, mat.transmissionColorG, mat.transmissionColorB))
         * saturate(mat.transmissionWeight);
}

#endif  // PYXIS_RAYTRACED_ANYHIT_SLANG
