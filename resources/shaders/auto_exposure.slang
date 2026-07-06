// Pyxis renderer — auto-exposure log-luminance reduction (AutoExposurePass).
//
// Two auto-exposure MODES (AutoExposureUniforms.mode), selected by
// RenderSettings::autoExposure (0=off, 1=on/legacy, 2=on/histogram):
//
//   AUTO_EXPOSURE_MODE_LEGACY (default when on) — unchanged from the
//   pre-Phase-C implementation: reads the fp32 LINEAR radiance (gLinearColor)
//   and accumulates, over LIT pixels (lum > AUTO_EXPOSURE_MIN_LUM), a
//   DETERMINISTIC fixed-point sum of log2(luminance) plus a lit-pixel count
//   AND a running max into a uint3 UAV (gStats: [0]=sum, [4]=count,
//   [8]=max). TonemapPass derives the "expose for the highlights" EV from
//   these exactly as before.
//
//   AUTO_EXPOSURE_MODE_HISTOGRAM (RTX-alignment design Phase C, ovrtx-parity)
//   — OmniRtxCameraAutoExposureAPI's documented model: a 64-bucket
//   log-luminance histogram (AUTO_EXPOSURE_HISTOGRAM_BUCKETS) covering
//   [AUTO_EXPOSURE_HISTOGRAM_MIN_LUMINANCE, AUTO_EXPOSURE_HISTOGRAM_MAX_LUMINANCE]
//   in log2 space; every pixel's luminance is CLAMPED into that range (not
//   excluded, unlike the legacy MIN_LUM threshold — "these are LUMINANCE
//   clamps on histogram input" per the design task) before bucketing. This
//   `main` entry only BUILDS the histogram (deterministic integer atomics,
//   same discipline as the legacy path); a second entry, ResolveHistogram,
//   scans it from a single thread to find the MEDIAN bucket (schema default
//   filter — see the enum's doc comment for why "average" isn't wired as a
//   runtime toggle yet) and writes the resulting log2(luminance) into the
//   stats buffer for TonemapPass to read. AutoExposurePass::Execute issues
//   BOTH dispatches every histogram-mode frame; NVRHI's automatic UAV-barrier
//   placement (enabled by default) orders them without any explicit barrier
//   call on our side.
//
// Determinism: every per-pixel contribution in both modes is an INTEGER
// atomic (add/max), so the reduction is order-independent and the result is
// bit-identical run to run (preserves the §33.7 byte-equal contract with
// auto-exposure on, in either mode). Each 8×8 group reduces into groupshared
// first, so only ONE (legacy) or up to 64 (histogram) global atomics fire
// per group instead of one per pixel.
//
// The caller (AutoExposurePass) clears gStats to 0 before the `main`
// dispatch and only runs either dispatch when auto-exposure is enabled.

#include "ShaderInterop.slang"

[[vk::binding(0, 0)]] Texture2D<float4>   gLinearColor;  // fp32 LINEAR radiance (RGBA32F)
[[vk::binding(1, 0)]] RWByteAddressBuffer  gStats;        // see AUTO_EXPOSURE_STATS_* layout
[[vk::binding(2, 0)]] ConstantBuffer<AutoExposureUniforms> gParams;

