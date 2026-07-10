// Pyxis renderer — PostSoftenPass. See PostSoftenPass.h for the full
// role/gating contract and post_soften.slang for the measured rationale.

#include "Passes/PostSoftenPass.h"

#include "RenderGraph/PassContext.h"
#include "RenderGraph/ShaderLoad.h"

#include <Pyxis/Platform/FileSystem/AssetLocator.h>
#include <Pyxis/Platform/Logging/Log.h>
#include <Pyxis/Platform/Logging/LogCategories.h>
#include <Pyxis/Renderer/Descs/RenderSettings.h>
#include <Pyxis/Renderer/Descs/RenderTargets.h>

#include <cmath>
#include <cstdint>

namespace pyxis {

namespace {

// Byte-for-byte lockstep with post_soften.slang's own PostSoftenParams —
// see that struct's doc comment (weight2 is derived in-shader from the
// 5-tap normalization, keeping this at exactly 16 bytes).
struct PostSoftenParams {
  uint32_t width;
  uint32_t height;
  float weight0;
  float weight1;
};

}  // namespace

PostSoftenPass::PostSoftenPass(nvrhi::IDevice* device) : _device(device) {
  auto& log = Logging::Get();
  const AssetLocator locator;
  const auto spvPath = locator.LocateResource("shaders/post_soften.spv");
  _shader = LoadSpirvShader(_device, spvPath.View(), nvrhi::ShaderType::Compute, "main",
                            "PostSoftenPass");
  if (!_shader)
    return;

  nvrhi::BindingLayoutDesc layoutDesc;
  layoutDesc.visibility = nvrhi::ShaderType::Compute;
  layoutDesc.bindingOffsets.shaderResource = 0;
  layoutDesc.bindingOffsets.sampler = 0;
  layoutDesc.bindingOffsets.constantBuffer = 0;
  layoutDesc.bindingOffsets.unorderedAccess = 0;
  layoutDesc.bindings = {
      nvrhi::BindingLayoutItem::Texture_SRV(0),             // 0 gColorIn
      nvrhi::BindingLayoutItem::Texture_UAV(1),             // 1 gColorOut
      nvrhi::BindingLayoutItem::VolatileConstantBuffer(2),  // 2 PostSoftenParams
  };
  _bindingLayout = _device->createBindingLayout(layoutDesc);
  if (!_bindingLayout) {
    log.Error(log::RENDER, "PostSoftenPass: createBindingLayout failed");
    return;
  }

  nvrhi::ComputePipelineDesc pipelineDesc;
  pipelineDesc.CS = _shader;
  pipelineDesc.bindingLayouts = {_bindingLayout};
  _pipeline = _device->createComputePipeline(pipelineDesc);
  if (!_pipeline) {
    log.Error(log::RENDER, "PostSoftenPass: createComputePipeline failed");
    return;
  }

  nvrhi::BufferDesc cbDesc;
  cbDesc.byteSize = sizeof(PostSoftenParams);
  cbDesc.isConstantBuffer = true;
  cbDesc.isVolatile = true;
  // One write per frame; 512 matches the Tonemap/AutoExposure precedent
  // (see DenoiseTemporalPass.cpp's maxVersions-starvation doc comment).
  cbDesc.maxVersions = 512;
  cbDesc.debugName = "PostSoften.Params";
  _paramsBuffer = _device->createBuffer(cbDesc);
  if (!_paramsBuffer) {
    log.Error(log::RENDER, "PostSoftenPass: createBuffer(params) failed");
    return;
  }

  _ready = true;
  log.Info(log::RENDER, "PostSoftenPass: initialised (display-space Gaussian ready)");
}

PostSoftenPass::~PostSoftenPass() = default;

void PostSoftenPass::EnsureTemp(uint32_t width, uint32_t height, nvrhi::Format format) {
  if (width == 0u || height == 0u || format == nvrhi::Format::UNKNOWN)
    return;
  if (_temp && _tempW == width && _tempH == height && _tempFormat == format)
    return;
  nvrhi::TextureDesc desc;
  desc.width = width;
  desc.height = height;
  desc.format = format;
  desc.dimension = nvrhi::TextureDimension::Texture2D;
  desc.isUAV = true;
  desc.isShaderResource = true;
  desc.debugName = "PostSoften.Temp";
  desc.initialState = nvrhi::ResourceStates::UnorderedAccess;
  desc.keepInitialState = true;
  _temp = _device->createTexture(desc);
  _tempW = width;
  _tempH = height;
  _tempFormat = format;
  _bindingSetCache.clear();  // the cached sets referenced the old temp.
}

nvrhi::BindingSetHandle PostSoftenPass::GetOrCreateBindingSet(nvrhi::ITexture* colorIn) {
  if (auto cached = _bindingSetCache.find(colorIn); cached != _bindingSetCache.end())
    return cached->second;
  constexpr std::size_t MAX_CACHE_ENTRIES = 4;
  if (_bindingSetCache.size() >= MAX_CACHE_ENTRIES)
    _bindingSetCache.clear();

  nvrhi::BindingSetDesc setDesc;
  setDesc.bindings = {
      nvrhi::BindingSetItem::Texture_SRV(0, colorIn),
      nvrhi::BindingSetItem::Texture_UAV(1, _temp.Get()),
      nvrhi::BindingSetItem::ConstantBuffer(2, _paramsBuffer),
  };
  nvrhi::BindingSetHandle set = _device->createBindingSet(setDesc, _bindingLayout);
  _bindingSetCache[colorIn] = set;
  return set;
}

void PostSoftenPass::Execute(nvrhi::ICommandList* commandList, const PassContext& context) {
  if (!_ready || commandList == nullptr || context.settings == nullptr
      || context.targets == nullptr)
    return;
  const float sigma = context.settings->realTimeQuality.postSoftenSigma;
  if (sigma <= 0.01f)
    return;  // default 0 — pass fully disabled, byte-identical output.
  nvrhi::ITexture* const color = context.targets->color;
  if (color == nullptr || !_temp)
    return;
  const nvrhi::TextureDesc& desc = color->getDesc();
  if (desc.width != _tempW || desc.height != _tempH || desc.format != _tempFormat)
    return;  // EnsureTemp didn't run for this size/format — renderer gate mismatch.

  // Normalized radius-2 Gaussian taps: w[i] = exp(-i^2 / (2 sigma^2)),
  // normalized so w0 + 2 w1 + 2 w2 == 1 (the shader derives w2 from that
  // identity — see post_soften.slang). RenderGraph::Execute already
  // brackets the pass in a Profiler::GpuScope named "pass.PostSoften".
  const float gauss0 = 1.0f;
  const float gauss1 = std::exp(-1.0f / (2.0f * sigma * sigma));
  const float gauss2 = std::exp(-4.0f / (2.0f * sigma * sigma));
  const float norm = gauss0 + 2.0f * gauss1 + 2.0f * gauss2;

  PostSoftenParams params{};
  params.width = desc.width;
  params.height = desc.height;
  params.weight0 = gauss0 / norm;
  params.weight1 = gauss1 / norm;
  commandList->writeBuffer(_paramsBuffer, &params, sizeof(params));

  const nvrhi::BindingSetHandle bindingSet = GetOrCreateBindingSet(color);
  if (!bindingSet)
    return;

  commandList->setTextureState(color, nvrhi::AllSubresources,
                               nvrhi::ResourceStates::ShaderResource);
  commandList->setTextureState(_temp, nvrhi::AllSubresources,
                               nvrhi::ResourceStates::UnorderedAccess);
  commandList->commitBarriers();

  nvrhi::ComputeState computeState;
  computeState.pipeline = _pipeline;
  computeState.bindings = {bindingSet};
  commandList->setComputeState(computeState);
  commandList->dispatch((desc.width + 7u) / 8u, (desc.height + 7u) / 8u, 1u);

  // Blurred temp back over the display color — every downstream consumer
  // keeps reading targets->color unchanged (see the .h's own rationale).
  commandList->copyTexture(color, nvrhi::TextureSlice{}, _temp.Get(), nvrhi::TextureSlice{});
}

}  // namespace pyxis
