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
[[vk::binding(2, 0)]] RWTexture2D<float4>             gOutput;             // BGRA8 display
// AOV outputs (M7 follow-up). Raygen writes all four every frame from
// the same TraceRay payload; the inspector picks which one the display
// path remaps. Storage-capable formats only — RGBA16F for color/normal,
// R32F for depth, R32_UINT for instance id. Bindings are appended past
// the existing 0..10 to keep the §11 closesthit binding indices stable.
[[vk::binding(11, 0)]] RWTexture2D<float4>            gOutColorHdr;        // RGBA16F radiance
[[vk::binding(12, 0)]] RWTexture2D<float4>            gOutNormal;          // RGBA16F world normal
[[vk::binding(13, 0)]] RWTexture2D<float>             gOutDepth;           // R32F  ray-distance
[[vk::binding(14, 0)]] RWTexture2D<uint>              gOutInstanceId;      // R32U  instance slot
[[vk::binding(15, 0)]] RWStructuredBuffer<PickResult> gPickResult;         // 1-elem pick latch
// M7 follow-up batch 2 — 3 more raw AOVs the inspector exposes.
[[vk::binding(16, 0)]] RWTexture2D<uint>              gOutMaterialId;      // R32U  material slot
[[vk::binding(17, 0)]] RWTexture2D<float4>            gOutBaseColor;       // RGBA16F OpenPBR baseColor
[[vk::binding(18, 0)]] RWTexture2D<float4>            gOutWorldPos;        // RGBA32F world hit pos

// Narkowicz ACES Filmic — pulled out of main() so the display branch
// for DEBUG_VIEW_COLOR can call it cleanly. Same constants as before;
// see the M9 TODO above the call site for the eventual ToneMapPass
// hand-off.
float3 acesFilmic(float3 x) {
    const float a = 2.51;
    const float b = 0.03;
    const float c = 2.43;
    const float d = 0.59;
    const float e = 0.14;
    return saturate((x * (a * x + b)) / (x * (c * x + d) + e));
}

