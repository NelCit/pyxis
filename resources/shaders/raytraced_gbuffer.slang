// Pyxis renderer — extended G-buffer pass, ONE shader file (RTX-alignment
// design, rtx-realtime-alignment-design.md, WP2-core).
//
// Replaces the P4-era thin visibility-only
// raytraced_gbuffer_{raygen,closesthit,miss,anyhit}.slang quartet. Per the
// design's "Full shader-rewrite mandate" (owner note, 2026-07-05) every RT
// pass now carries ALL its entry points inside one file; slangc compiles
// this SAME source four times (once per `-entry` name) via four
// `pyxis_compile_slang_shader` blocks in CMakeLists.txt.
//
// The raygen still traces exactly ONE thin-payload primary ray per pixel
// (VisibilityPayload, unchanged 20-byte shape — no fat HitInfo here; this
// pipeline shades nothing via the hit-shader stages, same as the pre-WP2
// design). What's new: the raygen now ALSO evaluates the primary hit's
// material ONCE, via `EvaluateSurfaceMaterial` (shading.slang) — the SAME
// function RaytracedLightingPass's ShadeSurfaceHit calls for its own
// (redundant, in this WP) primary-hit re-evaluation. Reusing the function
// (not re-deriving the math) is what keeps the two passes' primary-hit
// material values bit-identical wherever their inputs agree (see the
// packed-visibility note below for where they deliberately don't, yet).
//
// Writes, every frame, from the same primary-hit data:
//   * the packed 16 B VisibilityGpu record (Set 1 binding 0) — hitT /
//     instanceId / primitiveIndex stay fp32-exact; the two barycentric
//     floats are packed to f16 (`baryPacked`). RaytracedLightingPass reads
//     this back and reconstructs SLIGHTLY different (sub-golden-tolerance)
//     UV/normal inputs than what THIS raygen used (fp32-exact, straight
//     off the TraceRay payload) — an intentional, documented WP2 rewrite
//     tradeoff (rtx-realtime-alignment-design.md), absorbed by the Phase A
//     golden rebake.
//   * the denoiser-guide G-buffer: gAlbedo / gNormalRoughness / gEmissive
//     (Set 1 bindings 1-3) — new in WP2, unconsumed by any pass yet
//     (Phase B signal passes are out of this WP's scope).
//   * gSpecularAlbedo (Set 1 binding 17, DLSS Stage 2b) — the RR guide
//     buffer ProgrammingGuideDLSS_RR.md §4.1.2/§4.2.1 calls
//     `kBufferTypeSpecularAlbedo`: material F0 (view/roughness-independent
//     specular tint) run through the EnvBRDFApprox2 environment-BRDF fit
//     (§4.2.1) at THIS pixel's NdotV + roughness². Written UNCONDITIONALLY
//     (every frame, every pixel) rather than gated on DLSS being active:
//     `surf.pbr` is already fully evaluated for gAlbedo/gNormalRoughness
//     above, so the incremental cost is a dozen FMAs + one more UAV write,
//     and gating it would need a spec-constant pipeline variant (like
//     PROJECTION_MODE) for a per-pixel cost this small — not worth the
//     extra variant. Consumed only by DlssPass's Ray-Reconstruction path
//     (Private/Passes/DlssPass.cpp); every other pass ignores it.
//   * gViewZ / gMotionVector (Set 1 bindings 4-5) — MOVED here from
//     RaytracedLightingPass's raygen (RTX-alignment WP1); same cameraStatic
//     gate, same reprojection arithmetic, unchanged semantics.
//   * every id/geometry AOV + the pixel-picker latch that used to be
//     written by the lighting raygen (Set 1 bindings 6-16) — MOVED here
//     per the WP2 contract ("id/geometry AOVs + pick buffer... move
//     ownership to the gbuffer raygen"). `alpha` and `colorHdr` stay with
//     RaytracedLightingPass (only it knows the transparency-composited
//     coverage / radiance). KNOWN WP2-core DEVIATION: the pick latch's
//     colorR/G/B fields — which pre-WP2 carried the FULL lit+composited
//     radiance — now carry this pass's unlit `baseColor` (the gbuffer
//     raygen has no lighting result to report). Alpha still reflects
//     didHit. This is a foundation-phase simplification, not a bug: the
//     picker is an interactive debug readout, not a golden-tested AOV: the
//     lit color latch is a Phase-B/CompositePass follow-up once a pass
//     downstream of lighting owns the final radiance.
//
// Bindings — Set 0 (SceneBindings, shared verbatim with RaytracedLightingPass;
// declared by the shared modules this file includes):
//   set=0, binding=0  : CameraUniforms cbuffer (camera_ray.slang)
//   set=0, binding=1  : RaytracingAccelerationStructure (shading.slang)
//   set=0, binding=2  : StructuredBuffer<OpenPBRMaterialGPU> (raytraced_anyhit.slang)
//   set=0, binding=3  : StructuredBuffer<InstanceInfoGpu> (raytraced_anyhit.slang)
//   set=0, binding=4  : StructuredBuffer<MeshInfoGpu> (shading.slang)
//   set=0, binding=5  : StructuredBuffer<LightGpu> (dome_sample.slang; unused here,
//                        pulled in transitively — EvaluateSurfaceMaterial doesn't light)
//   set=0, binding=6  : StructuredBuffer<uint> mesh indices (shading.slang)
//   set=0, binding=7  : StructuredBuffer<float2> mesh UVs (shading.slang)
//   set=0, binding=8  : StructuredBuffer<VertexAttribGpu> (shading.slang)
//   set=0, binding=9  : StructuredBuffer<float4> mesh face normals (shading.slang)
//   set=0, binding=10 : Texture2D dome env map (dome_sample.slang; unused here)
//   set=0, binding=11 : SamplerState dome sampler (dome_sample.slang; unused here)
//   set=0, binding=12 : SamplerState material sampler (shading.slang)
//   set=0, binding=13 : Texture2D bindless textures[4096] (shading.slang)
//   set=0, binding=14 : MotionVectorCameraUniforms cbuffer (declared HERE — the
//                        one pass that reads it since the motionVector write moved)
// Set 1 (this pass's own I/O — declared HERE):
//   set=1, binding=0  : RWStructuredBuffer<VisibilityGpu> (packed 16 B)
//   set=1, binding=1  : RWTexture2D<float4> gAlbedo          RGBA16F
//   set=1, binding=2  : RWTexture2D<float4> gNormalRoughness RGBA16F (xyz normal, w roughness)
//   set=1, binding=3  : RWTexture2D<float4> gEmissive        RGBA16F
//   set=1, binding=4  : RWTexture2D<float>  gViewZ           R32F
//   set=1, binding=5  : RWTexture2D<float2> gMotionVector    RG16F
//   set=1, binding=17 : RWTexture2D<float4> gSpecularAlbedo  RGBA16F (DLSS Stage 2b, RR guide)
//   set=1, binding=6  : RWTexture2D<float4> gOutNormal       RGBA16F
//   set=1, binding=7  : RWTexture2D<float>  gOutDepth        R32F
//   set=1, binding=8  : RWTexture2D<uint>   gOutPrimId       R32U
//   set=1, binding=9  : RWTexture2D<uint>   gOutMaterialId   R32U
//   set=1, binding=10 : RWTexture2D<float4> gOutBaseColor    RGBA16F
//   set=1, binding=11 : RWTexture2D<float4> gOutWorldPos     RGBA32F
//   set=1, binding=12 : RWTexture2D<uint>   gOutElementId    R32U
//   set=1, binding=13 : RWTexture2D<float4> gOutNormalEye    RGBA16F
//   set=1, binding=14 : RWTexture2D<float4> gOutWorldPosEye  RGBA32F
//   set=1, binding=15 : RWStructuredBuffer<PickResult> gPickResult
//   set=1, binding=16 : ConstantBuffer<FrameUiUniforms> gFrameUi
//   set=1, binding=17 : RWTexture2D<float4> gSpecularAlbedo RGBA16F (DLSS Stage 2b, RR guide)

