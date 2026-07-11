// Pyxis renderer — PostBloomPass. See PostBloomPass.h for the role/order
// contract and post_bloom.slang for the measured rationale.

#include "Passes/PostBloomPass.h"

#include "RenderGraph/PassContext.h"
#include "RenderGraph/ShaderLoad.h"

#include <Pyxis/Platform/FileSystem/AssetLocator.h>
#include <Pyxis/Platform/Logging/Log.h>
#include <Pyxis/Platform/Logging/LogCategories.h>
#include <Pyxis/Renderer/Descs/RenderSettings.h>
#include <Pyxis/Renderer/Descs/RenderTargets.h>

#include <cstdint>
#include <initializer_list>

namespace pyxis {

namespace {

// Byte-for-byte lockstep with post_bloom.slang's own PostBloomParams
// (16 bytes; threshold/sigma live as constants in the shader).
struct PostBloomParams {
  uint32_t width;
  uint32_t height;
  float gain;
  uint32_t mode;  // 0 = extract + blurH, 1 = blurV + composite
};

std::uint64_t HashPointers(std::initializer_list<const void*> pointers) noexcept {
  std::uint64_t key = 1469598103934665603ull;
  for (const void* ptr : pointers)
  {
    auto bits = reinterpret_cast<std::uintptr_t>(ptr);
    for (int byte = 0; byte < 8; ++byte)
    {
      key ^= static_cast<std::uint64_t>(bits & 0xFFu);
      key *= 1099511628211ull;
      bits >>= 8;
    }
  }
  return key;
}

}  // namespace

PostBloomPass::PostBloomPass(nvrhi::IDevice* device) : _device(device) {
  auto& log = Logging::Get();
  const AssetLocator locator;
  const auto spvPath = locator.LocateResource("shaders/post_bloom.spv");
  _shader = LoadSpirvShader(_device, spvPath.View(), nvrhi::ShaderType::Compute, "main",
                            "PostBloomPass");
  if (!_shader)
    return;

  nvrhi::BindingLayoutDesc layoutDesc;
  layoutDesc.visibility = nvrhi::ShaderType::Compute;
  layoutDesc.bindingOffsets.shaderResource = 0;
  layoutDesc.bindingOffsets.sampler = 0;
  layoutDesc.bindingOffsets.constantBuffer = 0;
  layoutDesc.bindingOffsets.unorderedAccess = 0;
  layoutDesc.bindings = {
      nvrhi::BindingLayoutItem::Texture_SRV(0),             // 0 gSrcColor
      nvrhi::BindingLayoutItem::Texture_SRV(1),             // 1 gBaseColor
      nvrhi::BindingLayoutItem::Texture_UAV(2),             // 2 gOut
      nvrhi::BindingLayoutItem::VolatileConstantBuffer(3),  // 3 PostBloomParams
  };
  _bindingLayout = _device->createBindingLayout(layoutDesc);
  if (!_bindingLayout)
  {
    log.Error(log::RENDER, "PostBloomPass: createBindingLayout failed");
    return;
  }

  nvrhi::ComputePipelineDesc pipelineDesc;
  pipelineDesc.CS = _shader;
  pipelineDesc.bindingLayouts = {_bindingLayout};
  _pipeline = _device->createComputePipeline(pipelineDesc);
  if (!_pipeline)
  {
    log.Error(log::RENDER, "PostBloomPass: createComputePipeline failed");
    return;
  }

  nvrhi::BufferDesc cbDesc;
  cbDesc.byteSize = sizeof(PostBloomParams);
  cbDesc.isConstantBuffer = true;
  cbDesc.isVolatile = true;
  // TWO writes per frame (one per leg) — 512 keeps the same starvation
  // headroom as every other per-frame volatile CB (see
  // DenoiseTemporalPass.cpp's maxVersions doc comment).
  cbDesc.maxVersions = 512;
  cbDesc.debugName = "PostBloom.Params";
  _paramsBuffer = _device->createBuffer(cbDesc);
  if (!_paramsBuffer)
  {
    log.Error(log::RENDER, "PostBloomPass: createBuffer(params) failed");
    return;
  }

  _ready = true;
  log.Info(log::RENDER, "PostBloomPass: initialised (separable veiling-bloom ready)");
}

PostBloomPass::~PostBloomPass() = default;

void PostBloomPass::EnsureTemps(uint32_t width, uint32_t height, nvrhi::Format format) {
  if (width == 0u || height == 0u || format == nvrhi::Format::UNKNOWN)
    return;
  if (_tempA && _tempB && _tempW == width && _tempH == height && _tempFormat == format)
    return;
  nvrhi::TextureDesc desc;
  desc.width = width;
  desc.height = height;
  desc.format = format;
  desc.dimension = nvrhi::TextureDimension::Texture2D;
  desc.isUAV = true;
  desc.isShaderResource = true;
  desc.initialState = nvrhi::ResourceStates::UnorderedAccess;
  desc.keepInitialState = true;
  desc.debugName = "PostBloom.TempA";
  _tempA = _device->createTexture(desc);
  desc.debugName = "PostBloom.TempB";
  _tempB = _device->createTexture(desc);
  _tempW = width;
  _tempH = height;
  _tempFormat = format;
  _bindingSetCache.clear();  // cached sets referenced the old temps.
}

nvrhi::BindingSetHandle PostBloomPass::GetOrCreateBindingSet(nvrhi::ITexture* src,
                                                             nvrhi::ITexture* base,
                                                             nvrhi::ITexture* out) {
  const std::uint64_t key = HashPointers({src, base, out});
  if (auto cached = _bindingSetCache.find(key); cached != _bindingSetCache.end())
    return cached->second;
  constexpr std::size_t MAX_CACHE_ENTRIES = 8;
  if (_bindingSetCache.size() >= MAX_CACHE_ENTRIES)
    _bindingSetCache.clear();

  nvrhi::BindingSetDesc setDesc;
  setDesc.bindings = {
      nvrhi::BindingSetItem::Texture_SRV(0, src),
      nvrhi::BindingSetItem::Texture_SRV(1, base),
      nvrhi::BindingSetItem::Texture_UAV(2, out),
      nvrhi::BindingSetItem::ConstantBuffer(3, _paramsBuffer),
  };
  nvrhi::BindingSetHandle set = _device->createBindingSet(setDesc, _bindingLayout);
  _bindingSetCache[key] = set;
  return set;
}

void PostBloomPass::Execute(nvrhi::ICommandList* commandList, const PassContext& context) {
  if (!_ready || commandList == nullptr || context.settings == nullptr
      || context.targets == nullptr)
    return;
  const float gain = context.settings->realTimeQuality.postBloomGain;
  if (gain <= 0.0001f)
    return;  // default 0 — pass fully disabled, byte-identical output.
  nvrhi::ITexture* const color = context.targets->color;
  if (color == nullptr || !_tempA || !_tempB)
    return;
  const nvrhi::TextureDesc& desc = color->getDesc();
  if (desc.width != _tempW || desc.height != _tempH || desc.format != _tempFormat)
    return;  // EnsureTemps didn't run for this size/format.

  const uint32_t groupsX = (desc.width + 7u) / 8u;
  const uint32_t groupsY = (desc.height + 7u) / 8u;

  // Leg 1 — extract highlights + horizontal blur: color -> tempA.
  {
    PostBloomParams params{};
    params.width = desc.width;
    params.height = desc.height;
    params.gain = gain;
    params.mode = 0u;
    commandList->writeBuffer(_paramsBuffer, &params, sizeof(params));
    const nvrhi::BindingSetHandle set =
        GetOrCreateBindingSet(color, color, _tempA.Get());
    if (!set)
      return;
    commandList->setTextureState(color, nvrhi::AllSubresources,
                                 nvrhi::ResourceStates::ShaderResource);
    commandList->setTextureState(_tempA, nvrhi::AllSubresources,
                                 nvrhi::ResourceStates::UnorderedAccess);
    commandList->commitBarriers();
    nvrhi::ComputeState state;
    state.pipeline = _pipeline;
    state.bindings = {set};
    commandList->setComputeState(state);
    commandList->dispatch(groupsX, groupsY, 1u);
  }

  // Leg 2 — vertical blur + composite: tempA (+ color) -> tempB.
  {
    PostBloomParams params{};
    params.width = desc.width;
    params.height = desc.height;
    params.gain = gain;
    params.mode = 1u;
    commandList->writeBuffer(_paramsBuffer, &params, sizeof(params));
    const nvrhi::BindingSetHandle set =
        GetOrCreateBindingSet(_tempA.Get(), color, _tempB.Get());
    if (!set)
      return;
    commandList->setTextureState(_tempA, nvrhi::AllSubresources,
                                 nvrhi::ResourceStates::ShaderResource);
    commandList->setTextureState(_tempB, nvrhi::AllSubresources,
                                 nvrhi::ResourceStates::UnorderedAccess);
    commandList->commitBarriers();
    nvrhi::ComputeState state;
    state.pipeline = _pipeline;
    state.bindings = {set};
    commandList->setComputeState(state);
    commandList->dispatch(groupsX, groupsY, 1u);
  }

  // Composited temp back over the display color — downstream consumers
  // keep reading targets->color unchanged (see the .h's rationale).
  commandList->copyTexture(color, nvrhi::TextureSlice{}, _tempB.Get(), nvrhi::TextureSlice{});
}

}  // namespace pyxis
