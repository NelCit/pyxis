// Pyxis renderer — visibility-buffer pass (P4 pass split).
//
// RT pipeline A of the visibility-buffer architecture (design D1): the
// raygen builds the camera primary ray (shared camera_ray.slang module,
// bit-identical to the lighting raygen's reconstruction), traces ONE
// TraceRay with a thin 20-byte payload (maxRecursionDepth = 1 — primary
// only; no transparency loop, no shading), and writes one fp32-exact
// 32-byte VisibilityGpu record per pixel into the RWStructuredBuffer
// this pass owns. The RaytracedLightingPass reads the buffer (binding
// 34 of ITS layout) and defers all shading there.
//
// The anyhit is the shared alpha-test stub (raytraced_anyhit.slang
// logic compiled against the thin payload) so visibility semantics are
// bit-identical to the lighting pipeline's hit acceptance.
//
// Bindings (matched to raytraced_gbuffer_*.slang; this pipeline's OWN
// layout — gMaterials/gInstanceInfo keep the lighting pipeline's slot
// numbers so the shared anyhit module declares them once):
//   space=0, b0 : CameraUniforms cbuffer (uploaded per-frame from GpuScene::GetCamera)
//   space=0, t1 : RaytracingAccelerationStructure (TLAS, SceneResources::tlas)
//   space=0, u2 : RWStructuredBuffer<VisibilityGpu> (owned here — EnsureVisibilityBuffer)
//   space=0, t3 : StructuredBuffer<OpenPBRMaterialGPU> (anyhit alpha-test chain)
//   space=0, t4 : StructuredBuffer<InstanceInfoGpu>    (anyhit alpha-test chain)

#pragma once

#include "RenderGraph/IRenderPass.h"

#include <nvrhi/nvrhi.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <unordered_map>

namespace pyxis {

class GpuScene;
struct SceneResources;

class RaytracedGBufferPass final : public IRenderPass {
 public:
  RaytracedGBufferPass(nvrhi::IDevice* device, GpuScene& scene);
  ~RaytracedGBufferPass() override;
  RaytracedGBufferPass(const RaytracedGBufferPass&) = delete;
  RaytracedGBufferPass& operator=(const RaytracedGBufferPass&) = delete;

  void Execute(nvrhi::ICommandList* commandList, const PassContext& context) override;
  [[nodiscard]] std::string_view Name() const override { return "pass.RaytracedGBuffer"; }

  // Re-load the four .spv from disk, recreate the RT pipeline + shader
  // binding table. Layout / cbuffer / fallbacks / cached binding sets
  // survive (sets reference the layout, not the pipeline). Returns true
  // iff every step completed; on false the pre-reload pipeline is
  // preserved and rendering continues unchanged. Same contract as
  // RaytracedLightingPass::ReloadShaders.
  [[nodiscard]] bool ReloadShaders() noexcept override;

  // P4 — the width*height*32 B visibility buffer the raygen writes
  // (RWStructuredBuffer<VisibilityGpu>, binding 2) and the
  // RaytracedLightingPass reads (its binding 34). Owned here (this pass
  // is the writer, mirroring RaytracedLightingPass::EnsureLinearColor);
  // allocated/cached at the display target's dims, recreated on a size
  // change only — NEVER inside Execute. PyxisRenderer calls this each
  // RenderFrame and threads the result through PassContext::visibility.
  [[nodiscard]] nvrhi::IBuffer* EnsureVisibilityBuffer(uint32_t width, uint32_t height);

  // P5 (design D2) — make sure the RT pipeline variant for the ACTIVE
  // camera projection mode exists. The raygen's PROJECTION_MODE
  // specialization constant (its module includes camera_ray.slang) is
  // the only difference between variants: [0] = perspective (eager,
  // ctor), [1] = orthographic (lazy, first frame the camera reports
  // it). Called by PyxisRenderer each RenderFrame on the CPU frame
  // path BEFORE the graph walks — never creates inside Execute
  // (§30.10). Both RT passes select by the same
  // GpuScene::GetCamera().projectionMode source each frame, so they
  // always run the same variant. Same contract as
  // RaytracedLightingPass::EnsureProjectionPipeline.
  void EnsureProjectionPipeline();

