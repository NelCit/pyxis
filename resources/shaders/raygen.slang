// Pyxis renderer — ray-generation entry point.
//
// Plan §41 M3 ("raygen/closesthit/miss for one cube") + §10 / §23.
// Slang RT compilation contract: the [shader("raygeneration")]
// attribute pairs with `-stage raygeneration` in Slang.cmake; the
// pair tells slangc to emit a SPIR-V module with the right
// execution-model decoration so NVRHI can hand it to vkCreateRay
// TracingPipelinesKHR.
//
// M3 status: this is a *compile-only* skeleton. The real raygen body
// lands in the PathTracePass commit (next M3 step) once the GpuScene
// has uploaded the cube + built BLAS/TLAS, the descriptor-set layout
// exists, and the binding table is bound. Until then the body is
// empty so the slang→spv pipeline + the ShaderInterop include path
// are validated end-to-end without depending on resources that don't
// yet exist.

#include "ShaderInterop.slang"

[shader("raygeneration")]
void RayGenMain() {
    // Intentionally empty — see header. Once PathTracePass arrives:
    //   1. Compute pixel + jitter from DispatchRaysIndex().
    //   2. Build a primary ray from the camera uniforms.
    //   3. TraceRay against the bound TLAS with HitInfo as payload.
    //   4. Write payload.color to the bound output image.
}
