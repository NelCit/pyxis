// Pyxis renderer — NrdProvider's own G-buffer -> NRD input-encoding pack
// pre-pass (RTX-alignment 2026-07-10, NRD Stage 2 — per-frame evaluation
// path). ONE shader file, ONE entry point (PackMain), dispatched by
// NrdProvider::DispatchPack immediately before the RELAX_DIFFUSE_SPECULAR
// dispatch chain (see NrdProvider.cpp's Evaluate()).
//
// Converts Pyxis's OWN unpacked G-buffer guides into the encodings NRD's
// compiled-in shaders expect. The semantics below follow NRD's public
// front-end contract (nrd-src/Shaders/Include/NRD.hlsli's
// NRD_FrontEnd_PackNormalAndRoughness / RELAX_FrontEnd_PackRadianceAndHitDist,
// read for their documented behaviour) but every line here is this
// project's OWN clean-room reimplementation — NRD is a proprietary-
// licensed fetched dependency (see NrdProvider.h's file comment) and is
// never included or copied from, only cited.
//
//   1. IN_NORMAL_ROUGHNESS: gNormalRoughness (RGBA16F world-space normal,
//      components in [-1,1], + linear roughness in .w —
//      raytraced_gbuffer.slang) -> RGBA16_UNORM "best fit" unorm-packed
//      normal + roughness passthrough. NRD_NORMAL_ENCODING is compiled as
//      RGBA16_UNORM for this project (_cmake/Thirdparty.cmake overrides
//      NRD's own CMakeLists.txt default of R10G10B10A2_UNORM) and
//      NRD_ROUGHNESS_ENCODING stays at NRD's own LINEAR default (no
//      override needed) — see NrdProvider.cpp's ResizePackedTextures() for
//      the runtime double-check against nrd::GetLibraryDesc().
//   2. IN_DIFF_RADIANCE_HITDIST / IN_SPEC_RADIANCE_HITDIST: the raw
//      radiance+hitT signal passes already write (indirect_diffuse.slang's
//      gIndirectDiffuse, reflections.slang's gReflections — both RGBA16F,
//      .rgb radiance + .a hit distance / sentinel) -> sanitized
//      float4(radiance, hitDist). RELAX's own front-end contract does NOT
//      normalize hit distance the way REBLUR does, so this is a direct
//      (sanitized) passthrough, not a re-derivation.
//
// NOT handled here (out of this shader's scope — see NrdProvider.h's
// FrameInputs doc comments for the full citations):
//   - IN_MV is bound directly (no packing) — the sign/scale conversion
//     from Pyxis's own pixel-space motionVector convention happens via
//     nrd::CommonSettings::motionVectorScale in NrdProvider.cpp's
//     BuildCommonSettings(), not a texture rewrite.
//   - IN_VIEWZ is bound directly from gViewZ. KNOWN GAP: sky/miss pixels
//     read as viewZ == 0 ("at the camera") rather than NRD's expected
//     ">= denoisingRange" sentinel for "exclude from denoising" — flagged
//     in NrdProvider.h, not fixed here (raytraced_gbuffer.slang is out of
//     this stage's scope).
//
// Bindings — single Set 0 (this shader owns its own layout; NRD's SPIR-V
// binding-offset flattening, described in NrdProvider.h's file comment,
// only applies to NRD's OWN compiled shaders, not this one):
//   0 : Texture2D<float4>              gNormalRoughnessIn        (SRV)
//   1 : Texture2D<float4>              gDiffuseRadianceHitDistIn (SRV)
//   2 : Texture2D<float4>              gSpecRadianceHitDistIn    (SRV)
//   3 : RWTexture2D<float4>            gNormalRoughnessOut       (UAV, RGBA16_UNORM)
//   4 : RWTexture2D<float4>            gDiffRadianceHitDistOut   (UAV, RGBA16F)
//   5 : RWTexture2D<float4>            gSpecRadianceHitDistOut   (UAV, RGBA16F)
//   6 : ConstantBuffer<NrdPackParams>  gParams

#include "ShaderInterop.slang"

// Hand-rolled (NOT a ShaderInterop.slang PYXIS_INTEROP_STRUCT — this
// stage's scope keeps that shared file untouched) cbuffer layout; kept in
// exact byte-for-byte lockstep with NrdProvider.cpp's own local
// `struct NrdPackParams`. 16 bytes — satisfies the project's default
// cbuffer alignment rule (§10/§23) by hand since the shared macro isn't
// doing it for us here.
struct NrdPackParams
{
    uint  width;
    uint  height;
    float _reserved0;
    float _reserved1;
};

