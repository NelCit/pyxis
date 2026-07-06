// Pyxis renderer — AccumulationPass (RTX-alignment design,
// rtx-realtime-alignment-design.md, "KEY FINDING (2026-07-06): no true
// accumulation buffer — top lever for <0.05").
//
// Diagnosis that motivated this pass: `render.accumulationFrames` re-runs
// RenderFrame N times per headless session (frameIndex-decorrelated RNG),
// but nothing on the path-trace side actually averages those N samples —
// denoise-OFF measured FLAT across N=1..64. The only temporal averaging in
// the whole pipeline is the denoiser chain's own EMA history, which is
// CAPPED (maxAccumFrames=30 slow / 6 fast / 32 AO — see DenoiseTemporalPass
// / DenoiseAoPass). So a "converged" N=96 render plateaus at that EMA's
// noise FLOOR, not at the true path-traced image, and that residual noise
// inflates every RMSE comparison against a reference render.
//
// Fix: a genuine progressive average (running mean) of the RAW composite
// radiance across frames, computed here:
//     accumBuffer = lerp(accumBuffer, currentComposite, 1/(sampleIndex+1))
// This is the closed-form running mean (sampleIndex = count of PREVIOUSLY
// accumulated samples, 0-based): weight 1.0 on the first sample (pure
// overwrite, ignores whatever garbage a freshly-(re)allocated buffer
// holds), 1/2 on the second, 1/3 on the third, etc. Unlike the denoiser's
// EMA (a fixed-window exponential average that forgets old samples), this
// converges toward the true unbiased Monte-Carlo estimate as N grows —
// there is no floor.
//
// Placement (frozen by the design doc): "Sits after Composite (raw path),
// before AutoExposure". Concretely: registered between CompositePass and
// DlssPass in PyxisRenderer's pass list (PyxisRenderer.cpp), consuming and
// overwriting PassContext::linearColor exactly as CompositePass wrote it —
// same "read the shared scratch, write the blended result back into it"
// shape TaaPass uses for its own history, so every downstream consumer
// (DlssPass / AutoExposurePass / TaaPass / TonemapPass) sees the converged
// result via the one shared pointer without PassContext gaining a new
// field.
//
// CRITICAL design choice — accumulate RAW or DENOISED? This pass
// accumulates whatever CompositePass wrote, and PyxisRenderer::RenderFrame
// forces RealTimeQuality::passMask's DENOISE + TAA bits OFF whenever
// PASS_MASK_ACCUMULATE is set (see its own doc comment) — so in practice
// this always accumulates the RAW (pre-denoise, pre-TAA) unbiased
// composite. Rationale: the denoiser's EMA and TAA's own history are BOTH
// already temporal averages; running this pass on TOP of either would
// double-average (biasing the result, and wasting the denoiser's own
// history-length convergence machinery against a buffer this pass is
// re-converging independently) instead of converging to the same ground
// truth a longer accumulation would reach. The real-time denoiser chain
// remains exactly as-is for the interactive/low-N case; this pass is the
// quality/headless-N alternative that supersedes it at high N, per the
// design doc's "the denoiser can be bypassed at high N" framing.
//
// Gating: RealTimeQuality::passMask bit 7 (PASS_MASK_ACCUMULATE,
// ShaderInterop.slang) — driven from the EXISTING `render.realTimeQuality.
// passMask` config field (no RenderSettings POD change; §18 is frozen
// during concurrent Phase B work). Bit CLEAR (the default — every existing
// config/golden) makes this pass a pure passthrough: Execute() returns
// immediately without touching PassContext::linearColor, so the pre-
// existing single-frame / denoiser-EMA behaviour is byte-for-byte
// unchanged. Bit SET is the new progressive-accumulation mode.
//
// Reset (starts a fresh running mean, weight 1.0 on the next Execute()):
//   - Construction (first frame of a session).
//   - A resolution change (EnsureBuffer's own width/height check, mirrors
//     TaaPass::EnsureHistory).
//   - Transitioning from inactive -> active (a mode flip mid-session).
//   - PyxisRenderer::ResetAccumulation() (public API — the (viewer)
//     camera/settings-change hook the plan §18.6 always reserved for this
//     exact purpose; ResetAccumulation() was a no-op stub until this pass
//     gave it something to reset).
// Headless: static camera + fixed seed + frameIndex-varying RNG (already
// wired — SceneBindings' RtxSamplingUniforms) makes the N-frame
// accumulation loop deterministic; the same seed + same N always converges
// to the same running mean.
//
// A COMPUTE pass — no TraceRay, no SceneBindings (screen-space only, one
// pixel touches only its own address — no ping-pong needed, unlike TaaPass
// / the denoiser chain, which reproject at a MOVED pixel location and so
// need read-old/write-new to be different buffers).
// Bindings (single Set 0, matched to accumulate.slang):
//   0 : Texture2D<float4>   gCurrentColor (SRV) — CompositePass's linearColor
//   1 : RWTexture2D<float4> gAccumBuffer  (UAV) — this pass's own persistent
//                                                 running-mean buffer (read
//                                                 AND written per-pixel)
//   2 : ConstantBuffer<AccumulationUniforms>

#pragma once

#include "RenderGraph/IRenderPass.h"

#include <nvrhi/nvrhi.h>

#include <cstdint>
#include <string_view>
#include <unordered_map>

namespace pyxis {

class AccumulationPass final : public IRenderPass {
 public:
  explicit AccumulationPass(nvrhi::IDevice* device);
  ~AccumulationPass() override;
  AccumulationPass(const AccumulationPass&) = delete;
  AccumulationPass& operator=(const AccumulationPass&) = delete;

  void Execute(nvrhi::ICommandList* commandList, const PassContext& context) override;
  [[nodiscard]] std::string_view Name() const override { return "pass.Accumulation"; }

  // PyxisRenderer::ResetAccumulation() hook — starts a fresh running mean
  // (the NEXT Execute() overwrites the buffer at weight 1.0 instead of
  // blending). Cheap: only resets a counter, the stale buffer contents are
  // never read again once sampleWeight is back to 1.0.
  void Reset() noexcept {
    _sampleCount = 0;
    _wasActive = false;
  }

 private:
  [[nodiscard]] bool EnsureBuffer(uint32_t width, uint32_t height);
  [[nodiscard]] nvrhi::BindingSetHandle GetOrCreateBindingSet(nvrhi::ITexture* currentColor);

  nvrhi::IDevice* _device = nullptr;

  nvrhi::ShaderHandle _shader;
  nvrhi::BindingLayoutHandle _bindingLayout;
  nvrhi::ComputePipelineHandle _pipeline;
  nvrhi::BufferHandle _paramsBuffer;

  nvrhi::TextureHandle _accumBuffer;
  uint32_t _width = 0;
  uint32_t _height = 0;

  // Running-mean state. `_sampleCount` is the number of PREVIOUSLY
  // accumulated samples (0-based; drives sampleWeight = 1/(_sampleCount+1)
  // each Execute()). `_wasActive` tracks whether PASS_MASK_ACCUMULATE was
  // set last frame, so an off->on transition mid-session is detected as a
  // fresh start rather than blending against a stale/never-written buffer.
  uint32_t _sampleCount = 0;
  bool _wasActive = false;

  std::unordered_map<std::uint64_t, nvrhi::BindingSetHandle> _bindingSetCache;

  bool _ready = false;
};

}  // namespace pyxis
