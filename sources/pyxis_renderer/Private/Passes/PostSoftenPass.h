// Pyxis renderer — PostSoftenPass (RTX-alignment 2026-07-10, "image not
// smooth" — ovrtx output-character match).
//
// Applies post_soften.slang's small display-space Gaussian (radius 2) to
// the post-tonemap display color when RealTimeQuality::postSoftenSigma
// > 0, reproducing the DLSS-processed softness of the ovrtx reference
// output (see the shader's file comment for the measured numbers).
// Registered between TonemapPass and SsaaResolvePass; a zero sigma (the
// default) early-outs before any GPU work — byte-identical output,
// goldens untouched.
//
// Compute can't safely read+write one texture, so the dispatch blurs
// targets->color into an owned same-format temp and copyTexture's it
// back — one extra copy, chosen over ping-pong pointer surgery because
// every downstream consumer (SsaaResolve / BlitToSrgb / the headless EXR
// writer) reads targets->color directly and re-pointing them all would
// spread this pass's existence across the graph.
//
// Bindings — single Set 0 (matched to post_soften.slang):
//   0 : Texture2D<float4>   gColorIn   (SRV — display color)
//   1 : RWTexture2D<float4> gColorOut  (UAV — owned temp)
//   2 : ConstantBuffer<PostSoftenParams>

#pragma once

#include "RenderGraph/IRenderPass.h"

#include <nvrhi/nvrhi.h>

#include <cstdint>
#include <string_view>
#include <unordered_map>

namespace pyxis {

class PostSoftenPass final : public IRenderPass {
 public:
  explicit PostSoftenPass(nvrhi::IDevice* device);
  ~PostSoftenPass() override;
  PostSoftenPass(const PostSoftenPass&) = delete;
  PostSoftenPass& operator=(const PostSoftenPass&) = delete;

  void Execute(nvrhi::ICommandList* commandList, const PassContext& context) override;
  [[nodiscard]] std::string_view Name() const override { return "pass.PostSoften"; }

  // (Re)allocates the temp at `width` x `height` x `format` — called by
  // PyxisRenderer on the CPU frame path when the pass will run this frame
  // (never inside Execute, §30.10). Format tracks targets->color's own
  // (RGBA32F headless, whatever the viewer target uses otherwise).
  void EnsureTemp(uint32_t width, uint32_t height, nvrhi::Format format);

 private:
  [[nodiscard]] nvrhi::BindingSetHandle GetOrCreateBindingSet(nvrhi::ITexture* colorIn);

  nvrhi::IDevice* _device = nullptr;

  nvrhi::ShaderHandle _shader;
  nvrhi::BindingLayoutHandle _bindingLayout;
  nvrhi::ComputePipelineHandle _pipeline;
  nvrhi::BufferHandle _paramsBuffer;

  nvrhi::TextureHandle _temp;
  uint32_t _tempW = 0;
  uint32_t _tempH = 0;
  nvrhi::Format _tempFormat = nvrhi::Format::UNKNOWN;

  std::unordered_map<nvrhi::ITexture*, nvrhi::BindingSetHandle> _bindingSetCache;

  bool _ready = false;
};

}  // namespace pyxis
