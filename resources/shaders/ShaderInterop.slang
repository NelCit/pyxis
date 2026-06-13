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
    using float3x4 = hlslpp::float3x4;  // 3 SIMD rows = 48 B (InstanceInfoGpu)
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
//
// M7 follow-up — extended with the data the AOV inspector needs.
// Closesthit fills `normalWorld` + `instanceId`; miss leaves them at
// the sentinel defaults (zero normal, ~0u instance id, hitT = -1).
struct HitInfo {
    float3 color;
    float  hitT;     // ray-distance at hit; -1.0 sentinel from miss shader.
    float3 normalWorld;  // populated by closesthit; (0,0,0) on miss
    uint   instanceId;   // populated by closesthit; 0xFFFFFFFF on miss
    // M7 follow-up — extra AOV channels the inspector pipes into
    // their own RWTextures. baseColor is the §11 OpenPBR baseColor
    // straight from the material table (pre-shading); worldHit is
    // WorldRayOrigin + RayTCurrent * WorldRayDirection cached so
    // raygen doesn't re-derive it. Miss leaves them at zero.
    //
    // Payload size note: 80 bytes total (the M29 recursionDepth +
    // V2.B transmittance fields grew it past the original 68) — far
    // past the 32-byte register-only payload ceiling for DXR. The
    // runtime spills to the on-chip ray stack on every TraceRay,
    // costing latency per leaf traversal step. Acceptable for the
    // M7 AOV inspector (one primary ray per pixel; AOV writes happen
    // ONCE per dispatch); the pass-split design keeps this payload
    // for secondary rays only — the visibility pass uses a thin
    // 20-byte payload instead.
    float3 baseColor;
    uint   materialId;   // material slot from gInstanceMaterial[instanceId]
    float3 worldHit;
    uint   elementId;    // PrimitiveIndex() — per-face id within the BLAS;
                         // ~0u on miss. Hydra's `HdAovTokens->elementId`.
    // M29 / V2.B.1 — RT specular reflection recursion bookkeeping.
    // Raygen sets to 0; closesthit's reflection branch (lands at M30)
    // increments + propagates so we don't recurse past
    // MAX_RECURSION_DEPTH = 3. Cheap (one uint = 4 bytes); the payload
    // is already spilled past the 32-byte register-only ceiling.
    uint   recursionDepth;  // 0 on primary; closesthit increments for traced rays
    // V2.B transmission — the fraction of light from BEHIND this surface
    // that reaches the eye through it, per channel. closesthit sets it to
    // (1-reflWeight)·(1-opacity); opaque surfaces and the miss shader
    // leave it at 0. raygen's front-to-back transmission loop folds it
    // into a running throughput instead of tracing a recursive
    // continuation ray, so stacked transparent layers cost loop
    // iterations rather than hardware recursion depth. See
    // raytraced_lighting_raygen.slang.
    float3 transmittance;   // 0 = opaque; >0 lets the surface behind show through
};

// VisibilityPayload — the THIN ray payload of the P4 RaytracedGBufferPass
// pipeline (20 bytes vs HitInfo's 80 — fits the register-only payload
// budget, no ray-stack spill). Shader-only for the same float3-trap
// reason as HitInfo: it never crosses the C++/SPIR-V boundary as a
// memory blob. The GBuffer raygen converts it into the 32-byte
// VisibilityGpu buffer record below; the GBuffer closesthit / miss /
// anyhit are compiled against this type (DXR requires the anyhit's
// payload type to match the pipeline's TraceRay payload type — see
// raytraced_anyhit.slang).
struct VisibilityPayload {
    float  hitT;            // RayTCurrent() at the closest hit; -1.0 = miss
    float2 bary;            // BuiltInTriangleIntersectionAttributes.barycentrics
    uint   instanceId;      // InstanceID(); ~0u on miss
    uint   primitiveIndex;  // PrimitiveIndex(); ~0u on miss
};
#endif

