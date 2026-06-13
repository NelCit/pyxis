// Pyxis renderer — deferred first-hit lighting pass (P4 pass split).
//
// RT pipeline B of the visibility-buffer architecture (design D1).
// The raygen reads the per-pixel VisibilityGpu record the
// RaytracedGBufferPass produced (PassContext::visibility, binding 34),
// reconstructs every input the pre-split primary closesthit consumed,
// and runs the shared shading (shading.slang) at recursion depth 0 —
// shadow / AO / mirror-reflection rays and the front-to-back
// transparency continuation all trace from here. The megakernel
// closesthit survives as this pipeline's hit shader for SECONDARY rays
// only (transparent continuation + reflections); miss[0] serves
// secondary rays the dome background, miss[1] is the shadow-visibility
// miss. Refactor of the pre-split PathTracePass — it inherits the
// binding machinery, fallbacks, picker staging and the fp32
// linear-radiance scratch (P3) unchanged.
//
// Bindings (matched to raytraced_lighting_*.slang):
//   space=0, b0  : CameraUniforms cbuffer  (uploaded per-frame from GpuScene::GetCamera)
//   space=0, t1  : RaytracingAccelerationStructure (TLAS — secondary rays!)
//   space=0, u2  : RWTexture2D<float4>     (fp32 linear radiance — PassContext::
//                  linearColor; the display transform moved to TonemapPass at P3)
//   space=0, t34 : StructuredBuffer<VisibilityGpu> (PassContext::visibility — P4)

#pragma once

#include "RenderGraph/IRenderPass.h"

#include <Pyxis/Renderer/Descs/PickResult.h>

#include <nvrhi/nvrhi.h>

// Q3 review — NormalizeFeatureMask clamps the incoming OpenPBR feature
// mask to shaderinterop::OPENPBR_FEATURES_ALL (the dual-language interop
// header is on the renderer's private include path; the same one
// RaytracedLightingPass.cpp pulls for the spec-constant ids).
#include "ShaderInterop.slang"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace pyxis {

class GpuScene;
struct SceneResources;

class RaytracedLightingPass final : public IRenderPass {
 public:
  // P5 (design D2) — the C++ single source of truth for the RT
  // recursion / loop budgets. Feeds BOTH nvrhi::rt::PipelineDesc::
  // maxRecursionDepth (ctor + ReloadShaders) AND the Vulkan
  // specialization-constant values (SPEC_ID_MAX_RAY_RECURSION /
  // SPEC_ID_MAX_TRANSPARENT_SEGMENTS / SPEC_ID_REFL_MAX_SEGMENTS in
  // ShaderInterop.slang) — pre-P5 the 3 was a literal duplicated in
  // shading.slang AND twice in this .cpp, and the 16s were shader-local
  // consts. V2.B doctrine: depth 3 = first-hit shade (0) →
  // reflection/continuation closesthit (1) → its reflection ray (2) →
  // that ray's shadow/AO ray (3); the 16-segment caps bound the
  // front-to-back transparency composites (loop iterations, not
  // hardware recursion).
  static constexpr uint32_t MAX_RAY_RECURSION        = 3u;
  static constexpr uint32_t MAX_TRANSPARENT_SEGMENTS = 16u;
  static constexpr uint32_t REFL_MAX_SEGMENTS        = 16u;

  RaytracedLightingPass(nvrhi::IDevice* device, GpuScene& scene);
  ~RaytracedLightingPass() override;
  RaytracedLightingPass(const RaytracedLightingPass&) = delete;
  RaytracedLightingPass& operator=(const RaytracedLightingPass&) = delete;

  void Execute(nvrhi::ICommandList* commandList, const PassContext& context) override;
  [[nodiscard]] std::string_view Name() const override { return "pass.RaytracedLighting"; }

  // Re-load raygen / miss / closesthit .spv from disk, recreate the
  // RT pipeline + shader binding table. The binding LAYOUT, the
  // CameraUniforms cbuffer, and every fallback (material / instance /
  // lights / mesh / dome / sampler) survive intact — only the shaders
  // and pipeline-state objects depend on the .spv contents. Cached
  // binding sets ALSO survive (binding sets reference the layout, not
  // the pipeline). Returns true iff every step completed; on false the
  // pre-reload pipeline is preserved and rendering continues unchanged.
  [[nodiscard]] bool ReloadShaders() noexcept override;