#include "ShaderInterop.slang"
#include "camera_ray.slang"  // gCamera (0) + BuildCameraRay
#include "shading.slang"     // gTlas (1) + full Set-0 scene tables + EvaluateSurfaceMaterial

[[vk::binding(0, 1)]] RWStructuredBuffer<VisibilityGpu> gVisibility;
[[vk::binding(1, 1)]] RWTexture2D<float4>               gAlbedo;
[[vk::binding(2, 1)]] RWTexture2D<float4>               gNormalRoughness;
[[vk::binding(3, 1)]] RWTexture2D<float4>               gEmissive;
[[vk::binding(4, 1)]] RWTexture2D<float>                gViewZ;
[[vk::binding(5, 1)]] RWTexture2D<float2>               gMotionVector;
[[vk::binding(6, 1)]] RWTexture2D<float4>               gOutNormal;
[[vk::binding(7, 1)]] RWTexture2D<float>                gOutDepth;
[[vk::binding(8, 1)]] RWTexture2D<uint>                 gOutPrimId;
[[vk::binding(9, 1)]] RWTexture2D<uint>                 gOutMaterialId;
[[vk::binding(10, 1)]] RWTexture2D<float4>              gOutBaseColor;
[[vk::binding(11, 1)]] RWTexture2D<float4>              gOutWorldPos;
[[vk::binding(12, 1)]] RWTexture2D<uint>                gOutElementId;
[[vk::binding(13, 1)]] RWTexture2D<float4>              gOutNormalEye;
[[vk::binding(14, 1)]] RWTexture2D<float4>              gOutWorldPosEye;
[[vk::binding(15, 1)]] RWStructuredBuffer<PickResult>   gPickResult;
[[vk::binding(16, 1)]] ConstantBuffer<FrameUiUniforms>  gFrameUi;
[[vk::binding(17, 1)]] RWTexture2D<float4>              gSpecularAlbedo;

