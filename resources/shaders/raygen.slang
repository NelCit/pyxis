// Pyxis renderer — ray-generation entry point.
//
// Plan §41 M3 ("raygen/closesthit/miss for one cube") + §10 / §23.
//
// M3 simplification: 1 primary ray per pixel, no recursion, no NEE,
// no shadow rays. Builds a pinhole-camera primary ray from the
// CameraUniforms cbuffer, traces it against the bound TLAS, and
// writes the payload colour to the bound output image. Closesthit
// returns barycentric colour, miss returns sky blue — both already
// satisfy the M3 "cube renders" exit criterion.
//
// Bindings — these match `pyxis::PathTracePass` on the C++ side.
//   space=0, b0  : CameraUniforms cbuffer
//   space=0, t0  : RaytracingAccelerationStructure (TLAS)
//   space=0, u0  : RWTexture2D<float4> (output colour)

#include "ShaderInterop.slang"

ConstantBuffer<CameraUniforms>         gCamera : register(b0);
RaytracingAccelerationStructure        gTlas   : register(t0);
RWTexture2D<float4>                    gOutput : register(u0);

[shader("raygeneration")]
void RayGenMain() {
    const uint2 launchIndex = DispatchRaysIndex().xy;
    const uint2 launchDim   = DispatchRaysDimensions().xy;

    // NDC pixel center: map [0, dim) integer pixel coords to
    // [-1, +1] device coordinates (Y flipped — we treat row 0 as the
    // top of the image, while NDC's +Y is up).
    const float2 pixelCenter = float2(launchIndex) + 0.5f;
    const float2 ndc = float2(
        ( pixelCenter.x / float(launchDim.x)) * 2.0f - 1.0f,
        -((pixelCenter.y / float(launchDim.y)) * 2.0f - 1.0f));

    // Build a view-space ray direction from the inverse projection.
    // The clip-space point at depth +1 in row-vector convention is
    // (ndc.x, ndc.y, 1, 1); multiplying by viewFromClip puts it in
    // view space, and then dividing out w gives the actual point.
    // Subtracting the view-space origin (0, 0, 0) from that point is
    // just the point itself, so we normalise its xyz directly.
    const float4 ndcPos       = float4(ndc.x, ndc.y, 1.0f, 1.0f);
    const float4 viewPos4     = mul(ndcPos, gCamera.viewFromClip);
    const float3 viewDir      = normalize(viewPos4.xyz / viewPos4.w);

    // Translate the view-space origin + direction to world space via
    // the inverse view matrix.
    const float4 worldOrigin4 = mul(float4(0, 0, 0, 1), gCamera.worldFromView);
    const float4 worldDir4    = mul(float4(viewDir, 0), gCamera.worldFromView);

    RayDesc ray;
    ray.Origin    = worldOrigin4.xyz;
    ray.Direction = normalize(worldDir4.xyz);
    ray.TMin      = 0.001f;
    ray.TMax      = 10000.0f;

    HitInfo payload;
    payload.color = float3(0, 0, 0);
    payload.hitT  = -1.0f;

    // RAY_FLAG_NONE — keep the door open for future shadow/opaque-only
    // refinements. instanceMask = 0xFF matches what GpuScene's TLAS
    // builder writes per instance, so all of them are visible to the
    // primary ray.
    TraceRay(gTlas,
             RAY_FLAG_NONE,
             0xFFu,
             /*RayContributionToHitGroupIndex*/ 0,
             /*MultiplierForGeometryContributionToHitGroupIndex*/ 0,
             /*MissShaderIndex*/ 0,
             ray,
             payload);

    gOutput[launchIndex] = float4(payload.color, 1.0f);
}
