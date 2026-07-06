// Pyxis renderer — SceneBindings: the RTX-alignment design's Set-0
// immutable binding layout + per-frame binding set, shared verbatim by
// every RT pass (rtx-realtime-alignment-design.md, "Full shader-rewrite
// mandate" + "WP2 implementation contract"). Replaces the pre-WP2 flat
// 0-36 single-set table both RaytracedGBufferPass and
// RaytracedLightingPass used to declare (and largely duplicate)
// independently: scene tables, the camera cbuffers, and the bindless
// texture array now live in ONE nvrhi::IBindingLayout + ONE
// nvrhi::IBindingSet, created/refreshed here.
//
// Set-0 slots (binding N — matches every RT pipeline's
// `[[vk::binding(N, 0)]]` declarations; see camera_ray.slang /
// shading.slang / dome_sample.slang / raytraced_anyhit.slang /
// raytraced_gbuffer.slang for the shader side):
//   0  CameraUniforms cbuffer
//   1  RaytracingAccelerationStructure (TLAS)
//   2  StructuredBuffer<OpenPBRMaterialGPU>  materials
//   3  StructuredBuffer<InstanceInfoGpu>     instance info
//   4  StructuredBuffer<MeshInfoGpu>         mesh info
//   5  StructuredBuffer<LightGpu>            lights
//   6  StructuredBuffer<uint>                mesh index pool
//   7  StructuredBuffer<float2>              mesh UVs (tight)
//   8  StructuredBuffer<VertexAttribGpu>     mesh vertex attribs
//   9  StructuredBuffer<float4>              mesh face normals
//   10 Texture2D<float4>                     dome env map
//   11 SamplerState                          dome sampler
//   12 SamplerState                          material sampler
//   13 Texture2D[BINDLESS_TEXTURES_CAP]      bindless textures
//   14 MotionVectorCameraUniforms cbuffer
//   15 RealTimeQualityUniforms cbuffer (WP2-final — RenderSettings::RealTimeQuality mirror)
//   16 RtxSamplingUniforms cbuffer (Phase B — RNG seed/frame index + TAA jitter)
//
// A pass's pipelineDesc.globalBindingLayouts carries THIS layout
// (Layout()) as its FIRST entry (descriptor set 0) and its OWN Set-1
// layout as the second; at trace time `state.bindings = {sceneSet,
// passSet}` in that same order (NVRHI: one binding set per layout,
// index-matched to the layout array).
//
// WP2-final — the layout's visibility mask now includes Compute (in
// addition to AllRayTracing) so CompositePass (a compute pass) can also
// bind this SAME Set-0 layout/set instead of duplicating the camera +
// lights + dome bindings it needs for its miss-pixel dome background.
// Purely additive on the Vulkan descriptor-set-layout stage mask — every
// existing RT pass's pipeline is unaffected.

#pragma once

#include "ShaderInterop.slang"

#include <Pyxis/Renderer/Descs/CameraDesc.h>
#include <Pyxis/Renderer/Descs/RenderSettings.h>  // WP2-final — RealTimeQuality param of Update().

#include <nvrhi/nvrhi.h>

#include <array>
#include <cstddef>
#include <cstdint>

namespace pyxis {

struct SceneResources;

class SceneBindings {
 public:
  explicit SceneBindings(nvrhi::IDevice* device);
  ~SceneBindings();
  SceneBindings(const SceneBindings&) = delete;
  SceneBindings& operator=(const SceneBindings&) = delete;

  // The immutable Set-0 layout (built once here in the ctor). Null iff
  // the ctor failed to create the layout or any Set-0 fallback resource
  // — see IsOperational().
  [[nodiscard]] nvrhi::IBindingLayout* Layout() const noexcept { return _layout.Get(); }

  [[nodiscard]] bool IsOperational() const noexcept { return _ok; }

