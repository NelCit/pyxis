// Pyxis renderer — DenoiseAtrousPass (RTX-alignment design,
// rtx-realtime-alignment-design.md, Phase B denoiser chain).
//
// 5-iteration edge-stopping à-trous filter — see denoise_atrous.slang's
// file header for the full estimator. Ctor-injected a
// `DenoiseHistoryFixPass&` (structural dependency, same convention every
// pass's `SceneBindings&` ctor param uses). Owns its own ping-pong scratch
// pair (WITHIN one frame, across the 5 iterations — NOT cross-frame like
// DenoiserResources); the FINAL iteration's output is what CompositePass
// consumes (Diffuse()/Specular()) when PASS_MASK_DENOISE is set. A
// COMPUTE pass, no SceneBindings.
//
// Bindings (single Set 0, matched to denoise_atrous.slang):
//   0 : Texture2D<float4> gInDiffuse (SRV)
//   1 : Texture2D<float4> gInSpecular (SRV)
//   2 : Texture2D<float4> gNormalRoughness (SRV)
//   3 : Texture2D<float>  gViewZ (SRV)
//   4 : RWTexture2D<float4> gOutDiffuse (UAV)
//   5 : RWTexture2D<float4> gOutSpecular (UAV)
//   6 : ConstantBuffer<DenoiseAtrousUniforms>
//   7 : Texture2D<uint> gMaterialId (SRV, GBuffer guide — RTX-alignment
//       halo fix, 2026-07-05; 1x1 zero-filled fallback when
//       context.targets->materialIdAov is null)

#pragma once

#include "RenderGraph/IRenderPass.h"

#include <nvrhi/nvrhi.h>

#include <array>
#include <cstdint>
#include <string_view>
#include <unordered_map>

namespace pyxis {

class DenoiseHistoryFixPass;

class DenoiseAtrousPass final : public IRenderPass {
 public:
  static constexpr uint32_t ITERATION_COUNT = 5;

  DenoiseAtrousPass(nvrhi::IDevice* device, DenoiseHistoryFixPass& historyFixPass);
  ~DenoiseAtrousPass() override;
  DenoiseAtrousPass(const DenoiseAtrousPass&) = delete;
  DenoiseAtrousPass& operator=(const DenoiseAtrousPass&) = delete;

  void Execute(nvrhi::ICommandList* commandList, const PassContext& context) override;
  [[nodiscard]] std::string_view Name() const override { return "pass.DenoiseAtrous"; }

  // Not [[nodiscard]] — see DenoiseShadowPass::EnsureOutputs's identical
  // doc comment (two-output passes read back via the named accessors).
  nvrhi::ITexture* EnsureOutputs(uint32_t width, uint32_t height);

  // Valid only AFTER this frame's Execute() has run (odd iteration count
  // -> the final result always lands in ping-pong slot 0; see Execute()).
  [[nodiscard]] nvrhi::ITexture* Diffuse() const noexcept { return _pingDiffuse[0].Get(); }
  [[nodiscard]] nvrhi::ITexture* Specular() const noexcept { return _pingSpecular[0].Get(); }

 private:
  [[nodiscard]] nvrhi::BindingSetHandle GetOrCreateBindingSet(
      nvrhi::ITexture* inDiffuse, nvrhi::ITexture* inSpecular, nvrhi::ITexture* normalRoughness,
      nvrhi::ITexture* viewZ, nvrhi::ITexture* outDiffuse, nvrhi::ITexture* outSpecular,
      nvrhi::ITexture* materialId);

  nvrhi::IDevice* _device = nullptr;
  DenoiseHistoryFixPass* _historyFixPass = nullptr;

  nvrhi::ShaderHandle _shader;
  nvrhi::BindingLayoutHandle _bindingLayout;
  nvrhi::ComputePipelineHandle _pipeline;
  nvrhi::BufferHandle _paramsBuffer;

  std::array<nvrhi::TextureHandle, 2> _pingDiffuse;
  std::array<nvrhi::TextureHandle, 2> _pingSpecular;
  // 1x1 R32_UINT zero-filled fallback for binding 7 (gMaterialId) — see
  // denoise_atrous.slang's file header / DenoiseShadowPass's identical
  // fallback for why this is a behaviour-preserving no-op when
  // context.targets->materialIdAov is null.
  nvrhi::TextureHandle _fallbackMaterialId;
  uint32_t _outputW = 0;
  uint32_t _outputH = 0;

  std::unordered_map<std::uint64_t, nvrhi::BindingSetHandle> _bindingSetCache;

  bool _ready = false;
};

}  // namespace pyxis