// CameraUniforms — uploaded per-frame to a constant buffer the raygen
// shader reads to build primary rays. M3 ships pinhole-only (no DOF,
// no jitter); the §43.3 reservation for `apertureFStop` lives on
// CameraDesc and gets wired into the shader at M9+.
//
// Layout: 192 bytes (three 64-byte float4x4s). All matrices are
// row-major + column-vector convention (§10):
//   pos_view  = mul(viewFromWorld, pos_world)
//   pos_clip  = mul(projFromView,  pos_view)
// The raygen shader needs both directions:
//   `worldFromView` (= inverse of CameraDesc::viewFromWorld) — used to
//      translate the ray origin / direction from view → world
//      (primary-ray construction).
//   `viewFromClip` (= inverse of CameraDesc::projFromView) — used to
//      translate NDC pixel coordinates into a view-space direction
//      (primary-ray construction).
//   `viewFromWorld` (= CameraDesc::viewFromWorld, FORWARD) — used to
//      transform world-space hit position + normal into eye space for
//      the Neye / Peye AOVs (Hydra-canonical, comp tools assume
//      eye-space normals + positions).
// We pass the inverses pre-computed by the C++ side rather than
// computing them in shader; static cameras → one matrix-inverse per
// frame at most.
struct CameraUniforms {
    float4x4 worldFromView;     // inverse of CameraDesc::viewFromWorld
    float4x4 viewFromClip;      // inverse of CameraDesc::projFromView
    float4x4 viewFromWorld;     // forward — for Neye / Peye AOVs
    // Row 12 — photographic exposure adjustment in stops. Raygen
    // multiplies post-shading radiance by 2^exposure before the
    // ACES tonemap. 0 = ×1 = no adjustment. Mirrors
    // CameraDesc::exposure / UsdGeomCamera::exposure. Three pad
    // floats keep the cbuffer 16-byte aligned (std140 rule).
    float exposure;
    // M9-fidelity: pixel angular spread (radians per pixel along the
    // vertical axis). Drives ray-cone-footprint LOD selection in the
    // closesthit so distant surfaces sample mip levels instead of
    // mip 0 (the SampleLevel(s, uv, 0)-pinned path in M5–M9 aliased
    // hard at distance). Computed by the C++ side as
    //   pixelSpreadRadians = 2 × atan(0.5 / focalLengthInPixels)
    // ≈ 2 × tan(fovY / 2) / imageHeight for a standard pinhole.
    float pixelSpreadRadians;
    // V2.A.20 — UsdGeomCamera.projection. 0 = perspective (default;
    // raygen builds rays from origin through NDC), 1 = orthographic
    // (raygen takes the inverse-clip-projected NDC point as the
    // per-pixel ray ORIGIN in view space + a constant -Z direction).
    // The cbuffer is uint-friendly: HLSL's `uint`-via-`asuint`
    // pattern reads a float slot as a uint without packing trouble.
    uint  projectionMode;
    float _camPad1;
};

// FrameUiUniforms — per-frame viewer-only state the lighting raygen
// reads (cursor pixel for the picker gate; since the P3 split the
// debugViewMode / worldPosPeriod fields are TonemapUniforms' domain —
// kept here for the frozen layout, written 0). Lives in its own cbuffer at
// binding 19 so future per-frame camera knobs (samplesPerPixel,
// jitter offset, accumulation generation, ...) added to CameraUniforms
// can grow without colliding with UI state. M7 follow-up: split out
// of CameraUniforms after the audit flagged the conceptual conflict
// (UI state hitching a ride on the camera matrices).
//
// Pre-fix this lived as a 16-byte tail on CameraUniforms. Restored
// CameraUniforms to its original 128-byte two-matrix shape; the four
// uint slots below carry the viewer state at a dedicated binding.
struct FrameUiUniforms {
    // Row 0 — picker + display selector.
    uint  mousePixelX;          // top-left origin pixel coord, or PICK_PIXEL_NONE if no hover
    uint  mousePixelY;
    uint  debugViewMode;        // P3: dead here (Tonemap's domain); written 0
    uint  _reservedUi0;
    // Row 1 — AOV-specific knobs the editor surfaces as sliders.
    // worldPosPeriod: P3 — dead here (TonemapUniforms carries the live
    // copy); written 0. Other fields reserved for future per-AOV display
    // knobs.
    float worldPosPeriod;
    uint  _reservedUi1;
    uint  _reservedUi2;
    uint  _reservedUi3;
};

// SSAA box-downsample compute-pass uniforms (viewer mode). The pass
// reads a `factor`×-supersampled color texture and writes the
// backbuffer-resolution resolve. 16 bytes (one cbuffer row).
struct SsaaDownsampleUniforms {
    uint destWidth;    // output (backbuffer) dimensions in texels
    uint destHeight;
    uint factor;       // supersample factor per axis (2..4); 1 = passthrough
    uint _ssaaPad0;
};

// Auto-exposure (AutoExposurePass) — fixed-point log-luminance reduction.
// The reduction accumulates sum(log2(luminance)) over LIT pixels as a
// DETERMINISTIC integer (so the parallel InterlockedAdd is order-independent
// → byte-equal even with auto-exposure on). Per pixel we store
// (log2(lum) + OFFSET) * SCALE as a uint; OFFSET shifts the [-30,30] log range
// to non-negative, SCALE gives fractional bits. The tonemap recovers
// avgLog2Lum = (sum / SCALE) / count − OFFSET.
// OFFSET shifts log2(lum) non-negative; LIT pixels have lum > MIN_LUM so
// log2 >= -13.29, hence offset 14. The encoded per-pixel value is clamped to
// [LOG_MIN, LOG_MAX] before scaling so a firefly can't overflow the uint32
// global sum: max encoded ≈ (14+20)*16 = 544/pixel keeps sum < 2^32 for any
// resolution up to ~7.9M texels (4K + 2x SSAA), Pyxis's ceiling.
#define AUTO_EXPOSURE_LOG_OFFSET 14.0
#define AUTO_EXPOSURE_LOG_SCALE  16.0
#define AUTO_EXPOSURE_LOG_MIN    (-14.0)
#define AUTO_EXPOSURE_LOG_MAX    20.0
// Below this luminance a pixel is treated as background/unlit and excluded from
// the average (keeps the dark background from dragging the key down).
#define AUTO_EXPOSURE_MIN_LUM    1e-4

