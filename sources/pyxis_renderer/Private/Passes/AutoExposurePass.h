// Pyxis renderer — auto-exposure log-luminance reduction pass.
//
// Optional render-graph pass (§9), inserted between RaytracedLightingPass and
// TonemapPass. When RenderSettings::autoExposure selects LEGACY mode (1) it
// reduces the fp32 LINEAR radiance (PassContext::linearColor) to a
// geometric-mean luminance — accumulating sum(log2(lum)) + a lit-pixel count
// + a running max over the frame into the stats buffer
// (PassContext::autoExposureStats) via deterministic integer atomics — which
// TonemapPass then maps to an exposure that lands the highlights on the
// target. When it selects HISTOGRAM mode (2, RTX-alignment design Phase C,
// ovrtx-parity) the pass instead builds a 64-bucket log-luminance histogram
// (same file's `main` entry, mode-branched) and issues a SECOND dispatch
// (`ResolveHistogram`) that reduces it to a single median log2(luminance).
// Disabled (0, the default) the pass is a no-op, so the §33.7 byte-equal
// contract is untouched; enabled (either mode), every reduction step is
// integer/single-thread-deterministic so it stays byte-equal too.
//
// Bindings (matched to auto_exposure.slang; shared by BOTH the `main` and
// `ResolveHistogram` entry points / pipelines):
//   space=0, t0  : Texture2D<float4>  fp32 LINEAR radiance (gLinearColor)
//   space=0, u1  : RWByteAddressBuffer stats (see AUTO_EXPOSURE_STATS_*
//                  layout in ShaderInterop.slang)
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
  // RTX-alignment design (rtx-realtime-alignment-design.md), Phase C —
  // histogram auto-exposure's second dispatch: a single-thread
  // (numthreads(1,1,1)) scan over the 64-bucket histogram `_pipeline`'s
  // `main` entry just built, resolving it to one median log2(luminance)
  // value. Same shader FILE (auto_exposure.slang), same binding layout —
  // only the entry point differs, so both pipelines share `_bindingLayout`
  // and the one binding set GetOrCreateBindingSet returns. No-op (never
  // dispatched) when RenderSettings::autoExposure selects legacy mode.
  nvrhi::ShaderHandle _resolveShader;
  nvrhi::ComputePipelineHandle _resolvePipeline;
  nvrhi::BufferHandle _paramsBuffer;
  // Stats buffer — see AUTO_EXPOSURE_STATS_* in ShaderInterop.slang for the
  // byte layout (legacy sum/count/max, histogram buckets, resolved median).
  nvrhi::BufferHandle _statsBuffer;
  std::unordered_map<uint64_t, nvrhi::BindingSetHandle> _bindingSetCache;
  bool _ready = false;
};

}  // namespace pyxis
