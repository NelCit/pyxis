// Pyxis renderer — PostBloomPass (RTX-alignment 2026-07-11, "window
// borders shadowed" — ovrtx veiling-halo match).
//
// Adds post_bloom.slang's wide veiling bloom to the post-tonemap display
// color when RealTimeQuality::postBloomGain > 0 (threshold/sigma are
// fixed measured constants in the shader; see its file comment for the
// numbers and the ring-profile evidence). Registered between
// PostSoftenPass and SsaaResolvePass — the measured 0.07877 was bloom
// applied ON TOP of the softened image, so the order is normative. A zero
// gain (the default) early-outs before any GPU work — byte-identical
// output, goldens untouched.
//
// Two dispatches of the single shader entry (separable Gaussian):
//   1. extract+blurH:   targets->color -> _tempA
//   2. blurV+composite: _tempA (+ targets->color) -> _tempB
// then copyTexture _tempB back over targets->color — same owned-temp
// shape as PostSoftenPass, for the same reason (every downstream consumer
// reads targets->color directly).
//
// Bindings — single Set 0 (matched to post_bloom.slang):
//   0 : Texture2D<float4>   gSrcColor  (SRV — blur source for the leg)
//   1 : Texture2D<float4>   gBaseColor (SRV — display color)
//   2 : RWTexture2D<float4> gOut       (UAV — owned temp)
//   3 : ConstantBuffer<PostBloomParams>

#pragma once

#include "RenderGraph/IRenderPass.h"

#include <nvrhi/nvrhi.h>

#include <cstdint>
#include <string_view>
#include <unordered_map>

namespace pyxis {

class PostBloomPass final : public IRenderPass {
 public:
  explicit PostBloomPass(nvrhi::IDevice* device);
  ~PostBloomPass() override;
  PostBloomPass(const PostBloomPass&) = delete;
  PostBloomPass& operator=(const PostBloomPass&) = delete;

  void Execute(nvrhi::ICommandList* commandList, const PassContext& context) override;
  [[nodiscard]] std::string_view Name() const override { return "pass.PostBloom"; }

  // (Re)allocates both temps at `width` x `height` x `format` — called by
  // PyxisRenderer on the CPU frame path when the pass will run this frame
  // (never inside Execute, §30.10). Format tracks targets->color's own.
  void EnsureTemps(uint32_t width, uint32_t height, nvrhi::Format format);

 private:
  [[nodiscard]] nvrhi::BindingSetHandle GetOrCreateBindingSet(nvrhi::ITexture* src,
                                                              nvrhi::ITexture* base,
                                                              nvrhi::ITexture* out);

  nvrhi::IDevice* _device = nullptr;

  nvrhi::ShaderHandle _shader;
  nvrhi::BindingLayoutHandle _bindingLayout;
  nvrhi::ComputePipelineHandle _pipeline;
  nvrhi::BufferHandle _paramsBuffer;

  nvrhi::TextureHandle _tempA;
  nvrhi::TextureHandle _tempB;
  uint32_t _tempW = 0;
  uint32_t _tempH = 0;
  nvrhi::Format _tempFormat = nvrhi::Format::UNKNOWN;

  std::unordered_map<std::uint64_t, nvrhi::BindingSetHandle> _bindingSetCache;

  bool _ready = false;
};

}  // namespace pyxis
