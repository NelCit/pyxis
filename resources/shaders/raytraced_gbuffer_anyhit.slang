// Pyxis renderer — GBuffer-pass any-hit entry (P4 pass split).
//
// Thin entry over the shared alpha-test decision in
// raytraced_anyhit.slang, compiled against the GBuffer pipeline's thin
// VisibilityPayload (the anyhit's payload type must match the
// pipeline's TraceRay payload type). Same IgnoreHit gate as the
// lighting pipeline's entry — visibility semantics stay bit-identical
// across the two pipelines (§33.7).

#include "ShaderInterop.slang"
#include "raytraced_anyhit.slang"

// Slang emits the SPIR-V `OpEntryPoint` name as `"main"` regardless
// of the source-side function name; the C++ side passes `"main"` to
// NVRHI's createShader. See the matching note in
// raytraced_gbuffer_raygen.slang.
[shader("anyhit")]
void main(inout VisibilityPayload payload, in BuiltInTriangleIntersectionAttributes attribs) {
    if (AnyHitIsAlphaTested()) {
        IgnoreHit();
    }
    // All other hits — including opacity < 1 and transmission — are
    // accepted; the lighting pass decides how to shade them.
}