  // M7 follow-up — picker readback. Returns the cached PickResult the
  // last successful staging-buffer map copied out. Always returns the
  // last-known-good value; no error path. Default-constructed (depth
  // -1, instanceId ~0u) until the first frame's copy retires + the
  // next Execute()'s map reads it. Caller (PyxisRenderer) just
  // forwards this to the public LastPickResult() entry.
  [[nodiscard]] PickResult GetLastPickResult() const noexcept { return _lastPickResult; }

  // P3 pass split — the full-render-resolution fp32 LINEAR radiance scratch
  // the raygen writes at binding 2 (gLinearColor) and TonemapPass reads.
  // Owned here (the pass has the device + is the writer, mirroring
  // SsaaResolvePass::EnsureLinearOutput); allocated/cached at `width` x
  // `height` (the display target's dims so the ray-dispatch extents are
  // unchanged), recreated on a size change only — NEVER inside Execute.
  // RGBA32F: the display transform must see bit-identical values to the
  // in-kernel payload.color the old inline branch consumed (RGBA16F would
  // quantize). PyxisRenderer calls this each RenderFrame and threads the
  // result through PassContext::linearColor.
  [[nodiscard]] nvrhi::ITexture* EnsureLinearColor(uint32_t width, uint32_t height);

  // Q2 (openpbr-complete-design.md) — variant-cache key: the two
  // image-shaping specialization constants this pipeline carries.
  // Projection collapses to {0, 1} exactly like the shader branch
  // (`PROJECTION_MODE == 1u`); the OpenPBR feature mask rides in the
  // low 32 bits. Single normalization point so EnsureFeaturePipeline /
  // IsOperational / Execute can never disagree on a key.
  //
  // Q3 review — DEFENSIVE mask clamp at the renderer ingress. The mask
  // arrives from RenderSettings::openPbrFeatureMask, a full uint32 a
  // caller (CLI / Hydra setting / Omniverse carb path) can set to
  // arbitrary bits. The shader only tests the defined bits
  // (OPENPBR_FEATURES_ALL == 0x3F) — every other bit has NO image
  // effect — so masking them off here is purely image-safe AND bounds
  // the unbounded variant cache: a hostile/buggy caller sweeping the
  // top 26 bits would otherwise materialize one RT pipeline + SBT per
  // distinct uint32 (no eviction). With the clamp the cache is capped
  // at <= 64 mask combos x 2 projection modes. The default mask (0x3F)
  // is unchanged by the clamp, so no golden pixel moves. This is the
  // ONE canonical clamp: every consumer (the key below, IsOperational,
  // and Execute's _activeVariantKey lookup) routes through VariantKey,
  // and EnsureFeaturePipeline re-derives the same normalized mask via
  // NormalizeFeatureMask before it builds the pipeline so the spec
  // constant baked into the variant matches the key it is stored under.
  [[nodiscard]] static constexpr uint32_t NormalizeFeatureMask(
      uint32_t featureMask) noexcept {
    return featureMask & shaderinterop::OPENPBR_FEATURES_ALL;
  }
  [[nodiscard]] static constexpr std::uint64_t VariantKey(
      uint32_t projectionMode, uint32_t featureMask) noexcept {
    const std::uint64_t proj = (projectionMode == 1u) ? 1u : 0u;
    return (proj << 32) | static_cast<std::uint64_t>(NormalizeFeatureMask(featureMask));
  }

  // Q2 — make sure the RT pipeline variant for the ACTIVE
  // (projectionMode, featureMask) pair exists, and mark it as the key
  // Execute selects this frame (pure lookup there — §30.10). Replaces
  // the P5 EnsureProjectionPipeline: the raygen + closesthit now also
  // carry the SPEC_ID_OPENPBR_FEATURES bitmask, so the variant space
  // is a sparse map instead of a fixed two-array. perspective +
  // OPENPBR_FEATURES_ALL is built eagerly in the ctor (the v1 default);
  // every other key materializes lazily here, on PyxisRenderer's CPU
  // frame path BEFORE the graph walks — pipeline creation never
  // happens inside Execute. A failed build latches in
  // _variantBuildFailed (not retried, no per-frame log spam) until the
  // next ReloadShaders. `projectionMode` comes from the caller's
  // camera read (the same CameraDesc::projectionMode the CameraUniforms
  // upload uses); `featureMask` is OPENPBR_FEATURES_ALL until Q3
  // threads RenderSettings through PyxisRenderer.
  void EnsureFeaturePipeline(uint32_t projectionMode, uint32_t featureMask);

