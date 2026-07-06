// Pyxis renderer -- display-transform compute shader (TonemapPass).
//
// P3 of the pass-split design: the 11-way debug-view display branch extracted
// VERBATIM from the retired raygen.slang (exposure 2^stops + Narkowicz ACES on
// the COLOR path; the 10 debug encodings unchanged). Inputs are the fp32
// LINEAR radiance RaytracedLightingPass writes (gLinearColor, RGBA32F --
// bit-identical to the in-kernel payload.color the old inline branch consumed;
// NOT the RGBA16F colorHdr AOV, which would quantize the display path) plus
// the 10 raw debug-view AOV textures. Output is the BGRA8 display target (gOutput) the
// SSAA resolve / sRGB blit / headless writer / Kit export consume exactly as
// before.
//
// didHit is reconstructed from the depth AOV: gOutDepth stores hitT on a hit
// and 0.0 on a miss (raygen writes `(hitT > 0) ? hitT : 0`), so `depth > 0`
// is exactly the old `payload.hitT > 0.0f` -- primary hits always satisfy
// hitT >= TMin (0.001) > 0. The DEBUG_VIEW_* sentinel behaviours match:
// Normal's miss = mid-grey keys off didHit; PrimId's miss (~0u) hashes to
// black inside hashInstanceColor.
//
// Deterministic by construction: pure per-texel function of the inputs, no
// RNG, no neighbourhood reads. Thread per output texel.
//
// RTX-alignment design (rtx-realtime-alignment-design.md), Phase C — the
// COLOR path now dispatches on `gParams.tonemapOperator` (TONEMAP_OPERATOR_*,
// mirroring ovrtx's OmniRtxTonemapAPI enum 1:1) instead of hardcoding ACES,
// and the exposure gain is `exposureScale * 2^effectiveExposure` where
// `exposureScale` is 1.0 (no-op) unless RenderSettings::RealTimeQuality's
// physical-camera exposure model is active (see TonemapPass::Execute).
// Default operator (AcesApproximation) + default exposureScale (1.0)
// reproduce the pre-Phase-C image byte-for-byte.

#include "ShaderInterop.slang"

[[vk::binding(0, 0)]]  Texture2D<float4> gLinearColor;     // fp32 LINEAR radiance (RGBA32F)
// Raw debug-view AOVs (same formats the raygen UAV writes produce).
[[vk::binding(1, 0)]]  Texture2D<float4> gAovNormal;       // RGBA16F world normal
[[vk::binding(2, 0)]]  Texture2D<float>  gAovDepth;        // R32F hitT-or-0
[[vk::binding(3, 0)]]  Texture2D<uint>   gAovPrimId;       // R32U InstanceID (~0u miss)
[[vk::binding(4, 0)]]  Texture2D<uint>   gAovMaterialId;   // R32U material slot (~0u miss)
[[vk::binding(5, 0)]]  Texture2D<float4> gAovBaseColor;    // RGBA16F OpenPBR baseColor
[[vk::binding(6, 0)]]  Texture2D<float4> gAovWorldPos;     // RGBA32F world hit pos
[[vk::binding(7, 0)]]  Texture2D<float>  gAovAlpha;        // R8_UNORM 1.0 hit / 0.0 miss
[[vk::binding(8, 0)]]  Texture2D<uint>   gAovElementId;    // R32U PrimitiveIndex (~0u miss)
[[vk::binding(9, 0)]]  Texture2D<float4> gAovNormalEye;    // RGBA16F eye-space normal
[[vk::binding(10, 0)]] Texture2D<float4> gAovWorldPosEye;  // RGBA32F eye-space hit pos
[[vk::binding(11, 0)]] ConstantBuffer<TonemapUniforms> gParams;
[[vk::binding(12, 0)]] RWTexture2D<float4> gOutput;        // BGRA8 display output
[[vk::binding(13, 0)]] ByteAddressBuffer   gAutoExposureStats;  // see AUTO_EXPOSURE_STATS_* layout

// Narkowicz ACES Filmic -- moved verbatim from raygen.slang (P3 pass
// split). Same constants; the display branch for DEBUG_VIEW_COLOR
// calls it on the exposure-scaled radiance. KEPT BYTE-IDENTICAL for
// TONEMAP_OPERATOR_ACES_APPROXIMATION (Phase C default).
float3 acesFilmic(float3 x) {
    const float a = 2.51;
    const float b = 0.03;
    const float c = 2.43;
    const float d = 0.59;
    const float e = 0.14;
    return saturate((x * (a * x + b)) / (x * (c * x + d) + e));
}

