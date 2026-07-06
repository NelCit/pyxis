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
//   5  : Texture2D<float4> gPrevDiffuse (SRV)
//   6  : Texture2D<float4> gPrevSpecular (SRV)
//   7  : Texture2D<float4> gPrevNormalViewZ (SRV)
//   8  : RWTexture2D<float4> gCurrDiffuse (UAV)
//   9  : RWTexture2D<float4> gCurrSpecular (UAV)
//   10 : RWTexture2D<float4> gCurrNormalViewZ (UAV)
//   11 : ConstantBuffer<DenoiseTemporalUniforms>

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

  std::unordered_map<std::uint64_t, nvrhi::BindingSetHandle> _bindingSetCache;

  bool _ready = false;
};

}  // namespace pyxis
