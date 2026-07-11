// Pyxis renderer — DenoiseShadowPass (RTX-alignment design, Phase B).

#include "Passes/DenoiseShadowPass.h"

#include "RenderGraph/PassContext.h"
#include "RenderGraph/ShaderLoad.h"

#include <Pyxis/Platform/FileSystem/AssetLocator.h>
#include <Pyxis/Platform/Logging/Log.h>
#include <Pyxis/Platform/Logging/LogCategories.h>
#include <Pyxis/Renderer/Descs/RenderSettings.h>
#include <Pyxis/Renderer/Descs/RenderTargets.h>

#include "ShaderInterop.slang"

#include <cstring>
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

}  // namespace

DenoiseShadowPass::DenoiseShadowPass(nvrhi::IDevice* device) : _device(device) {
  auto& log = Logging::Get();
  const AssetLocator locator;
  const auto spvPath = locator.LocateResource("shaders/denoise_shadow.spv");
  _shader = LoadSpirvShader(_device, spvPath.View(), nvrhi::ShaderType::Compute, "main",
                            "DenoiseShadowPass");
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
      nvrhi::BindingLayoutItem::Texture_UAV(3),             // 3 gOutDiffuse
      nvrhi::BindingLayoutItem::Texture_UAV(4),             // 4 gOutSpecular
      nvrhi::BindingLayoutItem::VolatileConstantBuffer(5),  // 5 DenoiseShadowUniforms
      nvrhi::BindingLayoutItem::Texture_SRV(6),             // 6 gMaterialId (halo-fix edge-stop)
  };
  _bindingLayout = _device->createBindingLayout(layoutDesc);
  if (!_bindingLayout) {
    log.Error(log::RENDER, "DenoiseShadowPass: createBindingLayout failed");
    return;
  }

  nvrhi::ComputePipelineDesc pipelineDesc;
  pipelineDesc.CS = _shader;
  pipelineDesc.bindingLayouts = {_bindingLayout};
  _pipeline = _device->createComputePipeline(pipelineDesc);
  if (!_pipeline) {
    log.Error(log::RENDER, "DenoiseShadowPass: createComputePipeline failed");
    return;
  }

  nvrhi::BufferDesc cbDesc;
  cbDesc.byteSize = sizeof(shaderinterop::DenoiseShadowUniforms);
  cbDesc.isConstantBuffer = true;
  cbDesc.isVolatile = true;
  // 2 writes/frame; 16 (~8 frames of headroom) starved past frame ~8 of
  // `render.accumulationFrames` — same maxVersions-starvation finding as
  // DenoiseTemporalPass.Params (see its doc comment). 1024 = 512 frames'
  // worth at 2 writes/frame, matching the Tonemap/AutoExposure precedent.
  cbDesc.maxVersions = 1024;
  cbDesc.debugName = "DenoiseShadow.Params";
  _paramsBuffer = _device->createBuffer(cbDesc);
  if (!_paramsBuffer) {
    log.Error(log::RENDER, "DenoiseShadowPass: createBuffer(params) failed");
    return;
  }

  // 1x1 R32_UINT fallback for binding 6 — see the header's doc comment.
  // Same shape as TonemapPass's AOV fallback table; never written, so its
  // content is whatever the allocator hands back, but every pixel this
  // pass actually dispatches over reads it via an out-of-bounds Load
  // (well-defined as 0 in HLSL/Slang) except the single (0,0) texel, so
  // the materialId edge-stop reads 0 == 0 (match) everywhere in practice —
  // a behaviour-preserving no-op when the caller doesn't wire materialIdAov.
  {
    nvrhi::TextureDesc fbDesc;
    fbDesc.format = nvrhi::Format::R32_UINT;
    fbDesc.width = 1;
    fbDesc.height = 1;
    fbDesc.dimension = nvrhi::TextureDimension::Texture2D;
    fbDesc.isShaderResource = true;
    fbDesc.debugName = "DenoiseShadow.FbMaterialId";
    fbDesc.initialState = nvrhi::ResourceStates::ShaderResource;
    fbDesc.keepInitialState = true;
    _fallbackMaterialId = _device->createTexture(fbDesc);
    if (!_fallbackMaterialId) {
      log.Error(log::RENDER, "DenoiseShadowPass: createTexture(fallbackMaterialId) failed");
      return;
    }
  }

  _ready = true;
  log.Info(log::RENDER, "DenoiseShadowPass: initialised");
}

DenoiseShadowPass::~DenoiseShadowPass() = default;