// AutoExposurePass compute uniforms — source (HDR radiance) dimensions. The
// stats buffer (uint2: [0]=fixed-point log-lum sum, [1]=lit-pixel count) is a
// separate UAV the pass clears + accumulates and the tonemap reads.
struct AutoExposureUniforms {
    uint srcWidth;
    uint srcHeight;
    uint _aePad0;
    uint _aePad1;
};

// Tonemap compute-pass uniforms (P3 pass split — the display transform
// extracted from raygen's inline branch). exposure mirrors
// CameraUniforms.exposure (photographic stops, applied as 2^exposure
// before the ACES tonemap); debugViewMode / worldPosPeriod mirror the
// FrameUiUniforms fields the raygen display branch used to read.
// 32 bytes (two cbuffer rows).
struct TonemapUniforms {
    // Row 0 — display transform inputs.
    float exposure;        // photographic stops; gain = 2^exposure
    uint  debugViewMode;   // DEBUG_VIEW_COLOR / NORMAL / DEPTH / ...
    float worldPosPeriod;  // WorldPos sin-encode period (scene units)
    uint  destWidth;       // output (display target) dimensions in texels
    // Row 1 — dims tail + auto-exposure (reuses the former reserved pads, so
    // the 32-byte layout is unchanged).
    uint  destHeight;
    uint  autoExposureEnabled;  // 0 = use `exposure` as-is; 1 = add the auto EV
    float autoExposureKey;      // target middle-grey (0.18 default)
    uint  _pad2;
};

// Debug-view selector mirrored on both sides. The combo in ImGuiHost's
// Editor panel writes one of these into camera.debugViewMode; the
// TonemapPass branches on it to compute the BGRA8 display output
// (gOutput) from the fp32 linear radiance + the raw per-AOV textures.
static const uint DEBUG_VIEW_COLOR        = 0;   // post-tonemap radiance
static const uint DEBUG_VIEW_NORMAL       = 1;   // world normal as (n*0.5+0.5)
static const uint DEBUG_VIEW_DEPTH        = 2;   // 1/depth grayscale
static const uint DEBUG_VIEW_PRIM_ID      = 3;   // hashed prim-id colour (Hydra primId)
static const uint DEBUG_VIEW_MATERIAL_ID  = 4;   // hashed material-id colour
static const uint DEBUG_VIEW_BASE_COLOR   = 5;   // raw OpenPBR baseColor (no shading)
static const uint DEBUG_VIEW_WORLD_POS    = 6;   // worldHit / 10 then frac for visualisation
// Tier 1 Hydra-canonical (DCC compositing tools query these by name).
static const uint DEBUG_VIEW_ALPHA        = 7;   // 1.0 on hit, 0.0 on miss
static const uint DEBUG_VIEW_ELEMENT_ID   = 8;   // hashed face-id colour
static const uint DEBUG_VIEW_NORMAL_EYE   = 9;   // eye-space normal as (n*0.5+0.5)
static const uint DEBUG_VIEW_WORLD_POS_EYE = 10; // sin-encoded eye-space position

// Sentinel for camera.mousePixel{X,Y} meaning "mouse outside viewport".
// Raygen short-circuits the pick-write when either component matches.
static const uint PICK_PIXEL_NONE = 0xFFFFFFFFu;

// P5 (passes-split design D2) — Vulkan specialization-constant ids,
// mirrored here per §23 (single interop file) so the C++ side's
// nvrhi::ShaderSpecialization::constantID values and the shaders'
// `[[vk::constant_id(N)]]` declarations can never desync. ALWAYS use
// these explicit ids — a bare `[SpecializationConstant]` gets an
// order-dependent auto-assigned SpecId that silently drifts.
//
// Ids are scoped PER SPIR-V MODULE (each .spv has its own SpecId
// namespace), so SSAA_FACTOR in ssaa_resolve.slang reuses id 0 without
// colliding with PROJECTION_MODE in the RT modules.
//
//   RT modules (which module DECLARES each constant):
static const uint SPEC_ID_PROJECTION_MODE          = 0;  // camera_ray.slang (both raygens via include)
static const uint SPEC_ID_MAX_RAY_RECURSION        = 1;  // shading.slang (lighting raygen + closesthit)
static const uint SPEC_ID_MAX_TRANSPARENT_SEGMENTS = 2;  // raytraced_lighting_raygen.slang
static const uint SPEC_ID_REFL_MAX_SEGMENTS        = 3;  // shading.slang (lighting raygen + closesthit)
static const uint SPEC_ID_OPENPBR_FEATURES         = 4;  // openpbr_material.slang (lighting raygen + closesthit)
//   Compute modules:
static const uint SPEC_ID_SSAA_FACTOR              = 0;  // ssaa_resolve.slang

