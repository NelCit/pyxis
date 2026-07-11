// Pyxis renderer — DenoiseAtrousPass (RTX-alignment design, Phase B).

#include "Passes/DenoiseAtrousPass.h"

#include "Passes/DenoiseHistoryFixPass.h"
#include "RenderGraph/PassContext.h"
#include "RenderGraph/ShaderLoad.h"

#include <Pyxis/Platform/FileSystem/AssetLocator.h>
#include <Pyxis/Platform/Logging/Log.h>
#include <Pyxis/Platform/Logging/LogCategories.h>
#include <Pyxis/Renderer/Descs/RenderSettings.h>
#include <Pyxis/Renderer/Descs/RenderTargets.h>

#include "ShaderInterop.slang"

#include <array>
#include <cstdint>
#include <string>

namespace pyxis {

namespace {

nvrhi::TextureHandle MakeSignalTexture(nvrhi::IDevice* device, uint32_t width, uint32_t height,
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

std::uint64_t HashPointers(std::initializer_list<const void*> pointers) {
  std::uint64_t key = 1469598103934665603ull;
  for (const void* ptr : pointers) {
    auto bits = reinterpret_cast<std::uintptr_t>(ptr);
    for (int byte = 0; byte < 8; ++byte) {
      key ^= static_cast<std::uint64_t>(bits & 0xFFu);
      key *= 1099511628211ull;
      bits >>= 8;
    }
  }
  return key;
}

// à-trous step-size progression across the 5 iterations — the classic
// "holes" dilation (1, 2, 4, 8, 16).
constexpr std::array<uint32_t, DenoiseAtrousPass::ITERATION_COUNT> STEP_SIZES = {1u, 2u, 4u, 8u,
                                                                                 16u};

}  // namespace

DenoiseAtrousPass::DenoiseAtrousPass(nvrhi::IDevice* device, DenoiseHistoryFixPass& historyFixPass)
    : _device(device), _historyFixPass(&historyFixPass) {
  auto& log = Logging::Get();
  const AssetLocator locator;
  const auto spvPath = locator.LocateResource("shaders/denoise_atrous.spv");
  _shader = LoadSpirvShader(_device, spvPath.View(), nvrhi::ShaderType::Compute, "main",
                            "DenoiseAtrousPass");
  if (!_shader)
    return;

  nvrhi::BindingLayoutDesc layoutDesc;
  layoutDesc.visibility = nvrhi::ShaderType::Compute;
  layoutDesc.bindingOffsets.shaderResource = 0;
  layoutDesc.bindingOffsets.sampler = 0;
  layoutDesc.bindingOffsets.constantBuffer = 0;
  layoutDesc.bindingOffsets.unorderedAccess = 0;
  layoutDesc.bindings = {
      nvrhi::BindingLayoutItem::Texture_SRV(0),             // 0 gInDiffuse
      nvrhi::BindingLayoutItem::Texture_SRV(1),             // 1 gInSpecular
      nvrhi::BindingLayoutItem::Texture_SRV(2),             // 2 gNormalRoughness
      nvrhi::BindingLayoutItem::Texture_SRV(3),             // 3 gViewZ
      nvrhi::BindingLayoutItem::Texture_UAV(4),             // 4 gOutDiffuse
      nvrhi::BindingLayoutItem::Texture_UAV(5),             // 5 gOutSpecular
      nvrhi::BindingLayoutItem::VolatileConstantBuffer(6),  // 6 DenoiseAtrousUniforms
      nvrhi::BindingLayoutItem::Texture_SRV(7),             // 7 gMaterialId (halo-fix edge-stop)
  };
  _bindingLayout = _device->createBindingLayout(layoutDesc);
  if (!_bindingLayout) {
    log.Error(log::RENDER, "DenoiseAtrousPass: createBindingLayout failed");
    return;
  }

  nvrhi::ComputePipelineDesc pipelineDesc;
  pipelineDesc.CS = _shader;
  pipelineDesc.bindingLayouts = {_bindingLayout};
  _pipeline = _device->createComputePipeline(pipelineDesc);
  if (!_pipeline) {
    log.Error(log::RENDER, "DenoiseAtrousPass: createComputePipeline failed");
    return;
  }

  nvrhi::BufferDesc cbDesc;
  cbDesc.byteSize = sizeof(shaderinterop::DenoiseAtrousUniforms);
  cbDesc.isConstantBuffer = true;
  cbDesc.isVolatile = true;
  // 5 writes/frame (one per à-trous iteration); 32 (~6.4 frames of
  // headroom) starved WITHIN the first 8 frames of
  // `render.accumulationFrames` — same maxVersions-starvation finding as
  // DenoiseTemporalPass.Params (see its doc comment): a starved write
  // freezes that iteration's stepSize at version 0's stale value instead
  // of the intended 1/2/4/8/16 dilation. 2560 = 512 frames' worth at
  // 5 writes/frame, matching the Tonemap/AutoExposure precedent.
  cbDesc.maxVersions = 2560;
  cbDesc.debugName = "DenoiseAtrous.Params";
  _paramsBuffer = _device->createBuffer(cbDesc);
  if (!_paramsBuffer) {
    log.Error(log::RENDER, "DenoiseAtrousPass: createBuffer(params) failed");
    return;
  }

  // 1x1 R32_UINT fallback for binding 7 — see the header's doc comment /
  // DenoiseShadowPass's identical fallback.
  {
    nvrhi::TextureDesc fbDesc;
    fbDesc.format = nvrhi::Format::R32_UINT;
    fbDesc.width = 1;
    fbDesc.height = 1;
    fbDesc.dimension = nvrhi::TextureDimension::Texture2D;
    fbDesc.isShaderResource = true;
    fbDesc.debugName = "DenoiseAtrous.FbMaterialId";
    fbDesc.initialState = nvrhi::ResourceStates::ShaderResource;
    fbDesc.keepInitialState = true;
    _fallbackMaterialId = _device->createTexture(fbDesc);
    if (!_fallbackMaterialId) {
      log.Error(log::RENDER, "DenoiseAtrousPass: createTexture(fallbackMaterialId) failed");
      return;
    }
  }

  _ready = true;
  log.Info(log::RENDER, "DenoiseAtrousPass: initialised");
}

DenoiseAtrousPass::~DenoiseAtrousPass() = default;

nvrhi::ITexture* DenoiseAtrousPass::EnsureOutputs(uint32_t width, uint32_t height) {
  if (width == 0u || height == 0u)
    return nullptr;
  if (_pingDiffuse[0] && _outputW == width && _outputH == height)
    return _pingDiffuse[0].Get();

  _pingDiffuse[0] = MakeSignalTexture(_device, width, height, "DenoiseAtrous.Diffuse0");
  _pingDiffuse[1] = MakeSignalTexture(_device, width, height, "DenoiseAtrous.Diffuse1");
  _pingSpecular[0] = MakeSignalTexture(_device, width, height, "DenoiseAtrous.Specular0");
  _pingSpecular[1] = MakeSignalTexture(_device, width, height, "DenoiseAtrous.Specular1");
  _outputW = width;
  _outputH = height;
  _bindingSetCache.clear();

  if (!_pingDiffuse[0] || !_pingDiffuse[1] || !_pingSpecular[0] || !_pingSpecular[1]) {
    Logging::Get().Error(log::RENDER, "DenoiseAtrousPass: createTexture(signal, "
                                          + std::to_string(width) + "x" + std::to_string(height)
                                          + ") failed; pass will skip");
    return nullptr;
  }
  return _pingDiffuse[0].Get();
}

nvrhi::BindingSetHandle DenoiseAtrousPass::GetOrCreateBindingSet(
    nvrhi::ITexture* inDiffuse, nvrhi::ITexture* inSpecular, nvrhi::ITexture* normalRoughness,
    nvrhi::ITexture* viewZ, nvrhi::ITexture* outDiffuse, nvrhi::ITexture* outSpecular,
    nvrhi::ITexture* materialId) {
  const std::uint64_t key = HashPointers(
      {inDiffuse, inSpecular, normalRoughness, viewZ, outDiffuse, outSpecular, materialId});
  if (auto cached = _bindingSetCache.find(key); cached != _bindingSetCache.end())
    return cached->second;
  constexpr std::size_t MAX_CACHE_ENTRIES = 8;
  if (_bindingSetCache.size() >= MAX_CACHE_ENTRIES)
    _bindingSetCache.clear();

  nvrhi::BindingSetDesc setDesc;
  setDesc.bindings = {
      nvrhi::BindingSetItem::Texture_SRV(0, inDiffuse),
      nvrhi::BindingSetItem::Texture_SRV(1, inSpecular),
      nvrhi::BindingSetItem::Texture_SRV(2, normalRoughness),
      nvrhi::BindingSetItem::Texture_SRV(3, viewZ),
      nvrhi::BindingSetItem::Texture_UAV(4, outDiffuse),
      nvrhi::BindingSetItem::Texture_UAV(5, outSpecular),
      nvrhi::BindingSetItem::ConstantBuffer(6, _paramsBuffer),
      nvrhi::BindingSetItem::Texture_SRV(7, materialId),
  };
  nvrhi::BindingSetHandle set = _device->createBindingSet(setDesc, _bindingLayout);
  _bindingSetCache.emplace(key, set);
  return set;
}

void DenoiseAtrousPass::Execute(nvrhi::ICommandList* commandList, const PassContext& context) {
  if (!_ready || commandList == nullptr || context.settings == nullptr
      || context.targets == nullptr || _historyFixPass == nullptr)
    return;
  if ((context.settings->realTimeQuality.passMask & shaderinterop::PASS_MASK_DENOISE) == 0u)
    return;

  nvrhi::ITexture* const normalRoughness = context.gNormalRoughness;
  nvrhi::ITexture* const viewZ = context.gViewZ;
  nvrhi::ITexture* firstInputDiffuse = _historyFixPass->Diffuse();
  nvrhi::ITexture* firstInputSpecular = _historyFixPass->Specular();
  if (normalRoughness == nullptr || viewZ == nullptr || firstInputDiffuse == nullptr
      || firstInputSpecular == nullptr)
    return;
  if (!_pingDiffuse[0] || !_pingDiffuse[1] || !_pingSpecular[0] || !_pingSpecular[1])
    return;
  // Halo-fix materialId edge-stop (see denoise_atrous.slang's file header)
  // — falls back to the 1x1 zero-filled texture when the caller didn't
  // wire materialIdAov.
  nvrhi::ITexture* const materialId =
      context.targets->materialIdAov != nullptr ? context.targets->materialIdAov
                                                : _fallbackMaterialId.Get();
  if (materialId == nullptr)
    return;

  const nvrhi::TextureDesc& desc = firstInputDiffuse->getDesc();
  const uint32_t width = desc.width;
  const uint32_t height = desc.height;
  if (width == 0u || height == 0u)
    return;

  const uint32_t groupsX = (width + 7u) / 8u;
  const uint32_t groupsY = (height + 7u) / 8u;

  nvrhi::ITexture* currentInputDiffuse = firstInputDiffuse;
  nvrhi::ITexture* currentInputSpecular = firstInputSpecular;

  for (uint32_t iteration = 0u; iteration < ITERATION_COUNT; ++iteration) {
    const uint32_t outIndex = iteration % 2u;
    nvrhi::ITexture* const outDiffuse = _pingDiffuse[outIndex].Get();
    nvrhi::ITexture* const outSpecular = _pingSpecular[outIndex].Get();

    shaderinterop::DenoiseAtrousUniforms params{};
    params.destWidth = width;
    params.destHeight = height;
    params.stepSize = STEP_SIZES[iteration];
    // Diffuse luminance edge-stop DISABLED (phi -> inf; RTX-alignment
    // 2026-07-08, "wall speckle" root-cause). The diffuse channel is the
    // DEMODULATED indirect-diffuse GI — its legit content is smooth lighting
    // (texture detail is re-modulated later in composite), and its noise
    // spikes are huge (dome-NEE through windows: hundreds of units), so a
    // fixed phi behaves binary: any usable phi (2..8) leaves every spike a
    // preserved "edge" (exp(-100/phi) ~ 0 either way) that survives all 5
    // iterations AND 96-frame accumulation as deterministic salt-and-pepper
    // speckle — the user-visible "dirty walls" vs ovrtx. ReLAX solves this
    // with variance-guided phi (phi*sqrt(var) -> huge on noisy pixels,
    // tiny on converged ones); without a variance estimate (this chain's
    // documented SVGF simplification), phi=inf IS the high-variance limit,
    // and the normal/viewZ/materialId stops still do all the edge
    // preservation that matters for demodulated lighting. Measured (World
    // Lobby vs ovrtx RT, honest sRGB): whole-frame 0.08986 -> 0.08684
    // (-3.4%), walls id23 MSE -37%, ZERO per-material regressions; wall
    // speckle HF 0.114 -> 0.104 (ovrtx 0.096). The specular channel KEEPS
    // its tight NRD phi: on flat mirrors (glass floor) normal/depth/matId
    // are constant, so the luminance stop is the only weight preserving
    // reflected-CONTENT edges — phi=inf there measured flat on RMSE but
    // blurs the mirror image (perceptually worse; user-reported area).
    params.diffPhiLuminance = 1.0e6f;
    // Spec phi 1.0 -> 0.5 (RTX-alignment 2026-07-11, "lamp reflection too
    // wide"): the lamp-stem floor streak measured 21-25 px at half-max vs
    // ovrtx's 7-8, and the excess is THIS filter smearing the bright core
    // sideways on the flat floor (same-normal taps; exp(-0.5/1.0) = 0.6
    // barely resists) — the traced-lobe trim was a measured non-lever
    // (reflections.slang). A tighter phi preserves reflected-content
    // edges MORE (the historical constraint was only against loosening:
    // phi=inf blurs the glass-floor mirror; sharper is the untested
    // direction).
    params.specPhiLuminance = 0.5f;
    params._pad0 = 0u;
    params._pad1 = 0u;
    params._pad2 = 0u;
    commandList->writeBuffer(_paramsBuffer, &params, sizeof(params));

    const nvrhi::BindingSetHandle bindingSet = GetOrCreateBindingSet(
        currentInputDiffuse, currentInputSpecular, normalRoughness, viewZ, outDiffuse,
        outSpecular, materialId);
    if (!bindingSet)
      return;

    commandList->setTextureState(currentInputDiffuse, nvrhi::AllSubresources,
                                 nvrhi::ResourceStates::ShaderResource);
    commandList->setTextureState(currentInputSpecular, nvrhi::AllSubresources,
                                 nvrhi::ResourceStates::ShaderResource);
    commandList->setTextureState(normalRoughness, nvrhi::AllSubresources,
                                 nvrhi::ResourceStates::ShaderResource);
    commandList->setTextureState(viewZ, nvrhi::AllSubresources,
                                 nvrhi::ResourceStates::ShaderResource);
    commandList->setTextureState(materialId, nvrhi::AllSubresources,
                                 nvrhi::ResourceStates::ShaderResource);
    commandList->setTextureState(outDiffuse, nvrhi::AllSubresources,
                                 nvrhi::ResourceStates::UnorderedAccess);
    commandList->setTextureState(outSpecular, nvrhi::AllSubresources,
                                 nvrhi::ResourceStates::UnorderedAccess);
    commandList->commitBarriers();

    nvrhi::ComputeState state;
    state.pipeline = _pipeline;
    state.bindings = {bindingSet};
    commandList->setComputeState(state);
    commandList->dispatch(groupsX, groupsY, 1u);

    currentInputDiffuse = outDiffuse;
    currentInputSpecular = outSpecular;
  }
  // ITERATION_COUNT is odd, so the loop's final write lands in ping-pong
  // slot 0 — Diffuse()/Specular() read that slot directly (see the
  // header's doc comment); no extra bookkeeping needed here.
}

}  // namespace pyxis