nvrhi::ITexture* DenoiseShadowPass::EnsureOutputs(uint32_t width, uint32_t height) {
  if (width == 0u || height == 0u)
    return nullptr;
  if (_outputDiffuse && _outputW == width && _outputH == height)
    return _outputDiffuse.Get();

  _scratchDiffuse = MakeSignalTexture(_device, width, height, "DenoiseShadow.ScratchDiffuse");
  _scratchSpecular = MakeSignalTexture(_device, width, height, "DenoiseShadow.ScratchSpecular");
  _outputDiffuse = MakeSignalTexture(_device, width, height, "DenoiseShadow.OutputDiffuse");
  _outputSpecular = MakeSignalTexture(_device, width, height, "DenoiseShadow.OutputSpecular");
  _outputW = width;
  _outputH = height;
  _bindingSetCache.clear();

  if (!_scratchDiffuse || !_scratchSpecular || !_outputDiffuse || !_outputSpecular) {
    Logging::Get().Error(log::RENDER, "DenoiseShadowPass: createTexture(signal, "
                                          + std::to_string(width) + "x" + std::to_string(height)
                                          + ") failed; pass will skip");
    return nullptr;
  }
  return _outputDiffuse.Get();
}

nvrhi::BindingSetHandle DenoiseShadowPass::GetOrCreateBindingSet(
    nvrhi::ITexture* inDiffuse, nvrhi::ITexture* inSpecular, nvrhi::ITexture* normalRoughness,
    nvrhi::ITexture* outDiffuse, nvrhi::ITexture* outSpecular, nvrhi::ITexture* materialId) {
  const std::uint64_t key =
      HashPointers({inDiffuse, inSpecular, normalRoughness, outDiffuse, outSpecular, materialId});
  if (auto cached = _bindingSetCache.find(key); cached != _bindingSetCache.end())
    return cached->second;
  constexpr std::size_t MAX_CACHE_ENTRIES = 4;
  if (_bindingSetCache.size() >= MAX_CACHE_ENTRIES)
    _bindingSetCache.clear();

  nvrhi::BindingSetDesc setDesc;
  setDesc.bindings = {
      nvrhi::BindingSetItem::Texture_SRV(0, inDiffuse),
      nvrhi::BindingSetItem::Texture_SRV(1, inSpecular),
      nvrhi::BindingSetItem::Texture_SRV(2, normalRoughness),
      nvrhi::BindingSetItem::Texture_UAV(3, outDiffuse),
      nvrhi::BindingSetItem::Texture_UAV(4, outSpecular),
      nvrhi::BindingSetItem::ConstantBuffer(5, _paramsBuffer),
      nvrhi::BindingSetItem::Texture_SRV(6, materialId),
  };
  nvrhi::BindingSetHandle set = _device->createBindingSet(setDesc, _bindingLayout);
  _bindingSetCache.emplace(key, set);
  return set;
}