// Q2 OpenPBR-complete (openpbr-complete-design.md) — feature bits of
// the OPENPBR_FEATURE_MASK specialization constant
// (SPEC_ID_OPENPBR_FEATURES above). Each bit compiles one closure
// block of openpbr_material.slang in/out at pipeline-creation time.
// INVARIANT (load-bearing for the Q3 runtime toggles): a bit that is
// OFF produces EXACTLY the same image as that feature's weight
// parameter authored 0 (COAT off == coat_weight 0, FUZZ off ==
// fuzz_weight 0, …) — the shader short-circuits zero weights to the
// identical floats. The metal F82 lobe and the exact dielectric
// Fresnel are ALWAYS on (core correctness, not gated).
static const uint OPENPBR_FEATURE_COAT        = 1u << 0;  // coat layer (GGX + absorption + darkening + roughening)
static const uint OPENPBR_FEATURE_FUZZ        = 1u << 1;  // Zeltner LTC sheen
static const uint OPENPBR_FEATURE_TRANSMISSION = 1u << 2; // translucent base → T_pbr composite
static const uint OPENPBR_FEATURE_SUBSURFACE  = 1u << 3;  // SSS-as-diffuse fallback
static const uint OPENPBR_FEATURE_ANISOTROPY  = 1u << 4;  // anisotropic GGX (α_t/α_b)
static const uint OPENPBR_FEATURE_EON_DIFFUSE = 1u << 5;  // EON diffuse (off = Lambert)
static const uint OPENPBR_FEATURES_ALL =
    OPENPBR_FEATURE_COAT | OPENPBR_FEATURE_FUZZ | OPENPBR_FEATURE_TRANSMISSION
    | OPENPBR_FEATURE_SUBSURFACE | OPENPBR_FEATURE_ANISOTROPY
    | OPENPBR_FEATURE_EON_DIFFUSE;

// PickResult — one-element RWStructuredBuffer the raygen writes when
// the dispatched pixel matches camera.mousePixel. The viewer copies
// this into a staging buffer each frame and reads back the previous
// frame's value (one-frame stale; fine for human-paced UI). Carries
// EVERY AOV channel the inspector exposes plus the pixel coords so
// the editor's hover readout doesn't need a second cbuffer.
struct PickResult {
    // Row 0 — final radiance (HDR, pre-tonemap) + ray distance.
    float colorR;
    float colorG;
    float colorB;
    float depth;        // ray distance at hit; -1 on miss

    // Row 1 — world-space normal (xyz) + Hydra primId (per-instance
    // slot from TLAS InstanceID() at hit; 0xFFFFFFFF on miss).
    float normalX;
    float normalY;
    float normalZ;
    uint  instanceId;   // 0xFFFFFFFF on miss

    // Row 2 — raw OpenPBR baseColor (pre-shading) + material slot.
    float baseColorR;
    float baseColorG;
    float baseColorB;
    uint  materialId;   // 0xFFFFFFFF on miss

    // Row 3 — world-space hit position + pad.
    float worldHitX;
    float worldHitY;
    float worldHitZ;
    uint  _pickPad0;

    // Row 4 — pixel coords (mirrors camera.mousePixel{X,Y} so the
    // editor doesn't have to track its own copy of the cursor).
    uint  pixelX;
    uint  pixelY;
    uint  _pickPad1;
    uint  _pickPad2;

    // Row 5 — eye-space normal (Hydra Neye) + Hydra elementId
    // (PrimitiveIndex(), per-face id within the BLAS, ~0u on miss).
    float normalEyeX;
    float normalEyeY;
    float normalEyeZ;
    uint  elementId;

    // Row 6 — eye-space hit position (Hydra Peye) + alpha
    // (1.0 hit / 0.0 miss; binary today, future-proofed for
    // transmission / coverage).
    float worldPosEyeX;
    float worldPosEyeY;
    float worldPosEyeZ;
    float alpha;
};