[[vk::binding(0, 0)]] Texture2D<float4>   gNormalRoughnessIn;
[[vk::binding(1, 0)]] Texture2D<float4>   gDiffuseRadianceHitDistIn;
[[vk::binding(2, 0)]] Texture2D<float4>   gSpecRadianceHitDistIn;
[[vk::binding(3, 0)]] RWTexture2D<float4> gNormalRoughnessOut;
[[vk::binding(4, 0)]] RWTexture2D<float4> gDiffRadianceHitDistOut;
[[vk::binding(5, 0)]] RWTexture2D<float4> gSpecRadianceHitDistOut;
[[vk::binding(6, 0)]] ConstantBuffer<NrdPackParams> gParams;
// viewZ MISS-SENTINEL REMAP (input-gap fix, 2026-07-10): NRD expects
// sky/miss pixels at viewZ >= CommonSettings::denoisingRange (500000 —
// BuildCommonSettings) so they're excluded from denoising; Pyxis's gViewZ
// writes 0 on miss ("at the camera"), which corrupted RELAX's
// disocclusion/plane tests around every skyline edge. Remapped here
// (NRD-only) rather than at the G-buffer source, which the builtin
// denoiser chain + DLSS depth-convert consume with their own 0-on-miss
// conventions.
[[vk::binding(7, 0)]] Texture2D<float>    gViewZIn;
[[vk::binding(8, 0)]] RWTexture2D<float>  gViewZOut;

// "Best fit" unorm normal packing — the non-oct-packed encoding branch
// NRD's front-end pack function documents for RGBA8_UNORM/RGBA16_UNORM
// (NRD.hlsli): normalizing by the largest-magnitude component before the
// *0.5+0.5 remap spreads all three unorm channels across more of their
// available range than a raw unit-vector component would (a unit vector
// rarely has any single axis reach +-1), reducing banding versus a plain
// remap. Degenerate input (all-zero — e.g. sky/miss G-buffer pixels,
// raytraced_gbuffer.slang's miss branch writes gNormalRoughness = 0) falls
// back to a flat (0.5, 0.5, 0.5) encoding via the max(...,epsilon) guard
// rather than dividing by zero.
float3 PackBestFitNormal(float3 worldNormal)
{
    const float epsilon = 1e-8;
    const float maxComponent = max(abs(worldNormal.x), max(abs(worldNormal.y), abs(worldNormal.z)));
    worldNormal /= max(maxComponent, epsilon);
    return worldNormal * 0.5 + 0.5;
}

// RELAX's front-end radiance+hit-distance sanitize contract (NRD.hlsli
// documents this as the `sanitize = true` path of its own pack function):
// zero out NaN/Inf radiance, clamp hit distance to a valid non-negative
// range. reflections.slang's "-1 on miss/rough-fallback" sentinel clamps
// to 0 here (read by RELAX as "hit distance unknown / very close") rather
// than something more deliberate — an accepted simplification for this
// dormant stage, not a full miss-aware hit-distance design (see
// NrdProvider.h's FrameInputs doc comment for the follow-up note).
float4 SanitizeRadianceHitDist(float4 radianceHitDist)
{
    const bool radianceInvalid = any(isnan(radianceHitDist.rgb)) || any(isinf(radianceHitDist.rgb));
    const float3 radiance = radianceInvalid ? float3(0.0, 0.0, 0.0) : max(radianceHitDist.rgb, 0.0);

    const bool hitDistInvalid = isnan(radianceHitDist.a) || isinf(radianceHitDist.a);
    const float hitDist = hitDistInvalid ? 0.0 : max(radianceHitDist.a, 0.0);

    return float4(radiance, hitDist);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void PackMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (dispatchThreadId.x >= gParams.width || dispatchThreadId.y >= gParams.height)
        return;

    const uint2 pixel = dispatchThreadId.xy;

    const float4 normalRoughness = gNormalRoughnessIn[pixel];
    const float3 packedNormal = PackBestFitNormal(normalRoughness.xyz);
    // Roughness passthrough — NRD_ROUGHNESS_ENCODING stays at NRD's own
    // LINEAR default (no _cmake override needed), so no sqrt/square
    // transform is applied here; saturate() only guards against an
    // out-of-[0,1] upstream value.
    gNormalRoughnessOut[pixel] = float4(packedNormal, saturate(normalRoughness.w));

    gDiffRadianceHitDistOut[pixel] = SanitizeRadianceHitDist(gDiffuseRadianceHitDistIn[pixel]);
    gSpecRadianceHitDistOut[pixel] = SanitizeRadianceHitDist(gSpecRadianceHitDistIn[pixel]);

    // viewZ miss-sentinel remap (see gViewZIn's binding comment): 1e6 is
    // safely past BuildCommonSettings' denoisingRange (500000), NRD's
    // "exclude this pixel from denoising" contract for sky.
    const float viewZ = gViewZIn[pixel];
    gViewZOut[pixel] = (viewZ <= 0.0) ? 1.0e6 : viewZ;
}
