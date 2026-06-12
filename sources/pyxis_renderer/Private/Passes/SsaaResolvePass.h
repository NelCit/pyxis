// Pyxis renderer — SSAA box-downsample resolve pass (downsample ONLY).
//
// Deterministic supersampling resolve. When RenderSettings::ssaaFactor > 1 the
// rest of the render graph runs at `factor`× the output resolution per axis (the
// app allocates super-res AOVs); this compute pass box-averages the super-res
// color AOV down to the base-resolution LINEAR intermediate
// (PassContext::colorLinearResolved, renderer-owned). It STAYS LINEAR — averaging
// in linear is the correct downsample (the convex sRGB curve would bias a naive
// average of encoded values dark). The sRGB DISPLAY encode is a separate pass
// (BlitToSrgbPass); this pass does one thing. No jitter, no accumulation —
// bit-exact reproducible.
//
// A render-graph pass (§9): runs after PathTracePass, before BlitToSrgbPass.
// Reads context.targets->color (super-res LINEAR) + context.colorLinearResolved
// (base-res LINEAR out) + context.settings->ssaaFactor; no-ops when ssaaFactor < 2
// or either target is unbound (at factor 1 there is nothing to downsample and
// BlitToSrgbPass reads `color` directly). Owns its own compute shader + pipeline +
// binding layout, mirroring PathTracePass's self-contained construction.
//
// Bindings (matched to ssaa_resolve.slang, renamed from ssaa_downsample at P3):
//   space=0, t0 : Texture2D<float4>   source super-res LINEAR color
//   space=0, u1 : RWTexture2D<float4> destination base-res LINEAR color
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

  // The base-res LINEAR downsample target this pass writes (and BlitToSrgbPass
  // reads). Owned here (the pass has the device); allocated/cached at `width` x
  // `height`, recreated on a size change. RGBA16F to keep precision before the
  // sRGB encode. PyxisRenderer calls this at SSAA factor > 1 and threads the
  // result through PassContext::colorLinearResolved.
  [[nodiscard]] nvrhi::ITexture* EnsureLinearOutput(uint32_t width, uint32_t height);

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
  // Cached base-res LINEAR downsample target (see EnsureLinearOutput).
  nvrhi::TextureHandle _linearOut;
  uint32_t _linearOutW = 0;
  uint32_t _linearOutH = 0;
  bool _ready = false;
};

}  // namespace pyxis