// ---- RTX-alignment design (rtx-realtime-alignment-design.md), Phase C ----
// Tonemap operator set (ovrtx OmniRtxTonemapAPI parity). See
// ShaderInterop.slang's TONEMAP_OPERATOR_* doc comment for the schema
// citation and which sub-parameters are hardcoded vs. runtime-configurable.

// `reinhard` -- simple per-channel Reinhard: x / (1+x). Unbounded input,
// compresses smoothly, asymptotic to 1 as x -> infinity.
float3 reinhardTonemap(float3 x) {
    return x / (1.0 + x);
}

// `modifiedReinhard` -- Reinhard with a white point: values at/above
// `whitePoint` map to (approximately) 1, unlike plain Reinhard's
// asymptotic-only-at-infinity curve. w = MODIFIED_REINHARD_WHITE_POINT
// (ovrtx schema default 4.0).
float3 modifiedReinhardTonemap(float3 x, float whitePoint) {
    const float3 numerator = x * (1.0 + x / (whitePoint * whitePoint));
    return numerator / (1.0 + x);
}

// sRGB EOTF (decode gamma -> linear). Accurate piecewise curve, the exact
// inverse of BlitToSrgbPass's LinearToSrgb -- needed to undo hejiHableAlu's
// baked gamma (see below) before this pass's LINEAR output is re-encoded
// downstream.
float srgbToLinear(float val) {
    return (val <= 0.04045) ? (val / 12.92) : pow((val + 0.055) / 1.055, 2.4);
}
float3 srgbToLinear(float3 val) {
    return float3(srgbToLinear(val.x), srgbToLinear(val.y), srgbToLinear(val.z));
}

// `hejiHableAlu` -- Hejl/Burgess-Dawson filmic ALU curve (Jim Hejl & Richard
// Burgess-Dawson, popularised via John Hable's "Filmic Tonemapping
// Operators"). This curve BAKES an approximate ~2.2 gamma into itself -- it
// was designed to be written straight to an 8-bit sRGB backbuffer with NO
// further gamma correction applied. Pyxis's pipeline always applies its own
// accurate sRGB OETF downstream regardless of which operator ran (see
// BlitToSrgbPass / the headless PNG writer / Kit's HdxColorCorrectionTask),
// so writing this operator's raw output straight through would
// DOUBLE-ENCODE the gamma (crushed shadows, blown highlights, roughly
// gamma-squared instead of gamma-1). Compensated by decoding the baked
// gamma back to linear (srgbToLinear) before returning, so the later
// downstream sRGB re-encode reproduces exactly the gamma-space curve this
// operator intends -- net effect: the pixel that leaves this function, once
// sRGB-re-encoded downstream, equals what a naive (uncompensated) blit of
// this curve's raw output would have shown on a display with NO further
// correction.
float3 hejiHableAluTonemap(float3 x) {
    const float3 xClamped = max(float3(0.0, 0.0, 0.0), x - 0.004);
    const float3 encoded = (xClamped * (6.2 * xClamped + 0.5))
                          / (xClamped * (6.2 * xClamped + 1.7) + 0.06);
    return srgbToLinear(saturate(encoded));
}

// `hableUc2` -- Uncharted 2 filmic curve (John Hable, GDC 2010 "Uncharted 2:
// HDR Lighting"). ovrtx schema constants A=.15 B=.5 C=.1 D=.2 E=.02 F=.3,
// white point 11.2.
float3 uncharted2Partial(float3 x) {
    const float A = 0.15;
    const float B = 0.50;
    const float C = 0.10;
    const float D = 0.20;
    const float E = 0.02;
    const float F = 0.30;
    return ((x * (A * x + C * B) + D * E) / (x * (A * x + B) + D * F)) - E / F;
}
float3 hableUc2Tonemap(float3 x) {
    const float3 curved = uncharted2Partial(x);
    const float3 whiteScale = 1.0 / uncharted2Partial(float3(HABLE_UC2_WHITE_POINT,
                                                              HABLE_UC2_WHITE_POINT,
                                                              HABLE_UC2_WHITE_POINT));
    return curved * whiteScale;
}

