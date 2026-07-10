// Pyxis renderer — DlssPass (DLSS Stage 2a: Super Resolution upscale).
//
// rtx-realtime-alignment-design.md, "DLSS scope includes upscaling".
// Registered between CompositePass and AutoExposurePass
// (PyxisRenderer.cpp): reads the render-resolution fp32 LINEAR HDR
// CompositePass wrote (PassContext::linearColor) + the render-resolution
// gViewZ/gMotionVector G-buffer guides, and writes a NEW display-
// resolution fp32 LINEAR HDR texture via Streamline's DLSS-SR feature
// (DlssProvider::Evaluate). PyxisRenderer::RenderFrame redirects
// PassContext::linearColor to point at THIS pass's output afterwards —
// same "produces a differently-sized result and hands the pointer
// forward" shape SsaaResolvePass/TaaPass already use for
// colorLinearResolved / the in-place history copy, respectively.
//
// No-ops (leaves PassContext::linearColor exactly as CompositePass wrote
// it) unless the settings this pass sees resolved denoiser==DENOISER_DLSS
// AND the DlssProvider it was constructed with reports IsUsable() — both
// conditions PyxisRenderer::RenderFrame's {requested, effective} denoiser
// resolution already guarantees line up (effective can only be Dlss when
// the provider is usable), so this pass's own gate is a defensive
// re-check, not a second source of truth.
//
// Not an RT pass, not a compute pass with a Slang shader of its own — it
// calls into the opaque Streamline plugin DLL via DlssProvider, which
// records its own GPU work directly onto the SAME VkCommandBuffer NVRHI's
// ICommandList wraps (see Execute()'s native-handle extraction). Per
// ProgrammingGuideManualHooking.md 7.0, the host must restore the command
// buffer's bound pipeline/descriptor-set state after slEvaluateFeature —
// this pass is always the last thing that touches NVRHI's own binding
// state before AutoExposurePass/TonemapPass rebind their own pipelines
// from scratch, so no explicit restore call is needed here (every
// downstream pass already sets its full state before dispatching).

#pragma once

#include "RenderGraph/IRenderPass.h"

#include <nvrhi/nvrhi.h>

#include <cstdint>
#include <string_view>
#include <unordered_map>

namespace pyxis {

class DlssProvider;
class GpuScene;
class SceneBindings;

class DlssPass final : public IRenderPass {
 public:
  // `dlssProvider` is borrowed — owned by PyxisRenderer, outlives this
  // pass (same lifetime contract every other pass's ctor-injected
  // reference uses). `scene` supplies the active camera's near/far clip
  // + projection mode (CameraDesc); `sceneBindings` supplies the SAME
  // conformed (aspect-ratio-corrected) view/projection matrices + the
  // previous-frame reprojection matrix the shaders consumed this frame
  // (SceneBindings::Last*() accessors) so this pass never has to
  // duplicate SceneBindings' own aspect-conform / motion-vector-history
  // math — same ctor-injection pattern every other RT/compute pass uses.
  DlssPass(nvrhi::IDevice* device, DlssProvider& dlssProvider, GpuScene& scene,
          SceneBindings& sceneBindings);
  ~DlssPass() override;
  DlssPass(const DlssPass&) = delete;
  DlssPass& operator=(const DlssPass&) = delete;

  void Execute(nvrhi::ICommandList* commandList, const PassContext& context) override;
  [[nodiscard]] std::string_view Name() const override { return "pass.Dlss"; }

  // The display-resolution fp32 LINEAR HDR texture this pass writes when
  // active (RGBA32F — matches CompositePass::EnsureLinearColor's format
  // exactly, since AutoExposurePass/TonemapPass read whichever texture
  // PyxisRenderer::RenderFrame threads into PassContext::linearColor,
  // unaware of which pass produced it). (Re)allocated at `width` x
  // `height` on a size change only — never inside Execute (§30.10).
  // PyxisRenderer calls this each RenderFrame with the DISPLAY resolution
  // (targets.color's own dims), gated on effective denoiser == Dlss (see
  // its own call site). NOT [[nodiscard]] — same reasoning as
  // DenoiseShadowPass::EnsureOutputs: the call site only needs the sizing
  // side effect; Execute() reads the result back via Output() below once
  // this pass actually runs, and PassContext::linearColor only gets
  // redirected to it from INSIDE Execute (the pointer differs per frame
  // only on a resize, which the call site doesn't need to react to).
  nvrhi::ITexture* EnsureOutput(uint32_t width, uint32_t height);