// OpenPBRMaterialGPU — the bytewise layout the closesthit reads via
// the structured-buffer material table. Plan §11. The C++ side packs
// an OpenPBRMaterialDesc into this shape inside MaterialTable; the
// shader side indexes by `materialId` (set on the instance custom
// index) and reads `flags` to short-circuit lobe evaluation.
//
// Layout is 240 bytes (15 rows of 16). Rows 0-7 are the FROZEN
// 128-B M9/V2.A.24 base layout (base color + flags, bindless texture
// slots, base/dielectric/coat/emission scalars, emission RGB, UV
// transform); rows 8-14 are the Q1 OpenPBR-complete extension
// (specular/coat/fuzz/transmission/subsurface tints + weights). The
// full field-by-field row map is documented on the rows 8-14 block
// inside the struct below.
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
    // V2.A.23 follow-up — normal-map strength (OmniPBR `bump_factor`).
    // Scales the tangent-space normal's XY before the TBN transform.
    // 1.0 = full authored bump (default). Was `_reserved0`.
    float normalStrength;

    // Row 5 — M9 emission RGB. UsdPreviewSurface authors the emissive
    // tint as a color3f (`emissiveColor`); the closesthit emits
    // `emissionColor × emissionLuminance × (sampled emissionTex)`
    // when MATERIAL_FLAG_EMISSIVE is set. emissionLuminance lives on
    // row 3 above as a scalar multiplier (set to 1.0 by default for
    // UsdPreviewSurface materials with non-zero emissiveColor).
    float emissionR;
    float emissionG;
    float emissionB;
    uint  _reserved1;

    // Row 6 + 7 — V2.A.24 UV transform (OmniPBR texture_scale /
    // texture_rotate / texture_translate). Applied to the interpolated
    // UV before every texture sample as scale → rotate → translate
    // (matching MaterialX / OmniPBR convention) when
    // MATERIAL_FLAG_HAS_UV_TRANSFORM is set. Default = identity
    // (scale 1, rotate 0, translate 0).
    float uvScaleX;
    float uvScaleY;
    float uvRotateRadians;
    float uvTranslateX;
    float uvTranslateY;
    float _reserved2;
    float _reserved3;
    float _reserved4;

    // ------------------------------------------------------------------
    // Rows 8-14 — OpenPBR-complete parameter block (Q1,
    // openpbr-complete-design.md). Grows the struct 128 B -> 240 B
    // (15 rows of 16 — the tightest multiple-of-16 that fits the full
    // v1.1.1 parameter set; rows 0-7 above are byte-identical to the
    // 128-B layout so the Q1 packing change is image-safe). Packed by
    // PackMaterialGpu (Private/GpuScene/Detail.h) and, since Q2, READ
    // by the openpbr_material.slang closure stack (these rows feed the
    // specular/coat/fuzz/transmission/subsurface lobes). Row map:
    //   Row  8 — specularColor RGB + specularWeight
    //   Row  9 — coatColor RGB + coatIor
    //   Row 10 — coatDarkening + fuzzWeight + fuzzRoughness
    //            + baseDiffuseRoughness
    //   Row 11 — fuzzColor RGB + specularRoughnessAnisotropy
    //   Row 12 — transmissionColor RGB + subsurfaceWeight
    //   Row 13 — subsurfaceColor RGB + baseWeight
    //   Row 14 — transmissionWeight + 3 reserved slots
    // (geometry_thin_walled travels as MATERIAL_FLAG_THIN_WALLED, not
    // as a struct field. _reserved1-4 on rows 5/7 above stay zero.)
    // ------------------------------------------------------------------

    // Row 8 — specular tint (OpenPBR `specular_color`) + weight.
    float specularColorR;
    float specularColorG;
    float specularColorB;
    float specularWeight;

    // Row 9 — coat tint (OpenPBR `coat_color` = squared normal-
    // incidence transmittance) + coat IOR (default 1.6).
    float coatColorR;
    float coatColorG;
    float coatColorB;
    float coatIor;

    // Row 10 — coat darkening (default 1 = physical darkening ON),
    // fuzz scalars, EON diffuse roughness.
    float coatDarkening;
    float fuzzWeight;
    float fuzzRoughness;
    float baseDiffuseRoughness;

    // Row 11 — fuzz tint + GGX anisotropy
    // (OpenPBR `specular_roughness_anisotropy`).
    float fuzzColorR;
    float fuzzColorG;
    float fuzzColorB;
    float specularRoughnessAnisotropy;

    // Row 12 — transmission tint (depth-0 constant-tint semantics)
    // + subsurface selector weight.
    float transmissionColorR;
    float transmissionColorG;
    float transmissionColorB;
    float subsurfaceWeight;

    // Row 13 — subsurface albedo (diffuse-fallback rho) + base weight.
    float subsurfaceColorR;
    float subsurfaceColorG;
    float subsurfaceColorB;
    float baseWeight;

    // Row 14 — translucent-base selector weight + reserved tail.
    float transmissionWeight;
    float _reserved5;
    float _reserved6;
    float _reserved7;
};

// LightGpu — packed light entry the closesthit reads via the
// structured-buffer light table at binding 5 (M7). Same scalar-only
// rule as OpenPBRMaterialGPU (avoids the hlslpp::float3 16-vs-12-byte
// trap). Layout is 96 bytes (6 rows of 16) — row 5 carries the
// UsdLuxShapingAPI cone terms added at M9.
//
// M7 ships a deliberately **simple shading model** (no NdotL, no
// shadow rays, no NEE, no MIS, no importance sampling — see
// shading.slang for the full closeup). Each light contributes
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

    // Row 4 — Rect axisV + Dome rotation slot.
    float axisVx;
    float axisVy;
    float axisVz;
    // M9-fidelity: per-prim dome rotation around world-Y in radians.
    // Authored on the DomeLight's xformOpOrder. Miss shader rotates
    // the lat-long sample direction by -domeRotationYRadians.
    // Ignored for non-Dome kinds.
    float domeRotationYRadians;

    // Row 5 — UsdLuxShapingAPI cone (M9-fidelity). Spotlight
    // half-angle stored as cos(angle) for cheap dot-product
    // comparison in closesthit. cosOuter == 0.0 means "no cone"
    // (cos 90°) and the closesthit path skips the falloff. Inner
    // edge derives from softness: a 0..1 fraction of the half-
    // angle that defines the smooth-step interior. Cone axis is
    // derived per-kind in closesthit (Distant: -L.dir; Rect/Disk/
    // Sphere: normalize(cross(axisU, axisV))) so we don't carry
    // extra data here.
    float shapingConeCosOuter;       // cos(coneHalfAngle)
    float shapingConeCosInner;       // cos(coneHalfAngle × (1 - softness))
    float _reserved1;
    float _reserved2;
};

