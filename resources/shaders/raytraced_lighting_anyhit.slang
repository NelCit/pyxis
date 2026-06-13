// Pyxis renderer — lighting-pass any-hit entry (P4 pass split).
//
// Thin entry over the shared alpha-test decision in
// raytraced_anyhit.slang, compiled against the lighting pipeline's
// full HitInfo payload (the anyhit's payload type must match the
// pipeline's TraceRay payload type — secondary rays carry HitInfo).
// Same IgnoreHit gate as the GBuffer pipeline's entry — visibility
// semantics stay bit-identical across the two pipelines (§33.7).
//
// Fires for ALL ray types of this pipeline: the transparency-
// continuation + reflection rays (closesthit runs after) AND the
// shadow / AO rays (SKIP_CLOSEST_HIT_SHADER — the anyhit still fires
// and may IgnoreHit() for alpha-tested geometry, exactly as before
// the split).

#include "ShaderInterop.slang"
#include "raytraced_anyhit.slang"

// Slang emits the SPIR-V `OpEntryPoint` name as `"main"` regardless
// of the source-side function name; the C++ side passes `"main"` to
// NVRHI's createShader. See the matching note in
// raytraced_lighting_raygen.slang.
[shader("anyhit")]
void main(inout HitInfo payload, in BuiltInTriangleIntersectionAttributes attribs) {
    if (AnyHitIsAlphaTested()) {
        IgnoreHit();
        return;
    }
    // Transparent shadows / AO: shadow + AO rays carry
    // RAY_FLAG_SKIP_CLOSEST_HIT_SHADER (shading.slang's shadow/AO TraceRay). On
    // those rays, let TRANSMISSIVE (glass-like) surfaces pass instead of
    // occluding — physically light goes through glass, so a transmissive
    // surface should not cast an opaque black shadow (the documented wrong-ness
    // this replaces). Composite / reflection rays (closesthit runs, no SKIP
    // flag) still hit transmissive surfaces so the front-to-back transparency
    // loop shades them. Binary approximation — clear glass casts no shadow;
    // colored/attenuated transparent shadows are a follow-up.
    if ((RayFlags() & RAY_FLAG_SKIP_CLOSEST_HIT_SHADER) != 0u
        && AnyHitIsTransmissive()) {
        IgnoreHit();
        return;
    }
    // All other hits — including opacity < 1 on composite rays — are accepted;
    // the closesthit decides how to shade them.
}
