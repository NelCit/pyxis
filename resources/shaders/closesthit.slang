// Pyxis renderer — closest-hit shader.
//
// Plan §41 M5 + §15. Reads the per-instance OpenPBR material via a
// two-step lookup: `InstanceID()` (the per-instance slot packed into
// VkAccelerationStructureInstanceKHR.instanceCustomIndex per §15)
// indexes the instance→material side-table at binding 4 to get a
// material slot, which then indexes the material structured buffer
// at binding 3. The indirection frees the TLAS custom-index for the
// M6 instanceId AOV + future picking (§19.4).
//
// **No BSDF, no lighting, no texture sampling** — this is the M5
// stub the user replaces with the full branchless-on-flags OpenPBR
// closesthit (NEE direct lighting + BRDF importance sampling +
// texture sampling for HasBaseColorMap etc.) alongside lights at M7.
//
// What this shader exercises end-to-end at M5/M6:
//   * Material buffer binding (binding 3, structured-buffer SRV)
//   * Instance→material side-table (binding 4, M6 P0)
//   * §11 OpenPBRMaterialGPU layout (declared in ShaderInterop.slang;
//     scalar baseColorR/G/B avoids the float3 16-vs-12-byte trap)
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

#include "ShaderInterop.slang"

[[vk::binding(3, 0)]] StructuredBuffer<OpenPBRMaterialGPU> gMaterials;
[[vk::binding(4, 0)]] StructuredBuffer<uint>               gInstanceMaterial;

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

    // M5 stub: emit baseColor directly. The user's full OpenPBR
    // impl wires the actual BSDF here — branchless on `mat.flags`
    // bits per §11.6 (HasBaseColorMap → sample bindlessTextures
    // [mat.baseColorTex] etc.).
    payload.color = float3(mat.baseColorR, mat.baseColorG, mat.baseColorB);
    payload.hitT  = RayTCurrent();
}
