// Pyxis renderer — DenoiseShadowPass (RTX-alignment design,
// rtx-realtime-alignment-design.md, Phase B denoiser chain).
//
// SIGMA-class spatial-only filter on DirectLightingPass's demodulated
// diffuse/specular signal — see denoise_shadow.slang's file header for the
// full estimator. A COMPUTE pass, no SceneBindings (screen-space only).
// Two dispatches per Execute(): dispatch 0 reads the RAW gDirectDiffuse /
// gDirectSpecular (from PassContext) into an internal scratch pair,
// dispatch 1 re-filters the scratch (wider radius) into the final output
// pair CompositePass consumes when PASS_MASK_DENOISE is set.
//
// Bindings (single Set 0, matched to denoise_shadow.slang):
//   0 : Texture2D<float4> gInDiffuse (SRV)
//   1 : Texture2D<float4> gInSpecular (SRV)
//   2 : Texture2D<float4> gNormalRoughness (SRV, GBuffer guide)
//   3 : RWTexture2D<float4> gOutDiffuse (UAV)
//   4 : RWTexture2D<float4> gOutSpecular (UAV)
//   5 : ConstantBuffer<DenoiseShadowUniforms>
//   6 : Texture2D<uint> gMaterialId (SRV, GBuffer guide — RTX-alignment
//       halo fix, 2026-07-05; 1x1 zero-filled fallback when
//       context.targets->materialIdAov is null, same pattern TonemapPass's
//       AOV fallback table uses)

#pragma once

#include "RenderGraph/IRenderPass.h"

#include <nvrhi/nvrhi.h>

#include <cstdint>
#include <string_view>
#include <unordered_map>

namespace pyxis {

class DenoiseShadowPass final : public IRenderPass {
 public:
  explicit DenoiseShadowPass(nvrhi::IDevice* device);
  ~DenoiseShadowPass() override;
  DenoiseShadowPass(const DenoiseShadowPass&) = delete;
  DenoiseShadowPass& operator=(const DenoiseShadowPass&) = delete;

  void Execute(nvrhi::ICommandList* commandList, const PassContext& context) override;
  [[nodiscard]] std::string_view Name() const override { return "pass.DenoiseShadow"; }

  // (Re)creates the scratch + output texture pairs at width x height. One
  // resize cadence, matching every other pass's Ensure* contract — never
  // called inside Execute (§30.10). Return value is a convenience (the
  // diffuse output, non-null iff both output textures exist); callers
  // needing the specular texture use Specular() — NOT [[nodiscard]] since
  // PyxisRenderer's call site only needs the side effect (sizing) and
  // reads both textures back via the named accessors afterward.
  nvrhi::ITexture* EnsureOutputs(uint32_t width, uint32_t height);

  [[nodiscard]] nvrhi::ITexture* Diffuse() const noexcept { return _outputDiffuse.Get(); }
  [[nodiscard]] nvrhi::ITexture* Specular() const noexcept { return _outputSpecular.Get(); }

 private:
  [[nodiscard]] nvrhi::BindingSetHandle GetOrCreateBindingSet(
      nvrhi::ITexture* inDiffuse, nvrhi::ITexture* inSpecular, nvrhi::ITexture* normalRoughness,
      nvrhi::ITexture* outDiffuse, nvrhi::ITexture* outSpecular, nvrhi::ITexture* materialId);

  nvrhi::IDevice* _device = nullptr;

  nvrhi::ShaderHandle _shader;
  nvrhi::BindingLayoutHandle _bindingLayout;
  nvrhi::ComputePipelineHandle _pipeline;
  nvrhi::BufferHandle _paramsBuffer;  // volatile, rewritten between the two dispatches.

  nvrhi::TextureHandle _scratchDiffuse;
  nvrhi::TextureHandle _scratchSpecular;
  nvrhi::TextureHandle _outputDiffuse;
  nvrhi::TextureHandle _outputSpecular;
  // 1x1 R32_UINT zero-filled fallback for binding 6 (gMaterialId) when
  // context.targets->materialIdAov is null — see denoise_shadow.slang's
  // file header for why this makes the new edge-stop a behaviour-
  // preserving no-op in that case (every OOB Load reads back 0).
  nvrhi::TextureHandle _fallbackMaterialId;
  uint32_t _outputW = 0;
  uint32_t _outputH = 0;

  std::unordered_map<std::uint64_t, nvrhi::BindingSetHandle> _bindingSetCache;

  bool _ready = false;
};

}  // namespace pyxis
