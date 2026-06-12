// Pyxis renderer — lighting-pass ray-generation entry point (P4 pass
// split, design D1).
//
// The deferred first-hit shader: reads the VisibilityGpu record the
// RaytracedGBufferPass produced for this pixel and reconstructs every
// input the pre-split primary closesthit consumed —
//   * the camera ray via the shared BuildCameraRay (camera_ray.slang,
//     bit-identical to the ray the GBuffer raygen traced),
//   * worldHit = origin + hitT·dir (the identical expression the old
//     transparency loop used for its advance),
//   * the object→world transform from gInstanceInfo (P2 guaranteed the
//     packed floats match the TLAS instance desc, hence
//     ObjectToWorld3x4()),
// then runs the SAME ShadeSurfaceHit the megakernel closesthit calls
// (shading.slang) at recursionDepth 0 — its shadow / AO / reflection
// rays TraceRay from this raygen with the identical flags, origins,
// directions, TMax and miss indices as before the split.
//
// Transparency: continues the pre-split front-to-back composite from
// SEGMENT 1 (the GBuffer ray was segment 0) with the full HitInfo
// payload at recursionDepth 0 — same advance epsilon (1e-3), same
// break thresholds, same 16-segment cap INCLUDING the first.
// Primary-miss pixels take the dome background from the shared
// SampleDomeBackground (dome_sample.slang) — the same function the
// lighting miss shader calls for secondary rays.
//
// AOVs / picker / linear radiance write exactly as the pre-split
// raygen did: structural channels from the frontmost hit, colour from
// the composited transparent stack, full-precision picker latch, fp32
// gLinearColor for the TonemapPass (P3).
//
// Bindings — match pyxis::RaytracedLightingPass's NVRHI BindingLayout.
// The scene-side tables are declared by the shared modules
// (camera_ray / dome_sample / shading); this file declares the
// raygen-only outputs:
//   set=0, binding=2  : RWTexture2D<float4> fp32 linear radiance
//   set=0, binding=11..18, 20..23 : raw AOV UAVs
//   set=0, binding=15 : RWStructuredBuffer<PickResult> pick latch
//   set=0, binding=19 : FrameUiUniforms cbuffer
//   set=0, binding=34 : StructuredBuffer<VisibilityGpu> (GBuffer pass output)

#include "ShaderInterop.slang"
#include "camera_ray.slang"   // gCamera (0) + BuildCameraRay
#include "shading.slang"      // gTlas (1) + scene tables + ShadeSurfaceHit; pulls dome_sample.slang

// P3 pass split — full-precision LINEAR radiance (RGBA32F, renderer-internal
// scratch). The TonemapPass reads this and applies the display transform into
// the BGRA8 display target; fp32 keeps the display path bit-identical to the
// in-kernel payload.color the old inline branch consumed.
[[vk::binding(2, 0)]] RWTexture2D<float4>             gLinearColor;        // RGBA32F radiance
// AOV outputs (M7 follow-up). Raygen writes all of them every frame from
// the same first-hit data; the inspector picks which one the display
// path remaps. Storage-capable formats only — RGBA16F for color/normal,
// R32F for depth, R32_UINT for instance id. Bindings keep their
// pre-split numbers.
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
// one UAV per channel, written every frame from the same first-hit
// data (no second trace).
[[vk::binding(20, 0)]] RWTexture2D<float>             gOutAlpha;           // R8_UNORM 1.0 hit / 0.0 miss
[[vk::binding(21, 0)]] RWTexture2D<uint>              gOutElementId;       // R32U  per-face id (~0u miss)
[[vk::binding(22, 0)]] RWTexture2D<float4>            gOutNormalEye;       // RGBA16F eye-space normal
[[vk::binding(23, 0)]] RWTexture2D<float4>            gOutWorldPosEye;     // RGBA32F eye-space hit pos
// P4 — the visibility buffer the RaytracedGBufferPass wrote this frame
// (one fp32-exact VisibilityGpu record per pixel, row-major).
[[vk::binding(34, 0)]] StructuredBuffer<VisibilityGpu> gVisibility;

// P5 (design D2) — cap of the front-to-back transparency composite in
// main() below (per-segment LOOP iterations at recursion depth 0, not
// hardware recursion). Hoisted from a main() local to module scope as
// a specialization constant; same value (16), now C++-fed
// (RaytracedLightingPass::MAX_TRANSPARENT_SEGMENTS).
[[vk::constant_id(SPEC_ID_MAX_TRANSPARENT_SEGMENTS)]] const uint MAX_TRANSPARENT_SEGMENTS = 16u;

