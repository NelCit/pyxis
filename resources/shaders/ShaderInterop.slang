// Pyxis renderer — shared C++ / Slang interop header.
//
// Plan §10 + §23. The single file that defines structs, constants,
// and binding indices used by both the C++ side and the Slang
// shaders.
//
// Branches on `__cplusplus`: on the C++ side the file declares
// `namespace pyxis::shaderinterop` and aliases `float2 / float3 /
// float4 / float4x4 / uint` to their hlslpp / standard counterparts,
// then the struct definitions land inside the namespace. On the
// Slang side `__cplusplus` is unset, the namespace block is skipped,
// and the structs sit at file scope using Slang's built-in HLSL
// vector / matrix types.
//
// Layout rule (§23): every struct that lands in a constant buffer
// must be a multiple of 16 bytes; static_asserted on the C++ side.
// Slang's `-matrix-layout-row-major` + std140-equivalent layout
// gives the same guarantee on the shader side.

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

#ifndef __cplusplus
// HitInfo — payload struct passed between raygen / closesthit / miss
// shaders via TraceRay. Shader-only: Slang's `float3` is 12 bytes
// (tight) but the C++ `hlslpp::float3` is 16 bytes (SIMD-padded), so
// a dual-language `{ float3 color; float hitT; }` would have
// different `sizeof` on the two sides. Since HitInfo never crosses
// the C++/SPIR-V boundary as a memory blob (it lives only in shader
// registers between TraceRay and its callees), we simply omit it
// from the C++ view of this header.
//
// M3 carries one float3 colour + a hitT distance; expands later as
// path tracing arrives (radiance, RNG state, throughput, etc.).
struct HitInfo {
    float3 color;
    float  hitT;     // ray-distance at hit; -1.0 sentinel from miss shader.
};
#endif

// CameraUniforms — uploaded per-frame to a constant buffer the raygen
// shader reads to build primary rays. M3 ships pinhole-only (no DOF,
// no jitter); the §43.3 reservation for `apertureFStop` lives on
// CameraDesc and gets wired into the shader at M9+.
//
// Layout: 128 bytes (two 64-byte float4x4s). Both matrices are
// row-major + column-vector convention (§10):
//   pos_view  = mul(viewFromWorld, pos_world)
//   pos_clip  = mul(projFromView,  pos_view)
// The raygen shader uses the *inverses* — `worldFromView`
// (= inverse(viewFromWorld)) to translate ray origin into world,
// and `viewFromClip` (= inverse(projFromView)) to translate NDC
// pixel coordinates into a view-space direction. We pass the
// inverses pre-computed by the C++ side rather than computing them
// in shader; M3 ships static cameras so this is one matrix-inverse
// per frame at most.
struct CameraUniforms {
    float4x4 worldFromView;     // inverse of CameraDesc::viewFromWorld
    float4x4 viewFromClip;      // inverse of CameraDesc::projFromView
};

// OpenPBRMaterialGPU — the bytewise layout the closesthit reads via
// the structured-buffer material table. Plan §11. The C++ side packs
// an OpenPBRMaterialDesc into this shape inside MaterialTable; the
// shader side indexes by `materialId` (set on the instance custom
// index) and reads `flags` to short-circuit lobe evaluation.
//
// M5 ships the BASE LAYER only. The reserved* slots leave room for
// the §11 specular / transmission / coat / sheen / subsurface
// extensions to land at M5+ (post the user's full BSDF impl) without
// reshuffling existing fields. Layout is 80 bytes (5 rows of 16).
//
// **Scalars only** — no float3 / float4 vectors. Reason: `hlslpp::
// float3` is SIMD-padded to 16 bytes on the C++ side while Slang's
// `float3` in a structured buffer is 12 bytes tight; mixing the two
// produces an off-by-4 layout drift (caught by the static_asserts
// at the bottom). Scalar fields are byte-equivalent on both sides
// regardless of the layout rule the compiler picked. Closesthit
// reconstructs the float3 with `float3(m.baseColorR, m.baseColorG,
// m.baseColorB)` — verbose but unambiguous.
struct OpenPBRMaterialGPU {
    // Row 0 — base color + flags.
    float baseColorR;     // linear sRGB
    float baseColorG;
    float baseColorB;
    uint  flags;          // bitmask of MaterialFlag (§11.6)