void DenoiseShadowPass::Execute(nvrhi::ICommandList* commandList, const PassContext& context) {
  if (!_ready || commandList == nullptr || context.settings == nullptr
      || context.targets == nullptr)
    return;
  // Phase B — passMask bit 5 (PASS_MASK_DENOISE) gates the whole denoiser
  // chain; disabled by default until the chain is validated end-to-end
  // (see PyxisRenderer's registration comment). No-op leaves gDirectDiffuse
  // / gDirectSpecular's raw (un-denoised) contents as the values
  // CompositePass consumes, exactly the pre-Phase-B path.
  if ((context.settings->realTimeQuality.passMask & shaderinterop::PASS_MASK_DENOISE) == 0u)
    return;

  nvrhi::ITexture* const rawDiffuse = context.gDirectDiffuse;
  nvrhi::ITexture* const rawSpecular = context.gDirectSpecular;
  nvrhi::ITexture* const normalRoughness = context.gNormalRoughness;
  if (rawDiffuse == nullptr || rawSpecular == nullptr || normalRoughness == nullptr)
    return;
  if (!_scratchDiffuse || !_scratchSpecular || !_outputDiffuse || !_outputSpecular)
    return;
  // Halo-fix materialId edge-stop (see denoise_shadow.slang's file header)
  // — falls back to the 1x1 zero-filled texture when the caller didn't
  // wire materialIdAov, same defensive pattern TonemapPass's AOV table uses.
  nvrhi::ITexture* const materialId =
      context.targets->materialIdAov != nullptr ? context.targets->materialIdAov
                                                : _fallbackMaterialId.Get();
  if (materialId == nullptr)
    return;

  const nvrhi::TextureDesc& desc = rawDiffuse->getDesc();
  const uint32_t width = desc.width;
  const uint32_t height = desc.height;
  if (width == 0u || height == 0u)
    return;

  const uint32_t groupsX = (width + 7u) / 8u;
  const uint32_t groupsY = (height + 7u) / 8u;

  // ---- Dispatch 0: raw signal -> scratch (narrow radius). --------------
  // Radii 1/3 -> 2/8 (RTX-alignment 2026-07-11, "green sofas really
  // strange"): the felt poufs carry dense converged speckle that the
  // direct-only isolation pinned to THIS channel (flat albedo + smooth
  // normals + zero reflections there — full forensics in the design
  // doc). A ~4px total footprint can't average a 1-spp RIS + area-shadow
  // estimator over 25 disk lights; SIGMA's real kernels are far wider,
  // and this filter's three guides (occluder-hitT, normal, materialId
  // hard reject) are what protect penumbra boundaries at the wider
  // radii — same guided-scalar reasoning as DenoiseAoPass's radius 6.
  {
    shaderinterop::DenoiseShadowUniforms params{};
    params.destWidth = width;
    params.destHeight = height;
    params.radiusPixels = 2.0f;
    params._pad0 = 0u;
    commandList->writeBuffer(_paramsBuffer, &params, sizeof(params));

    const nvrhi::BindingSetHandle bindingSet =
        GetOrCreateBindingSet(rawDiffuse, rawSpecular, normalRoughness, _scratchDiffuse.Get(),
                              _scratchSpecular.Get(), materialId);
    if (!bindingSet)
      return;

    commandList->setTextureState(rawDiffuse, nvrhi::AllSubresources,
                                 nvrhi::ResourceStates::ShaderResource);
    commandList->setTextureState(rawSpecular, nvrhi::AllSubresources,
                                 nvrhi::ResourceStates::ShaderResource);
    commandList->setTextureState(normalRoughness, nvrhi::AllSubresources,
                                 nvrhi::ResourceStates::ShaderResource);
    commandList->setTextureState(materialId, nvrhi::AllSubresources,
                                 nvrhi::ResourceStates::ShaderResource);
    commandList->setTextureState(_scratchDiffuse.Get(), nvrhi::AllSubresources,
                                 nvrhi::ResourceStates::UnorderedAccess);
    commandList->setTextureState(_scratchSpecular.Get(), nvrhi::AllSubresources,
                                 nvrhi::ResourceStates::UnorderedAccess);
    commandList->commitBarriers();

    nvrhi::ComputeState state;
    state.pipeline = _pipeline;
    state.bindings = {bindingSet};
    commandList->setComputeState(state);
    commandList->dispatch(groupsX, groupsY, 1u);
  }

  // ---- Dispatch 1: scratch -> final output (wide radius). --------------
  {
    shaderinterop::DenoiseShadowUniforms params{};
    params.destWidth = width;
    params.destHeight = height;
    params.radiusPixels = 8.0f;
    params._pad0 = 0u;
    commandList->writeBuffer(_paramsBuffer, &params, sizeof(params));

    const nvrhi::BindingSetHandle bindingSet =
        GetOrCreateBindingSet(_scratchDiffuse.Get(), _scratchSpecular.Get(), normalRoughness,
                              _outputDiffuse.Get(), _outputSpecular.Get(), materialId);
    if (!bindingSet)
      return;

    commandList->setTextureState(_scratchDiffuse.Get(), nvrhi::AllSubresources,
                                 nvrhi::ResourceStates::ShaderResource);
    commandList->setTextureState(_scratchSpecular.Get(), nvrhi::AllSubresources,
                                 nvrhi::ResourceStates::ShaderResource);
    commandList->setTextureState(normalRoughness, nvrhi::AllSubresources,
                                 nvrhi::ResourceStates::ShaderResource);
    commandList->setTextureState(materialId, nvrhi::AllSubresources,
                                 nvrhi::ResourceStates::ShaderResource);
    commandList->setTextureState(_outputDiffuse.Get(), nvrhi::AllSubresources,
                                 nvrhi::ResourceStates::UnorderedAccess);
    commandList->setTextureState(_outputSpecular.Get(), nvrhi::AllSubresources,
                                 nvrhi::ResourceStates::UnorderedAccess);
    commandList->commitBarriers();

    nvrhi::ComputeState state;
    state.pipeline = _pipeline;
    state.bindings = {bindingSet};
    commandList->setComputeState(state);
    commandList->dispatch(groupsX, groupsY, 1u);
  }
}

}  // namespace pyxis