// Cheap integer hash → unit-interval float, used to recolor instance
// IDs into a visually-distinct palette for the InstanceID debug view.
// Murmur-style mix; avalanches enough that adjacent IDs land on
// different hues without a lookup table.
float3 hashInstanceColor(uint id) {
    if (id == 0xFFFFFFFFu) return float3(0.0, 0.0, 0.0);
    uint x = id * 2654435761u;
    x ^= x >> 16;
    x *= 2246822519u;
    x ^= x >> 13;
    x *= 3266489917u;
    x ^= x >> 16;
    const uint r = (x      ) & 0xFFu;
    const uint g = (x >>  8) & 0xFFu;
    const uint b = (x >> 16) & 0xFFu;
    return float3(float(r), float(g), float(b)) * (1.0 / 255.0);
}

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
    payload.color        = float3(0, 0, 0);
    payload.hitT         = -1.0f;
    payload.normalWorld  = float3(0, 0, 0);
    payload.instanceId   = 0xFFFFFFFFu;
    payload.baseColor    = float3(0, 0, 0);
    payload.materialId   = 0xFFFFFFFFu;
    payload.worldHit     = float3(0, 0, 0);
    payload._hitInfoPad0 = 0u;

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

    // ----- Raw AOVs -----------------------------------------------
    // Written every frame regardless of debugViewMode so the AOV
    // inspector + pixel picker + Save AOV can read RAW data — not
    // the post-tonemap remap. The inspector branch below picks
    // which one feeds the BGRA8 display output.
    gOutColorHdr[launchIndex]   = float4(payload.color, 1.0f);
    gOutNormal[launchIndex]     = float4(payload.normalWorld, 1.0f);
    gOutDepth[launchIndex]      = (payload.hitT > 0.0f) ? payload.hitT : 0.0f;
    gOutInstanceId[launchIndex] = payload.instanceId;
    gOutMaterialId[launchIndex] = payload.materialId;
    gOutBaseColor[launchIndex]  = float4(payload.baseColor, 1.0f);
    gOutWorldPos[launchIndex]   = float4(payload.worldHit, 1.0f);

    // ----- Pixel picker -------------------------------------------
    // Write the raw values into the 1-element pick buffer when this
    // dispatched pixel matches the cursor. The viewer kicks a copy
    // into a staging buffer per frame and reads back next frame.
    if (launchIndex.x == gCamera.mousePixelX && launchIndex.y == gCamera.mousePixelY)
    {
        PickResult p;
        p.colorR     = payload.color.x;
        p.colorG     = payload.color.y;
        p.colorB     = payload.color.z;
        p.depth      = payload.hitT;
        p.normalX    = payload.normalWorld.x;
        p.normalY    = payload.normalWorld.y;
        p.normalZ    = payload.normalWorld.z;
        p.instanceId = payload.instanceId;
        gPickResult[0] = p;
    }

    // ----- Display output (BGRA8 backbuffer) ----------------------
    // ACES tonemap on the Color path; (n*0.5+0.5) on Normal; 1/depth
    // grayscale on Depth; hash-coloured palette on InstanceID. The
    // M7 inline ACES is preserved on the Color branch only — see
    // the standing M9 TODO ("ToneMapPass takeover") above.
    float3 displayColor = float3(0.0, 0.0, 0.0);
    if (gCamera.debugViewMode == DEBUG_VIEW_COLOR)
    {
        displayColor = acesFilmic(payload.color);
    }
    else if (gCamera.debugViewMode == DEBUG_VIEW_NORMAL)
    {
        // Miss is (0,0,0) in the Normal AOV; display as middle grey
        // so the background isn't visually identical to a Z-aligned
        // surface. Hit normals: standard half-encode.
        displayColor = (payload.hitT > 0.0f)
            ? (payload.normalWorld * 0.5f + 0.5f)
            : float3(0.5f, 0.5f, 0.5f);
    }
    else if (gCamera.debugViewMode == DEBUG_VIEW_DEPTH)
    {
        // Inverse-depth grayscale: surfaces closer to the camera
        // appear brighter, miss (= 0 sentinel) renders black.
        // The 1/(1+d) curve avoids the divide-by-zero at d=0 and
        // gives a smooth response across the typical [0.1, 50]
        // viewer range without a configured min/max range.
        const float d = payload.hitT;
        const float intensity = (d > 0.0f) ? (1.0f / (1.0f + d * 0.1f)) : 0.0f;
        displayColor = float3(intensity, intensity, intensity);
    }
    else if (gCamera.debugViewMode == DEBUG_VIEW_INSTANCE_ID)
    {
        displayColor = hashInstanceColor(payload.instanceId);
    }
    else if (gCamera.debugViewMode == DEBUG_VIEW_MATERIAL_ID)
    {
        // Same hash function as InstanceID — different identifier
        // domains so the colour palette is independent. Reuses
        // hashInstanceColor since the math is identity (uint -> hue).
        displayColor = hashInstanceColor(payload.materialId);
    }
    else if (gCamera.debugViewMode == DEBUG_VIEW_BASE_COLOR)
    {
        // Raw OpenPBR baseColor straight off the material table —
        // pre-shading albedo. saturate() clamps in case a (post-v1)
        // emissive material's baseColor exceeds [0,1].
        displayColor = saturate(payload.baseColor);
    }
    else if (gCamera.debugViewMode == DEBUG_VIEW_WORLD_POS)
    {
        // Visualise world-space hit position via a 10-unit-period
        // fract pattern. Lets users verify camera<->scene alignment
        // without a magnitude-dependent colour scale; (0..10) maps
        // identically to (0..1) so a unit-cube object reads as a
        // tenth-tone gradient. Miss (worldHit == 0) reads as black.
        displayColor = (payload.hitT > 0.0f)
            ? frac(payload.worldHit * 0.1f)
            : float3(0.0, 0.0, 0.0);
    }
    gOutput[launchIndex] = float4(displayColor, 1.0f);
}