groupshared uint gGroupSum;    // legacy: fixed-point log-lum sum for this group
groupshared uint gGroupCount;  // legacy: lit-pixel count for this group
groupshared uint gGroupMax;    // legacy: fixed-point MAX log-lum for this group
// Histogram mode: one groupshared bucket per thread — [numthreads(8,8,1)]
// gives exactly 64 threads/group, matching AUTO_EXPOSURE_HISTOGRAM_BUCKETS
// 1:1, so the init / global-writeback loops below are branchless (thread
// `groupIndex` owns bucket `groupIndex`, no loop needed).
groupshared uint gHistogram[AUTO_EXPOSURE_HISTOGRAM_BUCKETS];

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID, uint groupIndex : SV_GroupIndex) {
    if (gParams.mode == AUTO_EXPOSURE_MODE_HISTOGRAM) {
        gHistogram[groupIndex] = 0u;
        GroupMemoryBarrierWithGroupSync();

        if (tid.x < gParams.srcWidth && tid.y < gParams.srcHeight) {
            const float3 rgb = gLinearColor.Load(int3(int2(tid.xy), 0)).xyz;
            const float  lumRaw = dot(rgb, float3(0.2126, 0.7152, 0.0722));
            // Clamp (not exclude) into the schema's histogram luminance range.
            const float lum = clamp(lumRaw, AUTO_EXPOSURE_HISTOGRAM_MIN_LUMINANCE,
                                     AUTO_EXPOSURE_HISTOGRAM_MAX_LUMINANCE);
            const float logMin = log2(AUTO_EXPOSURE_HISTOGRAM_MIN_LUMINANCE);
            const float logMax = log2(AUTO_EXPOSURE_HISTOGRAM_MAX_LUMINANCE);
            const float t = (log2(lum) - logMin) / (logMax - logMin);  // [0, 1]
            const uint bucket = min(uint(t * float(AUTO_EXPOSURE_HISTOGRAM_BUCKETS)),
                                    AUTO_EXPOSURE_HISTOGRAM_BUCKETS - 1u);
            uint prevIgnored;
            InterlockedAdd(gHistogram[bucket], 1u, prevIgnored);
        }
        GroupMemoryBarrierWithGroupSync();

        // One global atomic per bucket per group (<=64), vs one per pixel.
        const uint bucketCount = gHistogram[groupIndex];
        if (bucketCount > 0u) {
            uint prevIgnored;
            gStats.InterlockedAdd(AUTO_EXPOSURE_STATS_HISTOGRAM_OFFSET + groupIndex * 4u,
                                  bucketCount, prevIgnored);
        }
        return;
    }

    // ---- Legacy path (AUTO_EXPOSURE_MODE_LEGACY) — unchanged. ----
    if (groupIndex == 0u) {
        gGroupSum = 0u;
        gGroupCount = 0u;
        gGroupMax = 0u;
    }
    GroupMemoryBarrierWithGroupSync();

    if (tid.x < gParams.srcWidth && tid.y < gParams.srcHeight) {
        const float3 rgb = gLinearColor.Load(int3(int2(tid.xy), 0)).xyz;
        const float  lum = dot(rgb, float3(0.2126, 0.7152, 0.0722));
        if (lum > AUTO_EXPOSURE_MIN_LUM) {
            const float logLum  = clamp(log2(lum), AUTO_EXPOSURE_LOG_MIN, AUTO_EXPOSURE_LOG_MAX);
            const uint  fixedPt = uint((logLum + AUTO_EXPOSURE_LOG_OFFSET) * AUTO_EXPOSURE_LOG_SCALE);
            uint prev;
            InterlockedAdd(gGroupSum, fixedPt, prev);
            InterlockedAdd(gGroupCount, 1u, prev);
            InterlockedMax(gGroupMax, fixedPt, prev);
        }
    }
    GroupMemoryBarrierWithGroupSync();

    // One global atomic per group. Integer add/max are order-independent →
    // the whole reduction stays deterministic (byte-equal even with AE on).
    if (groupIndex == 0u) {
        uint prev;
        gStats.InterlockedAdd(0u, gGroupSum, prev);
        gStats.InterlockedAdd(4u, gGroupCount, prev);
        gStats.InterlockedMax(8u, gGroupMax, prev);
    }
}

// ResolveHistogram — single-thread deterministic scan over the 64 buckets
// `main` just finished building (see the AUTO_EXPOSURE_MODE_HISTOGRAM branch
// above), run as AutoExposurePass's second dispatch (1,1,1 — this is O(64),
// not O(pixels); a per-pixel scan in TonemapPass would redundantly re-run
// the same 64-iteration reduction on every one of the ~2M output texels).
// Finds the MEDIAN bucket (cumulative count first exceeds half the total
// lit-pixel count — every pixel contributes in histogram mode, so total ==
// srcWidth*srcHeight) and writes its representative log2(luminance) as a
// raw float bit-pattern (asuint) at AUTO_EXPOSURE_STATS_RESOLVED_EV_OFFSET.
// "average" is the schema's documented alternative filter but isn't wired as
// a runtime toggle (see AUTO_EXPOSURE_MODE_HISTOGRAM's doc comment) —
// median (the schema default) is the only filter implemented today.
[shader("compute")]
[numthreads(1, 1, 1)]
void ResolveHistogram(uint3 tid : SV_DispatchThreadID) {
    uint counts[AUTO_EXPOSURE_HISTOGRAM_BUCKETS];
    uint total = 0u;
    for (uint i = 0u; i < AUTO_EXPOSURE_HISTOGRAM_BUCKETS; ++i) {
        counts[i] = gStats.Load(AUTO_EXPOSURE_STATS_HISTOGRAM_OFFSET + i * 4u);
        total += counts[i];
    }

    const float logMin = log2(AUTO_EXPOSURE_HISTOGRAM_MIN_LUMINANCE);
    const float logMax = log2(AUTO_EXPOSURE_HISTOGRAM_MAX_LUMINANCE);
    const float bucketWidth = (logMax - logMin) / float(AUTO_EXPOSURE_HISTOGRAM_BUCKETS);

    float medianLog2Lum = logMin;  // degenerate (total == 0) fallback: histogram floor.
    if (total > 0u) {
        const uint half = total / 2u;
        uint cumulative = 0u;
        for (uint b = 0u; b < AUTO_EXPOSURE_HISTOGRAM_BUCKETS; ++b) {
            cumulative += counts[b];
            if (cumulative > half) {
                medianLog2Lum = logMin + (float(b) + 0.5) * bucketWidth;
                break;
            }
        }
    }
    gStats.Store(AUTO_EXPOSURE_STATS_RESOLVED_EV_OFFSET, asuint(medianLog2Lum));
}
