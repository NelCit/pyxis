// Pyxis renderer — any-hit shader.
//
// M9: invisibility-as-translucency stub. The DXR / Vulkan ray-tracing
// pipeline fires this shader on every triangle hit before the
// closesthit decision; calling IgnoreHit() makes traversal skip the
// hit and continue to the next intersection. We use that to make
// semi-translucent / transmissive / alpha-tested materials INVISIBLE
// for first-bounce shading — the ray passes through and we shade
// whatever opaque surface is behind. Proper transmission BSDF +
// alpha-from-texture lookups are M11+ work; this is the visually-
// passable stub that stops translucent geometry from blocking light
// incorrectly.
//
// The BLAS build path drops the `Opaque` flag on every geometry so
// this shader fires unconditionally. For opaque materials this
// returns immediately (no IgnoreHit); the cost is one extra anyhit
// invocation per opaque hit, well under the §34.3 KPI budget.

#include "ShaderInterop.slang"

[[vk::binding(3, 0)]] StructuredBuffer<OpenPBRMaterialGPU> gMaterials;
[[vk::binding(4, 0)]] StructuredBuffer<uint>               gInstanceMaterial;

// Slang emits the SPIR-V `OpEntryPoint` name as `"main"` regardless
// of the source-side function name; the C++ side passes `"main"` to
// NVRHI's createShader. See the matching note in raygen.slang.
[shader("anyhit")]
void main(inout HitInfo payload, in BuiltInTriangleIntersectionAttributes attribs) {
    const uint instanceId = InstanceID();
    const uint materialId = gInstanceMaterial[instanceId];
    const OpenPBRMaterialGPU mat = gMaterials[materialId];

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
    if ((mat.flags & MATERIAL_FLAG_ALPHA_TESTED) != 0u) {
        IgnoreHit();
    }
    // All other hits — including opacity < 1 and transmission — are
    // accepted; the closesthit decides how to shade them.
}