  // CPU-frame-path per-frame refresh — called ONCE by
  // PyxisRenderer::RenderFrame BEFORE the graph walks (the same
  // convention as EnsureVisibilityBuffer / EnsureFeaturePipeline), never
  // from inside a pass's Execute(). Uploads CameraUniforms
  // (worldFromView / viewFromClip / viewFromWorld / exposure /
  // pixelSpreadRadians / projectionMode — the ONE derivation both RT
  // passes used to duplicate pre-WP2) and MotionVectorCameraUniforms
  // (bit-exact `cameraStatic` gate against ITS OWN history, moved here
  // from RaytracedLightingPass — see ShaderInterop.slang's
  // MotionVectorCameraUniforms doc comment for the full contract), then
  // rebuilds the cached Set-0 binding set iff the borrowed scene
  // pointers or the bindless-texture-array fingerprint changed since
  // last frame. `outputHeight` feeds the ray-cone pixelSpreadRadians
  // derivation (the display target's height — same expression both RT
  // passes used pre-WP2).
  //
  // Returns the ready-to-bind Set-0 handle, or null when the layout
  // failed at construction OR `res.tlas` is null (nothing to trace yet
  // — callers take their existing "no TLAS" early-out on a null return,
  // exactly as they did on `res.tlas == nullptr` before WP2).
  // `settings` (WP2-final; widened from a bare `RealTimeQuality` reference —
  // RTX-alignment design "unexposed intensity" clamp fix) supplies BOTH the
  // shared gQuality cbuffer (binding 15) contents (`settings.realTimeQuality`
  // — uploaded unconditionally every call, cheap at 48 bytes, avoids a
  // second change-detection snapshot; see RealTimeQualityUniforms in
  // ShaderInterop.slang for which fields shaders actually read today) AND
  // the exposure-model fields (`exposureMode`/`exposureResponsivity`) this
  // function now needs to descale `maxRayIntensityDirect/Indirect/
  // Reflections` from ovrtx's EXPOSED-intensity convention into Pyxis's own
  // unexposed-radiance clamp space before upload — see ExposureMath.h's
  // ComputeEffectiveExposureScale, the shared formula also driving
  // TonemapPass::Execute's display path.
  // `frameIndex` / `taaJitterEnabled` (Phase B) feed the gSampling cbuffer
  // (binding 16): frameIndex truncates PassContext::frameIndex to 32 bits
  // (the plan §12.1 PCG32 seed term every stochastic estimator seeds
  // from); taaJitterEnabled gates the scrambled-Halton(2,3) sub-pixel
  // jitter sequence (plan §12.4) — false writes jitterX/jitterY = 0
  // exactly, keeping the default (TAA off) path byte-equal. `seed`
  // (RTX-alignment design, "interior illumination" follow-up) is the
  // OTHER gSampling.rngSeed term — RenderSettings::seed, sourced from
  // Configuration::render.seed at the call site (PyxisRenderer.cpp); 0 is
  // treated as "unset" and falls back to DEFAULT_RNG_SEED (SceneBindings.cpp)
  // rather than seeding PCG32 with zero. See RtxSamplingUniforms's doc
  // comment in ShaderInterop.slang.
  [[nodiscard]] nvrhi::BindingSetHandle Update(nvrhi::ICommandList* commandList,
                                               const CameraDesc& camera, uint32_t outputHeight,
                                               const SceneResources& res,
                                               const RenderSettings& settings,
                                               uint64_t frameIndex, bool taaJitterEnabled,
                                               uint32_t seed);

  // DLSS Stage 2a — CPU-side snapshot of the CONFORMED (aspect-ratio-
  // corrected) camera matrices + the previous-frame reprojection matrix
  // the LAST Update() call used, exposed so DlssPass can build
  // sl::Constants without duplicating this class's own aspect-conform
  // math or its cameraStatic/prevClipFromWorld history tracking (same
  // "one function, two consumers" reasoning as Passes/CameraJitter.h).
  // Meaningless (identity/zero) before the first Update() call.
  [[nodiscard]] const shaderinterop::float4x4& LastCameraViewToClip() const noexcept {
    return _lastCameraViewToClip;
  }
  [[nodiscard]] const shaderinterop::float4x4& LastClipToCameraView() const noexcept {
    return _lastClipToCameraView;
  }
  [[nodiscard]] const shaderinterop::float4x4& LastWorldFromView() const noexcept {
    return _lastWorldFromView;
  }
  // The prevClipFromWorld value THIS frame's motion vectors were computed
  // against (i.e. the value uploaded into MotionVectorCameraUniforms this
  // call) — NOT the same as _prevClipFromWorld below, which Update()
  // overwrites with the CURRENT frame's clipFromWorld at the end of the
  // same call (in preparation for the NEXT frame).
  [[nodiscard]] const shaderinterop::float4x4& LastPrevClipFromWorldForMotionVectors()
      const noexcept {
    return _lastPrevClipFromWorldForMotionVectors;
  }

