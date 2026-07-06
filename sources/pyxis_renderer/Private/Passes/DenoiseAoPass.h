// Pyxis renderer — DenoiseAoPass (noise-floor + vegetation spec,
// rtx-realtime-alignment-design.md, 2026-07-06, work item 1).
//
// Cheapest ReBLUR-occlusion-style filter for AmbientOcclusionPass's raw
// gAo — temporal accumulation into a dedicated single-channel ping-pong
// history plus a small edge-aware spatial pre-filter, both folded into
// ONE dispatch of denoise_ao.slang (one pass = one shader). A COMPUTE
// pass, no SceneBindings (screen-space only). Runs alongside the existing
// 4-pass denoiser chain, gated on the SAME RealTimeQuality::passMask bit 5
// (PASS_MASK_DENOISE); CompositePass picks this pass's Output() instead of
// the raw context.gAo when the bit is set (same raw-vs-denoised pattern as
// DenoiseShadowPass/DenoiseAtrousPass).
//
// Owns its own tiny ping-pong (NOT DenoiserResources — see
// denoise_ao.slang's file header for why a dedicated pair is cheaper here
// than widening that class's ownership contract).
//
// Bindings (single Set 0, matched to denoise_ao.slang):
//   0 : Texture2D<float4> gRawAo (SRV)
//   1 : Texture2D<float4> gNormalRoughness (SRV, GBuffer guide)
//   2 : Texture2D<float>  gViewZ (SRV, GBuffer guide)
//   3 : Texture2D<float2> gMotionVector (SRV, GBuffer guide)
//   4 : Texture2D<float4> gPrevAo (SRV)
//   5 : Texture2D<float4> gPrevGuide (SRV)
//   6 : RWTexture2D<float4> gCurrAo (UAV)
//   7 : RWTexture2D<float4> gCurrGuide (UAV)
//   8 : ConstantBuffer<DenoiseAoUniforms>

#pragma once

#include "RenderGraph/IRenderPass.h"

#include <nvrhi/nvrhi.h>

#include <array>
#include <cstdint>
#include <string_view>
#include <unordered_map>

namespace pyxis {

class DenoiseAoPass final : public IRenderPass {
 public:
  explicit DenoiseAoPass(nvrhi::IDevice* device);
  ~DenoiseAoPass() override;
  DenoiseAoPass(const DenoiseAoPass&) = delete;
  DenoiseAoPass& operator=(const DenoiseAoPass&) = delete;

  void Execute(nvrhi::ICommandList* commandList, const PassContext& context) override;
  [[nodiscard]] std::string_view Name() const override { return "pass.DenoiseAo"; }

  // The CURRENT frame's accumulated AO — valid to read only AFTER this
  // pass's Execute() has run this frame. Same "snapshot before Advance()"
  // contract as DenoiseTemporalPass::Diffuse()/Specular() — see this
  // class's .cpp for why.
  [[nodiscard]] nvrhi::ITexture* Output() const noexcept { return _lastWrittenAo; }

 private:
  [[nodiscard]] bool EnsureResources(uint32_t width, uint32_t height);
  [[nodiscard]] nvrhi::BindingSetHandle GetOrCreateBindingSet(
      nvrhi::ITexture* rawAo, nvrhi::ITexture* normalRoughness, nvrhi::ITexture* viewZ,
      nvrhi::ITexture* motionVector);

  nvrhi::IDevice* _device = nullptr;

  nvrhi::ShaderHandle _shader;
  nvrhi::BindingLayoutHandle _bindingLayout;
  nvrhi::ComputePipelineHandle _pipeline;
  nvrhi::BufferHandle _paramsBuffer;

  // Own ping-pong: {ao, historyLength, 0, 1} and {normal.xyz, viewZ}.
  std::array<nvrhi::TextureHandle, 2> _ao;
  std::array<nvrhi::TextureHandle, 2> _guide;
  uint32_t _width = 0;
  uint32_t _height = 0;
  uint32_t _prevIndex = 0;
  uint32_t _currIndex = 1;
  bool _hasHistory = false;

  nvrhi::ITexture* _lastWrittenAo = nullptr;

  std::unordered_map<std::uint64_t, nvrhi::BindingSetHandle> _bindingSetCache;

  bool _ready = false;
};

}  // namespace pyxis