// Set 0, binding 14 — the previous frame's world→clip transform + the
// CPU-computed "did the camera move" gate (RTX-alignment WP1, moved here
// from RaytracedLightingPass's own binding 8 in the pre-WP2 flat layout).
// See MotionVectorCameraUniforms' doc comment in ShaderInterop.slang for
// the full contract; SceneBindings (Private/Passes/SceneBindings.{h,cpp})
// owns the upload + the bit-exact history tracking now (moved out of
// RaytracedLightingPass, since only this raygen writes motionVector).
[[vk::binding(14, 0)]] ConstantBuffer<MotionVectorCameraUniforms> gMotionVectorCamera;

// P5 (design D2) — cap of the front-to-back transparency composite; this
// pipeline traces ONE primary ray only (maxRecursionDepth = 1), so unlike
// the lighting raygen there is no transparency loop here to cap.

// --------------------------------------------------------------------
// DLSS Stage 2b — Ray Reconstruction's specular-albedo guide buffer.
// Transcribed verbatim from ProgrammingGuideDLSS_RR.md §4.2.1 ("Specular
// Albedo Generation", citing Ray Tracing Gems, Chapter 32 — the
// split-sum environment-BRDF polynomial fit). `SpecularColor` is F0 (the
// material's view/roughness-INDEPENDENT specular reflectance at normal
// incidence); this function itself supplies the NdotV/roughness-dependent
// widening, so passing an already Fresnel-weighted color here would
// double-count the view-angle term.
// --------------------------------------------------------------------
float3 EnvBRDFApprox2(float3 SpecularColor, float alpha, float NoV) {
    NoV = abs(NoV);
    float4 X;
    X.x = 1.0f;
    X.y = NoV;
    X.z = NoV * NoV;
    X.w = NoV * X.z;
    float4 Y;
    Y.x = 1.0f;
    Y.y = alpha;
    Y.z = alpha * alpha;
    Y.w = alpha * Y.z;
    float2x2 M1 = float2x2(0.99044f, -1.28514f, 1.29678f, -0.755907f);
    float3x3 M2 = float3x3(1.0f, 2.92338f, 59.4188f, 20.3225f, -27.0302f, 222.592f, 121.563f,
                           626.13f, 316.627f);
    float2x2 M3 = float2x2(0.0365463f, 3.32707f, 9.0632f, -9.04756f);
    float3x3 M4 = float3x3(1.0f, 3.59685f, -1.36772f, 9.04401f, -16.3174f, 9.22949f, 5.56589f,
                           19.7886f, -20.2123f);
    float bias  = dot(mul(M1, X.xy), Y.xy) * rcp(dot(mul(M2, X.xyw), Y.xyw));
    float scale = dot(mul(M3, X.xy), Y.xy) * rcp(dot(mul(M4, X.xzw), Y.xyw));
    // Hack for specular reflectance of 0 (guide's own comment).
    bias *= saturate(SpecularColor.g * 50.0f);
    return mad(SpecularColor, max(0.0f, scale), max(0.0f, bias));
}

