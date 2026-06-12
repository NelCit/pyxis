// Pyxis renderer — GBuffer-pass miss shader (P4 pass split).
//
// Thin by design: re-asserts the hitT = -1 miss sentinel the raygen
// pre-initialised (the lighting raygen branches on hitT < 0 to run the
// dome-background path). The other payload fields keep their raygen
// sentinels (~0u ids, zero bary).

#include "ShaderInterop.slang"

// Slang emits the SPIR-V `OpEntryPoint` name as `"main"` regardless
// of the source-side function name; the C++ side passes `"main"` to
// NVRHI's createShader. See the matching note in
// raytraced_gbuffer_raygen.slang.
[shader("miss")]
void main(inout VisibilityPayload payload) {
    payload.hitT = -1.0f;
}
