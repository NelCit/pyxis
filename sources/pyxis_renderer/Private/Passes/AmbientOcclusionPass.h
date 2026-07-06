// Pyxis renderer — AmbientOcclusionPass (RTX-alignment design,
// rtx-realtime-alignment-design.md, WP2-signals — Phase A signal split).
//
// One hemispherical visibility ray per pixel, fired along the PRIMARY
// hit's shading normal (read from RaytracedGBufferPass's gNormalRoughness
// guide — this pass does NOT re-evaluate the material; RTAO needs only
// geometry). TMax = AO_RAY_LENGTH (ShaderInterop.slang). Deterministic —
// no RNG, no cosine-hemisphere sampling (that lands with Phase B's
// adaptive-sample-count RTAO); a single ray along N is the Phase A
// estimator. Matches "today's" shadow/AO ray flag semantics
// (RAY_FLAG_SKIP_CLOSEST_HIT_SHADER | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH,
// alpha-tested + transmissive geometry pass through via the shared anyhit
// gate) — see ambient_occlusion.slang.
//
// Bindings — Set 0 is the shared SceneBindings layout. Set 1 (this
// pass's own I/O, matched to ambient_occlusion.slang's
// `[[vk::binding(N, 1)]]` declarations):
//   set=1, binding=0 : StructuredBuffer<VisibilityGpu> (GBuffer pass output)
//   set=1, binding=1 : Texture2D<float4> gNormalRoughness (GBuffer guide, SRV)
//   set=1, binding=2 : RWTexture2D<float4> gAo (RGBA16F; .r = 0 occluded / 1 clear)

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
class SceneBindings;

class AmbientOcclusionPass final : public IRenderPass {
 public:
  AmbientOcclusionPass(nvrhi::IDevice* device, GpuScene& scene, SceneBindings& sceneBindings);
  ~AmbientOcclusionPass() override;
  AmbientOcclusionPass(const AmbientOcclusionPass&) = delete;
  AmbientOcclusionPass& operator=(const AmbientOcclusionPass&) = delete;

  void Execute(nvrhi::ICommandList* commandList, const PassContext& context) override;
  [[nodiscard]] std::string_view Name() const override { return "pass.AmbientOcclusion"; }

  // (Re)creates the gAo output at the display target's dims — CPU frame
  // path only (PyxisRenderer::RenderFrame, before the graph walks), same
  // convention as RaytracedGBufferPass::EnsureVisibilityBuffer. Not
  // threaded through PassContext: unlike visibility/linearColor, no
  // sibling pass consumes this output in Phase A — Execute reads the
  // member directly.
  [[nodiscard]] nvrhi::ITexture* EnsureOutput(uint32_t width, uint32_t height);

  // Same per-projection-mode variant scheme as RaytracedGBufferPass
  // (PROJECTION_MODE is the only axis this pass's pipeline varies on —
  // it never touches OpenPBR closures, so there is no feature-mask
  // dimension). Called on PyxisRenderer's CPU frame path, never inside
  // Execute (§30.10).
  void EnsureProjectionPipeline();

  [[nodiscard]] bool IsOperational(uint32_t projectionMode) const noexcept {
    if (!_shadersOk)
      return false;
    const std::size_t variant = (projectionMode == 1u) ? 1u : 0u;
    if (_pipelines[variant] && _shaderTables[variant])
      return true;
    return !_variantBuildFailed[variant];
  }

  // Debug/diagnostic accessor — PyxisRenderer::DebugSignalTexture reads
  // this for --save-aov "ao". Raw opaque pointer (§18.9).
  [[nodiscard]] nvrhi::ITexture* Output() const noexcept { return _output.Get(); }

 private:
  [[nodiscard]] nvrhi::BindingSetHandle GetOrCreateBindingSet(nvrhi::IBuffer* visibility,
                                                              nvrhi::ITexture* normalRoughness);

  nvrhi::IDevice* _device = nullptr;
  GpuScene* _scene = nullptr;
  SceneBindings* _sceneBindings = nullptr;

  nvrhi::ShaderHandle _raygenShader;
  nvrhi::ShaderHandle _missShader;
  nvrhi::ShaderHandle _anyHitShader;
  nvrhi::BindingLayoutHandle _passLayout;

  static constexpr std::size_t PROJECTION_VARIANT_COUNT = 2;
  std::array<nvrhi::rt::PipelineHandle, PROJECTION_VARIANT_COUNT> _pipelines;
  std::array<nvrhi::rt::ShaderTableHandle, PROJECTION_VARIANT_COUNT> _shaderTables;
  std::array<bool, PROJECTION_VARIANT_COUNT> _variantBuildFailed{};

  nvrhi::TextureHandle _output;
  uint32_t _outputW = 0;
  uint32_t _outputH = 0;

  std::unordered_map<nvrhi::IBuffer*, nvrhi::BindingSetHandle> _bindingSetCache;
  // Snapshot of {visibility, gNormalRoughness} — mismatch invalidates the cache.
  std::array<const void*, 2> _lastBindings{};

  bool _shadersOk = false;
};

}  // namespace pyxis
