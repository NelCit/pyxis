// Pyxis renderer — DenoiseHistoryFixPass (RTX-alignment design, Phase B).

#include "Passes/DenoiseHistoryFixPass.h"

#include "Passes/DenoiseTemporalPass.h"
#include "RenderGraph/PassContext.h"
#include "RenderGraph/ShaderLoad.h"

#include <Pyxis/Platform/FileSystem/AssetLocator.h>
#include <Pyxis/Platform/Logging/Log.h>
#include <Pyxis/Platform/Logging/LogCategories.h>
#include <Pyxis/Renderer/Descs/RenderSettings.h>
#include <Pyxis/Renderer/Descs/RenderTargets.h>

#include "ShaderInterop.slang"

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

DenoiseHistoryFixPass::DenoiseHistoryFixPass(nvrhi::IDevice* device,
                                             DenoiseTemporalPass& temporalPass)
    : _device(device), _temporalPass(&temporalPass) {
  auto& log = Logging::Get();
  const AssetLocator locator;
  const auto spvPath = locator.LocateResource("shaders/denoise_history_fix.spv");
  _shader = LoadSpirvShader(_device, spvPath.View(), nvrhi::ShaderType::Compute, "main",
                            "DenoiseHistoryFixPass");
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
      nvrhi::BindingLayoutItem::Texture_UAV(2),             // 2 gOutDiffuse
      nvrhi::BindingLayoutItem::Texture_UAV(3),             // 3 gOutSpecular
      nvrhi::BindingLayoutItem::VolatileConstantBuffer(4),  // 4 DenoiseHistoryFixUniforms
      nvrhi::BindingLayoutItem::Texture_SRV(5),             // 5 gMaterialId (halo-fix edge-stop)
      nvrhi::BindingLayoutItem::Texture_SRV(6),             // 6 gFastDiffuse (thin-geometry fix)
      nvrhi::BindingLayoutItem::Texture_SRV(7),             // 7 gFastSpecular (thin-geometry fix)
      nvrhi::BindingLayoutItem::Texture_SRV(8),             // 8 gNormalRoughness (thin-geometry fix)
      nvrhi::BindingLayoutItem::Texture_SRV(9),             // 9 gViewZ (thin-geometry fix)
  };
  _bindingLayout = _device->createBindingLayout(layoutDesc);
  if (!_bindingLayout) {
    log.Error(log::RENDER, "DenoiseHistoryFixPass: createBindingLayout failed");
    return;
  }

  nvrhi::ComputePipelineDesc pipelineDesc;
  pipelineDesc.CS = _shader;
  pipelineDesc.bindingLayouts = {_bindingLayout};
  _pipeline = _device->createComputePipeline(pipelineDesc);
  if (!_pipeline) {
    log.Error(log::RENDER, "DenoiseHistoryFixPass: createComputePipeline failed");
    return;
  }

  nvrhi::BufferDesc cbDesc;
  cbDesc.byteSize = sizeof(shaderinterop::DenoiseHistoryFixUniforms);
  cbDesc.isConstantBuffer = true;
  // Volatile: writeBuffer'd once per Execute — same maxVersions-starvation
  // finding as DenoiseTemporalPass.Params (see its doc comment): 8 was
  // insufficient for `render.accumulationFrames` past ~frame 8 with no
  // per-frame GPU wait in headless. 512 matches the Tonemap/AutoExposure
  // precedent.
  cbDesc.isVolatile = true;
  cbDesc.maxVersions = 512;
  cbDesc.debugName = "DenoiseHistoryFix.Params";
  _paramsBuffer = _device->createBuffer(cbDesc);
  if (!_paramsBuffer) {
    log.Error(log::RENDER, "DenoiseHistoryFixPass: createBuffer(params) failed");
    return;
  }

  // 1x1 R32_UINT fallback for binding 5 — see the header's doc comment /
  // DenoiseShadowPass's identical fallback.
  {
    nvrhi::TextureDesc fbDesc;
    fbDesc.format = nvrhi::Format::R32_UINT;
    fbDesc.width = 1;
    fbDesc.height = 1;
    fbDesc.dimension = nvrhi::TextureDimension::Texture2D;
    fbDesc.isShaderResource = true;
    fbDesc.debugName = "DenoiseHistoryFix.FbMaterialId";
    fbDesc.initialState = nvrhi::ResourceStates::ShaderResource;
    fbDesc.keepInitialState = true;
    _fallbackMaterialId = _device->createTexture(fbDesc);
    if (!_fallbackMaterialId) {
      log.Error(log::RENDER, "DenoiseHistoryFixPass: createTexture(fallbackMaterialId) failed");
      return;
    }
  }

  _ready = true;
  log.Info(log::RENDER, "DenoiseHistoryFixPass: initialised");
}

DenoiseHistoryFixPass::~DenoiseHistoryFixPass() = default;

nvrhi::ITexture* DenoiseHistoryFixPass::EnsureOutputs(uint32_t width, uint32_t height) {
  if (width == 0u || height == 0u)
    return nullptr;
  if (_outputDiffuse && _outputW == width && _outputH == height)
    return _outputDiffuse.Get();

  _outputDiffuse = MakeSignalTexture(_device, width, height, "DenoiseHistoryFix.OutputDiffuse");
  _outputSpecular = MakeSignalTexture(_device, width, height, "DenoiseHistoryFix.OutputSpecular");
  _outputW = width;
  _outputH = height;
  _bindingSetCache.clear();

  if (!_outputDiffuse || !_outputSpecular) {
    Logging::Get().Error(log::RENDER, "DenoiseHistoryFixPass: createTexture(signal, "
                                          + std::to_string(width) + "x" + std::to_string(height)
                                          + ") failed; pass will skip");
    return nullptr;
  }
  return _outputDiffuse.Get();
}

