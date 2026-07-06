// Pyxis renderer — TaaPass (RTX-alignment design,
// rtx-realtime-alignment-design.md, Phase B).
//
// Jittered-camera TAA on the linear pre-tonemap color — see taa.slang's
// file header for the full estimator. Runs between AutoExposurePass and
// TonemapPass, gated on RealTimeQuality::passMask bit 6 (PASS_MASK_TAA).
// Default OFF in headless (determinism — a jittered, temporally-blended
// image is not reproducible frame-to-frame the way the SSAA path is);
// the viewer default flip is Phase C's calibration work.
//
// Writes its blended result into its OWN history texture (ping-ponged
// across frames — reading the previous frame's history at a reprojected
// pixel while writing this frame's at the current pixel in the SAME
// dispatch is a hazard, same reasoning DenoiserResources documents), then
// COPIES that texture's contents back into PassContext::linearColor (the
// SAME texture CompositePass wrote and TonemapPass will read) so every
// downstream consumer sees the anti-aliased result via the one shared
// pointer, without PassContext gaining a new field.
//
// A COMPUTE pass — no TraceRay, no SceneBindings (screen-space only).
// Bindings (single Set 0, matched to taa.slang):
//   0 : Texture2D<float4> gCurrentColor (SRV)
//   1 : Texture2D<float4> gPrevHistory (SRV)
//   2 : Texture2D<float2> gMotionVector (SRV)
//   3 : RWTexture2D<float4> gOutHistory (UAV)
//   4 : ConstantBuffer<TaaUniforms>

#pragma once

#include "RenderGraph/IRenderPass.h"

#include <nvrhi/nvrhi.h>

#include <array>
#include <cstdint>
#include <string_view>
#include <unordered_map>

namespace pyxis {

class TaaPass final : public IRenderPass {
 public:
  explicit TaaPass(nvrhi::IDevice* device);
  ~TaaPass() override;
  TaaPass(const TaaPass&) = delete;
  TaaPass& operator=(const TaaPass&) = delete;

  void Execute(nvrhi::ICommandList* commandList, const PassContext& context) override;
  [[nodiscard]] std::string_view Name() const override { return "pass.Taa"; }

 private:
  [[nodiscard]] bool EnsureHistory(uint32_t width, uint32_t height);
  [[nodiscard]] nvrhi::BindingSetHandle GetOrCreateBindingSet(nvrhi::ITexture* currentColor,
                                                              nvrhi::ITexture* motionVector);

  nvrhi::IDevice* _device = nullptr;

  nvrhi::ShaderHandle _shader;
  nvrhi::BindingLayoutHandle _bindingLayout;
  nvrhi::ComputePipelineHandle _pipeline;
  nvrhi::BufferHandle _paramsBuffer;

  std::array<nvrhi::TextureHandle, 2> _history;
  uint32_t _width = 0;
  uint32_t _height = 0;
  uint32_t _prevIndex = 0;
  uint32_t _currIndex = 1;
  bool _hasHistory = false;

  std::unordered_map<std::uint64_t, nvrhi::BindingSetHandle> _bindingSetCache;

  bool _ready = false;
};

}  // namespace pyxis