// Material F0 (view-independent specular tint) from the SAME OpenPBR
// closure fields EvaluateSurfaceMaterial already derived for gAlbedo/
// gNormalRoughness above — dielectric F0 = specularColor × the modulated
// normal-incidence reflectance OpenPBRModulatedIor already computes for
// the mirror-reflection weight (openpbr_material.slang; `xiFsOut` IS
// FresnelDielectricExact(1.0, etaPrime), see that function's doc
// comment), metal F0 = OpenPBRConductorFresnel at mu=1 (its Schlick term
// collapses to plain F0 = specularWeight·baseWeight·baseColor there),
// lerped by metalness — the same lerp OpenPBRReflectionWeight uses for
// its own (NdotV- and roughness-weighted) mirror term, evaluated instead
// at the angle-independent mu=1 point since EnvBRDFApprox2 above supplies
// its own NdotV/roughness dependence.
float3 OpenPBRSpecularF0(OpenPBRSurfaceParams p) {
    float xiFs;
    OpenPBRModulatedIor(p.specularIor, p.specularWeight, p.coatIor,
                        OpenPBRFeature(OPENPBR_FEATURE_COAT) ? saturate(p.coatWeight) : 0.0,
                        xiFs);
    const float3 f0Dielectric = saturate(p.specularColor) * xiFs;
    const float3 f0Metal =
        OpenPBRConductorFresnel(1.0, p.baseColor, p.baseWeight, p.specularColor, p.specularWeight);
    return lerp(f0Dielectric, f0Metal, saturate(p.baseMetalness));
}