nvrhi::BindingSetHandle DenoiseHistoryFixPass::GetOrCreateBindingSet(
    nvrhi::ITexture* inDiffuse, nvrhi::ITexture* inSpecular, nvrhi::ITexture* materialId,
    nvrhi::ITexture* fastDiffuse, nvrhi::ITexture* fastSpecular, nvrhi::ITexture* normalRoughness,
    nvrhi::ITexture* viewZ) {
  const std::uint64_t key = HashPointers(
      {inDiffuse, inSpecular, materialId, fastDiffuse, fastSpecular, normalRoughness, viewZ});
  if (auto cached = _bindingSetCache.find(key); cached != _bindingSetCache.end())
    return cached->second;
  constexpr std::size_t MAX_CACHE_ENTRIES = 4;
  if (_bindingSetCache.size() >= MAX_CACHE_ENTRIES)
    _bindingSetCache.clear();

  nvrhi::BindingSetDesc setDesc;
  setDesc.bindings = {
      nvrhi::BindingSetItem::Texture_SRV(0, inDiffuse),
      nvrhi::BindingSetItem::Texture_SRV(1, inSpecular),
      nvrhi::BindingSetItem::Texture_UAV(2, _outputDiffuse.Get()),
      nvrhi::BindingSetItem::Texture_UAV(3, _outputSpecular.Get()),
      nvrhi::BindingSetItem::ConstantBuffer(4, _paramsBuffer),
      nvrhi::BindingSetItem::Texture_SRV(5, materialId),
      nvrhi::BindingSetItem::Texture_SRV(6, fastDiffuse),
      nvrhi::BindingSetItem::Texture_SRV(7, fastSpecular),
      nvrhi::BindingSetItem::Texture_SRV(8, normalRoughness),
      nvrhi::BindingSetItem::Texture_SRV(9, viewZ),
  };
  nvrhi::BindingSetHandle set = _device->createBindingSet(setDesc, _bindingLayout);
  _bindingSetCache.emplace(key, set);
  return set;
}

void DenoiseHistoryFixPass::Execute(nvrhi::ICommandList* commandList,
                                    const PassContext& context) {
  if (!_ready || commandList == nullptr || context.settings == nullptr
      || context.targets == nullptr || _temporalPass == nullptr)
    return;
  if ((context.settings->realTimeQuality.passMask & shaderinterop::PASS_MASK_DENOISE) == 0u)
    return;

  nvrhi::ITexture* const inDiffuse = _temporalPass->Diffuse();
  nvrhi::ITexture* const inSpecular = _temporalPass->Specular();
  if (inDiffuse == nullptr || inSpecular == nullptr)
    return;
  if (!_outputDiffuse || !_outputSpecular)
    return;
  // Halo-fix materialId edge-stop (see denoise_history_fix.slang's file
  // header) — falls back to the 1x1 zero-filled texture when the caller
  // didn't wire materialIdAov.
  nvrhi::ITexture* const materialId =
      context.targets->materialIdAov != nullptr ? context.targets->materialIdAov
                                                : _fallbackMaterialId.Get();
  if (materialId == nullptr)
    return;
  // Work item 4 (thin-geometry flattening fix) — the FAST accumulator
  // (preferred fallback at low local geometry coherence) plus the current
  // G-buffer guides (for the local coherence estimate itself). All four
  // are the SAME textures every other pass in this chain already
  // consumes; a missing one degrades to "nothing new displayed" like
  // every other required input above.
  nvrhi::ITexture* const fastDiffuse = _temporalPass->FastDiffuse();
  nvrhi::ITexture* const fastSpecular = _temporalPass->FastSpecular();
  nvrhi::ITexture* const normalRoughness = context.gNormalRoughness;
  nvrhi::ITexture* const viewZ = context.gViewZ;
  if (fastDiffuse == nullptr || fastSpecular == nullptr || normalRoughness == nullptr
      || viewZ == nullptr)
    return;

  const nvrhi::TextureDesc& desc = inDiffuse->getDesc();
  const uint32_t width = desc.width;
  const uint32_t height = desc.height;
  if (width == 0u || height == 0u)
    return;

  shaderinterop::DenoiseHistoryFixUniforms params{};
  params.destWidth = width;
  params.destHeight = height;
  params._pad0 = 0u;
  params._pad1 = 0u;
  commandList->writeBuffer(_paramsBuffer, &params, sizeof(params));

  const nvrhi::BindingSetHandle bindingSet = GetOrCreateBindingSet(
      inDiffuse, inSpecular, materialId, fastDiffuse, fastSpecular, normalRoughness, viewZ);
  if (!bindingSet)
    return;

  commandList->setTextureState(inDiffuse, nvrhi::AllSubresources,
                               nvrhi::ResourceStates::ShaderResource);
  commandList->setTextureState(inSpecular, nvrhi::AllSubresources,
                               nvrhi::ResourceStates::ShaderResource);
  commandList->setTextureState(fastDiffuse, nvrhi::AllSubresources,
                               nvrhi::ResourceStates::ShaderResource);
  commandList->setTextureState(fastSpecular, nvrhi::AllSubresources,
                               nvrhi::ResourceStates::ShaderResource);
  commandList->setTextureState(normalRoughness, nvrhi::AllSubresources,
                               nvrhi::ResourceStates::ShaderResource);
  commandList->setTextureState(viewZ, nvrhi::AllSubresources,
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

  const uint32_t groupsX = (width + 7u) / 8u;
  const uint32_t groupsY = (height + 7u) / 8u;
  commandList->dispatch(groupsX, groupsY, 1u);
}

}  // namespace pyxis
