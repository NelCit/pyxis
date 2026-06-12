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
// Both pipelines place the two tables this reads at the same slots:
// gMaterials at binding 3, gInstanceInfo at binding 4 (set 0).

#ifndef PYXIS_RAYTRACED_ANYHIT_SLANG
#define PYXIS_RAYTRACED_ANYHIT_SLANG

#include "ShaderInterop.slang"

[[vk::binding(3, 0)]] StructuredBuffer<OpenPBRMaterialGPU> gMaterials;
// P2 packing — the packed per-instance record (materialSlot half) replaces the
// old gInstanceMaterial uint table at the same binding.
[[vk::binding(4, 0)]] StructuredBuffer<InstanceInfoGpu>    gInstanceInfo;

// V2.B follow-up — only IgnoreHit for true alpha cutout
// (`MATERIAL_FLAG_ALPHA_TESTED`). Translucent + transmission
// materials are accepted by the anyhit so the closesthit can
// shade them with proper alpha continuation (opacity < 1) and
// Snell refraction (TRANSMISSION_ENABLED). Without accepting,
// those materials were rendering as completely invisible — the
// ray would skip them and shade whatever opaque surface was
// behind, indistinguishable from a hole in the model.
//
// Side-effect: shadow rays now block on translucent geometry
// too. Physically light should pass through glass; we accept
// that minor wrong-ness here to keep the anyhit independent of
// ray context. The §V2.B refraction follow-up's separate ray
// path through glass will compensate visually.
bool AnyHitIsAlphaTested() {
    const uint instanceId = InstanceID();
    const uint materialId = gInstanceInfo[instanceId].materialSlot;
    const OpenPBRMaterialGPU mat = gMaterials[materialId];
    return (mat.flags & MATERIAL_FLAG_ALPHA_TESTED) != 0u;
}

#endif  // PYXIS_RAYTRACED_ANYHIT_SLANG
