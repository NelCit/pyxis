// Pyxis renderer — linear→sRGB present blit pass.
//
// The viewer's display present-encode, split out of SsaaResolvePass so each pass
// has a single responsibility. Reads the base-res LINEAR color and writes the
// sRGB-OETF-encoded result into the present target (RenderTargets::colorResolved),
// which the app raw-copies into the sRGB swapchain backbuffer. This is the SAME
// single sRGB encode the headless PNG writer and the Kit pxr viewport
// (HdxColorCorrectionTask) apply, so the standalone viewer matches them.
//
// Source selection (per frame, from PassContext):
//   * SSAA factor 1   → targets->color           (path-tracer LINEAR color AOV)
//   * SSAA factor > 1 → context.colorLinearResolved (SsaaResolvePass LINEAR output)
// No-ops when colorResolved (the present target) is unbound — only the standalone
// viewer binds it; Kit's PyxisEngine never does, so Omniverse is unaffected.
//
// Bindings (matched to blit_to_srgb.slang):
//   space=0, t0 : Texture2D<float4>   base-res LINEAR color
//   space=0, u1 : RWTexture2D<float4> base-res sRGB-encoded output
//   space=0, b2 : ConstantBuffer<BlitToSrgbUniforms> { destWidth, destHeight, pad }

#pragma once

#include "RenderGraph/IRenderPass.h"

#include <nvrhi/nvrhi.h>

#include <cstdint>
#include <string_view>
#include <unordered_map>

namespace pyxis {

class BlitToSrgbPass final : public IRenderPass {
 public:
  explicit BlitToSrgbPass(nvrhi::IDevice* device);
  ~BlitToSrgbPass() override = default;
  BlitToSrgbPass(const BlitToSrgbPass&) = delete;
  BlitToSrgbPass& operator=(const BlitToSrgbPass&) = delete;

  void Execute(nvrhi::ICommandList* commandList, const PassContext& context) override;
  [[nodiscard]] std::string_view Name() const override { return "pass.BlitToSrgb"; }

 private:
  [[nodiscard]] nvrhi::BindingSetHandle GetOrCreateBindingSet(nvrhi::ITexture* source,
                                                              nvrhi::ITexture* dest);

  nvrhi::IDevice* _device = nullptr;
  nvrhi::ShaderHandle _shader;
  nvrhi::BindingLayoutHandle _bindingLayout;
  nvrhi::ComputePipelineHandle _pipeline;
  nvrhi::BufferHandle _paramsBuffer;
  std::unordered_map<uint64_t, nvrhi::BindingSetHandle> _bindingSetCache;
  bool _ready = false;
};

}  // namespace pyxis
