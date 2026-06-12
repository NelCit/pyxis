// Pyxis renderer — shadow-ray miss shader.
//
// M9-fidelity. The closesthit's per-light visibility check shoots a
// shadow ray with RAY_FLAG_SKIP_CLOSEST_HIT_SHADER |
// RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH. With that flag combo
// only the anyhit + miss shaders fire — the regular closesthit is
// bypassed (we don't want to recursively shade). The anyhit may
// IgnoreHit() for translucent materials (M9 stub); if any opaque
// triangle is hit between surface and light, the miss never fires
// and `payload.color = 1.0` doesn't get set → light is occluded.
// If no opaque hit, miss fires and writes `1.0` → light is visible.

#include "ShaderInterop.slang"

// Shadow-ray payload: a single float that the closesthit reads as
// "0.0 = occluded, 1.0 = visible". We pack into HitInfo's color.x
// for cbuffer-shape compatibility (HitInfo is the only payload type
// the pipeline knows about); the shadow path only uses .color.x.

[shader("miss")]
void main(inout HitInfo payload) {
    // Ray reached the light without being absorbed by an opaque hit.
    // Flag the surface as "lit by this light".
    payload.color = float3(1.0, 1.0, 1.0);
}