// Slang emits the SPIR-V `OpEntryPoint` name as `"main"` regardless
// of the source-side function name (verified via spirv-dis); the C++
// side passes `"main"` to NVRHI's createShader so we keep the function
// name aligned to avoid the VUID-VkPipelineShaderStageCreateInfo-
// pName-00707 mismatch.
[shader("raygeneration")]
void main() {
    const uint2 launchIndex = DispatchRaysIndex().xy;
    const uint2 launchDim   = DispatchRaysDimensions().xy;

    // The camera primary ray — bit-identical to the one the GBuffer
    // raygen traced (same shared function, same gCamera bits), so
    // `origin + hitT·dir` reconstructs the exact hit point.
    float3 worldOriginXyz;
    float3 worldDirXyz;
    BuildCameraRay(launchIndex, launchDim, worldOriginXyz, worldDirXyz);

    const VisibilityGpu vis = gVisibility[launchIndex.y * launchDim.x + launchIndex.x];

    // V2.B transmission loop state. Opacity see-through is composited
    // FRONT-TO-BACK as an iterative sequence of primary-depth
    // TraceRays — NOT a recursive continuation ray inside closesthit.
    // Each iteration re-traces from just past the previous hit at
    // recursion depth 0, so an arbitrary stack of transparent surfaces
    // costs cheap loop iterations instead of hardware recursion depth
    // (capped at 3 per plan_for_v2.md §V2.B). Segment 0 is the GBuffer
    // ray (deferred-shaded below); the loop continues from segment 1
    // with today's exact arithmetic.
    //
    // The FIRST hit feeds every AOV (frontmost surface — unchanged
    // semantics); only the displayed/HDR colour reflects the
    // composited stack. The loop is deterministic (no RNG, fixed
    // iteration order, fixed cap) so the headless byte-identical-EXR
    // contract (§33.7) holds. RAY_FLAG_NONE + instanceMask 0xFF matches
    // GpuScene's TLAS so every instance is visible to the primary ray.
    // MAX_TRANSPARENT_SEGMENTS is the P5 module-scope specialization
    // constant (declared above; value unchanged at 16).
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

    if (vis.hitT < 0.0f)
    {
        // Primary miss — the background the old miss shader produced
        // for segment 0, via the shared dome function (same bits: the
        // miss shader saw WorldRayDirection() = the BuildCameraRay
        // direction). The remaining firstHit fields keep the miss
        // sentinels initialised above — exactly what miss.slang wrote.
        firstHit.color = SampleDomeBackground(worldDirXyz);
        radiance += throughput * firstHit.color;
        // Nothing lies behind the background — no continuation.
    }
    else
    {
        // Primary hit — deferred-shade it with the SAME function the
        // megakernel closesthit calls for secondary rays, at
        // recursionDepth 0 (the depth the primary closesthit ran at).
        // worldHit reconstruction uses the identical expression the
        // old loop's advance used (segRay.Origin + segRay.Direction *
        // segPayload.hitT).
        const float3 worldHit = worldOriginXyz + worldDirXyz * vis.hitT;
        const InstanceInfoGpu inst = gInstanceInfo[vis.instanceId];
        // Raygen has no WorldToObject3x4() intrinsic — invert the
        // packed object→world affine. Only MATERIAL_FLAG_OBJECT_
        // PROJECTION materials read it (see shading.slang's note on
        // the ULP-level divergence vs the driver-provided inverse).
        const float3x4 worldToObject = InvertAffine3x4(inst.objectToWorld);
        const SurfaceShadeResult shade = ShadeSurfaceHit(
            vis.instanceId, vis.primitiveIndex, float2(vis.baryX, vis.baryY),
            worldHit, worldDirXyz, vis.hitT,
            inst.objectToWorld, worldToObject,
            /*recursionDepth*/ 0u);

        // Assemble the segment-0 payload exactly as the megakernel
        // closesthit would have filled it.
        firstHit.color          = shade.emitted;
        firstHit.hitT           = vis.hitT;
        firstHit.normalWorld    = shade.normalWorld;
        firstHit.instanceId     = vis.instanceId;
        firstHit.baseColor      = shade.baseColor;
        firstHit.materialId     = shade.materialId;
        firstHit.worldHit       = worldHit;
        firstHit.elementId      = vis.primitiveIndex;
        firstHit.transmittance  = shade.transmittance;

        radiance += throughput * firstHit.color;

        // Opaque (or near-opaque) surface: no light from behind —
        // same break the old loop took after segment 0 (the negated
        // form keeps the exact comparison, NaN semantics included).
        const float3 t0 = firstHit.transmittance;
        if (!(max(max(t0.x, t0.y), t0.z) < 1e-3f))
        {
            throughput *= t0;

            // Advance just past the first hit along the same direction
            // and continue the front-to-back composite from segment 1
            // — today's loop arithmetic exactly (seg 0 was the primary).
            RayDesc segRay;
            segRay.Origin    = worldHit + worldDirXyz * 1e-3f;
            segRay.Direction = worldDirXyz;
            segRay.TMin      = 1e-3f;
            segRay.TMax      = 10000.0f;

            [loop]
            for (uint seg = 1u; seg < MAX_TRANSPARENT_SEGMENTS; ++seg)
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

                radiance += throughput * segPayload.color;

                // Miss (hitT < 0): background/envmap is already in
                // segPayload.color and accumulated above; nothing lies
                // behind it.
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
        }
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