// --------------------------------------------------------------------
// RayGen — traces the primary ray, evaluates the hit's material ONCE
// (shared EvaluateSurfaceMaterial), and writes every G-buffer / guide /
// id / pick output plus the packed visibility record.
// --------------------------------------------------------------------
[shader("raygeneration")]
void RayGenMain() {
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

    const bool didHit = payload.hitT >= 0.0f;

    if (didHit)
    {
        const float3 worldHit = worldOriginXyz + worldDirXyz * payload.hitT;
        const InstanceInfoGpu inst = gInstanceInfo[payload.instanceId];
        // Raygen has no WorldToObject3x4() intrinsic (only hit shaders do)
        // — invert the packed object→world affine, exactly as the lighting
        // raygen's primary-hit path already does (camera_ray.slang /
        // shading.slang's InvertAffine3x4).
        const float3x4 worldToObject = InvertAffine3x4(inst.objectToWorld);
        const SurfaceMaterialEval surf = EvaluateSurfaceMaterial(
            payload.instanceId, payload.primitiveIndex, payload.bary,
            worldHit, worldDirXyz, payload.hitT,
            inst.objectToWorld, worldToObject);
        const float3 emission = OpenPBREmission(surf.pbr);

        // ----- Material G-buffer (new in WP2; unconsumed until the
        // Phase B signal passes land) -----------------------------------
        gAlbedo[launchIndex]          = float4(surf.baseColor, 1.0f);
        gNormalRoughness[launchIndex] = float4(surf.normalWorld, surf.pbr.specularRoughness);
        gEmissive[launchIndex]        = float4(emission, 1.0f);

        // DLSS Stage 2b — RR's kBufferTypeSpecularAlbedo guide (see the
        // EnvBRDFApprox2 / OpenPBRSpecularF0 helpers above this function).
        {
            const float3 viewDirWorld = -worldDirXyz;  // already-normalized; camera -> hit
            const float  ndotV        = dot(surf.normalWorld, viewDirWorld);
            const float  specAlpha    = max(0.001, surf.pbr.specularRoughness * surf.pbr.specularRoughness);
            const float3 specularF0   = OpenPBRSpecularF0(surf.pbr);
            gSpecularAlbedo[launchIndex] = float4(EnvBRDFApprox2(specularF0, specAlpha, ndotV), 1.0f);
        }

        // ----- id / geometry AOVs (moved here from the lighting raygen) --
        gOutNormal[launchIndex]     = float4(surf.normalWorld, 1.0f);
        gOutDepth[launchIndex]      = payload.hitT;
        gOutPrimId[launchIndex]     = payload.instanceId;
        gOutMaterialId[launchIndex] = surf.materialId;
        gOutBaseColor[launchIndex]  = float4(surf.baseColor, 1.0f);
        gOutWorldPos[launchIndex]   = float4(worldHit, 1.0f);
        gOutElementId[launchIndex]  = payload.primitiveIndex;

        // Eye-space Neye/Peye (Hydra-canonical): direction-only transform
        // for the normal (w=0 drops translation), full affine for the
        // position. Camera matrices are orthonormal rotation + translation
        // (no scale/skew), so a unit world normal stays unit in eye space.
        const float3 nEye = mul((float3x3)gCamera.viewFromWorld, surf.normalWorld);
        const float3 pEye = mul(gCamera.viewFromWorld, float4(worldHit, 1.0f)).xyz;
        gOutNormalEye[launchIndex]   = float4(nEye, 1.0f);
        gOutWorldPosEye[launchIndex] = float4(pEye, 1.0f);

        // ----- Denoiser-guide AOVs (RTX-alignment WP1, moved here) -------
        // viewZ: the view-space camera looks down -Z (camera_ray.slang's
        // BuildCameraRay), so pEye.z is negative in front of the lens;
        // negate for a POSITIVE distance-along-forward value.
        gViewZ[launchIndex] = -pEye.z;

        // motionVector: skip the reprojection arithmetic entirely when the
        // CPU-computed `cameraStatic` gate is set (nothing moved since last
        // frame, or no history yet on frame 0) — see
        // MotionVectorCameraUniforms' doc comment in ShaderInterop.slang.
        float2 motionVectorPixels = float2(0.0f, 0.0f);
        if (gMotionVectorCamera.cameraStatic == 0u)
        {
            const float2 currentPixel = float2(launchIndex) + 0.5f;
            const float4 prevClip = mul(gMotionVectorCamera.prevClipFromWorld,
                                         float4(worldHit, 1.0f));
            const float3 prevNdc = prevClip.xyz / prevClip.w;
            const float2 prevPixel = float2(
                (prevNdc.x * 0.5f + 0.5f) * float(launchDim.x),
                (0.5f - prevNdc.y * 0.5f) * float(launchDim.y));
            motionVectorPixels = currentPixel - prevPixel;
        }
        gMotionVector[launchIndex] = motionVectorPixels;

        // ----- Pixel picker (moved here) ---------------------------------
        // KNOWN WP2-core DEVIATION — see the file header: colorR/G/B carry
        // this pass's unlit baseColor, NOT the final lit radiance (this
        // pass has no lighting result). alpha reflects didHit.
        if (launchIndex.x == gFrameUi.mousePixelX && launchIndex.y == gFrameUi.mousePixelY)
        {
            PickResult p;
            p.colorR       = surf.baseColor.x;
            p.colorG       = surf.baseColor.y;
            p.colorB       = surf.baseColor.z;
            p.depth        = payload.hitT;
            p.normalX      = surf.normalWorld.x;
            p.normalY      = surf.normalWorld.y;
            p.normalZ      = surf.normalWorld.z;
            p.instanceId   = payload.instanceId;
            p.baseColorR   = surf.baseColor.x;
            p.baseColorG   = surf.baseColor.y;
            p.baseColorB   = surf.baseColor.z;
            p.materialId   = surf.materialId;
            p.worldHitX    = worldHit.x;
            p.worldHitY    = worldHit.y;
            p.worldHitZ    = worldHit.z;
            p._pickPad0    = 0u;
            p.pixelX       = launchIndex.x;
            p.pixelY       = launchIndex.y;
            p._pickPad1    = 0u;
            p._pickPad2    = 0u;
            p.normalEyeX   = nEye.x;
            p.normalEyeY   = nEye.y;
            p.normalEyeZ   = nEye.z;
            p.elementId    = payload.primitiveIndex;
            p.worldPosEyeX = pEye.x;
            p.worldPosEyeY = pEye.y;
            p.worldPosEyeZ = pEye.z;
            p.alpha        = 1.0f;
            gPickResult[0] = p;
        }
    }
    else
    {
        gAlbedo[launchIndex]          = float4(0.0f, 0.0f, 0.0f, 1.0f);
        gNormalRoughness[launchIndex] = float4(0.0f, 0.0f, 0.0f, 0.0f);
        gEmissive[launchIndex]        = float4(0.0f, 0.0f, 0.0f, 1.0f);
        gSpecularAlbedo[launchIndex]  = float4(0.0f, 0.0f, 0.0f, 1.0f);

        gOutNormal[launchIndex]     = float4(0.0f, 0.0f, 0.0f, 1.0f);
        gOutDepth[launchIndex]      = 0.0f;
        gOutPrimId[launchIndex]     = 0xFFFFFFFFu;
        gOutMaterialId[launchIndex] = 0xFFFFFFFFu;
        gOutBaseColor[launchIndex]  = float4(0.0f, 0.0f, 0.0f, 1.0f);
        gOutWorldPos[launchIndex]   = float4(0.0f, 0.0f, 0.0f, 1.0f);
        gOutElementId[launchIndex]  = 0xFFFFFFFFu;
        gOutNormalEye[launchIndex]   = float4(0.0f, 0.0f, 0.0f, 1.0f);
        gOutWorldPosEye[launchIndex] = float4(0.0f, 0.0f, 0.0f, 1.0f);

        gViewZ[launchIndex]         = 0.0f;
        gMotionVector[launchIndex]  = float2(0.0f, 0.0f);

        if (launchIndex.x == gFrameUi.mousePixelX && launchIndex.y == gFrameUi.mousePixelY)
        {
            PickResult p;
            p.colorR = 0.0f; p.colorG = 0.0f; p.colorB = 0.0f; p.depth = -1.0f;
            p.normalX = 0.0f; p.normalY = 0.0f; p.normalZ = 0.0f; p.instanceId = 0xFFFFFFFFu;
            p.baseColorR = 0.0f; p.baseColorG = 0.0f; p.baseColorB = 0.0f; p.materialId = 0xFFFFFFFFu;
            p.worldHitX = 0.0f; p.worldHitY = 0.0f; p.worldHitZ = 0.0f; p._pickPad0 = 0u;
            p.pixelX = launchIndex.x; p.pixelY = launchIndex.y; p._pickPad1 = 0u; p._pickPad2 = 0u;
            p.normalEyeX = 0.0f; p.normalEyeY = 0.0f; p.normalEyeZ = 0.0f; p.elementId = 0xFFFFFFFFu;
            p.worldPosEyeX = 0.0f; p.worldPosEyeY = 0.0f; p.worldPosEyeZ = 0.0f; p.alpha = 0.0f;
            gPickResult[0] = p;
        }
    }

    // Packed 16 B record — RTX-alignment WP2 rewrite: hitT / instanceId /
    // primitiveIndex fp32-exact; barycentrics packed to f16 (see
    // ShaderInterop.slang's VisibilityGpu doc comment).
    VisibilityGpu vis;
    vis.hitT           = payload.hitT;
    vis.baryPacked      = f32tof16(payload.bary.x) | (f32tof16(payload.bary.y) << 16u);
    vis.instanceId     = payload.instanceId;
    vis.primitiveIndex = payload.primitiveIndex;
    gVisibility[launchIndex.y * launchDim.x + launchIndex.x] = vis;
}

