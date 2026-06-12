// Pyxis renderer — GBuffer-pass closest-hit shader (P4 pass split).
//
// Thin by design: fills the 20-byte VisibilityPayload from the hit
// intrinsics — exactly the four values the lighting raygen needs to
// reconstruct every shading input (RayTCurrent / barycentrics /
// InstanceID / PrimitiveIndex). No shading runs in this pipeline.

#include "ShaderInterop.slang"

// Slang emits the SPIR-V `OpEntryPoint` name as `"main"` regardless
// of the source-side function name; the C++ side passes `"main"` to
// NVRHI's createShader. See the matching note in
// raytraced_gbuffer_raygen.slang.
[shader("closesthit")]
void main(inout VisibilityPayload payload, in BuiltInTriangleIntersectionAttributes attribs) {
    payload.hitT           = RayTCurrent();
    payload.bary           = attribs.barycentrics;
    payload.instanceId     = InstanceID();
    payload.primitiveIndex = PrimitiveIndex();
}