  // P6 review / Q2 — pass-health probe for PyxisRenderer's frame path,
  // mirroring RaytracedGBufferPass::IsOperational (which stays
  // projection-only; the GBuffer pipeline is mask-independent). True
  // iff the ctor loaded all shaders AND the requested variant's
  // pipeline+SBT exist or can still be lazily built (i.e. the lazy
  // build hasn't latched a failure). PyxisRenderer calls this AFTER
  // EnsureFeaturePipeline each frame: when false, it nulls
  // PassContext::linearColor so TonemapPass degrades to the pre-split
  // untouched-output no-op instead of tonemapping a never-written
  // linearColor.
  [[nodiscard]] bool IsOperational(uint32_t projectionMode, uint32_t featureMask) const noexcept {
    if (!_shadersOk)
      return false;
    const std::uint64_t key = VariantKey(projectionMode, featureMask);
    if (const auto it = _variants.find(key); it != _variants.end())
      return it->second.pipeline != nullptr && it->second.shaderTable != nullptr;
    return !_variantBuildFailed.contains(key);  // lazy build still possible.
  }

 private:
  // Build the binding set for the supplied linear-radiance output + the
  // visibility buffer (P4) + targets + scene-resource view (RFC 0003 — the
  // per-Execute SceneResources snapshot replaces the old public GpuScene
  // getters). Cached per `nvrhi::ITexture*` identity (the RGBA32F linearColor
  // output at binding 2) so we don't churn descriptor sets on every frame; a
  // resize invalidates pointers and the cache is bounded so stale entries get
  // evicted. Takes the full RenderTargets so the M7 raw AOV outputs + pick
  // buffer get bound alongside the scene-side buffers.
  [[nodiscard]] nvrhi::BindingSetHandle GetOrCreateBindingSet(
      nvrhi::ITexture* linearColor, nvrhi::IBuffer* visibility,
      struct RenderTargets const& targets, const SceneResources& res);

  nvrhi::IDevice* _device = nullptr;
  GpuScene* _scene = nullptr;

  // Compiled BASE shaders (unspecialized .spv-backed handles). Built
  // once in the ctor; the per-variant specialized handles derive from
  // these via createShaderSpecialization and live inside the pipeline
  // objects below (NVRHI rt pipelines keep their desc's shader refs).
  nvrhi::ShaderHandle _raygenShader;
  nvrhi::ShaderHandle _missShader;
  // M9-fidelity: second miss shader for shadow rays. The shared
  // shading TraceRays with miss-shader-index=1; this miss writes
  // payload.color=1.0 = visible. Pre-trace the shading zeroes the
  // payload, so any opaque blocker leaves visibility=0 = occluded.
  nvrhi::ShaderHandle _shadowMissShader;
  nvrhi::ShaderHandle _closestHitShader;
  nvrhi::ShaderHandle _anyHitShader;
  nvrhi::BindingLayoutHandle _bindingLayout;
  // Q2 (openpbr-complete-design.md) — RT pipeline variant cache keyed
  // by VariantKey(projectionMode, openPbrFeatureMask). Replaces the P5
  // fixed two-variant arrays. The SBT derives from its pipeline
  // (createShaderTable), so the pair is always created / inserted
  // together. _variantBuildFailed latches failed lazy builds so the
  // per-frame hook doesn't retry (and re-log); both containers are
  // cleared by ReloadShaders, which then rebuilds the last-active key.
  // _activeVariantKey is written by EnsureFeaturePipeline on the CPU
  // frame path each frame (render thread only, §31) and read by
  // Execute as a pure map lookup; the ctor initializes it to
  // perspective + OPENPBR_FEATURES_ALL, the eagerly-built default.
  struct VariantEntry {
    nvrhi::rt::PipelineHandle pipeline;
    nvrhi::rt::ShaderTableHandle shaderTable;
  };
  std::unordered_map<std::uint64_t, VariantEntry> _variants;
  std::unordered_set<std::uint64_t> _variantBuildFailed;
  std::uint64_t _activeVariantKey = 0;  // set in the ctor (perspective + ALL)

