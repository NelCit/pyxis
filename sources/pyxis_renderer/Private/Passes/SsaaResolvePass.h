// Pyxis renderer — SSAA box-downsample resolve pass.
//
// Deterministic supersampling resolve. When RenderSettings::ssaaFactor
// > 1 the rest of the render graph runs at `factor`× the output
// resolution per axis (the app allocates super-res AOVs); this compute
// pass box-averages the super-res color AOV down to the output-
// resolution `colorResolved` target. Gamma-aware (averages in linear
// light) so edges don't darken under the convex sRGB curve. No jitter,
// no accumulation — bit-exact reproducible, the noise-free AA the v2
// deterministic-renderer doctrine calls for.
//
// A render-graph pass (§9): runs after PathTracePass in the linear
// graph. Reads context.targets->color (super-res) + ->colorResolved
// (output-res) and context.settings->ssaaFactor; no-ops when
// ssaaFactor < 2 or either target is unbound (the non-SSAA path leaves
// the resolve target untouched + the caller consumes `color`
// directly). Owns its own compute shader + pipeline + binding layout,
// mirroring PathTracePass's self-contained construction.
//
// Bindings (matched to ssaa_downsample.slang):
//   space=0, t0 : Texture2D<float4>   source super-res color (RGBA8 UNORM)
//   space=0, u1 : RWTexture2D<float4> destination output-res color
//   space=0, b2 : ConstantBuffer<SsaaDownsampleUniforms>

#pragma once

#include "RenderGraph/IRenderPass.h"

#include <nvrhi/nvrhi.h>

#include <cstdint>
#include <string_view>
#include <unordered_map>

namespace pyxis {

class SsaaResolvePass final : public IRenderPass {
 public:
  explicit SsaaResolvePass(nvrhi::IDevice* device);
  ~SsaaResolvePass() override = default;
  SsaaResolvePass(const SsaaResolvePass&) = delete;
  SsaaResolvePass& operator=(const SsaaResolvePass&) = delete;

  void Execute(nvrhi::ICommandList* commandList, const PassContext& context) override;
  [[nodiscard]] std::string_view Name() const override { return "pass.SsaaResolve"; }

 private:
  // Binding sets are keyed on (source, dest) texture identity so a
  // swapchain / SSAA-factor rebuild that swaps the textures recreates
  // the set rather than binding stale pointers. Bounded cache.
  [[nodiscard]] nvrhi::BindingSetHandle GetOrCreateBindingSet(
      nvrhi::ITexture* source, nvrhi::ITexture* dest);

  nvrhi::IDevice* _device = nullptr;
  nvrhi::ShaderHandle _shader;
  nvrhi::BindingLayoutHandle _bindingLayout;
  nvrhi::ComputePipelineHandle _pipeline;
  nvrhi::BufferHandle _paramsBuffer;
  std::unordered_map<uint64_t, nvrhi::BindingSetHandle> _bindingSetCache;
  bool _ready = false;
};

}  // namespace pyxis
