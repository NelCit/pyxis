// Pyxis renderer — auto-exposure log-luminance reduction (AutoExposurePass).
//
// Reads the fp32 LINEAR radiance (gLinearColor — the same buffer the tonemap
// reads) and accumulates, over LIT pixels (lum > AUTO_EXPOSURE_MIN_LUM), a
// DETERMINISTIC fixed-point sum of log2(luminance) plus a lit-pixel count into
// a uint2 UAV (gStats: [0]=sum, [4]=count). The tonemap then derives the auto
// exposure:
//   avgLog2Lum       = sum / SCALE / count − OFFSET   (geometric-mean luminance)
//   effectiveExposure = log2(key) − avgLog2Lum  (+ the manual exposure bias)
//
// Determinism: the per-pixel contribution is an INTEGER, so the parallel
// InterlockedAdd is order-independent and the result is bit-identical run to
// run (preserves the §33.7 byte-equal contract even with auto-exposure on).
// Each 8×8 group reduces into groupshared first, so only ONE global atomic
// fires per group (64× less contention).
//
// The caller (AutoExposurePass) clears gStats to 0 before this dispatch and
// only runs the pass when auto-exposure is enabled.

#include "ShaderInterop.slang"

[[vk::binding(0, 0)]] Texture2D<float4>   gLinearColor;  // fp32 LINEAR radiance (RGBA32F)
[[vk::binding(1, 0)]] RWByteAddressBuffer  gStats;        // [0]=fixed-point sum, [4]=lit count
[[vk::binding(2, 0)]] ConstantBuffer<AutoExposureUniforms> gParams;

groupshared uint gGroupSum;    // fixed-point log-lum sum for this group
groupshared uint gGroupCount;  // lit-pixel count for this group

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID, uint groupIndex : SV_GroupIndex) {
    if (groupIndex == 0u) {
        gGroupSum = 0u;
        gGroupCount = 0u;
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
        }
    }
    GroupMemoryBarrierWithGroupSync();

    // One global atomic per group.
    if (groupIndex == 0u) {
        uint prev;
        gStats.InterlockedAdd(0u, gGroupSum, prev);
        gStats.InterlockedAdd(4u, gGroupCount, prev);
    }
}
