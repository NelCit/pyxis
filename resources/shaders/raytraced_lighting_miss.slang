// Pyxis renderer — lighting-pass miss shader (P4 pass split).
//
// Since the visibility-buffer split this miss (miss-index 0) serves
// ONLY SECONDARY rays — the lighting raygen's transparency-continuation
// segments and the reflection rays the megakernel closesthit /
// raygen-first-hit trace. The PRIMARY-miss background is computed by
// raytraced_lighting_raygen.slang directly (visibility-buffer hitT < 0)
// via the SAME SampleDomeBackground function this shader calls — the
// shared module (dome_sample.slang) keeps the two paths bit-identical
// (§33.7).
//
// Lat-long mapping, dome tint/rotation, and the no-dome sky-blue
// fallback all live in dome_sample.slang (extracted verbatim from the
// pre-split miss.slang).
//
// **Still no IBL importance sampling** — that's the M7-full closesthit
// replacement (alias-table over the env-map per §7).

#include "ShaderInterop.slang"
#include "dome_sample.slang"

// Slang emits the SPIR-V `OpEntryPoint` name as `"main"` regardless
// of the source-side function name; the C++ side passes `"main"` to
// NVRHI's createShader. See the matching note in
// raytraced_lighting_raygen.slang.
[shader("miss")]
void main(inout HitInfo payload) {
    payload.color = SampleDomeBackground(WorldRayDirection());
    payload.hitT  = -1.0f;  // miss sentinel — the continuation loops branch on this.
    // M7 follow-up — leave the AOV-inspector fields at sentinel
    // defaults so a primary-ray miss reads as (zero normal, no
    // instance) in the Normal / InstanceID AOVs. baseColor /
    // materialId / worldHit added in the same M7 follow-up batch
    // for the BaseColor / MaterialID / WorldPos AOVs.
    payload.normalWorld = float3(0.0, 0.0, 0.0);
    payload.instanceId  = 0xFFFFFFFFu;
    payload.baseColor   = float3(0.0, 0.0, 0.0);
    payload.materialId  = 0xFFFFFFFFu;
    payload.worldHit    = float3(0.0, 0.0, 0.0);
    payload.elementId   = 0xFFFFFFFFu;  // Hydra-canonical miss sentinel
}