  // The currently-allocated output, or null if EnsureOutput hasn't run
  // yet this size. Execute() reads this (never calls EnsureOutput itself
  // — §30.10, no allocations inside Execute).
  [[nodiscard]] nvrhi::ITexture* Output() const noexcept { return _output.Get(); }

  // The RENDER-resolution R32F scratch this pass's own depth-conversion
  // dispatch writes (dlss_depth_convert.slang — linear gViewZ -> standard
  // normalized-device depth, the buffer type Streamline's DLSS-SR plugin
  // actually requires; see that shader's file comment for the full
  // citation). PyxisRenderer calls this alongside EnsureOutput, at render
  // (not display) resolution, gated the same way. NOT [[nodiscard]] for
  // the same reason as EnsureOutput above.
  nvrhi::ITexture* EnsureDepthScratch(uint32_t width, uint32_t height);

 private:
  [[nodiscard]] nvrhi::BindingSetHandle GetOrCreateDepthConvertBindingSet(
      nvrhi::ITexture* viewZ, nvrhi::ITexture* dlssDepth);

  nvrhi::IDevice* _device = nullptr;
  DlssProvider* _dlssProvider = nullptr;    // borrowed.
  GpuScene* _scene = nullptr;               // borrowed.
  SceneBindings* _sceneBindings = nullptr;  // borrowed.

  nvrhi::TextureHandle _output;
  uint32_t _outputW = 0;
  uint32_t _outputH = 0;
  bool _outputCreateFailedLogged = false;

  // 1x1 R32F kBufferTypeExposure buffer (RTX-alignment 2026-07-08). Holds
  // this frame's display exposure gain so DLSS uses PYX's exposure instead of
  // its own auto-exposure estimate. Created once in the ctor (fixed size, no
  // resize); written each frame in Execute via commandList->writeTexture. Null
  // if creation failed (Evaluate then falls back to useAutoExposure).
  nvrhi::TextureHandle _exposureTexture;

  // Depth-conversion compute pipeline (built once in the ctor, same as
  // every other compute pass in this codebase) + its own render-resolution
  // scratch output + per-frame params cbuffer.
  nvrhi::ShaderHandle _depthConvertShader;
  nvrhi::BindingLayoutHandle _depthConvertLayout;
  nvrhi::ComputePipelineHandle _depthConvertPipeline;
  nvrhi::BufferHandle _depthConvertParamsBuffer;
  nvrhi::TextureHandle _dlssDepth;
  uint32_t _dlssDepthW = 0;
  uint32_t _dlssDepthH = 0;
  bool _dlssDepthCreateFailedLogged = false;
  bool _depthConvertReady = false;
  std::unordered_map<std::uint64_t, nvrhi::BindingSetHandle> _depthConvertBindingSetCache;

  // RR pre-exposure compute (dlss_expose.slang — RTX-alignment 2026-07-11).
  // DLSSDOptions has no useAutoExposure/kBufferTypeExposure hook, so the RR
  // path pre-multiplies colorIn's RGB by ComputeEffectiveExposureScale
  // before slEvaluateFeature and divides the reconstructed output by it
  // after (see the shader's file comment for the measured rationale). Same
  // build-once-in-ctor shape as the depth converter above; _exposeReady
  // false (shader/pipeline creation failed) simply skips both dispatches —
  // RR then behaves exactly as before this change.
  [[nodiscard]] nvrhi::BindingSetHandle GetOrCreateExposeBindingSet(nvrhi::ITexture* color);
  nvrhi::ShaderHandle _exposeShader;
  nvrhi::BindingLayoutHandle _exposeLayout;
  nvrhi::ComputePipelineHandle _exposePipeline;
  nvrhi::BufferHandle _exposeParamsBuffer;
  bool _exposeReady = false;
  std::unordered_map<nvrhi::ITexture*, nvrhi::BindingSetHandle> _exposeBindingSetCache;
};

}  // namespace pyxis
