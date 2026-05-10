// Pyxis renderer — primary-ray path-trace pass.
//
// Plan §41 M3. M3 simplification per the v1 scope discussion: 1 ray
// per pixel, no recursion, no NEE, no shadow rays. The closesthit
// shader returns barycentric colour; the miss shader returns flat
// sky blue. That's enough to validate the full RT-pipeline plumbing
// (BLAS / TLAS bound via descriptor set, raygen / miss / closesthit
// stitched together by a shader binding table, dispatchRays
// cascading the right shader records per-hit) without wading into
// the multi-bounce path-tracing integral that M5+ brings.
//
// Bindings (matched to raygen.slang):
//   space=0, b0  : CameraUniforms cbuffer  (uploaded per-frame from GpuScene::GetCamera)
//   space=0, t0  : RaytracingAccelerationStructure (TLAS, GpuScene::GetTlas)
//   space=0, u0  : RWTexture2D<float4>     (output colour AOV)

#pragma once

#include "RenderGraph/IRenderPass.h"

#include <Pyxis/Renderer/Descs/PickResult.h>

#include <nvrhi/nvrhi.h>

#include <string_view>
#include <unordered_map>

namespace pyxis {

class GpuScene;

class PathTracePass final : public IRenderPass {
 public:
  PathTracePass(nvrhi::IDevice* device, GpuScene& scene);
  ~PathTracePass() override;
  PathTracePass(const PathTracePass&) = delete;
  PathTracePass& operator=(const PathTracePass&) = delete;

  void Execute(nvrhi::ICommandList* commandList, const PassContext& context) override;
  [[nodiscard]] std::string_view Name() const override { return "pass.PathTrace"; }

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

 private:
  // Build the binding set for the supplied targets. Cached per
  // `nvrhi::ITexture*` identity (the BGRA8 output) so we don't churn
  // descriptor sets on every frame; a swapchain rebuild invalidates
  // pointers and the cache is bounded so stale entries get evicted.
  // Takes the full RenderTargets so the M7 raw AOV outputs + pick
  // buffer get bound alongside the existing scene-side buffers.
  [[nodiscard]] nvrhi::BindingSetHandle GetOrCreateBindingSet(struct RenderTargets const& targets);

  nvrhi::IDevice* _device = nullptr;
  GpuScene* _scene = nullptr;

  // Compiled shaders + pipeline + shader binding table. Built once
  // in the ctor; reused every frame.
  nvrhi::ShaderHandle _raygenShader;
  nvrhi::ShaderHandle _missShader;
  nvrhi::ShaderHandle _closestHitShader;
  nvrhi::BindingLayoutHandle _bindingLayout;
  nvrhi::rt::PipelineHandle _pipeline;
  nvrhi::rt::ShaderTableHandle _shaderTable;

  // Per-frame constant buffer carrying CameraUniforms (worldFromView
  // + viewFromClip inverses).
  nvrhi::BufferHandle _cameraUniformsBuffer;
  // M7 follow-up — per-frame viewer-only UI state (cursor pixel +
  // AOV-inspector mode). Lives in its own cbuffer at binding 19 so
  // the §10 camera contract stays clean even as the AOV inspector
  // grows new knobs. 16 bytes; written every Execute when the editor
  // changes the picked AOV or the cursor moves.
  nvrhi::BufferHandle _frameUiBuffer;

  // M5: 1-element fallback material buffer (the closesthit reads
  // `materials[instanceMaterial[InstanceID()]]` so binding 3 must
  // always be a non-null buffer even when the scene has no materials
  // yet). GpuScene's own materials buffer takes precedence at
  // binding-set creation time when present; this fallback covers
  // cube-fixture scenes that never call AcquireMaterial.
  nvrhi::BufferHandle _fallbackMaterialBuffer;

  // M6: 1-element fallback instance→material side-table. Same role
  // as `_fallbackMaterialBuffer` but for binding 4 — the closesthit's
  // `instanceMaterial[InstanceID()]` lookup must point at a non-null
  // buffer even before GpuScene has built its first TLAS. Holds one
  // zero-uint so any rogue InstanceID resolves to material slot 0
  // (the sentinel grey material).
  nvrhi::BufferHandle _fallbackInstanceMaterialBuffer;

  // M7: 1-element fallback light buffer. Bound at binding 5 when
  // GpuScene has no lights (no AddLight call yet). Holds a single
  // LightGpu with intensity = 0 so the closesthit's per-light
  // contribution loop sees one disabled entry and falls through to
  // the M5/M6 baseColor-only path — preserving byte-equal across
  // M5 + M6 fixtures that don't author lights.
  nvrhi::BufferHandle _fallbackLightBuffer;

  // M7 NdotL: fallbacks for the three new normal-lookup buffers.
  // Each holds a single zero-init element so an empty / not-yet-
  // committed scene's closesthit invocations (rare — scenes with
  // no meshes can't have hits anyway) resolve to face-normal (0,0,0)
  // → NdotL=0 → unlit, but no out-of-bounds reads.
  nvrhi::BufferHandle _fallbackInstanceMeshBuffer;
  nvrhi::BufferHandle _fallbackMeshFaceNormalsBuffer;
  nvrhi::BufferHandle _fallbackMeshFaceOffsetsBuffer;

  // M7-IBL: 1×1 black RGBA32F fallback texture + a default linear-
  // clamp sampler. Bound at bindings 9/10 when the scene has no dome
  // light with a resolved env-map — sampling returns black so the
  // miss shader's "use authored color" branch fires unchanged.
  nvrhi::TextureHandle _fallbackDomeTexture;
  nvrhi::SamplerHandle _fallbackDomeSampler;

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
    Materials = 0,
    InstanceMaterial,
    Lights,
    InstanceMesh,
    MeshFaceNormals,
    MeshFaceOffsets,
    DomeTexture,
    BindlessSampler,
    ColorHdrAov,
    NormalAov,
    DepthAov,
    InstanceAov,
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

  bool _shadersOk = false;  // true if ctor loaded all three shaders + built pipeline.
  // Tiny no-UAV fallback textures for each raw AOV format. Bound when
  // the caller doesn't supply that AOV (headless mode + the M2-era
  // color-only paths). Same lifetime as the existing fallbacks above.
  nvrhi::TextureHandle _fallbackColorHdrAov;
  nvrhi::TextureHandle _fallbackNormalAov;
  nvrhi::TextureHandle _fallbackDepthAov;
  nvrhi::TextureHandle _fallbackInstanceAov;
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