// --------------------------------------------------------------------
// ClosestHit — thin by design: fills the 20-byte VisibilityPayload from
// the hit intrinsics. No shading runs in this pipeline's hit shaders;
// the raygen above did the material eval directly from ITS OWN payload
// (no visibility-buffer round-trip needed within this same pass).
// --------------------------------------------------------------------
[shader("closesthit")]
void ClosestHitMain(inout VisibilityPayload payload, in BuiltInTriangleIntersectionAttributes attribs) {
    payload.hitT           = RayTCurrent();
    payload.bary           = attribs.barycentrics;
    payload.instanceId     = InstanceID();
    payload.primitiveIndex = PrimitiveIndex();
}

// --------------------------------------------------------------------
// Miss — re-asserts the hitT = -1 miss sentinel the raygen pre-initialised.
// --------------------------------------------------------------------
[shader("miss")]
void MissMain(inout VisibilityPayload payload) {
    payload.hitT = -1.0f;
}

// --------------------------------------------------------------------
// AnyHit — the shared alpha-test decision (raytraced_anyhit.slang,
// pulled in transitively via shading.slang), against this pipeline's
// thin VisibilityPayload. Same IgnoreHit gate as RaytracedLightingPass's
// entry — visibility semantics stay bit-identical across both pipelines
// (§33.7).
// --------------------------------------------------------------------
[shader("anyhit")]
void AnyHitMain(inout VisibilityPayload payload, in BuiltInTriangleIntersectionAttributes attribs) {
    if (AnyHitIsAlphaTested()) {
        IgnoreHit();
    }
    // All other hits — including opacity < 1 and transmission — are
    // accepted; the lighting pass decides how to shade them.
}
