// Pyxis renderer — DenoiseTemporalPass (RTX-alignment design,
// rtx-realtime-alignment-design.md, Phase B denoiser chain).
//
// ReLAX-class reprojection + accumulation of gIndirectDiffuse /
// gReflections — see denoise_temporal.slang's file header for the full
// estimator. Owns the cross-frame ping-ponged history
// (Private/Passes/DenoiserResources.h). A COMPUTE pass, no SceneBindings.
//
// Bindings (single Set 0, matched to denoise_temporal.slang):
//   0  : Texture2D<float4> gRawDiffuse (SRV)
//   1  : Texture2D<float4> gRawSpecular (SRV)
//   2  : Texture2D<float2> gMotionVector (SRV)
//   3  : Texture2D<float>  gViewZ (SRV)
//   4  : Texture2D<float4> gNormalRoughness (SRV)
//   5  : Texture2D<float4> gPrevDiffuseSlow (SRV)
//   6  : Texture2D<float4> gPrevSpecularSlow (SRV)
//   7  : Texture2D<float4> gPrevNormalViewZ (SRV)
//   8  : Texture2D<float4> gPrevDiffuseFast (SRV)
//   9  : Texture2D<float4> gPrevSpecularFast (SRV)
//   10 : RWTexture2D<float4> gCurrDiffuseSlow (UAV)
//   11 : RWTexture2D<float4> gCurrSpecularSlow (UAV)
//   12 : RWTexture2D<float4> gCurrNormalViewZ (UAV)
//   13 : RWTexture2D<float4> gCurrDiffuseFast (UAV)
//   14 : RWTexture2D<float4> gCurrSpecularFast (UAV)
//   15 : ConstantBuffer<DenoiseTemporalUniforms>
//
// Noise-floor + vegetation spec (rtx-realtime-alignment-design.md,
// 2026-07-06), work item 2 — dual fast+slow history; see
// denoise_temporal.slang's file header for the full estimator.

#pragma once

#include "Passes/DenoiserResources.h"
#include "RenderGraph/IRenderPass.h"

#include <nvrhi/nvrhi.h>

#include <cstdint>
#include <string_view>
#include <unordered_map>

namespace pyxis {

class DenoiseTemporalPass final : public IRenderPass {
 public:
  explicit DenoiseTemporalPass(nvrhi::IDevice* device);
  ~DenoiseTemporalPass() override;
  DenoiseTemporalPass(const DenoiseTemporalPass&) = delete;
  DenoiseTemporalPass& operator=(const DenoiseTemporalPass&) = delete;

  void Execute(nvrhi::ICommandList* commandList, const PassContext& context) override;
  [[nodiscard]] std::string_view Name() const override { return "pass.DenoiseTemporal"; }

  // The CURRENT frame's accumulated diffuse/specular — valid to read only
  // AFTER this pass's Execute() has run this frame (consumed by
  // DenoiseHistoryFixPass, ctor-injected a reference to this pass).
  //
  // Deliberately NOT `_resources.CurrDiffuse()/CurrSpecular()` — Execute()
  // calls `_resources.Advance()` at its own end (so ITS NEXT invocation's
  // internal reprojection reads the right "previous" buffer), which flips
  // which ping-pong slot Curr*() names. A same-frame downstream reader
  // (DenoiseHistoryFixPass) calling these accessors AFTER Advance() has
  // already run would therefore read the OTHER (stale / never-written-
  // this-frame) slot instead of what Execute() just wrote. Caching the
  // just-written pointers BEFORE the Advance() call sidesteps that
  // entirely — see Execute()'s own comment at the call site.
  [[nodiscard]] nvrhi::ITexture* Diffuse() const noexcept { return _lastWrittenDiffuse; }
  [[nodiscard]] nvrhi::ITexture* Specular() const noexcept { return _lastWrittenSpecular; }

  // Work item 2 (ReLAX dual history) / work item 4 (thin-geometry fix) —
  // the FAST (short-memory) accumulator, same "snapshot before Advance()"
  // contract as Diffuse()/Specular() above. DenoiseHistoryFixPass reads
  // these as its preferred fallback at low local geometry coherence.
  [[nodiscard]] nvrhi::ITexture* FastDiffuse() const noexcept { return _lastWrittenDiffuseFast; }
  [[nodiscard]] nvrhi::ITexture* FastSpecular() const noexcept { return _lastWrittenSpecularFast; }

 private:
  [[nodiscard]] nvrhi::BindingSetHandle GetOrCreateBindingSet(
      nvrhi::ITexture* rawDiffuse, nvrhi::ITexture* rawSpecular, nvrhi::ITexture* motionVector,
      nvrhi::ITexture* viewZ, nvrhi::ITexture* normalRoughness);

  nvrhi::IDevice* _device = nullptr;

  nvrhi::ShaderHandle _shader;
  nvrhi::BindingLayoutHandle _bindingLayout;
  nvrhi::ComputePipelineHandle _pipeline;
  nvrhi::BufferHandle _paramsBuffer;

  DenoiserResources _resources;
  // See Diffuse()/Specular()'s doc comment — snapshotted just before
  // Execute() calls _resources.Advance().
  nvrhi::ITexture* _lastWrittenDiffuse = nullptr;
  nvrhi::ITexture* _lastWrittenSpecular = nullptr;
  // Work item 2 — same snapshot contract, for the FAST accumulator.
  nvrhi::ITexture* _lastWrittenDiffuseFast = nullptr;
  nvrhi::ITexture* _lastWrittenSpecularFast = nullptr;

  std::unordered_map<std::uint64_t, nvrhi::BindingSetHandle> _bindingSetCache;

  bool _ready = false;
};

}  // namespace pyxis
