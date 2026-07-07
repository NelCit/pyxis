// Pyxis renderer — sharc_resolve.slang: the SHARC Resolve compute pass.
//
// ONE shader per pass (RTX-alignment convention). Runs once per frame over the
// radiance-cache capacity, BEFORE IndirectDiffusePass. For each voxel cell it
// folds this frame's accumulated per-cell radiance sum (written by the
// IndirectDiffuse UPDATE path via SharcAccumulate) into a cross-frame running
// mean stored in the resolved buffer, then clears the per-frame sum so the next
// frame starts fresh. Over the N accumulation frames the resolved radiance
// converges to the infinite-bounce equilibrium. See radiance_cache.slang for
// the algorithm + provenance (clean-room, not NVIDIA source).
//
// Bindings — Set 0 (this pass owns the whole layout; no SceneBindings):
//   set=0, binding=0 : RWByteAddressBuffer gAccum    (16 B/cell: uint r,g,b sum + uint count)
//   set=0, binding=1 : RWByteAddressBuffer gResolved (16 B/cell: float r,g,b + float frameWeight)

#include "radiance_cache.slang"

[[vk::binding(0, 0)]] RWByteAddressBuffer gAccum;
[[vk::binding(1, 0)]] RWByteAddressBuffer gResolved;

[numthreads(256, 1, 1)]
void ResolveMain(uint3 dtid : SV_DispatchThreadID)
{
    const uint slot = dtid.x;
    if (slot >= SHARC_CAPACITY) return;

    const uint accumOff = slot * 16u;
    const uint4 a = gAccum.Load4(accumOff);
    const uint count = a.w;
    if (count == 0u) return;  // untouched this frame — keep resolved as-is

    const float3 frameMean =
        (float3(a.xyz) / SHARC_RADIANCE_SCALE) / float(count);

    const uint resOff = slot * 16u;
    const float4 prev = asfloat(gResolved.Load4(resOff));
    const float newW = min(prev.w + 1.0, SHARC_ACCUM_FRAME_MAX);
    // Running mean with a capped frame weight: newRadiance = lerp(prev, frameMean, 1/newW).
    const float3 newRadiance = lerp(prev.xyz, frameMean, 1.0 / newW);

    gResolved.Store4(resOff, asuint(float4(newRadiance, newW)));
    // Clear this frame's accumulation for the next Update pass.
    gAccum.Store4(accumOff, uint4(0u, 0u, 0u, 0u));
}
