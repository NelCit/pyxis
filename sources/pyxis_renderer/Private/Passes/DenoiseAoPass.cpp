// Pyxis renderer — DenoiseAoPass (noise-floor + vegetation spec, work item 1).

#include "Passes/DenoiseAoPass.h"

#include "RenderGraph/PassContext.h"
#include "RenderGraph/ShaderLoad.h"

#include <Pyxis/Platform/FileSystem/AssetLocator.h>
#include <Pyxis/Platform/Logging/Log.h>
#include <Pyxis/Platform/Logging/LogCategories.h>
#include <Pyxis/Renderer/Descs/RenderSettings.h>
#include <Pyxis/Renderer/Descs/RenderTargets.h>

#include "ShaderInterop.slang"

#include <cstdint>
#include <string>

namespace pyxis {

namespace {

nvrhi::TextureHandle MakeHistoryTexture(nvrhi::IDevice* device, uint32_t width, uint32_t height,
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

DenoiseAoPass::DenoiseAoPass(nvrhi::IDevice* device) : _device(device) {
  auto& log = Logging::Get();
  const AssetLocator locator;
  const auto spvPath = locator.LocateResource("shaders/denoise_ao.spv");
  _shader = LoadSpirvShader(_device, spvPath.View(), nvrhi::ShaderType::Compute, "main",
                            "DenoiseAoPass");
  if (!_shader)
    return;

  nvrhi::BindingLayoutDesc layoutDesc;
  layoutDesc.visibility = nvrhi::ShaderType::Compute;
  layoutDesc.bindingOffsets.shaderResource = 0;
  layoutDesc.bindingOffsets.sampler = 0;
  layoutDesc.bindingOffsets.constantBuffer = 0;
  layoutDesc.bindingOffsets.unorderedAccess = 0;
  layoutDesc.bindings = {
      nvrhi::BindingLayoutItem::Texture_SRV(0),             // 0 gRawAo
      nvrhi::BindingLayoutItem::Texture_SRV(1),             // 1 gNormalRoughness
      nvrhi::BindingLayoutItem::Texture_SRV(2),             // 2 gViewZ
      nvrhi::BindingLayoutItem::Texture_SRV(3),             // 3 gMotionVector
      nvrhi::BindingLayoutItem::Texture_SRV(4),             // 4 gPrevAo
      nvrhi::BindingLayoutItem::Texture_SRV(5),             // 5 gPrevGuide
      nvrhi::BindingLayoutItem::Texture_UAV(6),             // 6 gCurrAo
      nvrhi::BindingLayoutItem::Texture_UAV(7),             // 7 gCurrGuide
      nvrhi::BindingLayoutItem::VolatileConstantBuffer(8),  // 8 DenoiseAoUniforms
  };
  _bindingLayout = _device->createBindingLayout(layoutDesc);
  if (!_bindingLayout) {
    log.Error(log::RENDER, "DenoiseAoPass: createBindingLayout failed");
    return;
  }

  nvrhi::ComputePipelineDesc pipelineDesc;
  pipelineDesc.CS = _shader;
  pipelineDesc.bindingLayouts = {_bindingLayout};
  _pipeline = _device->createComputePipeline(pipelineDesc);
  if (!_pipeline) {
    log.Error(log::RENDER, "DenoiseAoPass: createComputePipeline failed");
    return;
  }

  nvrhi::BufferDesc cbDesc;
  cbDesc.byteSize = sizeof(shaderinterop::DenoiseAoUniforms);
  cbDesc.isConstantBuffer = true;
  // Volatile: writeBuffer'd once per Execute — same maxVersions-starvation
  // finding as DenoiseTemporalPass.Params (see its doc comment). 512
  // matches the established precedent for every other per-pass volatile
  // cbuffer in this chain.
  cbDesc.isVolatile = true;
  cbDesc.maxVersions = 512;
  cbDesc.debugName = "DenoiseAo.Params";
  _paramsBuffer = _device->createBuffer(cbDesc);
  if (!_paramsBuffer) {
    log.Error(log::RENDER, "DenoiseAoPass: createBuffer(params) failed");
    return;
  }

  _ready = true;
  log.Info(log::RENDER, "DenoiseAoPass: initialised");
}

DenoiseAoPass::~DenoiseAoPass() = default;

bool DenoiseAoPass::EnsureResources(uint32_t width, uint32_t height) {
  if (width == 0u || height == 0u)
    return false;
  if (_width == width && _height == height && _ao[0] && _ao[1] && _guide[0] && _guide[1])
    return true;

  std::array<nvrhi::TextureHandle, 2> ao;
  std::array<nvrhi::TextureHandle, 2> guide;
  ao[0] = MakeHistoryTexture(_device, width, height, "DenoiseAo.Ao0");
  ao[1] = MakeHistoryTexture(_device, width, height, "DenoiseAo.Ao1");
  guide[0] = MakeHistoryTexture(_device, width, height, "DenoiseAo.Guide0");
  guide[1] = MakeHistoryTexture(_device, width, height, "DenoiseAo.Guide1");
  if (!ao[0] || !ao[1] || !guide[0] || !guide[1]) {
    Logging::Get().Error(log::RENDER, "DenoiseAoPass: createTexture(history, "
                                          + std::to_string(width) + "x" + std::to_string(height)
                                          + ") failed; keeping previous allocation");
    return false;
  }

  _ao = ao;
  _guide = guide;
  _width = width;
  _height = height;
  _prevIndex = 0u;
  _currIndex = 1u;
  _hasHistory = false;
  _bindingSetCache.clear();
  return true;
}

nvrhi::BindingSetHandle DenoiseAoPass::GetOrCreateBindingSet(nvrhi::ITexture* rawAo,
                                                             nvrhi::ITexture* normalRoughness,
                                                             nvrhi::ITexture* viewZ,
                                                             nvrhi::ITexture* motionVector) {
  // The ping-pong prev/curr textures flip identity every frame — fold them
  // into the cache key too (same reasoning as DenoiseTemporalPass's cache).
  const std::uint64_t key = HashPointers(
      {rawAo, normalRoughness, viewZ, motionVector, _ao[_prevIndex].Get(), _guide[_prevIndex].Get(),
       _ao[_currIndex].Get(), _guide[_currIndex].Get()});
  if (auto cached = _bindingSetCache.find(key); cached != _bindingSetCache.end())
    return cached->second;
  constexpr std::size_t MAX_CACHE_ENTRIES = 4;
  if (_bindingSetCache.size() >= MAX_CACHE_ENTRIES)
    _bindingSetCache.clear();

  nvrhi::BindingSetDesc setDesc;
  setDesc.bindings = {
      nvrhi::BindingSetItem::Texture_SRV(0, rawAo),
      nvrhi::BindingSetItem::Texture_SRV(1, normalRoughness),
      nvrhi::BindingSetItem::Texture_SRV(2, viewZ),
      nvrhi::BindingSetItem::Texture_SRV(3, motionVector),
      nvrhi::BindingSetItem::Texture_SRV(4, _ao[_prevIndex]),
      nvrhi::BindingSetItem::Texture_SRV(5, _guide[_prevIndex]),
      nvrhi::BindingSetItem::Texture_UAV(6, _ao[_currIndex]),
      nvrhi::BindingSetItem::Texture_UAV(7, _guide[_currIndex]),
      nvrhi::BindingSetItem::ConstantBuffer(8, _paramsBuffer),
  };
  nvrhi::BindingSetHandle set = _device->createBindingSet(setDesc, _bindingLayout);
  _bindingSetCache.emplace(key, set);
  return set;
}

void DenoiseAoPass::Execute(nvrhi::ICommandList* commandList, const PassContext& context) {
  if (!_ready || commandList == nullptr || context.settings == nullptr
      || context.targets == nullptr)
    return;
  if ((context.settings->realTimeQuality.passMask & shaderinterop::PASS_MASK_DENOISE) == 0u)
    return;

  nvrhi::ITexture* const rawAo = context.gAo;
  nvrhi::ITexture* const normalRoughness = context.gNormalRoughness;
  nvrhi::ITexture* const viewZ = context.gViewZ;
  nvrhi::ITexture* const motionVector = context.gMotionVector;
  if (rawAo == nullptr || normalRoughness == nullptr || viewZ == nullptr || motionVector == nullptr)
    return;

  const nvrhi::TextureDesc& desc = rawAo->getDesc();
  const uint32_t width = desc.width;
  const uint32_t height = desc.height;
  if (width == 0u || height == 0u)
    return;
  if (!EnsureResources(width, height))
    return;

  shaderinterop::DenoiseAoUniforms params{};
  params.destWidth = width;
  params.destHeight = height;
  params.hasHistory = _hasHistory ? 1u : 0u;
  // RTX-alignment 2026-07-11 ("shadow really not smooth"): the planter
  // contact-shadow blotch is AO-field residue (aoRayLength-0.005 A/B in
  // the design doc). AO is a texture-free scalar whose normal/viewZ
  // guides already protect silhouettes, so filter it MUCH harder than
  // the ReBLUR real-time defaults (radius 2 / cap 32): radius 6 (13x13
  // guided window) + accumulation cap matched to the 96-frame headless
  // convergence window. ovrtx runs its AO denoiser on "aggressive" (the
  // generatedSchema default) — same philosophy.
  params.maxAccumFrames = 96u;
  params.spatialRadiusPixels = 6.0f;
  params._pad0 = 0u;
  params._pad1 = 0u;
  params._pad2 = 0u;
  commandList->writeBuffer(_paramsBuffer, &params, sizeof(params));

  const nvrhi::BindingSetHandle bindingSet =
      GetOrCreateBindingSet(rawAo, normalRoughness, viewZ, motionVector);
  if (!bindingSet)
    return;

  commandList->setTextureState(rawAo, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
  commandList->setTextureState(normalRoughness, nvrhi::AllSubresources,
                               nvrhi::ResourceStates::ShaderResource);
  commandList->setTextureState(viewZ, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
  commandList->setTextureState(motionVector, nvrhi::AllSubresources,
                               nvrhi::ResourceStates::ShaderResource);
  commandList->setTextureState(_ao[_prevIndex].Get(), nvrhi::AllSubresources,
                               nvrhi::ResourceStates::ShaderResource);
  commandList->setTextureState(_guide[_prevIndex].Get(), nvrhi::AllSubresources,
                               nvrhi::ResourceStates::ShaderResource);
  commandList->setTextureState(_ao[_currIndex].Get(), nvrhi::AllSubresources,
                               nvrhi::ResourceStates::UnorderedAccess);
  commandList->setTextureState(_guide[_currIndex].Get(), nvrhi::AllSubresources,
                               nvrhi::ResourceStates::UnorderedAccess);
  commandList->commitBarriers();

  nvrhi::ComputeState state;
  state.pipeline = _pipeline;
  state.bindings = {bindingSet};
  commandList->setComputeState(state);

  const uint32_t groupsX = (width + 7u) / 8u;
  const uint32_t groupsY = (height + 7u) / 8u;
  commandList->dispatch(groupsX, groupsY, 1u);

  // Snapshot the JUST-WRITTEN texture before flipping the ping-pong roles
  // below — same "snapshot before Advance()" contract as
  // DenoiseTemporalPass::Diffuse()/Specular(); a same-frame downstream
  // reader (CompositePass) must see what THIS Execute() wrote, not next
  // frame's "prev" slot.
  _lastWrittenAo = _ao[_currIndex].Get();

  _prevIndex ^= 1u;
  _currIndex ^= 1u;
  _hasHistory = true;
}

}  // namespace pyxis
