// Pyxis renderer — linear→sRGB present blit pass implementation.

#include "Passes/BlitToSrgbPass.h"

#include "RenderGraph/PassContext.h"
#include "RenderGraph/ShaderLoad.h"

#include <Pyxis/Platform/FileSystem/AssetLocator.h>
#include <Pyxis/Platform/Logging/Log.h>
#include <Pyxis/Platform/Logging/LogCategories.h>
#include <Pyxis/Renderer/Descs/RenderSettings.h>
#include <Pyxis/Renderer/Descs/RenderTargets.h>
#include <Pyxis/Renderer/Profiler.h>

#include <cstddef>
#include <cstdint>

namespace pyxis {

namespace {
// Matches the BlitToSrgbUniforms cbuffer in blit_to_srgb.slang (16 bytes / one
// cbuffer row). Padded so sizeof == 16 (std140 vector alignment).
struct BlitToSrgbUniforms {
  uint32_t destWidth = 0;
  uint32_t destHeight = 0;
  uint32_t pad0 = 0;
  uint32_t pad1 = 0;
};
static_assert(sizeof(BlitToSrgbUniforms) == 16, "BlitToSrgbUniforms must be one 16-byte row");
}  // namespace

BlitToSrgbPass::BlitToSrgbPass(nvrhi::IDevice* device) : _device(device) {
  auto& log = Logging::Get();
  const AssetLocator locator;
  const auto spvPath = locator.LocateResource("shaders/blit_to_srgb.spv");

  _shader = LoadSpirvShader(_device, spvPath.View(), nvrhi::ShaderType::Compute, "main",
                            "BlitToSrgbPass");
  if (!_shader)
    return;

  nvrhi::BindingLayoutDesc layoutDesc;
  layoutDesc.visibility = nvrhi::ShaderType::Compute;
  // Zero the per-type binding offsets so the layout numbers map 1:1 to the
  // shader's [[vk::binding(N, 0)]] slots (same convention as SsaaResolvePass).
  layoutDesc.bindingOffsets.shaderResource = 0;
  layoutDesc.bindingOffsets.sampler = 0;
  layoutDesc.bindingOffsets.constantBuffer = 0;
  layoutDesc.bindingOffsets.unorderedAccess = 0;
  layoutDesc.bindings = {
      nvrhi::BindingLayoutItem::Texture_SRV(0),             // source LINEAR color
      nvrhi::BindingLayoutItem::Texture_UAV(1),             // sRGB-encoded output
      nvrhi::BindingLayoutItem::VolatileConstantBuffer(2),  // BlitToSrgbUniforms
  };
  _bindingLayout = _device->createBindingLayout(layoutDesc);
  if (!_bindingLayout) {
    log.Error(log::RENDER, "BlitToSrgbPass: createBindingLayout failed");
    return;
  }

  nvrhi::ComputePipelineDesc pipelineDesc;
  pipelineDesc.CS = _shader;
  pipelineDesc.bindingLayouts = {_bindingLayout};
  _pipeline = _device->createComputePipeline(pipelineDesc);
  if (!_pipeline) {
    log.Error(log::RENDER, "BlitToSrgbPass: createComputePipeline failed");
    return;
  }

  nvrhi::BufferDesc cbDesc;
  cbDesc.byteSize = sizeof(BlitToSrgbUniforms);
  cbDesc.isConstantBuffer = true;
  cbDesc.isVolatile = true;
  cbDesc.maxVersions = 16;
  cbDesc.debugName = "BlitToSrgb.Params";
  _paramsBuffer = _device->createBuffer(cbDesc);
  if (!_paramsBuffer) {
    log.Error(log::RENDER, "BlitToSrgbPass: createBuffer(params) failed");
    return;
  }

  _ready = true;
  log.Info(log::RENDER, "BlitToSrgbPass: initialised (linear->sRGB present-encode)");
}

nvrhi::BindingSetHandle BlitToSrgbPass::GetOrCreateBindingSet(nvrhi::ITexture* source,
                                                             nvrhi::ITexture* dest) {
  uint64_t key = 1469598103934665603ull;
  auto mix = [&key](const void* ptr) {
    auto bits = reinterpret_cast<std::uintptr_t>(ptr);
    for (int byte = 0; byte < 8; ++byte) {
      key ^= static_cast<uint64_t>(bits & 0xFFu);
      key *= 1099511628211ull;
      bits >>= 8;
    }
  };
  mix(source);
  mix(dest);

  if (auto cached = _bindingSetCache.find(key); cached != _bindingSetCache.end())
    return cached->second;

  nvrhi::BindingSetDesc setDesc;
  setDesc.bindings = {
      nvrhi::BindingSetItem::Texture_SRV(0, source),
      nvrhi::BindingSetItem::Texture_UAV(1, dest),
      nvrhi::BindingSetItem::ConstantBuffer(2, _paramsBuffer),
  };
  nvrhi::BindingSetHandle set = _device->createBindingSet(setDesc, _bindingLayout);
  if (_bindingSetCache.size() > 8)
    _bindingSetCache.clear();
  _bindingSetCache.emplace(key, set);
  return set;
}

void BlitToSrgbPass::Execute(nvrhi::ICommandList* commandList, const PassContext& context) {
  if (!_ready || commandList == nullptr || context.settings == nullptr
      || context.targets == nullptr)
    return;

  // The present target (only the standalone viewer binds it; Kit never does, so
  // this no-ops for Omniverse).
  nvrhi::ITexture* const dest = context.targets->colorResolved;
  if (dest == nullptr)
    return;

  // Source LINEAR color: the SSAA downsample output at factor > 1, else the
  // path-tracer color AOV directly.
  const bool supersampled = context.settings->ssaaFactor > 1u && context.colorLinearResolved;
  nvrhi::ITexture* const source =
      supersampled ? context.colorLinearResolved : context.targets->color;
  if (source == nullptr)
    return;

  // Output dims = the present target's. (At factor 1 the source is full-res; at
  // factor > 1 the downsampled intermediate is already base-res.)
  const nvrhi::TextureDesc& destDesc = dest->getDesc();
  const uint32_t destWidth = destDesc.width;
  const uint32_t destHeight = destDesc.height;
  if (destWidth == 0u || destHeight == 0u)
    return;

  BlitToSrgbUniforms params{};
  params.destWidth = destWidth;
  params.destHeight = destHeight;
  commandList->writeBuffer(_paramsBuffer, &params, sizeof(params));

  const nvrhi::BindingSetHandle bindingSet = GetOrCreateBindingSet(source, dest);
  if (!bindingSet)
    return;

  commandList->setTextureState(source, nvrhi::AllSubresources,
                               nvrhi::ResourceStates::ShaderResource);
  commandList->setTextureState(dest, nvrhi::AllSubresources,
                               nvrhi::ResourceStates::UnorderedAccess);
  commandList->commitBarriers();

  nvrhi::ComputeState state;
  state.pipeline = _pipeline;
  state.bindings = {bindingSet};
  commandList->setComputeState(state);

  const uint32_t groupsX = (destWidth + 7u) / 8u;
  const uint32_t groupsY = (destHeight + 7u) / 8u;
  commandList->dispatch(groupsX, groupsY, 1u);
}

}  // namespace pyxis
