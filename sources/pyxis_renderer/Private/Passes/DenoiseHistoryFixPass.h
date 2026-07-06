// Pyxis renderer — DenoiseHistoryFixPass (RTX-alignment design,
// rtx-realtime-alignment-design.md, Phase B denoiser chain).
//
// Widens spatial support for low-history-length pixels coming out of
// DenoiseTemporalPass — see denoise_history_fix.slang's file header.
// Ctor-injected a `DenoiseTemporalPass&` (structural dependency, same
// convention every pass's `SceneBindings&` ctor param uses) so it reads
// that pass's CURRENT-frame output directly rather than round-tripping
// through PassContext. A COMPUTE pass, no SceneBindings.
//
// Bindings (single Set 0, matched to denoise_history_fix.slang):
//   0 : Texture2D<float4> gInDiffuse (SRV)
//   1 : Texture2D<float4> gInSpecular (SRV)
//   2 : RWTexture2D<float4> gOutDiffuse (UAV)
//   3 : RWTexture2D<float4> gOutSpecular (UAV)
//   4 : ConstantBuffer<DenoiseHistoryFixUniforms>
//   5 : Texture2D<uint> gMaterialId (SRV, GBuffer guide — RTX-alignment
//       halo fix, 2026-07-05; 1x1 zero-filled fallback when
//       context.targets->materialIdAov is null)
//   6 : Texture2D<float4> gFastDiffuse (SRV, DenoiseTemporalPass FAST output)
//   7 : Texture2D<float4> gFastSpecular (SRV, DenoiseTemporalPass FAST output)
//   8 : Texture2D<float4> gNormalRoughness (SRV, GBuffer guide)
//   9 : Texture2D<float>  gViewZ (SRV, GBuffer guide)
//
// Noise-floor + vegetation spec (rtx-realtime-alignment-design.md,
// 2026-07-06), work item 4 — thin-geometry flattening fix; see
// denoise_history_fix.slang's file header for the full estimator.

#pragma once

#include "RenderGraph/IRenderPass.h"

#include <nvrhi/nvrhi.h>

#include <cstdint>
#include <string_view>
#include <unordered_map>

namespace pyxis {

class DenoiseTemporalPass;

class DenoiseHistoryFixPass final : public IRenderPass {
 public:
  DenoiseHistoryFixPass(nvrhi::IDevice* device, DenoiseTemporalPass& temporalPass);
  ~DenoiseHistoryFixPass() override;
  DenoiseHistoryFixPass(const DenoiseHistoryFixPass&) = delete;
  DenoiseHistoryFixPass& operator=(const DenoiseHistoryFixPass&) = delete;

  void Execute(nvrhi::ICommandList* commandList, const PassContext& context) override;
  [[nodiscard]] std::string_view Name() const override { return "pass.DenoiseHistoryFix"; }

  // Not [[nodiscard]] — see DenoiseShadowPass::EnsureOutputs's identical
  // doc comment (two-output passes read back via the named accessors).
  nvrhi::ITexture* EnsureOutputs(uint32_t width, uint32_t height);

  [[nodiscard]] nvrhi::ITexture* Diffuse() const noexcept { return _outputDiffuse.Get(); }
  [[nodiscard]] nvrhi::ITexture* Specular() const noexcept { return _outputSpecular.Get(); }

 private:
  [[nodiscard]] nvrhi::BindingSetHandle GetOrCreateBindingSet(
      nvrhi::ITexture* inDiffuse, nvrhi::ITexture* inSpecular, nvrhi::ITexture* materialId,
      nvrhi::ITexture* fastDiffuse, nvrhi::ITexture* fastSpecular, nvrhi::ITexture* normalRoughness,
      nvrhi::ITexture* viewZ);

  nvrhi::IDevice* _device = nullptr;
  DenoiseTemporalPass* _temporalPass = nullptr;

  nvrhi::ShaderHandle _shader;
  nvrhi::BindingLayoutHandle _bindingLayout;
  nvrhi::ComputePipelineHandle _pipeline;
  nvrhi::BufferHandle _paramsBuffer;

  nvrhi::TextureHandle _outputDiffuse;
  nvrhi::TextureHandle _outputSpecular;
  // 1x1 R32_UINT zero-filled fallback for binding 5 (gMaterialId) — see
  // denoise_history_fix.slang's file header / DenoiseShadowPass's
  // identical fallback for why this is a behaviour-preserving no-op when
  // context.targets->materialIdAov is null.
  nvrhi::TextureHandle _fallbackMaterialId;
  uint32_t _outputW = 0;
  uint32_t _outputH = 0;

  std::unordered_map<std::uint64_t, nvrhi::BindingSetHandle> _bindingSetCache;

  bool _ready = false;
};

}  // namespace pyxis