  // Per-frame constant buffer carrying CameraUniforms (worldFromView
  // + viewFromClip inverses).
  nvrhi::BufferHandle _cameraUniformsBuffer;
  // M7 follow-up — per-frame viewer-only UI state (cursor pixel +
  // AOV-inspector mode). Lives in its own cbuffer at binding 19 so
  // the §10 camera contract stays clean even as the AOV inspector
  // grows new knobs. 16 bytes; written every Execute when the editor
  // changes the picked AOV or the cursor moves.
  nvrhi::BufferHandle _frameUiBuffer;

  // 1-element fallback structured buffers — one per DISTINCT ELEMENT STRIDE
  // (P2 collapse of the old 14 per-binding fallbacks). Each scene-side
  // structured-buffer binding must point at a non-null buffer whose stride
  // matches the shader's declared element type even when GpuScene hasn't
  // allocated the real buffer yet (empty scenes / first frames). Contents
  // are (re)written every Execute while the matching scene buffer is null:
  //   * _fallbackMaterialBuffer  (240 B OpenPBRMaterialGPU) — binding 3,
  //     default grey so `material = Invalid` renders the same recognisable
  //     grey as GpuScene's sentinel slot 0.
  //   * _fallbackLightBuffer     (96 B LightGpu) — binding 5, one disabled
  //     (intensity = 0) light so the per-light loop falls through to the
  //     M5/M6 baseColor-only path (byte-equal unlit fixtures).
  //   * _fallbackInstanceInfoBuffer (64 B InstanceInfoGpu) — binding 4,
  //     zero record: any rogue InstanceID() resolves to material/mesh
  //     slot 0 (the sentinels), exactly as the old zero-uint tables did.
  //   * _fallbackStride32Buffer  (32 B) — bindings 6 (MeshInfoGpu) + 29
  //     (VertexAttribGpu) share it: both fallback contents are all-zero
  //     (offsets 0 / zero-magnitude sentinel attributes).
  //   * _fallbackFloat4Buffer    (16 B) — binding 7 face normals, zero ⇒
  //     NdotL = 0, unlit but no out-of-bounds read.
  //   * _fallbackUvBuffer        (8 B tight float2) — binding 24 (the
  //     gMeshUvs stride contract; see MeshUvPack.h).
  //   * _fallbackUintBuffer      (4 B) — binding 26 mesh indices.
  nvrhi::BufferHandle _fallbackMaterialBuffer;
  nvrhi::BufferHandle _fallbackLightBuffer;
  nvrhi::BufferHandle _fallbackInstanceInfoBuffer;
  nvrhi::BufferHandle _fallbackStride32Buffer;
  nvrhi::BufferHandle _fallbackFloat4Buffer;
  nvrhi::BufferHandle _fallbackUvBuffer;
  nvrhi::BufferHandle _fallbackUintBuffer;

  // M7-IBL: 1×1 black RGBA32F fallback texture + a default linear-
  // clamp sampler. Bound at bindings 9/10 when the scene has no dome
  // light with a resolved env-map — sampling returns black so the
  // miss shader's "use authored color" branch fires unchanged.
  nvrhi::TextureHandle _fallbackDomeTexture;
  nvrhi::SamplerHandle _fallbackDomeSampler;

  // P3 — cached fp32 linear-radiance output (see EnsureLinearColor).
  nvrhi::TextureHandle _linearColor;
  uint32_t _linearColorW = 0;
  uint32_t _linearColorH = 0;
  // P6 review — latched after the first logged createTexture failure so
  // the per-resize allocation path doesn't spam the log every frame.
  bool _linearColorCreateFailedLogged = false;

  // Output binding-set cache, keyed on the output texture pointer.
  // Bounded — a swapchain rebuild churns through up to ~3 swapchain
  // images, so 6 entries is more than enough headroom and the
  // eviction-on-overflow keeps stale dangling pointers from
  // accumulating across re-init.
  std::unordered_map<nvrhi::ITexture*, nvrhi::BindingSetHandle> _bindingSetCache;