// `iray` -- Reinhard-family tonemap with NVIDIA Iray's documented
// burn-highlights / crush-blacks / saturation controls. NO PUBLIC FORMULA
// FOUND for Iray's actual mia_exposure_photographic implementation (checked
// migenius's Iray Programmer's Manual, irayplugins.com's Maya doc, and the
// Daz3D reference guide, 2026-07-05 -- all three describe the three
// parameters QUALITATIVELY, none publish the underlying equation). This is
// our own reconstruction, matching the documented behaviour exactly:
//   burnHighlights (IRAY_BURN_HIGHLIGHTS, schema default 0.7, "per
//     component"): 1.0 = linear transfer (no compression, can clip);
//     0.0 = full per-channel Reinhard (x/(1+x), asymptotic -- "infinite
//     input maps to white"). Interpolated between the two.
//   crushBlacks (IRAY_CRUSH_BLACKS, schema default 0.5): 0.0 = no change
//     (linear lower range); raised toward 1.0 adds a gentle TOE (power
//     curve) that pushes low intensities toward black -- doesn't touch the
//     already-<=1 top end much, matching "regain some punch in the lower
//     range" without re-introducing highlight clipping.
//   saturation (IRAY_SATURATION, schema default 1.0): blend toward
//     luminance; 1.0 = unmodified.
float3 irayTonemap(float3 x) {
    const float3 reinhard = x / (1.0 + x);
    float3 y = lerp(reinhard, x, IRAY_BURN_HIGHLIGHTS);
    y = saturate(y);
    const float toeExponent = 1.0 + IRAY_CRUSH_BLACKS;
    y = pow(y, float3(toeExponent, toeExponent, toeExponent));
    const float lum = dot(y, float3(0.2126, 0.7152, 0.0722));
    y = lerp(float3(lum, lum, lum), y, IRAY_SATURATION);
    return y;
}

