// Pyxis renderer — auto-exposure log-luminance reduction pass.
//
// Optional render-graph pass (§9), inserted between RaytracedLightingPass and
// TonemapPass. When RenderSettings::autoExposure is enabled it reduces the fp32
// LINEAR radiance (PassContext::linearColor) to a geometric-mean luminance —
// accumulating sum(log2(lum)) + a lit-pixel count over the frame into the
// shared 8-byte stats buffer (PassContext::autoExposureStats) via deterministic
// integer atomics — which TonemapPass then maps to an exposure that lands the
// average on the target middle-grey. Disabled (the default) the pass is a
// no-op, so the §33.7 byte-equal contract is untouched; enabled, the integer
// reduction is order-independent so it stays byte-equal too.
//
// Bindings (matched to auto_exposure.slang):
//   space=0, t0  : Texture2D<float4>  fp32 LINEAR radiance (gLinearColor)
//   space=0, u1  : RWByteAddressBuffer stats (uint2: sum, count)
//   space=0, b2  : ConstantBuffer<AutoExposureUniforms> (volatile)

#pragma once

#include "RenderGraph/IRenderPass.h"

#include <nvrhi/nvrhi.h>

#include <cstdint>
#include <string_view>
#include <unordered_map>

namespace pyxis {

class AutoExposurePass final : public IRenderPass {
 public:
  explicit AutoExposurePass(nvrhi::IDevice* device);
  ~AutoExposurePass() override = default;
  AutoExposurePass(const AutoExposurePass&) = delete;
  AutoExposurePass& operator=(const AutoExposurePass&) = delete;

  void Execute(nvrhi::ICommandList* commandList, const PassContext& context) override;
  [[nodiscard]] std::string_view Name() const override { return "pass.AutoExposure"; }

  // The 8-byte uint2 stats buffer (sum, count) this pass owns + accumulates;
  // PyxisRenderer threads it into PassContext::autoExposureStats so TonemapPass
  // reads it. Null until the pass initialised successfully.
  [[nodiscard]] nvrhi::IBuffer* StatsBuffer() const { return _statsBuffer.Get(); }

 private:
  [[nodiscard]] nvrhi::BindingSetHandle GetOrCreateBindingSet(nvrhi::ITexture* source,
                                                              nvrhi::IBuffer* stats);

  nvrhi::IDevice* _device = nullptr;
  nvrhi::ShaderHandle _shader;
  nvrhi::BindingLayoutHandle _bindingLayout;
  nvrhi::ComputePipelineHandle _pipeline;
  nvrhi::BufferHandle _paramsBuffer;
  nvrhi::BufferHandle _statsBuffer;  // 8-byte uint2: [0]=log-lum sum, [4]=lit count.
  std::unordered_map<uint64_t, nvrhi::BindingSetHandle> _bindingSetCache;
  bool _ready = false;
};

}  // namespace pyxis
