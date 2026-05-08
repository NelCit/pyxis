// Pyxis renderer — shared C++ / Slang interop header.
//
// Plan §10 + §23. The single file that defines structs, constants,
// and binding indices used by both the C++ side and the Slang
// shaders. Built against:
//
//   * On the C++ side `__cplusplus` is set; we alias `float2 / float3 /
//     float4 / float4x4 / uint` to their hlslpp / standard
//     counterparts inside `namespace pyxis::shaderinterop`. Every
//     struct defined below lives in that namespace.
//   * On the Slang side `__cplusplus` is unset; the structs land at
//     file scope and use Slang's built-in HLSL vector / matrix types.
//
// Layout rule (§23): every struct that lands in a constant buffer or
// shader-binding-table record must be a multiple of 16 bytes. We
// static_assert that on the C++ side; the Slang compiler enforces
// std140-equivalent layout via `-matrix-layout-row-major` already
// (§10).
//
// Adding a struct here:
//   1. Declare it between the `#ifdef __cplusplus namespace pyxis::
//      shaderinterop {` opening and the corresponding closing
//      `#endif` so the C++ namespace wraps it.
//   2. Pad to a 16-byte multiple.
//   3. Add a `static_assert(sizeof(Foo) % 16 == 0, "...")` line in
//      the matching #ifdef block at the bottom of this file.

#ifndef PYXIS_SHADER_INTEROP_SLANG
#define PYXIS_SHADER_INTEROP_SLANG

#ifdef __cplusplus
    #include <hlsl++.h>
    #include <cstdint>

    namespace pyxis::shaderinterop {

    using float2   = hlslpp::float2;
    using float3   = hlslpp::float3;
    using float4   = hlslpp::float4;
    using float4x4 = hlslpp::float4x4;
    using uint     = uint32_t;
#endif

// -----------------------------------------------------------------------------
// HitInfo — payload struct passed between raygen / closesthit / miss
// shaders via TraceRay. M3 stub carries one float3 colour + a hitT
// distance; expands later as path tracing arrives (radiance, RNG
// state, throughput, etc.).
// -----------------------------------------------------------------------------
struct HitInfo {
    float3 color;
    float  hitT;     // ray-distance at hit; -1.0 sentinel from miss shader.
};

#ifdef __cplusplus
    static_assert(sizeof(HitInfo) == 16,
                  "HitInfo must stay 16 bytes (float3 + float fits exactly).");
    static_assert(sizeof(HitInfo) % 16 == 0,
                  "ShaderInterop structs must be 16-byte aligned (§23).");

    }  // namespace pyxis::shaderinterop
#endif

#endif  // PYXIS_SHADER_INTEROP_SLANG