    // Row 1 — bindless slot indices in Texture_SRV space=2 for the
    // base-layer texture maps. ~0u means "no texture for this slot;
    // closesthit uses the matching scalar above". The bindless lookup
    // is gated by the corresponding MaterialFlag bit so the sentinel
    // is only a fallback.
    uint  baseColorTex;
    uint  normalTex;
    uint  metallicTex;
    uint  roughnessTex;

    // Row 2 — base-layer + dielectric scalars.
    float roughness;
    float metalness;
    float opacity;
    float specularIor;

    // Row 3 — coat + emission.
    float coatWeight;
    float coatRoughness;
    float emissionLuminance;
    uint  emissionTex;

    // Row 4 — remaining bindless slots reserved for the §11 extension
    // surfaces the user wires alongside the full BSDF impl.
    uint  opacityTex;
    uint  transmissionTex;
    uint  coatRoughnessTex;
    uint  _reserved0;
};

// LightGpu — packed light entry the closesthit reads via the
// structured-buffer light table at binding 5 (M7). Same scalar-only
// rule as OpenPBRMaterialGPU (avoids the hlslpp::float3 16-vs-12-byte
// trap). Layout is 80 bytes (5 rows of 16).
//
// M7 ships a deliberately **simple shading model** (no NdotL, no
// shadow rays, no NEE, no MIS, no importance sampling — see
// closesthit.slang for the full closeup). Each light contributes
// `color * intensity * weight` where weight is 1.0 for direct
// (Distant / Rect) and 0.5 for ambient (Dome). The `direction` /
// `position` / `axisU` / `axisV` fields are stored for the future
// full M7 BSDF + NEE pass the user fills in; the M7-simple
// closesthit ignores them.
//
// `kind` mirrors `pyxis::LightDesc::Kind` (0=Distant, 1=Dome, 2=Rect).
struct LightGpu {
    // Row 0 — color + intensity.
    float colorR;
    float colorG;
    float colorB;
    float intensity;        // 0 = sentinel / disabled (M7-simple skips)

    // Row 1 — direction (Distant) + kind tag.
    float dirX;
    float dirY;
    float dirZ;
    uint  kind;             // 0=Distant, 1=Dome, 2=Rect

    // Row 2 — Rect position (world).
    float posX;
    float posY;
    float posZ;
    uint  envMapTex;        // Dome's lat-long EXR bindless slot; ~0u if procedural

    // Row 3 — Rect axisU.
    float axisUx;
    float axisUy;
    float axisUz;
    uint  doubleSided;      // 0 = single-sided, 1 = double-sided (Rect)

    // Row 4 — Rect axisV.
    float axisVx;
    float axisVy;
    float axisVz;
    uint  _reserved0;
};

// Light kind enum mirroring pyxis::LightDesc::Kind. Closesthit
// branches on this for the M7-simple per-kind weighting.
static const uint LIGHT_KIND_DISTANT = 0;
static const uint LIGHT_KIND_DOME    = 1;
static const uint LIGHT_KIND_RECT    = 2;

#ifdef __cplusplus
    // HitInfo is shader-only (see the `#ifndef __cplusplus` guard
    // above), so its size assert lives shader-side only too. Below
    // we lock the layout of the structs that *do* cross the C++ /
    // SPIR-V boundary as memory blobs (CameraUniforms via cbuffer,
    // OpenPBRMaterialGPU via structured buffer).
    static_assert(sizeof(CameraUniforms) == 128,
                  "CameraUniforms must be 128 bytes — two 64-byte float4x4s "
                  "(see resources/shaders/ShaderInterop.slang).");
    static_assert(sizeof(CameraUniforms) % 16 == 0,
                  "CameraUniforms must be 16-byte aligned (cbuffer std140 rule).");
    static_assert(sizeof(OpenPBRMaterialGPU) == 80,
                  "OpenPBRMaterialGPU must be 80 bytes (5 rows of 16).");
    static_assert(sizeof(OpenPBRMaterialGPU) % 16 == 0,
                  "OpenPBRMaterialGPU must be 16-byte aligned (§23 cbuffer / "
                  "structured-buffer alignment rule).");

    // Sentinel bindless slot meaning "no texture bound for this lobe".
    // The closesthit checks the matching MaterialFlag bit before
    // sampling, so this value is only a fallback — it should never be
    // dereferenced through the bindless table.
    constexpr uint INVALID_BINDLESS_TEXTURE = ~0u;

    }  // namespace pyxis::shaderinterop
#endif

#ifndef __cplusplus
static const uint INVALID_BINDLESS_TEXTURE = 0xFFFFFFFFu;
#endif

#endif  // PYXIS_SHADER_INTEROP_SLANG
