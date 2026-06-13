// Pyxis renderer — display-transform (tonemap + debug-view encode) pass.
//
// P3 of the pass-split design (_documentation/passes-split-design.md D1): the
// 11-way display branch extracted VERBATIM from the retired raygen.slang
// (now tonemap.slang). Reads the fp32
// LINEAR radiance RaytracedLightingPass writes (PassContext::linearColor, RGBA32F —
// the display path never sees RGBA16F quantization) plus the 10 raw debug-view
// AOVs, applies exposure (2^stops) + Narkowicz ACES on the COLOR path (or the
// matching debug encode), and writes the BGRA8 display output
// (RenderTargets::color) the SSAA resolve / sRGB blit / headless writer / Kit
// export consume exactly as before.
//
// A render-graph pass (§9): runs after RaytracedLightingPass, before SsaaResolvePass
// (the resolve downsamples the TONEMAPPED LINEAR display color, same as when
// raygen wrote it inline). No-ops when targets->color or context.linearColor
// is unbound, and mirrors RaytracedLightingPass's TLAS/camera gates so a skipped trace
// leaves the display output untouched (matching the old inline behaviour).
// Exposure comes from the same GpuScene camera RaytracedLightingPass uploads into
// CameraUniforms.exposure; debugViewMode / worldPosPeriod from RenderSettings
// exactly as RaytracedLightingPass fills FrameUiUniforms.
//
// Bindings (matched to tonemap.slang):
//   space=0, t0      : Texture2D<float4>  fp32 LINEAR radiance (gLinearColor)
//   space=0, t1..t10 : the 10 raw debug-view AOV textures (SRV)
//   space=0, b11     : ConstantBuffer<TonemapUniforms> (volatile)
//   space=0, u12     : RWTexture2D<float4> BGRA8 display output

#pragma once

#include "RenderGraph/IRenderPass.h"

#include <nvrhi/nvrhi.h>

#include <cstdint>
#include <string_view>
#include <unordered_map>

namespace pyxis {

class GpuScene;

class TonemapPass final : public IRenderPass {
 public:
  TonemapPass(nvrhi::IDevice* device, GpuScene& scene);
  ~TonemapPass() override = default;
  TonemapPass(const TonemapPass&) = delete;
  TonemapPass& operator=(const TonemapPass&) = delete;

  void Execute(nvrhi::ICommandList* commandList, const PassContext& context) override;
  [[nodiscard]] std::string_view Name() const override { return "pass.Tonemap"; }

 private:
  // Binding sets are keyed on an FNV1a-64 hash over EVERY borrowed pointer the
  // set references — (linearColor, display target) plus the 10 resolved AOV
  // pointers — so any caller-side AOV swap (resize, null→real flip) lands on a
  // new key instead of binding stale pointers. Bounded cache, same eviction
  // policy as SsaaResolvePass / BlitToSrgbPass.
  [[nodiscard]] nvrhi::BindingSetHandle GetOrCreateBindingSet(
      nvrhi::ITexture* source, nvrhi::ITexture* dest,
      nvrhi::ITexture* const (&aovs)[10], nvrhi::IBuffer* autoExposureStats);

  nvrhi::IDevice* _device = nullptr;
  GpuScene* _scene = nullptr;
  nvrhi::ShaderHandle _shader;
  nvrhi::BindingLayoutHandle _bindingLayout;
  nvrhi::ComputePipelineHandle _pipeline;
  nvrhi::BufferHandle _paramsBuffer;
  std::unordered_map<uint64_t, nvrhi::BindingSetHandle> _bindingSetCache;

  // 1×1 SRV fallbacks for the 10 debug-view AOVs, one per format — bound when
  // the caller doesn't supply that AOV (Kit binds only color/colorHdr) so the
  // binding stays valid; only read if the matching debug view is selected,
  // which those callers never do. Same shape as RaytracedLightingPass's AOV fallbacks.
  nvrhi::TextureHandle _fallbackNormalAov;
  nvrhi::TextureHandle _fallbackDepthAov;
  nvrhi::TextureHandle _fallbackPrimIdAov;
  nvrhi::TextureHandle _fallbackMaterialAov;
  nvrhi::TextureHandle _fallbackBaseColorAov;
  nvrhi::TextureHandle _fallbackWorldPosAov;
  nvrhi::TextureHandle _fallbackAlphaAov;
  nvrhi::TextureHandle _fallbackElementIdAov;
  nvrhi::TextureHandle _fallbackNormalEyeAov;
  nvrhi::TextureHandle _fallbackWorldPosEyeAov;

  // 8-byte raw-buffer fallback for the auto-exposure stats SRV (binding 13),
  // bound when the caller doesn't supply PassContext::autoExposureStats (Kit /
  // tests that never enable auto-exposure). Zero-filled → count 0 → the shader's
  // auto-exposure branch falls through to the manual exposure.
  nvrhi::BufferHandle _fallbackAutoExposureStats;

  bool _ready = false;
};

}  // namespace pyxis