// Cheap integer hash → unit-interval float, used to recolor instance
// IDs into a visually-distinct palette for the InstanceID debug view.
// Murmur-style mix; avalanches enough that adjacent IDs land on
// different hues without a lookup table. Moved verbatim from
// raygen.slang (P3 pass split); the ~0u miss sentinel maps to black.
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

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
    if (tid.x >= gParams.destWidth || tid.y >= gParams.destHeight)
        return;
    const uint2 launchIndex = tid.xy;
    const int3  texel = int3(int2(launchIndex), 0);

    // Reconstruct the old raygen `didHit` (= payload.hitT > 0.0f) from the
    // depth AOV -- see the header comment for the sentinel equivalence.
    const float depth  = gAovDepth.Load(texel);
    const bool  didHit = depth > 0.0f;

    // ----- Display output (BGRA8 backbuffer) ----------------------
    // Operator dispatch on the Color path; (n*0.5+0.5) on Normal; 1/depth
    // grayscale on Depth; hash-coloured palette on InstanceID.
    float3 displayColor = float3(0.0, 0.0, 0.0);
    if (gParams.debugViewMode == DEBUG_VIEW_COLOR)
    {
        // `raw` is a hard exception to everything below: true unprocessed
        // radiance, no exposure of any kind (manual, auto, or physical-
        // camera), no tonemap curve -- a debugging window straight onto the
        // HDR values (matches ovrtx's own "raw" semantics: a linear/no-op
        // passthrough distinct from `none`, which still applies exposure).
        if (gParams.tonemapOperator == TONEMAP_OPERATOR_RAW)
        {
            gOutput[launchIndex] = float4(gLinearColor.Load(texel).xyz, 1.0f);
            return;
        }

        // Photographic exposure: scale scene radiance by 2^exposure
        // before tonemapping. Mirrors UsdGeomCamera.exposure semantics
        // (negative darkens, positive brightens). World Lobby et al.
        // calibrate light intensities for a strongly-negative camera
        // exposure (~-10 stops); without this scale the dome blows out.
        //
        // Auto-exposure: when enabled, expose for the HIGHLIGHTS (legacy
        // mode) or the histogram median (histogram mode) — see
        // AUTO_EXPOSURE_MODE_* below and each branch's own comment.
        // Auto FULLY REPLACES the manual exposure — mutually exclusive
        // (the editor disables the manual slider when auto is on), so a
        // scene with hot UsdLux lights and no authored camera exposure
        // (the OpenPBR Playground) displays correctly with zero manual
        // tuning.
        float effectiveExposure = gParams.exposure;
        if (gParams.autoExposureEnabled != 0u)
        {
            if (gParams.autoExposureMode == AUTO_EXPOSURE_MODE_HISTOGRAM)
            {
                // RTX-alignment design Phase C (ovrtx-parity) — AutoExposurePass's
                // ResolveHistogram dispatch already reduced the 64-bucket
                // histogram to a single median log2(luminance), stored as a
                // raw float bit-pattern. Map it to an EV via the schema's
                // white-point scale (10) — same "target / measured" shape
                // as the legacy path's `log2(target) - brightLog2`.
                const float filteredLog2Lum =
                    asfloat(gAutoExposureStats.Load(AUTO_EXPOSURE_STATS_RESOLVED_EV_OFFSET));
                effectiveExposure = clamp(
                    log2(AUTO_EXPOSURE_HISTOGRAM_WHITE_POINT_SCALE) - filteredLog2Lum,
                    -24.0, 8.0);
            }
            else
            {
                // Legacy (AUTO_EXPOSURE_MODE_LEGACY) — unchanged: expose for
                // the highlights (AutoExposurePass tracked the max AND the
                // geometric-mean log2(lum) over lit pixels) so the brightest
                // content lands on the target and nothing clips to white:
                //   brightLog2 = min(maxLog2Lum, avgLog2Lum + MAX_HEADROOM)
                //   autoEV     = log2(target) − brightLog2
                // The MAX is clamped to a few stops above the average so a
                // single firefly / a sliver of a visible light can't crush
                // the image to black (pure-max fragility); when the scene
                // has no such outlier the true max drives the exposure.
                const uint count = gAutoExposureStats.Load(4u);
                if (count > 0u)
                {
                    const uint  sum        = gAutoExposureStats.Load(0u);
                    const uint  maxFixed   = gAutoExposureStats.Load(8u);
                    const float avgLog2Lum = (float(sum) / AUTO_EXPOSURE_LOG_SCALE) / float(count)
                                           - AUTO_EXPOSURE_LOG_OFFSET;
                    const float maxLog2Lum = (float(maxFixed) / AUTO_EXPOSURE_LOG_SCALE)
                                           - AUTO_EXPOSURE_LOG_OFFSET;
                    const float brightLog2 = min(maxLog2Lum,
                                                 avgLog2Lum + AUTO_EXPOSURE_MAX_HEADROOM);
                    const float target = (gParams.autoExposureKey > 0.0)
                                           ? gParams.autoExposureKey : 1.0;
                    effectiveExposure = clamp(log2(target) - brightLog2, -24.0, 8.0);
                }
            }
        }
        // RTX-alignment design Phase C — physical-camera exposureScale (a
        // linear multiplier; 1.0 = legacy no-op) rides UNDER the stops-based
        // gain above ("legacy exposure stops ride on top" — see
        // TonemapPass::Execute for where exposureScale is computed).
        const float exposureGain = gParams.exposureScale * exp2(effectiveExposure);
        const float3 radiance = gLinearColor.Load(texel).xyz;
        const float3 exposed = radiance * exposureGain;

        if (gParams.tonemapOperator == TONEMAP_OPERATOR_NONE)
            displayColor = exposed;
        else if (gParams.tonemapOperator == TONEMAP_OPERATOR_REINHARD)
            displayColor = reinhardTonemap(exposed);
        else if (gParams.tonemapOperator == TONEMAP_OPERATOR_MODIFIED_REINHARD)
            displayColor = modifiedReinhardTonemap(exposed, MODIFIED_REINHARD_WHITE_POINT);
        else if (gParams.tonemapOperator == TONEMAP_OPERATOR_HEJI_HABLE_ALU)
            displayColor = hejiHableAluTonemap(exposed);
        else if (gParams.tonemapOperator == TONEMAP_OPERATOR_HABLE_UC2)
            displayColor = hableUc2Tonemap(exposed);
        else if (gParams.tonemapOperator == TONEMAP_OPERATOR_IRAY)
            displayColor = irayTonemap(exposed);
        else  // TONEMAP_OPERATOR_ACES_APPROXIMATION — default; byte-identical
              // to the pre-Phase-C behaviour when selected.
            displayColor = acesFilmic(exposed);
    }
    else if (gParams.debugViewMode == DEBUG_VIEW_NORMAL)
    {
        // Miss is (0,0,0) in the Normal AOV; display as middle grey
        // so the background isn't visually identical to a Z-aligned
        // surface. Hit normals: standard half-encode.
        displayColor = didHit
            ? (gAovNormal.Load(texel).xyz * 0.5f + 0.5f)
            : float3(0.5f, 0.5f, 0.5f);
    }
    else if (gParams.debugViewMode == DEBUG_VIEW_DEPTH)
    {
        // Inverse-depth grayscale: surfaces closer to the camera
        // appear brighter, miss (= 0 sentinel) renders black.
        // The 1/(1+d) curve avoids the divide-by-zero at d=0 and
        // gives a smooth response across the typical [0.1, 50]
        // viewer range without a configured min/max range.
        const float d = depth;
        const float intensity = (d > 0.0f) ? (1.0f / (1.0f + d * 0.1f)) : 0.0f;
        displayColor = float3(intensity, intensity, intensity);
    }
    else if (gParams.debugViewMode == DEBUG_VIEW_PRIM_ID)
    {
        displayColor = hashInstanceColor(gAovPrimId.Load(texel));
    }
    else if (gParams.debugViewMode == DEBUG_VIEW_MATERIAL_ID)
    {
        // Same hash function as InstanceID — different identifier
        // domains so the colour palette is independent. Reuses
        // hashInstanceColor since the math is identity (uint -> hue).
        displayColor = hashInstanceColor(gAovMaterialId.Load(texel));
    }
    else if (gParams.debugViewMode == DEBUG_VIEW_BASE_COLOR)
    {
        // Raw OpenPBR baseColor straight off the material table —
        // pre-shading albedo. saturate() clamps in case a (post-v1)
        // emissive material's baseColor exceeds [0,1].
        displayColor = saturate(gAovBaseColor.Load(texel).xyz);
    }
    else if (gParams.debugViewMode == DEBUG_VIEW_WORLD_POS)
    {
        // Visualise world-space hit position via a SMOOTH periodic
        // encoding — sin remap to [0,1] with a configurable period.
        // The sin encoding is C-infinity smooth so no discontinuities
        // exist anywhere in world space (the raw worldPos values stay
        // unmodified in gOutWorldPos / the picker readout).
        //
        // Period comes from the editor's WorldPos period slider via
        // TonemapUniforms; defaults to 10 m for human-scale scenes.
        // Guard against 0 (would divide by zero).
        const float period = max(gParams.worldPosPeriod, 1e-3);
        const float WORLD_POS_TAU = 6.28318530717958647692;
        const float omega = WORLD_POS_TAU / period;
        displayColor = didHit
            ? (sin(gAovWorldPos.Load(texel).xyz * omega) * 0.5f + 0.5f)
            : float3(0.0, 0.0, 0.0);
    }
    // Tier 1 Hydra-canonical display branches.
    else if (gParams.debugViewMode == DEBUG_VIEW_ALPHA)
    {
        // Binary today; future-proofed for transmission / coverage.
        // White = hit, black = miss. The alpha AOV stores exactly the
        // old `didHit ? 1.0f : 0.0f` encode (R8_UNORM holds 0.0 / 1.0
        // exactly), so reading it back reproduces the raygen branch.
        const float a = gAovAlpha.Load(texel);
        displayColor = float3(a, a, a);
    }
    else if (gParams.debugViewMode == DEBUG_VIEW_ELEMENT_ID)
    {
        // Same hash palette as InstanceID — different identifier
        // domain (per-face vs per-instance) so adjacent faces land
        // on visually-distinct hues. Miss → black.
        displayColor = hashInstanceColor(gAovElementId.Load(texel));
    }
    else if (gParams.debugViewMode == DEBUG_VIEW_NORMAL_EYE)
    {
        // Standard half-encode like the world-normal branch; miss
        // renders middle grey so the background isn't visually
        // identical to a Z-aligned eye-space surface.
        displayColor = didHit
            ? (gAovNormalEye.Load(texel).xyz * 0.5f + 0.5f)
            : float3(0.5f, 0.5f, 0.5f);
    }
    else if (gParams.debugViewMode == DEBUG_VIEW_WORLD_POS_EYE)
    {
        // Same sin-encoded periodic visualisation as WORLD_POS, but
        // sampled in eye space. Useful for verifying that a Hydra
        // delegate's Peye matches the renderer's view transform.
        const float period = max(gParams.worldPosPeriod, 1e-3);
        const float WORLD_POS_TAU_EYE = 6.28318530717958647692;
        const float omegaEye = WORLD_POS_TAU_EYE / period;
        displayColor = didHit
            ? (sin(gAovWorldPosEye.Load(texel).xyz * omegaEye) * 0.5f + 0.5f)
            : float3(0.0, 0.0, 0.0);
    }
    gOutput[launchIndex] = float4(displayColor, 1.0f);
}