// Light kind enum mirroring pyxis::LightDesc::Kind. Closesthit
// branches on this for the M7-simple per-kind weighting. The
// position-based kinds (Rect / Cylinder / Geometry / Portal +
// any future area light) all share the same closesthit path:
// "point light at L.position with 1/r² falloff." Distant uses
// parallel rays; Dome contributes uniform ambient.
static const uint LIGHT_KIND_DISTANT  = 0;
static const uint LIGHT_KIND_DOME     = 1;
static const uint LIGHT_KIND_RECT     = 2;
static const uint LIGHT_KIND_CYLINDER = 3;  // §22 MINOR-additive — UsdLuxCylinderLight
static const uint LIGHT_KIND_GEOMETRY = 4;  // §22 MINOR-additive — UsdLuxGeometryLight
static const uint LIGHT_KIND_PORTAL   = 5;  // §22 MINOR-additive — UsdLuxPortalLight

// MaterialFlag bitmask mirror — sources/pyxis_renderer/Private/Materials/
// MaterialFlag.h is the C++ source of truth; the closesthit reads
// `mat.flags & MATERIAL_FLAG_*` to short-circuit lobe + texture lookups.
// Keep these in lockstep with the C++ enum.
static const uint MATERIAL_FLAG_DOUBLE_SIDED            = 1u << 0;
static const uint MATERIAL_FLAG_HAS_BASE_COLOR_MAP      = 1u << 1;
static const uint MATERIAL_FLAG_HAS_NORMAL_MAP          = 1u << 2;
static const uint MATERIAL_FLAG_HAS_METALLIC_MAP        = 1u << 3;
static const uint MATERIAL_FLAG_HAS_ROUGHNESS_MAP       = 1u << 4;
static const uint MATERIAL_FLAG_HAS_EMISSION_MAP        = 1u << 5;
static const uint MATERIAL_FLAG_HAS_OPACITY_MAP         = 1u << 6;
static const uint MATERIAL_FLAG_HAS_TRANSMISSION_MAP    = 1u << 7;
static const uint MATERIAL_FLAG_HAS_COAT_ROUGHNESS_MAP  = 1u << 8;
static const uint MATERIAL_FLAG_ALPHA_TESTED            = 1u << 9;
static const uint MATERIAL_FLAG_COAT_ENABLED            = 1u << 10;
static const uint MATERIAL_FLAG_TRANSMISSION_ENABLED    = 1u << 11;
static const uint MATERIAL_FLAG_EMISSIVE                = 1u << 12;
static const uint MATERIAL_FLAG_FLIP_TANGENT_U          = 1u << 13;
static const uint MATERIAL_FLAG_FLIP_TANGENT_V          = 1u << 14;
static const uint MATERIAL_FLAG_HAS_UV_TRANSFORM        = 1u << 15;
static const uint MATERIAL_FLAG_WORLD_PROJECTION        = 1u << 16;
static const uint MATERIAL_FLAG_OBJECT_PROJECTION       = 1u << 17;
// Q1 OpenPBR-complete — OpenPBR `geometry_thin_walled`. The Q2
// transmission path treats the surface as a thin sheet (ignore the
// underside; transmission_color = through-sheet transmittance).
static const uint MATERIAL_FLAG_THIN_WALLED             = 1u << 18;

// InstanceInfoGpu — packed per-instance record at binding 4 (P2 buffer
// packing; replaces the two parallel per-instance uint side-tables
// gInstanceMaterial + gInstanceMesh). Indexed by `InstanceID()` (= the
// §15 instance slot). Layout is 64 bytes (4 rows of 16):
//   Rows 0-2 — `objectToWorld`, the instance object→world 3×4 affine
//   (owner-directed: a real float3x4, NOT 12 scalars — the scalar-only
//   rule below exists for the float3 16-vs-12-byte trap; hlslpp's
//   float3x4 is 3 SIMD rows = 48 bytes, byte-identical to Slang's
//   row-major float3x4, same as the float4x4s in CameraUniforms).
//   Stored row-major with EXACTLY the float values the TLAS instance
//   desc transform carries (RebuildTlasIfDirty writes both from the
//   same hlslpp::store). P2 populates it for the P4 visibility-buffer
//   split (whose lighting raygen has no ObjectToWorld3x4() intrinsic);
//   no shader reads it yet.
//   Row 3 — the §15 material slot + the M7-NdotL mesh slot the
//   closesthit/anyhit table lookups consume today.
struct InstanceInfoGpu {
    // Rows 0-2 — object→world 3×4 affine, row-major.
    float3x4 objectToWorld;
    // Row 3 — table slots (+ 2 reserved pads).
    uint  materialSlot;   // material slot for gMaterials (0 = sentinel grey)
    uint  meshSlot;       // mesh slot for gMeshInfo (0 = sentinel)
    uint  _pad0;
    uint  _pad1;
};

