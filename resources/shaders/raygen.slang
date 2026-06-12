// Pyxis renderer — ray-generation entry point.
//
// Plan §41 M3 ("raygen/closesthit/miss for one cube") + §10 / §23.
//
// M3 simplification: 1 primary ray per pixel, no recursion, no NEE,
// no shadow rays. Builds a pinhole-camera primary ray from the
// CameraUniforms cbuffer, traces it against the bound TLAS, and
// writes the payload colour to the fp32 linear-radiance image
// (gLinearColor) the TonemapPass consumes. The display transform
// (exposure + ACES + the 11-way debug-view encode) lives in
// tonemap.slang since the P3 pass split — raygen writes RAW data only.
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
//   set=0, binding=2 : RWTexture2D<float4> fp32 linear radiance (raygen)
//   set=0, binding=3..8 : materials / instance side-tables / lights /
//                         per-mesh normals — see closesthit.slang
//   set=0, binding=9..10: dome HDRI texture + sampler — see miss.slang

#include "ShaderInterop.slang"

[[vk::binding(0, 0)]] ConstantBuffer<CameraUniforms>  gCamera;
[[vk::binding(1, 0)]] RaytracingAccelerationStructure gTlas;
// P3 pass split — full-precision LINEAR radiance (RGBA32F, renderer-internal
// scratch). The TonemapPass reads this and applies the display transform into
// the BGRA8 display target; fp32 keeps the display path bit-identical to the
// in-kernel payload.color the old inline branch consumed.
[[vk::binding(2, 0)]] RWTexture2D<float4>             gLinearColor;        // RGBA32F radiance
// AOV outputs (M7 follow-up). Raygen writes all four every frame from
// the same TraceRay payload; the inspector picks which one the display
// path remaps. Storage-capable formats only — RGBA16F for color/normal,
// R32F for depth, R32_UINT for instance id. Bindings are appended past
// the existing 0..10 to keep the §11 closesthit binding indices stable.
[[vk::binding(11, 0)]] RWTexture2D<float4>            gOutColorHdr;        // RGBA16F radiance
[[vk::binding(12, 0)]] RWTexture2D<float4>            gOutNormal;          // RGBA16F world normal
[[vk::binding(13, 0)]] RWTexture2D<float>             gOutDepth;           // R32F  ray-distance
[[vk::binding(14, 0)]] RWTexture2D<uint>              gOutPrimId;          // R32U  Hydra primId (per-instance slot)
[[vk::binding(15, 0)]] RWStructuredBuffer<PickResult> gPickResult;         // 1-elem pick latch
// M7 follow-up batch 2 — 3 more raw AOVs the inspector exposes.
[[vk::binding(16, 0)]] RWTexture2D<uint>              gOutMaterialId;      // R32U  material slot
[[vk::binding(17, 0)]] RWTexture2D<float4>            gOutBaseColor;       // RGBA16F OpenPBR baseColor
[[vk::binding(18, 0)]] RWTexture2D<float4>            gOutWorldPos;        // RGBA32F world hit pos
// M7 follow-up batch 3 — viewer-only per-frame state. Lives in its
// own cbuffer (rather than tail-on-CameraUniforms) so future per-frame
// camera uniforms can grow without touching UI bindings.
[[vk::binding(19, 0)]] ConstantBuffer<FrameUiUniforms> gFrameUi;
// Tier 1 Hydra-canonical AOVs — every DCC delegate (usdview, Solaris,
// Omniverse) queries these by name. Same shape as the existing AOVs:
// one UAV per channel, written every frame from the same TraceRay
// payload (no second trace).
[[vk::binding(20, 0)]] RWTexture2D<float>             gOutAlpha;           // R8_UNORM 1.0 hit / 0.0 miss
[[vk::binding(21, 0)]] RWTexture2D<uint>              gOutElementId;       // R32U  per-face id (~0u miss)
[[vk::binding(22, 0)]] RWTexture2D<float4>            gOutNormalEye;       // RGBA16F eye-space normal
[[vk::binding(23, 0)]] RWTexture2D<float4>            gOutWorldPosEye;     // RGBA32F eye-space hit pos

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
    float3 worldOriginXyz;
    float3 worldDirXyz;
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

    RayDesc segRay;
    segRay.Origin    = worldOriginXyz;
    segRay.Direction = worldDirXyz;
    segRay.TMin      = 0.001f;
    segRay.TMax      = 10000.0f;

    // V2.B transmission loop. Opacity see-through is composited
    // FRONT-TO-BACK here as an iterative sequence of primary-depth
    // TraceRays — NOT a recursive continuation ray inside closesthit.
    // Each iteration re-traces from just past the previous hit at
    // recursion depth 0, so an arbitrary stack of transparent surfaces
    // costs cheap loop iterations instead of hardware recursion depth
    // (capped at 3 per plan_for_v2.md §V2.B). closesthit returns this
    // surface's emitted radiance in payload.color and the light it lets
    // through from behind in payload.transmittance; we accumulate the
    // former and fold the latter into a running throughput.
    //
    // The FIRST hit's payload feeds every AOV (frontmost surface —
    // unchanged semantics); only the displayed/HDR colour reflects the
    // composited stack. The loop is deterministic (no RNG, fixed
    // iteration order, fixed cap) so the headless byte-identical-EXR
    // contract (§33.7) holds. RAY_FLAG_NONE + instanceMask 0xFF matches
    // GpuScene's TLAS so every instance is visible to the primary ray.
    const uint MAX_TRANSPARENT_SEGMENTS = 16u;
    float3  radiance   = float3(0, 0, 0);
    float3  throughput = float3(1, 1, 1);
    HitInfo firstHit;
    firstHit.color           = float3(0, 0, 0);
    firstHit.hitT            = -1.0f;
    firstHit.normalWorld     = float3(0, 0, 0);
    firstHit.instanceId      = 0xFFFFFFFFu;
    firstHit.baseColor       = float3(0, 0, 0);
    firstHit.materialId      = 0xFFFFFFFFu;
    firstHit.worldHit        = float3(0, 0, 0);
    firstHit.elementId       = 0xFFFFFFFFu;
    firstHit.recursionDepth  = 0;
    firstHit.transmittance   = float3(0, 0, 0);

    [loop]
    for (uint seg = 0u; seg < MAX_TRANSPARENT_SEGMENTS; ++seg)
    {
        HitInfo segPayload;
        segPayload.color           = float3(0, 0, 0);
        segPayload.hitT            = -1.0f;
        segPayload.normalWorld     = float3(0, 0, 0);
        segPayload.instanceId      = 0xFFFFFFFFu;
        segPayload.baseColor       = float3(0, 0, 0);
        segPayload.materialId      = 0xFFFFFFFFu;
        segPayload.worldHit        = float3(0, 0, 0);
        segPayload.elementId       = 0xFFFFFFFFu;
        segPayload.recursionDepth  = 0;   // M29 / V2.B.1 — primary depth
        segPayload.transmittance   = float3(0, 0, 0);

        TraceRay(gTlas,
                 RAY_FLAG_NONE,
                 0xFFu,
                 /*RayContributionToHitGroupIndex*/ 0,
                 /*MultiplierForGeometryContributionToHitGroupIndex*/ 0,
                 /*MissShaderIndex*/ 0,
                 segRay,
                 segPayload);

        if (seg == 0u) firstHit = segPayload;

        radiance += throughput * segPayload.color;

        // Miss (hitT < 0): background/envmap is already in segPayload.color
        // and accumulated above; nothing lies behind it.
        if (segPayload.hitT < 0.0f) break;

        // Opaque (or near-opaque) surface: no light from behind.
        const float3 t = segPayload.transmittance;
        if (max(max(t.x, t.y), t.z) < 1e-3f) break;

        throughput *= t;

        // Advance just past this hit along the same direction and
        // re-trace for what lies behind it.
        const float3 hitPos = segRay.Origin + segRay.Direction * segPayload.hitT;
        segRay.Origin = hitPos + segRay.Direction * 1e-3f;
        segRay.TMin   = 1e-3f;
    }

    // Reassemble the payload the downstream AOV / picker / display code
    // expects: structural channels from the frontmost hit, colour from
    // the composited transparent stack.
    HitInfo payload = firstHit;
    payload.color = radiance;

    // ----- Raw AOVs -----------------------------------------------
    // Written every frame regardless of debugViewMode so the AOV
    // inspector + pixel picker + Save AOV can read RAW data — not
    // the post-tonemap remap. The TonemapPass (tonemap.slang) picks
    // which one feeds the BGRA8 display output.
    gOutColorHdr[launchIndex]   = float4(payload.color, 1.0f);
    gOutNormal[launchIndex]     = float4(payload.normalWorld, 1.0f);
    gOutDepth[launchIndex]      = (payload.hitT > 0.0f) ? payload.hitT : 0.0f;
    gOutPrimId[launchIndex]     = payload.instanceId;
    gOutMaterialId[launchIndex] = payload.materialId;
    gOutBaseColor[launchIndex]  = float4(payload.baseColor, 1.0f);
    gOutWorldPos[launchIndex]   = float4(payload.worldHit, 1.0f);

    // Tier 1 Hydra-canonical AOVs. Eye-space normal + position need
    // the FORWARD viewFromWorld matrix — direction-only for the
    // normal (w=0 so translation column drops), full affine for the
    // position. Camera matrices are orthonormal rotation + translation
    // (no scale / skew) so a unit world normal stays unit in eye space.
    const bool didHit  = (payload.hitT > 0.0f);
    const float3 nEye  = mul((float3x3)gCamera.viewFromWorld, payload.normalWorld);
    const float3 pEye  = mul(gCamera.viewFromWorld, float4(payload.worldHit, 1.0f)).xyz;
    gOutAlpha[launchIndex]       = didHit ? 1.0f : 0.0f;
    gOutElementId[launchIndex]   = payload.elementId;
    gOutNormalEye[launchIndex]   = float4(nEye, 1.0f);
    gOutWorldPosEye[launchIndex] = float4(pEye, 1.0f);

    // ----- Linear radiance (TonemapPass input) --------------------
    // P3 pass split: the display transform moved to tonemap.slang.
    // Write the composited radiance at FULL fp32 precision so the
    // downstream exposure + ACES sees bit-identical values to the
    // in-kernel payload.color the old inline display branch read
    // (gOutColorHdr above is RGBA16F — quantized, never used for
    // display).
    gLinearColor[launchIndex] = float4(payload.color, 1.0f);

    // ----- Pixel picker -------------------------------------------
    // Write the raw values into the 1-element pick buffer when this
    // dispatched pixel matches the cursor. The viewer kicks a copy
    // into a staging buffer per frame and reads back next frame.
    if (launchIndex.x == gFrameUi.mousePixelX && launchIndex.y == gFrameUi.mousePixelY)
    {
        PickResult p;
        p.colorR       = payload.color.x;
        p.colorG       = payload.color.y;
        p.colorB       = payload.color.z;
        p.depth        = payload.hitT;
        p.normalX      = payload.normalWorld.x;
        p.normalY      = payload.normalWorld.y;
        p.normalZ      = payload.normalWorld.z;
        p.instanceId   = payload.instanceId;
        p.baseColorR   = payload.baseColor.x;
        p.baseColorG   = payload.baseColor.y;
        p.baseColorB   = payload.baseColor.z;
        p.materialId   = payload.materialId;
        p.worldHitX    = payload.worldHit.x;
        p.worldHitY    = payload.worldHit.y;
        p.worldHitZ    = payload.worldHit.z;
        p._pickPad0    = 0u;
        p.pixelX       = launchIndex.x;
        p.pixelY       = launchIndex.y;
        p._pickPad1    = 0u;
        p._pickPad2    = 0u;
        // Tier 1 Hydra-canonical channels — same eye-space transforms +
        // alpha gate the AOV writes above used, just packed into the
        // pick-buffer rows for the editor's hover readout.
        p.normalEyeX   = nEye.x;
        p.normalEyeY   = nEye.y;
        p.normalEyeZ   = nEye.z;
        p.elementId    = payload.elementId;
        p.worldPosEyeX = pEye.x;
        p.worldPosEyeY = pEye.y;
        p.worldPosEyeZ = pEye.z;
        p.alpha        = didHit ? 1.0f : 0.0f;
        gPickResult[0] = p;
    }
}
