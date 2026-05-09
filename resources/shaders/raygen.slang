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
// Bindings — these match `pyxis::PathTracePass`'s NVRHI BindingLayout
// on the C++ side. NVRHI translates a sequential `addItem(...)` list
// into Vulkan binding numbers per item (set 0, binding 0, 1, 2, ...);
// HLSL's separate `register(b0)/t0/u0` namespaces collapse to one
// binding-number space under -target spirv, so we use explicit
// `[[vk::binding(N, S)]]` attributes to lock the slot assignment.
//
// Raygen consumes only the first three bindings; bindings 3..10 are
// declared by closesthit.slang + miss.slang (full list in
// closesthit.slang's header comment) and bound across all three
// stages by the shared layout (visibility = AllRayTracing).
//
//   set=0, binding=0 : CameraUniforms cbuffer (raygen)
//   set=0, binding=1 : RaytracingAccelerationStructure (raygen)
//   set=0, binding=2 : RWTexture2D<float4> output (raygen)
//   set=0, binding=3..8 : materials / instance side-tables / lights /
//                         per-mesh normals — see closesthit.slang
//   set=0, binding=9..10: dome HDRI texture + sampler — see miss.slang

#include "ShaderInterop.slang"

[[vk::binding(0, 0)]] ConstantBuffer<CameraUniforms>  gCamera;
[[vk::binding(1, 0)]] RaytracingAccelerationStructure gTlas;
[[vk::binding(2, 0)]] RWTexture2D<float4>             gOutput;

// Slang emits the SPIR-V `OpEntryPoint` name as `"main"` regardless
// of the source-side function name (verified via spirv-dis); the C++
// side passes `"main"` to NVRHI's createShader so we keep the function
// name aligned to avoid the VUID-VkPipelineShaderStageCreateInfo-
// pName-00707 mismatch.
[shader("raygeneration")]
void main() {
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
    // Column-vector convention everywhere: `v' = M·v`, matrix on the
    // left. The clip-space point at depth +1 is (ndc.x, ndc.y, 1, 1);
    // applying viewFromClip puts it in view space, dividing out w
    // gives the actual point. Subtracting the view-space origin
    // (0, 0, 0) from that point is just the point itself, so we
    // normalise its xyz directly.
    const float4 ndcPos       = float4(ndc.x, ndc.y, 1.0f, 1.0f);
    const float4 viewPos4     = mul(gCamera.viewFromClip, ndcPos);
    const float3 viewDir      = normalize(viewPos4.xyz / viewPos4.w);

    // Translate the view-space origin + direction to world space via
    // the inverse view matrix. Direction has w=0 so the translation
    // column doesn't contribute (= proper basis-vector transform).
    const float4 worldOrigin4 = mul(gCamera.worldFromView, float4(0, 0, 0, 1));
    const float4 worldDir4    = mul(gCamera.worldFromView, float4(viewDir, 0));

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

    // M7-simple tone-map: ACES Filmic (Narkowicz approximation).
    //
    // TODO(M9): REMOVE this block when the §9 ToneMapPass lands as
    // a separate render-graph stage. The ToneMapPass owns the
    // tone-map step end-to-end; leaving the operator inlined here
    // would tone-map twice (raygen + ToneMapPass), crushing
    // highlights and desaturating mid-tones. The grep marker is
    // `M7-simple tone-map`.
    //
    // The closesthit's NdotL Lambert + Dome ambient and the miss
    // shader's HDRI sample both produce HDR values that can exceed
    // 1.0 (Sun × NdotL × baseColor → ~1.2; HDRI sky pixels → up to
    // ~10.0 in bright cloud / sun areas). The swapchain blit clamps
    // anything above 1.0 to white, so without a tone-map the bright
    // outdoor look of the bundled Poly Haven HDRI shows up as a
    // featureless white background and over-bright surfaces.
    //
    // Narkowicz's ACES Fitted approximation (single rational
    // function per channel) is the canonical production choice —
    // used by Unreal, Unity HDRP, plus most real-time path tracers.
    // Maps [0, ∞) → [0, 1] with a filmic shoulder + toe; preserves
    // hue close enough that the §11 OpenPBR baseColor's perceptual
    // identity survives. M9 swaps this for the proper ACES Fitted
    // (RRT + ODT matrices) + OCIO display transform alongside the
    // §9 ToneMapPass.
    //
    // Reference: Krzysztof Narkowicz, "ACES Filmic Tone Mapping
    // Curve" (2015). Constants below are the published fit; do not
    // touch without re-fitting against the ACES RRT + sRGB ODT.
    const float3 x = payload.color;
    const float aces_a = 2.51f;
    const float aces_b = 0.03f;
    const float aces_c = 2.43f;
    const float aces_d = 0.59f;
    const float aces_e = 0.14f;
    const float3 toneMapped = saturate(
        (x * (aces_a * x + aces_b)) / (x * (aces_c * x + aces_d) + aces_e));
    gOutput[launchIndex] = float4(toneMapped, 1.0f);
}
