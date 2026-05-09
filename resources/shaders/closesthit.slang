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
//   set=0, binding=0  : CameraUniforms cbuffer (raygen-only)
//   set=0, binding=1  : RaytracingAccelerationStructure TLAS (raygen)
//   set=0, binding=2  : RWTexture2D<float4> output image (raygen)
//   set=0, binding=3  : StructuredBuffer<OpenPBRMaterialGPU> materials
//                       (closesthit — added at M5)
//   set=0, binding=4  : StructuredBuffer<uint> instance→material slot
//                       (closesthit — added at M6, plan §15)
//   set=0, binding=5  : StructuredBuffer<LightGpu> lights
//                       (closesthit + miss — added at M7-simple)
//   set=0, binding=6  : StructuredBuffer<uint> instance→mesh slot
//                       (closesthit — added at M7 NdotL)
//   set=0, binding=7  : StructuredBuffer<float4> mesh face normals
//                       (closesthit — added at M7 NdotL; flat
//                        per-mesh concatenation, indexed via offset 8)
//   set=0, binding=8  : StructuredBuffer<uint> mesh face-normal offsets
//                       (closesthit — added at M7 NdotL)
//   set=0, binding=9  : Texture2D<float4> dome HDRI env-map
//                       (miss — added at M7 IBL)
//   set=0, binding=10 : SamplerState dome HDRI sampler
//                       (miss — added at M7 IBL)

#include "ShaderInterop.slang"

[[vk::binding(3, 0)]] StructuredBuffer<OpenPBRMaterialGPU> gMaterials;
[[vk::binding(4, 0)]] StructuredBuffer<uint>               gInstanceMaterial;
[[vk::binding(5, 0)]] StructuredBuffer<LightGpu>           gLights;
[[vk::binding(6, 0)]] StructuredBuffer<uint>               gInstanceMesh;
[[vk::binding(7, 0)]] StructuredBuffer<float4>             gMeshFaceNormals;
[[vk::binding(8, 0)]] StructuredBuffer<uint>               gMeshFaceOffsets;

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

    // M7 NdotL: resolve the hit-triangle's object-space face normal
    // via the M7 NdotL side-tables, then transform to world space
    // using Vulkan's per-instance ObjectToWorld3x4 builtin.
    //
    // ObjectToWorld3x4() returns a column-major 3x4 affine; its
    // upper-3x3 transforms direction vectors. For a uniform-scale +
    // rotation transform (the M7 fixture cases) this is correct as-
    // is. Non-uniform scale would need the inverse-transpose of the
    // upper 3x3 (M9 polish).
    const uint meshSlot = gInstanceMesh[instanceId];
    const uint faceOffset = gMeshFaceOffsets[meshSlot];
    const float3 nLocal = gMeshFaceNormals[faceOffset + PrimitiveIndex()].xyz;
    const float3x4 objectToWorld = ObjectToWorld3x4();
    const float3x3 worldFromObject3x3 = float3x3(
        objectToWorld[0].xyz,
        objectToWorld[1].xyz,
        objectToWorld[2].xyz);
    const float3 nWorld = normalize(mul(worldFromObject3x3, nLocal));

    // M7-simple lighting + NdotL Lambert for direct lights. Loop
    // over every authored light, accumulate the per-light Lambert
    // contribution. The no-lights case (PathTracePass binds a single
    // intensity=0 sentinel) leaves `directSum` zero → falls through
    // to baseColor-only emission, preserving M5/M6 byte-equal.
    //
    // - Distant: lightDir = -L.direction (the world-space direction
    //   the light's photons travel; NdotL uses the negated form
    //   pointing FROM surface TO light source).
    // - Rect: lightDir = normalize(L.position - hitWorld), treating
    //   the rect as a point at its center (no area sampling).
    // - Dome: uniform ambient (no NdotL — hemispheric integration
    //   is what the user fills in at M7-full alongside the full BSDF).
    uint lightCount;
    uint _stride;
    gLights.GetDimensions(lightCount, _stride);
    float3 directSum = float3(0.0, 0.0, 0.0);
    float3 ambientSum = float3(0.0, 0.0, 0.0);
    float  totalContribution = 0.0;
    const float3 hitWorld = WorldRayOrigin() + RayTCurrent() * WorldRayDirection();
    for (uint i = 0; i < lightCount; ++i) {
        LightGpu L = gLights[i];
        if (L.intensity <= 0.0) continue;  // sentinel / disabled
        const float3 Lcol = float3(L.colorR, L.colorG, L.colorB) * L.intensity;
        if (L.kind == LIGHT_KIND_DOME) {
            // Dome ambient with NdotL against the world up axis.
            // The proper hemispheric integral
            //   ∫ radiance(ω) · max(0, dot(N, ω)) dω
            // is what the M7-full alias-table importance sampler
            // (§7) computes; the simplest approximation that
            // responds to surface orientation is Lambert against the
            // up axis: surfaces facing up get full dome, surfaces
            // facing horizontally get zero, surfaces facing down get
            // zero. Half-strength scale matches the M7-simple
            // "ambient is half-bright" convention so a single dome
            // doesn't out-bright a single sun.
            const float NdotUp = max(0.0, dot(nWorld, float3(0.0, 1.0, 0.0)));
            ambientSum += Lcol * 0.5 * NdotUp;
            totalContribution += L.intensity * 0.5;
            continue;
        }
        float3 lightDir;
        if (L.kind == LIGHT_KIND_RECT) {
            const float3 toRect = float3(L.posX, L.posY, L.posZ) - hitWorld;
            lightDir = normalize(toRect);
        } else {
            // Distant
            lightDir = -float3(L.dirX, L.dirY, L.dirZ);
        }
        const float NdotL = max(0.0, dot(nWorld, lightDir));
        directSum += Lcol * NdotL;
        totalContribution += L.intensity;  // pre-NdotL — used only as the no-lights fallthrough sentinel
    }

    payload.color = (totalContribution > 0.0) ? (baseColor * (directSum + ambientSum)) : baseColor;
    payload.hitT  = RayTCurrent();
    // M7 follow-up — fields the raygen-side AOV inspector reads.
    // Normal is the world-space face normal we already computed for
    // the Lambert pass; instanceId is the per-instance slot the
    // closesthit branched on for material lookup. baseColor /
    // materialId / worldHit feed the new BaseColor / MaterialID /
    // WorldPos AOVs respectively — all already in flight as locals.
    payload.normalWorld = nWorld;
    payload.instanceId  = instanceId;
    payload.baseColor   = baseColor;
    payload.materialId  = materialId;
    payload.worldHit    = hitWorld;
    payload._hitInfoPad0 = 0u;
}