// MeshInfoGpu — packed per-mesh-slot record at binding 6 (P2 buffer
// packing; replaces the five parallel per-mesh uint offset tables
// gMeshFaceOffsets / gMeshUvOffsets / gMeshIndexOffsets /
// gMeshVertexNormalOffsets / gMeshTangentOffsets). One 32-byte cache
// line per hit instead of five buffer loads. Indexed by the mesh slot
// from InstanceInfoGpu. Layout is 32 bytes (2 rows of 16).
//
// faceOffset / uvOffset / vertexAttribOffset are running sums over the
// live meshes in ascending slot order (the side-table concatenation
// order — identical values to the old offset tables). vertexAttribOffset
// serves BOTH per-vertex streams: the vertex-normal and tangent uploads
// each pad to exactly vertexCount entries per mesh, so their offsets
// were equal by construction (verified in UploadMeshVertexAttribs; _r0
// is reserved as a second offset if they ever diverge). indexOffset is
// the mesh's element offset into the pooled index buffer (§14.5) —
// stable across commits because the pool never re-packs.
struct MeshInfoGpu {
    uint faceOffset;          // into gMeshFaceNormals (per-triangle)
    uint uvOffset;            // into gMeshUvs (per-vertex)
    uint indexOffset;         // into gMeshIndices (index-pool elements)
    uint vertexAttribOffset;  // into gMeshVertexAttribs (per-vertex)
    uint vertexCount;
    uint _r0;
    uint _r1;
    uint _r2;
};

// VertexAttribGpu — interleaved per-vertex shading attributes at
// binding 29 (P2 buffer packing; merges the two parallel padded float4
// streams gMeshVertexNormals + gMeshTangents). attrib[i] = {normal[i],
// tangent[i]}; exactly vertexCount entries per mesh. Owner-directed:
// real float4 fields, not 8 scalars — hlslpp::float4 is exactly 16
// bytes on both sides (the scalar-only rule guards the float3 trap
// only), so the layout matches Slang bit-for-bit just like the old
// float4 streams did. Layout is 32 bytes (2 rows of 16).
//
// normal.w is reserved (the old vertex-normal stream was float4 with w
// unused); tangent.w is the MikkTSpace bitangent-handedness sign. A
// zero-magnitude xyz remains the per-field "not authored" sentinel —
// a mesh can author normals but no tangents (or vice versa), and the
// closesthit detects each independently exactly as it did against the
// two separate streams.
struct VertexAttribGpu {
    float4 normal;    // xyz normal; w reserved (was the stream's pad)
    float4 tangent;   // xyz tangent; w = bitangent handedness (±1), MikkTSpace
};

// VisibilityGpu — one per-pixel record of the P4 visibility buffer
// (design D1): RWStructuredBuffer written by the RaytracedGBufferPass
// raygen at binding 2 of ITS layout, read by the RaytracedLightingPass
// raygen at binding 34. fp32-exact — NEVER quantized: the lighting
// raygen reconstructs worldHit = origin + hitT·dir and the barycentric
// attribute interpolation from these floats, and any rounding here
// would break the §33.7 byte-identical contract. Indexed row-major:
// `vis[y * width + x]`. Layout is 32 bytes (2 rows of 16); scalar-only
// per the float3-trap rule above.
struct VisibilityGpu {
    float hitT;            // RayTCurrent() of the primary hit; -1.0 = miss
    float baryX;           // triangle barycentrics (attribs.barycentrics)
    float baryY;
    uint  instanceId;      // InstanceID(); ~0u on miss
    uint  primitiveIndex;  // PrimitiveIndex(); ~0u on miss
    uint  _pad0;
    uint  _pad1;
    uint  _pad2;
};

// Bindless texture array capacity at RaytracedLightingPass binding 28.
// Cap chosen to comfortably cover v1 production scenes (Lobby 169,
// World Lobby ~2K, Moana production sets ~50K is the post-v1 §5 target via
// true createBindlessLayout). The closesthit indexes via
// `mat.baseColorTex`, gated on MATERIAL_FLAG_HAS_BASE_COLOR_MAP.
static const uint BINDLESS_TEXTURES_CAP = 4096u;

