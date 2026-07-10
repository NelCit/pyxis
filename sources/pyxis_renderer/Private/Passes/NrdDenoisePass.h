// Pyxis renderer — NrdDenoisePass (NRD Stage 3a: the thin render-graph
// pass driving NrdProvider::Evaluate).
//
// RTX-alignment 2026-07-10. Mirrors DlssPass's shape (Private/Passes/
// DlssPass.h — read that file's comment for the shared reasoning): a
// provider-backed pass that no-ops unless the settings it sees resolved
// denoiser == DENOISER_NRD AND its provider reports IsUsable(). Unlike
// DlssPass (whose DlssProvider is PyxisRenderer-owned and ctor-injected,
// because Streamline must interpose device creation), this pass OWNS its
// NrdProvider outright (std::unique_ptr) — NRD needs no device-creation
// hook, the provider only exists in PYXIS_WITH_NRD builds, and this
// pass's sources are CMake-gated behind the SAME if(PYXIS_WITH_NRD)
// block (sources/pyxis_renderer/CMakeLists.txt) as NrdProvider.{h,cpp},
// so no #ifdef is needed anywhere inside.
//
// Data flow per frame (when active): reads the raw signal/guide textures
// off PassContext (gMotionVector / gNormalRoughness / gViewZ /
// gIndirectDiffuse / gReflections), assembles NrdProvider::FrameInputs
// (camera matrices from SceneBindings' Last*() snapshot + this pass's own
// previous-frame copies, jitter from Passes/CameraJitter.h), and calls
// NrdProvider::Evaluate — which records the pack pre-pass + the full
// RELAX_DIFFUSE_SPECULAR dispatch chain onto the pass's command list and
// writes the two RGBA16F outputs this pass owns (EnsureOutputs). On
// success Execute sets PassContext::nrdDenoisedDiffuse/-Specular (the two
// `mutable` hand-forward fields — see PassContext.h's own comment) so
// CompositePass prefers them over the builtin à-trous chain's outputs; on
// failure it leaves them null and the composite falls back to the builtin
// results automatically — that IS the graceful-degradation design, same
// ladder shape DlssPass documents.
//
// Private-only — never crosses the pyxis_renderer.dll boundary, and only
// exists in PYXIS_WITH_NRD=ON builds at all.

#pragma once

#include "RenderGraph/IRenderPass.h"

#include <nvrhi/nvrhi.h>

#include <cstdint>
#include <memory>
#include <string_view>

namespace pyxis {

class NrdProvider;
class SceneBindings;

class NrdDenoisePass final : public IRenderPass {
 public:
  // `device` is borrowed — owned by PyxisRenderer, outlives this pass
  // (same lifetime contract every other pass uses). `sceneBindings`
  // supplies the SAME conformed camera matrices the shaders consumed this
  // frame (SceneBindings::Last*() accessors) so this pass never
  // re-derives them — identical reasoning to DlssPass's ctor injection.
  // Constructs this pass's OWN NrdProvider (see file comment); a provider
  // that fails to construct leaves IsUsable() == false and this pass
  // permanently no-ops, logged once by the provider itself.
  NrdDenoisePass(nvrhi::IDevice* device, SceneBindings& sceneBindings);
  ~NrdDenoisePass() override;
  NrdDenoisePass(const NrdDenoisePass&) = delete;
  NrdDenoisePass& operator=(const NrdDenoisePass&) = delete;

  void Execute(nvrhi::ICommandList* commandList, const PassContext& context) override;
  [[nodiscard]] std::string_view Name() const override { return "pass.NrdDenoise"; }

  // Forwards NrdProvider::IsUsable() — false when NRD instance/pipeline
  // construction failed (the pass then no-ops every frame). PyxisRenderer
  // uses this for its {requested, effective} denoiser downgrade ladder,
  // mirroring how DlssProvider::IsUsable() gates DENOISER_DLSS.
  [[nodiscard]] bool IsUsable() const noexcept;

  // (Re)allocates the two RGBA16F denoised-output textures at `width` x
  // `height` AND forwards NrdProvider::Resize(width, height) (pool +
  // packed textures) — the one pre-frame call the renderer makes on the
  // CPU frame path, gated on effective denoiser == Nrd (PyxisRenderer
  // wires this; same convention as DlssPass::EnsureOutput). Never called
  // from Execute — §30.10, no allocations inside a per-frame body. NOT
  // [[nodiscard]] for the same reason as DlssPass::EnsureOutput: callers
  // only need the sizing side effect; Execute() reads the results back
  // via the members.
  void EnsureOutputs(uint32_t width, uint32_t height);

 private:
  nvrhi::IDevice* _device = nullptr;        // borrowed.
  SceneBindings* _sceneBindings = nullptr;  // borrowed.

  // Owned — see the file comment for why this provider is pass-owned
  // rather than PyxisRenderer-owned like DlssProvider.
  std::unique_ptr<NrdProvider> _provider;

  // The two caller-facing denoised outputs Evaluate() writes
  // (OUT_DIFF/OUT_SPEC_RADIANCE_HITDIST — radiance .rgb + history-length
  // .a, see NrdProvider::FrameInputs' doc comment). RGBA16F, UAV + SRV.
  nvrhi::TextureHandle _outDiffuse;
  nvrhi::TextureHandle _outSpecular;
  uint32_t _outputW = 0;
  uint32_t _outputH = 0;
  bool _outputCreateFailedLogged = false;

  // Previous-frame camera matrices for NrdProvider::FrameInputs'
  // worldToViewPrev/viewToClipPrev — row-major flats, same layout the
  // Last*() accessors store to. Seeded prev = current on the first
  // Execute (frame 0 reads as a static camera, matching the headless
  // static-camera case); refreshed at the end of every Execute that got
  // as far as assembling matrices, success or not (the camera moved on
  // regardless of whether NRD consumed the frame).
  float _prevWorldToViewFlat[16] = {};
  float _prevViewToClipFlat[16] = {};
  bool _hasPrevMatrices = false;

  // One-shot Evaluate-failure log gate (the failure itself repeats every
  // frame in the failing configuration — one line, not a log flood; same
  // pattern as DlssPass's _outputCreateFailedLogged).
  bool _evaluateFailedLogged = false;
};

}  // namespace pyxis