 private:
  // Build the binding set for the supplied visibility buffer + scene-resource
  // view. Cached per `nvrhi::IBuffer*` identity (the visibility buffer at
  // binding 2 doubles as the cache key); the scene-side borrowed pointers are
  // snapshot-compared each call — any flip (lazy material/instance allocation,
  // TLAS rebuild) invalidates the cache. Bounded, same eviction policy as the
  // lighting pass.
  [[nodiscard]] nvrhi::BindingSetHandle GetOrCreateBindingSet(
      nvrhi::IBuffer* visibility, const SceneResources& res);

  nvrhi::IDevice* _device = nullptr;
  GpuScene* _scene = nullptr;

  // Compiled BASE shaders (unspecialized .spv-backed handles). Built
  // once in the ctor; the per-variant specialized raygen derives from
  // _raygenShader via createShaderSpecialization and lives inside the
  // pipeline objects below.
  nvrhi::ShaderHandle _raygenShader;
  nvrhi::ShaderHandle _missShader;
  nvrhi::ShaderHandle _closestHitShader;
  nvrhi::ShaderHandle _anyHitShader;
  nvrhi::BindingLayoutHandle _bindingLayout;
  // P5 (design D2) — one RT pipeline + SBT per camera projection mode
  // (see EnsureProjectionPipeline). Index = 0 perspective /
  // 1 orthographic; Execute selects by GpuScene::GetCamera()'s
  // projectionMode — a pure array lookup, no creation. Same shape +
  // failure latch as the lighting pass's variants.
  static constexpr std::size_t PROJECTION_VARIANT_COUNT = 2;
  std::array<nvrhi::rt::PipelineHandle, PROJECTION_VARIANT_COUNT> _pipelines;
  std::array<nvrhi::rt::ShaderTableHandle, PROJECTION_VARIANT_COUNT> _shaderTables;
  std::array<bool, PROJECTION_VARIANT_COUNT> _variantBuildFailed{};

  // Per-frame constant buffer carrying CameraUniforms — same values the
  // lighting pass uploads (the raygen only reads worldFromView /
  // viewFromClip / projectionMode, but the full struct keeps the upload
  // code identical between the two passes).
  nvrhi::BufferHandle _cameraUniformsBuffer;

  // 1-element fallback structured buffers for the anyhit's alpha-test
  // chain when GpuScene hasn't allocated the real tables yet — same
  // shape + per-Execute default writes as the lighting pass's
  // fallbacks (grey material / zero instance record).
  nvrhi::BufferHandle _fallbackMaterialBuffer;
  nvrhi::BufferHandle _fallbackInstanceInfoBuffer;

  // P4 — cached visibility buffer (see EnsureVisibilityBuffer).
  nvrhi::BufferHandle _visibility;
  uint32_t _visibilityW = 0;
  uint32_t _visibilityH = 0;

  // Binding-set cache, keyed on the visibility buffer pointer (recreated
  // on resize). Bounded; eviction-on-overflow keeps stale dangling
  // pointers from accumulating across re-init.
  std::unordered_map<nvrhi::IBuffer*, nvrhi::BindingSetHandle> _bindingSetCache;

  // Snapshot of the borrowed scene pointers bound into the set —
  // mismatch invalidates the cache (lazy-allocation flips, TLAS
  // rebuild). Mirrors the lighting pass's BindingsSnapshot, just
  // smaller (this layout binds three scene resources).
  enum class BindingSlot : std::size_t {
    Tlas = 0,
    Materials,
    InstanceInfo,
    Count,
  };
  static constexpr std::size_t BINDING_SLOT_COUNT =
      static_cast<std::size_t>(BindingSlot::Count);
  using BindingsSnapshot = std::array<const void*, BINDING_SLOT_COUNT>;
  BindingsSnapshot _lastBindings{};

  bool _shadersOk = false;  // true if ctor loaded all four shaders + built pipeline.
};

}  // namespace pyxis
