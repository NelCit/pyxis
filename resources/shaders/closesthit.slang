// Pyxis renderer — closest-hit shader.
//
// Plan §41 M3. Slang [shader("closesthit")] + Slang.cmake
// `-stage closesthit`.
//
// M3 stub: writes the triangle barycentrics into payload.color so the
// cube's hit faces show three-way colour interpolation when M3's
// PathTracePass wires the raygen + miss + closesthit + TLAS up. This
// gives a visually obvious "I hit something" signal independent of
// any material or light evaluation.
//
// The full OpenPBR closest-hit (NEE direct lighting, BRDF sampling,
// recursive bounces) lands at M5 / M7. M3 deliberately ships only
// the shader-stage scaffolding so the M3 box render is a *visible*
// validation of BLAS/TLAS/dispatch correctness, not a check of the
// shading pipeline.

#include "ShaderInterop.slang"

// Slang emits the SPIR-V `OpEntryPoint` name as `"main"` regardless
// of the source-side function name; the C++ side passes `"main"` to
// NVRHI's createShader. See the matching note in raygen.slang.
[shader("closesthit")]
void main(inout HitInfo payload, in BuiltInTriangleIntersectionAttributes attribs) {
    // Barycentrics-as-colour: red = u, green = v, blue = 1 - u - v.
    const float baryW = 1.0f - attribs.barycentrics.x - attribs.barycentrics.y;
    payload.color = float3(attribs.barycentrics.x, attribs.barycentrics.y, baryW);
    payload.hitT  = RayTCurrent();
}
