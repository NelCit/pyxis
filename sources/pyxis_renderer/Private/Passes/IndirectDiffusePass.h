// Pyxis renderer — IndirectDiffusePass (RTX-alignment design,
// rtx-realtime-alignment-design.md, WP2-signals Phase A / Phase B
// estimator swap).
//
// Phase B: real 1 spp cosine-hemisphere path-traced bounce (see
// indirect_diffuse.slang's file header for the full estimator). The
// RayGen traces a bounce ray; ClosestHitMain evaluates the bounce hit's
// material + fires its own single-sample NEE shadow ray + emission;
// MissMain samples the dome at full resolution. Grew from the Phase A
// RayGen-only pipeline (no hit groups, no TraceRay) to a full
// RayGen/ClosestHit/Miss/AnyHit shape — the pass-type stayed an RT
// pipeline throughout (never a compute pass), so this upgrade is exactly
// the body + pipeline-shape change the Phase A header anticipated.
//
// Bindings — Set 0 is the shared SceneBindings layout. Set 1 (this
// pass's own I/O, matched to indirect_diffuse.slang's
// `[[vk::binding(N, 1)]]` declarations):
//   set=1, binding=0 : StructuredBuffer<VisibilityGpu> (GBuffer pass output)
//   set=1, binding=1 : RWTexture2D<float4> gIndirectDiffuse (RGBA16F; .a = 1)
//   set=1, binding=2 : RWByteAddressBuffer gShHash     (SHARC cache — checksum table)
//   set=1, binding=3 : RWByteAddressBuffer gShAccum    (SHARC cache — per-frame sum)
//   set=1, binding=4 : RWByteAddressBuffer gShResolved (SHARC cache — resolved radiance)
// The three SHARC buffers are owned by SharcResolvePass (ctor-injected); they
// are always bound but touched by the shader only when gQuality.giMode != 0
// (PASS_MASK_SHARC_GI), so the builtin path stays byte-identical when off.

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
class SharcResolvePass;

class IndirectDiffusePass final : public IRenderPass {
 public:
  IndirectDiffusePass(nvrhi::IDevice* device, GpuScene& scene, SceneBindings& sceneBindings,
                      SharcResolvePass& sharc);
  ~IndirectDiffusePass() override;
  IndirectDiffusePass(const IndirectDiffusePass&) = delete;
  IndirectDiffusePass& operator=(const IndirectDiffusePass&) = delete;

  void Execute(nvrhi::ICommandList* commandList, const PassContext& context) override;
  [[nodiscard]] std::string_view Name() const override { return "pass.IndirectDiffuse"; }

  [[nodiscard]] nvrhi::ITexture* EnsureOutput(uint32_t width, uint32_t height);
  void EnsureProjectionPipeline();

  [[nodiscard]] bool IsOperational(uint32_t projectionMode) const noexcept {
    if (!_shadersOk)
      return false;
    const std::size_t variant = (projectionMode == 1u) ? 1u : 0u;
    if (_pipelines[variant] && _shaderTables[variant])
      return true;
    return !_variantBuildFailed[variant];
  }

  [[nodiscard]] nvrhi::ITexture* Output() const noexcept { return _output.Get(); }

 private:
  [[nodiscard]] nvrhi::BindingSetHandle GetOrCreateBindingSet(nvrhi::IBuffer* visibility);

  nvrhi::IDevice* _device = nullptr;
  GpuScene* _scene = nullptr;
  SceneBindings* _sceneBindings = nullptr;
  SharcResolvePass* _sharc = nullptr;  // owns the 3 SHARC cache buffers (bound in Set 1)

  nvrhi::ShaderHandle _raygenShader;
  nvrhi::ShaderHandle _closestHitShader;
  nvrhi::ShaderHandle _missShader;
  nvrhi::ShaderHandle _shadowMissShader;
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

  bool _shadersOk = false;
};

}  // namespace pyxis
