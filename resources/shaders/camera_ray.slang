// Pyxis renderer — shared camera primary-ray construction (P4 pass split).
//
// Extracted VERBATIM from the retired raygen.slang so the GBuffer raygen
// (raytraced_gbuffer_raygen.slang) and the Lighting raygen
// (raytraced_lighting_raygen.slang) compute bit-identical primary rays —
// the lighting pass reconstructs `worldHit = origin + hitT * dir` from the
// visibility buffer, so the two kernels MUST agree on every ray bit
// (§33.7 byte-identical contract). Do not "simplify" the expressions:
// their exact shape is what slangc compiled when the goldens were baked.
//
// Declares the CameraUniforms cbuffer at binding 0 — both RT pipelines
// place gCamera there (set=0, binding=0), so the one declaration serves
// every includer (shading.slang includes this header for gCamera too).

#ifndef PYXIS_CAMERA_RAY_SLANG
#define PYXIS_CAMERA_RAY_SLANG

#include "ShaderInterop.slang"

[[vk::binding(0, 0)]] ConstantBuffer<CameraUniforms> gCamera;

// Build the world-space primary ray for `launchIndex` of a
// `launchDim`-sized dispatch. Origin/direction land in the two out
// params; TMin/TMax stay at the call sites (0.001 / 10000 for the
// primary segment — identical to the pre-split raygen).
void BuildCameraRay(uint2 launchIndex, uint2 launchDim,
                    out float3 worldOriginXyz, out float3 worldDirXyz)
{
    // NDC pixel center: map [0, dim) integer pixel coords to
    // [-1, +1] device coordinates (Y flipped — we treat row 0 as the
    // top of the image, while NDC's +Y is up).
    const float2 pixelCenter = float2(launchIndex) + 0.5f;
    const float2 ndc = float2(
        ( pixelCenter.x / float(launchDim.x)) * 2.0f - 1.0f,
        -((pixelCenter.y / float(launchDim.y)) * 2.0f - 1.0f));

    // V2.A.20 — primary ray construction branches on projection mode:
    //
    //   Perspective (the v1 default):
    //     - Origin in view space = (0, 0, 0).
    //     - Direction in view space = normalize(viewFromClip · ndc).
    //     - Same ray construction as M3–M8a (kept verbatim below).
    //
    //   Orthographic (V2.A.20):
    //     - Direction in view space = (0, 0, -1) — uniform parallel
    //       rays along the camera's -Z axis (DirectX / Vulkan view-
    //       space convention; UsdGeomCamera.projection=orthographic
    //       lands the same way after the viewFromWorld transform).
    //     - Origin in view space = the inverse-projected NDC point on
    //       the near plane, whose xy components live in view-space
    //       units. Each pixel fires from a different view-space (x, y)
    //       → "scan" of parallel rays across the camera's image plane.
    //
    // Column-vector convention everywhere: `v' = M·v`. ndc.z = 1 maps
    // to the far plane on the perspective path (matrix-divide gives
    // the per-pixel ray direction); ndc.z = -1 maps to the near plane
    // on the ortho path (the per-pixel origin in view space).
    if (gCamera.projectionMode == 1u)  // orthographic
    {
        const float4 ndcOnNear   = float4(ndc.x, ndc.y, -1.0f, 1.0f);
        const float4 viewOrigin  = mul(gCamera.viewFromClip, ndcOnNear);
        const float3 viewOrigin3 = viewOrigin.xyz / viewOrigin.w;
        const float4 worldOrigin4 =
            mul(gCamera.worldFromView, float4(viewOrigin3, 1.0f));
        const float4 worldDir4 =
            mul(gCamera.worldFromView, float4(0.0f, 0.0f, -1.0f, 0.0f));
        worldOriginXyz = worldOrigin4.xyz;
        worldDirXyz    = normalize(worldDir4.xyz);
    }
    else  // perspective (default)
    {
        // Build a view-space ray direction from the inverse projection.
        // The clip-space point at depth +1 is (ndc.x, ndc.y, 1, 1);
        // applying viewFromClip puts it in view space, dividing out w
        // gives the actual point. Subtracting the view-space origin
        // (0, 0, 0) is just the point itself, so we normalise xyz
        // directly.
        const float4 ndcPos   = float4(ndc.x, ndc.y, 1.0f, 1.0f);
        const float4 viewPos4 = mul(gCamera.viewFromClip, ndcPos);
        const float3 viewDir  = normalize(viewPos4.xyz / viewPos4.w);

        // Translate to world space via the inverse view matrix.
        // Direction has w=0 so the translation column doesn't
        // contribute (= proper basis-vector transform).
        const float4 worldOrigin4 = mul(gCamera.worldFromView, float4(0, 0, 0, 1));
        const float4 worldDir4    = mul(gCamera.worldFromView, float4(viewDir, 0));
        worldOriginXyz = worldOrigin4.xyz;
        worldDirXyz    = normalize(worldDir4.xyz);
    }
}

#endif  // PYXIS_CAMERA_RAY_SLANG
