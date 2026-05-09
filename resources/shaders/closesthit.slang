// Pyxis renderer — closest-hit shader.
//
// Plan §41 M5. Reads the per-instance OpenPBR material from the
// material structured buffer (binding 3, populated by GpuScene's
// MaterialTable upload at CommitResources) and emits its baseColor
// as the payload colour. **No BSDF, no lighting, no texture
// sampling** — this is the M5 stub the user replaces with the full
// branchless-on-flags OpenPBR closesthit (NEE direct lighting +
// BRDF importance sampling + texture sampling for HasBaseColorMap
// etc.) alongside lights at M7.
//
// What this shader exercises end-to-end at M5:
//   * Material buffer binding (binding 3, structured-buffer SRV)
//   * Per-instance material lookup via InstanceID() — set by
//     GpuScene to the material slot at TLAS rebuild
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

#include "ShaderInterop.slang"

[[vk::binding(3, 0)]] StructuredBuffer<OpenPBRMaterialGPU> gMaterials;

// Slang emits the SPIR-V `OpEntryPoint` name as `"main"` regardless
// of the source-side function name; the C++ side passes `"main"` to
// NVRHI's createShader. See the matching note in raygen.slang.
[shader("closesthit")]
void main(inout HitInfo payload, in BuiltInTriangleIntersectionAttributes attribs) {
    // InstanceID() returns the `instanceID` GpuScene packed into
    // VkAccelerationStructureInstanceKHR.instanceCustomIndex at
    // TLAS rebuild — the MATERIAL slot of the hit instance's
    // bound material (or 0 for "no material assigned" → sentinel
    // magenta material packed at slot 0 by CommitResources).
    const uint materialId = InstanceID();
    const OpenPBRMaterialGPU mat = gMaterials[materialId];

    // M5 stub: emit baseColor directly. The user's full OpenPBR
    // impl wires the actual BSDF here — branchless on `mat.flags`
    // bits per §11.6 (HasBaseColorMap → sample bindlessTextures
    // [mat.baseColorTex] etc.).
    payload.color = float3(mat.baseColorR, mat.baseColorG, mat.baseColorB);
    payload.hitT  = RayTCurrent();
}