#ifdef __cplusplus
    // HitInfo is shader-only (see the `#ifndef __cplusplus` guard
    // above), so its size assert lives shader-side only too. Below
    // we lock the layout of the structs that *do* cross the C++ /
    // SPIR-V boundary as memory blobs (CameraUniforms via cbuffer,
    // OpenPBRMaterialGPU via structured buffer).
    static_assert(sizeof(CameraUniforms) == 208,
                  "CameraUniforms must be 208 bytes — three 64-byte float4x4s "
                  "(worldFromView + viewFromClip + viewFromWorld) plus a 16-byte "
                  "row carrying exposure (in stops) + 3 pad floats (std140 16-byte "
                  "alignment). See resources/shaders/ShaderInterop.slang.");
    static_assert(sizeof(CameraUniforms) % 16 == 0,
                  "CameraUniforms must be 16-byte aligned (cbuffer std140 rule).");
    static_assert(sizeof(FrameUiUniforms) == 32,
                  "FrameUiUniforms must be 32 bytes (2 cbuffer rows: picker / "
                  "display selector + per-AOV knobs).");
    static_assert(sizeof(FrameUiUniforms) % 16 == 0,
                  "FrameUiUniforms must be 16-byte aligned (cbuffer std140 rule).");
    static_assert(sizeof(TonemapUniforms) == 32,
                  "TonemapUniforms must be 32 bytes (2 cbuffer rows: exposure / "
                  "debug-view selector / WorldPos period / dest dims + pads).");
    static_assert(sizeof(TonemapUniforms) % 16 == 0,
                  "TonemapUniforms must be 16-byte aligned (cbuffer std140 rule).");
    static_assert(sizeof(PickResult) == 112,
                  "PickResult must be 112 bytes (7 rows of 16) — original 5 rows + "
                  "Tier 1 Hydra Neye/elementId on row 5, Peye/alpha on row 6. "
                  "See resources/shaders/ShaderInterop.slang.");
    static_assert(sizeof(PickResult) % 16 == 0,
                  "PickResult must be 16-byte aligned (structured-buffer rule).");
    static_assert(sizeof(OpenPBRMaterialGPU) == 240,
                  "OpenPBRMaterialGPU must be 240 bytes (15 rows of 16) — rows 0-7 "
                  "are the frozen 128-B M9/V2.A.24 layout (base + textures + UV "
                  "transform); rows 8-14 carry the Q1 OpenPBR-complete parameter "
                  "block (specular/coat/fuzz/transmission/subsurface tints + "
                  "weights). See the row map on the struct.");
    static_assert(sizeof(OpenPBRMaterialGPU) % 16 == 0,
                  "OpenPBRMaterialGPU must be 16-byte aligned (§23 cbuffer / "
                  "structured-buffer alignment rule).");
    static_assert(sizeof(LightGpu) == 96,
                  "LightGpu must be 96 bytes (6 rows of 16) — row 5 carries the "
                  "M9 UsdLuxShapingAPI cone terms. The GPU light table at "
                  "binding 5 is packed with this exact stride; see "
                  "PackLightGpu in Private/GpuScene/Detail.h.");
    static_assert(sizeof(LightGpu) % 16 == 0,
                  "LightGpu must be 16-byte aligned (§23 structured-buffer rule).");
    static_assert(sizeof(InstanceInfoGpu) == 64,
                  "InstanceInfoGpu must be 64 bytes (4 rows of 16) — rows 0-2 carry "
                  "the object→world 3×4 affine, row 3 the material/mesh slots. The "
                  "packed instance table at binding 4 is uploaded with this exact "
                  "stride; see RebuildTlasIfDirty / PackInstanceInfo in "
                  "Private/GpuScene/Commit.cpp.");
    static_assert(sizeof(InstanceInfoGpu) % 16 == 0,
                  "InstanceInfoGpu must be 16-byte aligned (§23 structured-buffer rule).");
    static_assert(sizeof(MeshInfoGpu) == 32,
                  "MeshInfoGpu must be 32 bytes (2 rows of 16) — the five legacy "
                  "per-mesh offset tables packed into one record; see UploadMeshInfo "
                  "in Private/GpuScene/Commit.cpp.");
    static_assert(sizeof(MeshInfoGpu) % 16 == 0,
                  "MeshInfoGpu must be 16-byte aligned (§23 structured-buffer rule).");
    static_assert(sizeof(VertexAttribGpu) == 32,
                  "VertexAttribGpu must be 32 bytes (2 rows of 16) — interleaved "
                  "per-vertex normal + tangent; see UploadMeshVertexAttribs in "
                  "Private/GpuScene/Commit.cpp.");
    static_assert(sizeof(VertexAttribGpu) % 16 == 0,
                  "VertexAttribGpu must be 16-byte aligned (§23 structured-buffer rule).");
    static_assert(sizeof(VisibilityGpu) == 32,
                  "VisibilityGpu must be 32 bytes (2 rows of 16) — the P4 visibility "
                  "buffer is allocated at width*height*32 with this exact stride; see "
                  "RaytracedGBufferPass::EnsureVisibilityBuffer.");
    static_assert(sizeof(VisibilityGpu) % 16 == 0,
                  "VisibilityGpu must be 16-byte aligned (§23 structured-buffer rule).");

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
