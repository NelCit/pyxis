// Pyxis renderer — NrdDenoisePass (NRD Stage 3a). See NrdDenoisePass.h
// for the role/gating contract and the DlssPass parallel.

#include "Passes/NrdDenoisePass.h"

#include "Nrd/NrdProvider.h"
#include "Passes/CameraJitter.h"
#include "Passes/SceneBindings.h"
#include "RenderGraph/PassContext.h"

#include <Pyxis/Platform/Logging/Log.h>
#include <Pyxis/Platform/Logging/LogCategories.h>
#include <Pyxis/Renderer/Descs/RenderSettings.h>
#include <Pyxis/Renderer/Profiler.h>

#include "ShaderInterop.slang"

#include <hlsl++.h>

#include <cstdint>
#include <cstring>
#include <string>

namespace pyxis {

namespace {

// RGBA16F denoised-output target — the format NRD's
// OUT_DIFF/OUT_SPEC_RADIANCE_HITDIST contract asks for (NRDDescs.h:
// "R11G11B10f+"; RGBA16F matches the raw signal textures CompositePass
// already consumes, so preferring the denoised pointer downstream needs
// no format special-casing). UAV (NRD's final dispatch writes it as a
// storage texture) + SRV (CompositePass reads it).
nvrhi::TextureHandle MakeOutput(nvrhi::IDevice* device, uint32_t width, uint32_t height,
                                const char* debugName) {
  nvrhi::TextureDesc desc;
  desc.width = width;
  desc.height = height;
  desc.format = nvrhi::Format::RGBA16_FLOAT;
  desc.dimension = nvrhi::TextureDimension::Texture2D;
  desc.isUAV = true;
  desc.isShaderResource = true;
  desc.debugName = debugName;
  desc.initialState = nvrhi::ResourceStates::UnorderedAccess;
  desc.keepInitialState = true;
  return device->createTexture(desc);
}

}  // namespace

NrdDenoisePass::NrdDenoisePass(nvrhi::IDevice* device, SceneBindings& sceneBindings)
    : _device(device), _sceneBindings(&sceneBindings),
      _provider(std::make_unique<NrdProvider>(device)) {
  // NrdProvider's own ctor already logged success/failure in detail; one
  // pass-level line here so "why is NrdDenoisePass inert" is answerable
  // from the log without knowing the provider's internals.
  Logging::Get().Info(log::RENDER, std::string{"NrdDenoisePass: constructed (provider "}
                                       + (_provider->IsUsable() ? "usable" : "UNUSABLE — pass will no-op")
                                       + ")");
}

// Out-of-line so std::unique_ptr<NrdProvider>'s deleter sees the complete
// type here (the header only forward-declares NrdProvider).
NrdDenoisePass::~NrdDenoisePass() = default;

bool NrdDenoisePass::IsUsable() const noexcept {
  return _provider != nullptr && _provider->IsUsable();
}

void NrdDenoisePass::EnsureOutputs(uint32_t width, uint32_t height) {
  if (width == 0u || height == 0u)
    return;

  // Provider pools/packed textures resize on the same pre-frame CPU path
  // — NrdProvider::Resize is itself a no-op when the size didn't change,
  // and Evaluate() validates the agreement per call.
  if (_provider)
    _provider->Resize(width, height);

  if (_outputW == width && _outputH == height && _outDiffuse && _outSpecular)
    return;

  nvrhi::TextureHandle outDiffuse = MakeOutput(_device, width, height, "NrdDenoise.OutDiffuse");
  nvrhi::TextureHandle outSpecular = MakeOutput(_device, width, height, "NrdDenoise.OutSpecular");
  if (!outDiffuse || !outSpecular)
  {
    if (!_outputCreateFailedLogged)
    {
      Logging::Get().Error(log::RENDER, "NrdDenoisePass: createTexture(outputs, "
                                            + std::to_string(width) + "x" + std::to_string(height)
                                            + ") failed; keeping previous allocation");
      _outputCreateFailedLogged = true;
    }
    return;
  }
  _outDiffuse = outDiffuse;
  _outSpecular = outSpecular;
  _outputW = width;
  _outputH = height;
  _outputCreateFailedLogged = false;
}

void NrdDenoisePass::Execute(nvrhi::ICommandList* commandList, const PassContext& context) {
  if (commandList == nullptr || context.settings == nullptr || context.profiler == nullptr
      || _provider == nullptr || _sceneBindings == nullptr)
    return;

  // Defensive re-check, same shape as DlssPass::Execute's: the renderer's
  // {requested, effective} denoiser resolution is the real gate; this
  // pass re-checks because (unlike passMask-gated passes) it has no
  // dedicated bit telling it "run this frame".
  if (context.settings->realTimeQuality.denoiser != DENOISER_NRD || !_provider->IsUsable())
    return;

  nvrhi::ITexture* const motionVector = context.gMotionVector;
  nvrhi::ITexture* const normalRoughness = context.gNormalRoughness;
  nvrhi::ITexture* const viewZ = context.gViewZ;
  nvrhi::ITexture* const rawDiffuse = context.gIndirectDiffuse;
  nvrhi::ITexture* const rawSpecular = context.gReflections;
  if (motionVector == nullptr || normalRoughness == nullptr || viewZ == nullptr
      || rawDiffuse == nullptr || rawSpecular == nullptr)
    return;

  // §30.10 — no allocations inside Execute; PyxisRenderer's CPU frame
  // path already called EnsureOutputs(renderWidth, renderHeight). A size
  // mismatch here means that call was skipped (or failed) this frame —
  // no-op and let the composite use the builtin chain's outputs.
  const nvrhi::TextureDesc& renderDesc = viewZ->getDesc();
  if (!_outDiffuse || !_outSpecular || _outputW != renderDesc.width
      || _outputH != renderDesc.height)
    return;

  // The graph-level GpuScope (RenderGraph::Execute, named via Name())
  // brackets the recorded GPU work; this CPU scope brackets the
  // FrameInputs assembly + provider dispatch translation, same split as
  // DlssPass's own inner CpuScope.
  const Profiler::CpuScope cpuScope(*context.profiler, "pass.NrdDenoise.cpu");

  // ---- Camera matrices — SceneBindings' snapshot of THIS frame's
  // conformed matrices (the exact values the raygen consumed — see the
  // Last*() accessors' doc comments), flattened row-major via
  // hlslpp::store exactly like DlssPass. NrdProvider::FrameInputs takes
  // Pyxis's own row-major layout and transposes internally for NRD (see
  // its doc comment) — no transpose here.
  //
  // worldToView: SceneBindings only snapshots worldFromView (view ->
  // world); NRD wants the world -> view direction, so invert on the CPU
  // — same hlslpp::inverse pattern DlssPass::Execute already uses for
  // prevClipToClip / viewFromWorld (RR path).
  const hlslpp::float4x4 viewFromWorld = hlslpp::inverse(_sceneBindings->LastWorldFromView());
  float worldToViewFlat[16];
  float viewToClipFlat[16];
  hlslpp::store(worldToViewFlat, viewFromWorld);
  hlslpp::store(viewToClipFlat, _sceneBindings->LastCameraViewToClip());

  // Frame 0 (or first NRD-active frame): no previous matrices yet — seed
  // prev = current, which reads as a static camera, matching the headless
  // static-camera case (and NRD's CLEAR_AND_RESTART first frame ignores
  // history anyway).
  if (!_hasPrevMatrices)
  {
    std::memcpy(_prevWorldToViewFlat, worldToViewFlat, sizeof(_prevWorldToViewFlat));
    std::memcpy(_prevViewToClipFlat, viewToClipFlat, sizeof(_prevViewToClipFlat));
    _hasPrevMatrices = true;
  }

  // ---- Jitter — the SAME per-frame offset SceneBindings uploads for the
  // raygen (Passes/CameraJitter.h's one-function-two-consumers contract;
  // see DlssPass's identical block for why an independent formula here
  // would be a correctness bug). Like DLSS, NRD needs the renderer to
  // keep the jitter sequence enabled whenever the effective denoiser is
  // Nrd — that's PyxisRenderer's wiring concern, not this pass's.
  const shaderinterop::float2 jitter = ComputeHaltonJitter(context.frameIndex);

  NrdProvider::FrameInputs inputs{};
  inputs.frameIndex = context.frameIndex;
  inputs.renderWidth = renderDesc.width;
  inputs.renderHeight = renderDesc.height;
  inputs.worldToView = worldToViewFlat;
  inputs.viewToClip = viewToClipFlat;
  inputs.worldToViewPrev = _prevWorldToViewFlat;
  inputs.viewToClipPrev = _prevViewToClipFlat;
  inputs.jitterX = jitter.x;
  inputs.jitterY = jitter.y;
  inputs.motionVector = motionVector;
  inputs.normalRoughness = normalRoughness;
  inputs.viewZ = viewZ;
  inputs.diffuseRadianceHitDist = rawDiffuse;
  inputs.specRadianceHitDist = rawSpecular;
  inputs.outDiffuseRadianceHitDist = _outDiffuse.Get();
  inputs.outSpecRadianceHitDist = _outSpecular.Get();

  const bool evaluateOk = _provider->Evaluate(commandList, inputs);

  // Refresh the previous-frame matrices for NEXT frame regardless of this
  // frame's Evaluate outcome — the camera moved on whether or not NRD
  // consumed the frame. (Evaluate copied the values it needed into
  // nrd::CommonSettings synchronously, so overwriting the member arrays
  // the FrameInputs pointers referenced is safe here.)
  std::memcpy(_prevWorldToViewFlat, worldToViewFlat, sizeof(_prevWorldToViewFlat));
  std::memcpy(_prevViewToClipFlat, viewToClipFlat, sizeof(_prevViewToClipFlat));

  if (!evaluateOk)
  {
    // Leave context.nrdDenoisedDiffuse/-Specular null — CompositePass
    // falls back to the builtin chain's outputs automatically; that IS
    // the degradation design (NrdDenoisePass.h's file comment). Logged
    // once, not per frame: the provider already logged the specific
    // failing step, and the failure repeats identically until the
    // configuration changes.
    if (!_evaluateFailedLogged)
    {
      Logging::Get().Error(log::RENDER,
                          "NrdDenoisePass: NrdProvider::Evaluate failed; composite falls back to "
                          "the builtin denoiser outputs (logged once)");
      _evaluateFailedLogged = true;
    }
    return;
  }
  _evaluateFailedLogged = false;

  // Hand the denoised results forward through the shared per-frame
  // context (the two `mutable` fields — PassContext.h's own comment).
  // Resource states are left to NVRHI's tracking, same as every other
  // pass: Evaluate's last dispatch left these in UnorderedAccess, and
  // CompositePass's SRV bind transitions them like any other input.
  context.nrdDenoisedDiffuse = _outDiffuse.Get();
  context.nrdDenoisedSpecular = _outSpecular.Get();
}

}  // namespace pyxis
