// Pyxis renderer — closest-hit shader.
//
// Plan §41 M5/M6/M7 + §15. Reads the per-instance OpenPBR material
// via a two-step lookup: `InstanceID()` (the per-instance slot
// packed into VkAccelerationStructureInstanceKHR.instanceCustomIndex
// per §15) indexes the instance→material side-table at binding 4 to
// get a material slot, which then indexes the material structured
// buffer at binding 3. The indirection frees the TLAS custom-index
// for the M6 instanceId AOV + future picking (§19.4).
//
// **M7-simple lighting (no NdotL, no shadow rays, no NEE, no MIS,
// no importance sampling)**. Each light contributes
// `color * intensity * weight` where weight is 1.0 for direct
// (Distant / Rect) and 0.5 for ambient (Dome). Final colour is
// `baseColor * sum_of_contributions`. The no-lights fallback
// (PathTracePass binds a 1-element intensity=0 sentinel) emits
// baseColor directly so M5/M6 unlit fixtures stay byte-equal.
//
// The user's full OpenPBR + NEE+MIS+IBL impl replaces this body
// alongside the BSDF lobes per §11.6 — branchless on `mat.flags`
// bits, sampling lights via NEE, querying the dome via the alias
// table at §7.
//
// What this shader exercises end-to-end at M5/M6/M7:
//   * Material buffer binding (binding 3, structured-buffer SRV)
//   * Instance→material side-table (binding 4, M6 P0)
//   * Lights buffer (binding 5, M7-simple)
//   * §11 OpenPBRMaterialGPU layout + LightGpu layout (declared in
//     ShaderInterop.slang; scalar fields avoid the float3
//     16-vs-12-byte trap)
//
// Bindings — match pyxis::PathTracePass's NVRHI BindingLayout. The
// raygen / miss shaders share the same set 0; binding numbers stay
// stable across stages so the single binding set covers all three.
//
//   set=0, binding=0 : CameraUniforms cbuffer (raygen-only)
//   set=0, binding=1 : RaytracingAccelerationStructure TLAS (raygen)
//   set=0, binding=2 : RWTexture2D<float4> output image (raygen)
//   set=0, binding=3 : StructuredBuffer<OpenPBRMaterialGPU> materials
//                      (closesthit — added at M5)
//   set=0, binding=4 : StructuredBuffer<uint> instance→material slot
//                      (closesthit — added at M6, plan §15)
//   set=0, binding=5 : StructuredBuffer<LightGpu> lights
//                      (closesthit — added at M7-simple)

#include "ShaderInterop.slang"

[[vk::binding(3, 0)]] StructuredBuffer<OpenPBRMaterialGPU> gMaterials;
[[vk::binding(4, 0)]] StructuredBuffer<uint>               gInstanceMaterial;
[[vk::binding(5, 0)]] StructuredBuffer<LightGpu>           gLights;

// Slang emits the SPIR-V `OpEntryPoint` name as `"main"` regardless
// of the source-side function name; the C++ side passes `"main"` to
// NVRHI's createShader. See the matching note in raygen.slang.
[shader("closesthit")]
void main(inout HitInfo payload, in BuiltInTriangleIntersectionAttributes attribs) {
    // §15: InstanceID() is the per-instance slot. The side-table
    // resolves it to a material slot (0 = "no material assigned" →
    // sentinel grey material at materials[0]).
    const uint instanceId = InstanceID();
    const uint materialId = gInstanceMaterial[instanceId];
    const OpenPBRMaterialGPU mat = gMaterials[materialId];
    const float3 baseColor = float3(mat.baseColorR, mat.baseColorG, mat.baseColorB);

    // M7-simple lighting. Loop over every authored light, accumulate
    // `color * intensity * weight`. The no-lights case (PathTracePass
    // binds a single intensity=0 sentinel) leaves `lightingSum`
    // contributing zero → totalContribution == 0 → falls through to
    // baseColor-only emission, preserving M5/M6 byte-equal.
    uint lightCount;
    uint _stride;
    gLights.GetDimensions(lightCount, _stride);
    float3 lightingSum = float3(0.0, 0.0, 0.0);
    float  totalContribution = 0.0;
    for (uint i = 0; i < lightCount; ++i) {
        LightGpu L = gLights[i];
        if (L.intensity <= 0.0) continue;  // sentinel / disabled
        // Dome counts as half-strength ambient; Distant + Rect as
        // full-strength direct (no NdotL, no area sampling — the
        // user fills both in alongside the full BSDF).
        float weight = (L.kind == LIGHT_KIND_DOME) ? 0.5 : 1.0;
        lightingSum += float3(L.colorR, L.colorG, L.colorB) * L.intensity * weight;
        totalContribution += L.intensity * weight;
    }

    payload.color = (totalContribution > 0.0) ? (baseColor * lightingSum) : baseColor;
    payload.hitT  = RayTCurrent();
}