  // BindingsSnapshot — every borrowed-pointer that participates in a
  // binding-set, captured per Execute as a flat std::array<void*>.
  // std::array's element-wise operator== / operator= keep the per-frame
  // compare + assign idiomatic. Slot order pinned by the scoped-enum
  // indices below; the construction site documents which scene/target
  // getter feeds each slot.
  enum class BindingSlot : std::size_t {
    // P6 review — the TLAS handle itself. GpuScene replaces the TLAS
    // OBJECT on the first transform-only commit after an append-built
    // TLAS (Commit.cpp upgrades it to AllowUpdate with a new
    // nvrhi handle); without this slot the cached set would keep the
    // orphaned pre-upgrade accel struct bound while the GBuffer pass
    // (which snapshots res.tlas) traces the new one — cross-pass
    // scene divergence for every secondary ray.
    Tlas = 0,
    Materials,
    InstanceInfo,
    Lights,
    MeshInfo,
    MeshFaceNormals,
    DomeTexture,
    BindlessSampler,
    MeshUvs,
    MeshIndices,
    MeshVertexAttribs,
    DomeSampler,
    // P4 — the GBuffer pass's visibility buffer (binding 34). A resize
    // recreates it, so the pointer participates in invalidation.
    Visibility,
    ColorHdrAov,
    NormalAov,
    DepthAov,
    PrimIdAov,
    MaterialAov,
    BaseColorAov,
    WorldPosAov,
    PickResult,
    // Tier 1 Hydra-canonical AOVs.
    AlphaAov,
    ElementIdAov,
    NormalEyeAov,
    WorldPosEyeAov,
    Count,
  };
  static constexpr std::size_t BINDING_SLOT_COUNT =
      static_cast<std::size_t>(BindingSlot::Count);
  using BindingsSnapshot = std::array<const void*, BINDING_SLOT_COUNT>;
  BindingsSnapshot _lastBindings{};

  // M8a bindless textures: cached fingerprint of the scene's bindless-
  // texture array (binding 28). The textures-array writes one
  // BindingSetItem per live texture; cached binding sets become stale
  // when EITHER the array length grows / shrinks OR a slot's
  // ITexture* changes (the M8a audit caught the latter — with the
  // free-list slot-recycle, DestroyTexture + later AcquireTexture
  // reuses the same slot index but with a different pointer, leaving
  // the count unchanged). Fingerprint = FNV1a-64 over (count, every
  // live ITexture*); recompute each Execute, invalidate on mismatch.
  // Cheap: one pointer-load + xor-multiply per live texture per
  // frame, well under 1µs even at World Lobby scale.
  std::uint64_t _lastBindlessTextureFingerprint = 0;

  bool _shadersOk = false;  // true if ctor loaded all five shaders + built pipeline.
  // Tiny no-UAV fallback textures for each raw AOV format. Bound when
  // the caller doesn't supply that AOV (headless mode + the M2-era
  // color-only paths). Same lifetime as the existing fallbacks above.
  nvrhi::TextureHandle _fallbackColorHdrAov;
  nvrhi::TextureHandle _fallbackNormalAov;
  nvrhi::TextureHandle _fallbackDepthAov;
  nvrhi::TextureHandle _fallbackPrimIdAov;
  nvrhi::TextureHandle _fallbackMaterialAov;
  nvrhi::TextureHandle _fallbackBaseColorAov;
  nvrhi::TextureHandle _fallbackWorldPosAov;
  nvrhi::BufferHandle  _fallbackPickResult;
  // Tier 1 Hydra-canonical fallbacks.
  nvrhi::TextureHandle _fallbackAlphaAov;
  nvrhi::TextureHandle _fallbackElementIdAov;
  nvrhi::TextureHandle _fallbackNormalEyeAov;
  nvrhi::TextureHandle _fallbackWorldPosEyeAov;

  // Cached one-frame-stale picker readback. Updated at the top of
  // each Execute() by mapping the staging buffer (if a copy was
  // submitted in the prior frame). The first Execute returns the
  // default-constructed value (depth -1, instanceId ~0u).
  PickResult _lastPickResult{};
  bool       _pickStagingHasFrame = false;
};

}  // namespace pyxis
