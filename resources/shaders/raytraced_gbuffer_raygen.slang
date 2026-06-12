// Pyxis renderer — GBuffer-pass ray-generation entry point (P4 pass
// split, design D1).
//
// The visibility-buffer producer: builds the camera primary ray via the
// shared BuildCameraRay (camera_ray.slang — bit-identical to the
// lighting raygen's reconstruction), traces ONE TraceRay with the thin
// VisibilityPayload (no transparency loop — the lighting raygen
// continues the front-to-back composite from segment 1), and writes the
// 32-byte fp32 VisibilityGpu record the RaytracedLightingPass consumes.
// Ray parameters (RAY_FLAG_NONE, mask 0xFF, missIndex 0, TMin 0.001,
// TMax 10000) are identical to the pre-split primary segment.
//
// Bindings — match pyxis::RaytracedGBufferPass's NVRHI BindingLayout
// (set 0, this pipeline's OWN layout; gMaterials/gInstanceInfo keep the
// lighting pipeline's slot numbers so the shared anyhit module declares
// them once):
//   set=0, binding=0 : CameraUniforms cbuffer (camera_ray.slang)
//   set=0, binding=1 : RaytracingAccelerationStructure (here)
//   set=0, binding=2 : RWStructuredBuffer<VisibilityGpu> (here)
//   set=0, binding=3 : StructuredBuffer<OpenPBRMaterialGPU> (anyhit)
//   set=0, binding=4 : StructuredBuffer<InstanceInfoGpu> (anyhit)

#include "ShaderInterop.slang"
#include "camera_ray.slang"

[[vk::binding(1, 0)]] RaytracingAccelerationStructure   gTlas;
[[vk::binding(2, 0)]] RWStructuredBuffer<VisibilityGpu> gVisibility;

// Slang emits the SPIR-V `OpEntryPoint` name as `"main"` regardless
// of the source-side function name (verified via spirv-dis); the C++
// side passes `"main"` to NVRHI's createShader so we keep the function
// name aligned to avoid the VUID-VkPipelineShaderStageCreateInfo-
// pName-00707 mismatch.
[shader("raygeneration")]
void main() {
    const uint2 launchIndex = DispatchRaysIndex().xy;
    const uint2 launchDim   = DispatchRaysDimensions().xy;

    float3 worldOriginXyz;
    float3 worldDirXyz;
    BuildCameraRay(launchIndex, launchDim, worldOriginXyz, worldDirXyz);

    RayDesc ray;
    ray.Origin    = worldOriginXyz;
    ray.Direction = worldDirXyz;
    ray.TMin      = 0.001f;
    ray.TMax      = 10000.0f;

    // Miss sentinels mirror the fat-payload convention: hitT = -1,
    // ids = ~0u. The GBuffer miss shader only re-asserts hitT.
    VisibilityPayload payload;
    payload.hitT           = -1.0f;
    payload.bary           = float2(0.0f, 0.0f);
    payload.instanceId     = 0xFFFFFFFFu;
    payload.primitiveIndex = 0xFFFFFFFFu;

    TraceRay(gTlas,
             RAY_FLAG_NONE,
             0xFFu,
             /*RayContributionToHitGroupIndex*/ 0,
             /*MultiplierForGeometryContributionToHitGroupIndex*/ 0,
             /*MissShaderIndex*/ 0,
             ray,
             payload);

    // fp32-exact record — never quantized (design D1); the lighting
    // raygen reconstructs every closesthit input from these four
    // values + the camera ray.
    VisibilityGpu vis;
    vis.hitT           = payload.hitT;
    vis.baryX          = payload.bary.x;
    vis.baryY          = payload.bary.y;
    vis.instanceId     = payload.instanceId;
    vis.primitiveIndex = payload.primitiveIndex;
    vis._pad0          = 0u;
    vis._pad1          = 0u;
    vis._pad2          = 0u;
    gVisibility[launchIndex.y * launchDim.x + launchIndex.x] = vis;
}