 private:
  nvrhi::IDevice* _device = nullptr;
  nvrhi::BindingLayoutHandle _layout;

  nvrhi::BufferHandle _cameraUniformsBuffer;
  nvrhi::BufferHandle _motionVectorCameraBuffer;
  // History for the cameraStatic gate (ShaderInterop.slang's
  // MotionVectorCameraUniforms doc comment) — moved here from
  // RaytracedLightingPass (WP2-core): this is now the ONLY upload
  // point, so there is exactly one "previous frame" to track.
  shaderinterop::float4x4 _prevClipFromWorld{};
  bool _hasPrevCamera = false;

  // DLSS Stage 2a — see the public Last*() accessors above.
  shaderinterop::float4x4 _lastCameraViewToClip{};
  shaderinterop::float4x4 _lastClipToCameraView{};
  shaderinterop::float4x4 _lastWorldFromView{};
  shaderinterop::float4x4 _lastPrevClipFromWorldForMotionVectors{};

  // WP2-final — the shared RealTimeQuality cbuffer (binding 15), written
  // every Update() call from the caller-supplied RenderSettings::RealTimeQuality.
  nvrhi::BufferHandle _realTimeQualityBuffer;

  // Phase B — the shared RtxSamplingUniforms cbuffer (binding 16), written
  // every Update() call from `frameIndex` + the internally-derived Halton
  // jitter (see Update()'s doc comment).
  nvrhi::BufferHandle _rtxSamplingBuffer;

  // 1-element fallback buffers/texture/samplers — one per Set-0 scene
  // resource, same shapes + default contents as the per-pass fallbacks
  // BOTH RT passes carried independently pre-WP2 (see
  // RaytracedLightingPass.cpp's history for the byte-for-byte defaults:
  // default-grey material, disabled-light sentinel, zero instance/mesh
  // records, white 1x1 dome texture "so tint-only domes stay lit").
  // Bindings 4 (mesh info) and 8 (vertex attribs) share ONE 32-byte
  // fallback (MeshInfoGpu and VertexAttribGpu are both 32 B) exactly as
  // the pre-WP2 per-pass fallback did.
  nvrhi::BufferHandle _fallbackMaterialBuffer;
  nvrhi::BufferHandle _fallbackInstanceInfoBuffer;
  nvrhi::BufferHandle _fallbackLightBuffer;
  nvrhi::BufferHandle _fallbackStride32Buffer;  // shared: mesh info (4) + vertex attribs (8)
  nvrhi::BufferHandle _fallbackMeshIndicesBuffer;      // binding 6
  nvrhi::BufferHandle _fallbackMeshUvsBuffer;          // binding 7 (tight float2)
  nvrhi::BufferHandle _fallbackMeshFaceNormalsBuffer;  // binding 9
  nvrhi::TextureHandle _fallbackDomeTexture;
  // Shared fallback sampler for BOTH the dome sampler (11) and the
  // material sampler (12) when GpuScene hasn't created its own yet —
  // same simplification the pre-WP2 lighting pass used (identical
  // filter settings; only the addressing differs, and the fallback
  // case is a truly-empty scene where addressing mode is moot).
  nvrhi::SamplerHandle _fallbackSampler;

  nvrhi::BindingSetHandle _set;

  // Snapshot of the borrowed scene pointers participating in the Set-0
  // binding set — mismatch invalidates the cached set (lazy-allocation
  // flips, TLAS rebuild), same scheme every pass's BindingsSnapshot uses.
  enum class BindingSlot : std::size_t {
    Tlas = 0,
    Materials,
    InstanceInfo,
    MeshInfo,
    Lights,
    MeshIndices,
    MeshUvs,
    MeshVertexAttribs,
    MeshFaceNormals,
    DomeTexture,
    DomeSampler,
    MaterialSampler,
    Count,
  };
  static constexpr std::size_t BINDING_SLOT_COUNT = static_cast<std::size_t>(BindingSlot::Count);
  using BindingsSnapshot = std::array<const void*, BINDING_SLOT_COUNT>;
  BindingsSnapshot _lastBindings{};
  // M8a bindless textures fingerprint — see RaytracedLightingPass's
  // identical comment for why count-alone doesn't catch slot-pointer
  // churn from the free-list slot-recycle path.
  std::uint64_t _lastBindlessTextureFingerprint = 0;

  bool _ok = false;  // true iff the ctor created the layout + every fallback.
};

}  // namespace pyxis
