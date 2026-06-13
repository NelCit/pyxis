// Pyxis renderer — auto-exposure log-luminance reduction pass implementation.

#include "Passes/AutoExposurePass.h"

#include "RenderGraph/PassContext.h"
#include "RenderGraph/ShaderLoad.h"

#include <Pyxis/Platform/FileSystem/AssetLocator.h>
#include <Pyxis/Platform/Logging/Log.h>
#include <Pyxis/Platform/Logging/LogCategories.h>
#include <Pyxis/Renderer/Descs/RenderSettings.h>
#include <Pyxis/Renderer/Profiler.h>

// Dual-language ShaderInterop header — declares AutoExposureUniforms for C++.
#include "ShaderInterop.slang"

#include <cstdint>

namespace pyxis {

AutoExposurePass::AutoExposurePass(nvrhi::IDevice* device) : _device(device) {
  auto& log = Logging::Get();
  const AssetLocator locator;
  const auto spvPath = locator.LocateResource("shaders/auto_exposure.spv");

  _shader = LoadSpirvShader(_device, spvPath.View(), nvrhi::ShaderType::Compute, "main",
                            "AutoExposurePass");
  if (!_shader)
    return;

  nvrhi::BindingLayoutDesc layoutDesc;
  layoutDesc.visibility = nvrhi::ShaderType::Compute;
  layoutDesc.bindingOffsets.shaderResource = 0;
  layoutDesc.bindingOffsets.sampler = 0;
  layoutDesc.bindingOffsets.constantBuffer = 0;
  layoutDesc.bindingOffsets.unorderedAccess = 0;
  layoutDesc.bindings = {
      nvrhi::BindingLayoutItem::Texture_SRV(0),              // fp32 linear radiance
      nvrhi::BindingLayoutItem::RawBuffer_UAV(1),            // stats (uint2: sum, count)
      nvrhi::BindingLayoutItem::VolatileConstantBuffer(2),  // AutoExposureUniforms
  };
  _bindingLayout = _device->createBindingLayout(layoutDesc);
  if (!_bindingLayout) {
    log.Error(log::RENDER, "AutoExposurePass: createBindingLayout failed");
    return;
  }

  nvrhi::ComputePipelineDesc pipelineDesc;
  pipelineDesc.CS = _shader;
  pipelineDesc.bindingLayouts = {_bindingLayout};
  _pipeline = _device->createComputePipeline(pipelineDesc);
  if (!_pipeline) {
    log.Error(log::RENDER, "AutoExposurePass: createComputePipeline failed");
    return;
  }

  nvrhi::BufferDesc cbDesc;
  cbDesc.byteSize = sizeof(shaderinterop::AutoExposureUniforms);
  cbDesc.isConstantBuffer = true;
  cbDesc.isVolatile = true;
  cbDesc.maxVersions = 512;  // matches TonemapPass — covers the bench-frame window.
  cbDesc.debugName = "AutoExposure.Params";
  _paramsBuffer = _device->createBuffer(cbDesc);
  if (!_paramsBuffer) {
    log.Error(log::RENDER, "AutoExposurePass: createBuffer(params) failed");
    return;
  }

  // The reduction accumulator: uint3 (log-lum sum, lit count, log-lum MAX). Raw
  // views for the ByteAddressBuffer UAV (this pass) + SRV (TonemapPass). 12
  // bytes, never resized.
  nvrhi::BufferDesc statsDesc;
  statsDesc.byteSize = 12u;
  statsDesc.canHaveUAVs = true;
  statsDesc.canHaveRawViews = true;
  statsDesc.debugName = "AutoExposure.Stats";
  statsDesc.initialState = nvrhi::ResourceStates::ShaderResource;
  statsDesc.keepInitialState = true;
  _statsBuffer = _device->createBuffer(statsDesc);
  if (!_statsBuffer) {
    log.Error(log::RENDER, "AutoExposurePass: createBuffer(stats) failed");
    return;
  }

  _ready = true;
  log.Info(log::RENDER, "AutoExposurePass: initialised");
}

nvrhi::BindingSetHandle AutoExposurePass::GetOrCreateBindingSet(nvrhi::ITexture* source,
                                                               nvrhi::IBuffer* stats) {
  // FNV1a-64 over the two borrowed pointers — same identity scheme as the other
  // compute passes. `stats` is renderer-owned + constant; `source` (linearColor)
  // is retired/recreated on resize, so a resize lands on a new key.
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
  mix(stats);

  if (auto cached = _bindingSetCache.find(key); cached != _bindingSetCache.end())
    return cached->second;

  nvrhi::BindingSetDesc setDesc;
  setDesc.bindings = {
      nvrhi::BindingSetItem::Texture_SRV(0, source),
      nvrhi::BindingSetItem::RawBuffer_UAV(1, stats),
      nvrhi::BindingSetItem::ConstantBuffer(2, _paramsBuffer),
  };
  nvrhi::BindingSetHandle set = _device->createBindingSet(setDesc, _bindingLayout);
  // Clear on miss: a key miss means linearColor was retired (resize), and NVRHI
  // binding sets hold strong refs — keeping old entries would pin retired
  // generations. Steady state keeps exactly one live entry.
  if (!_bindingSetCache.empty())
    _bindingSetCache.clear();
  _bindingSetCache.emplace(key, set);
  return set;
}

void AutoExposurePass::Execute(nvrhi::ICommandList* commandList, const PassContext& context) {
  if (!_ready || commandList == nullptr || context.settings == nullptr)
    return;
  // Disabled by default → no-op (byte-equal contract untouched, zero overhead).
  if (!context.settings->autoExposure)
    return;
  nvrhi::ITexture* const source = context.linearColor;
  nvrhi::IBuffer* const stats = _statsBuffer.Get();
  if (source == nullptr || stats == nullptr)
    return;

  const nvrhi::TextureDesc& srcDesc = source->getDesc();
  const uint32_t srcWidth = srcDesc.width;
  const uint32_t srcHeight = srcDesc.height;
  if (srcWidth == 0u || srcHeight == 0u)
    return;

  const Profiler::GpuScope gpuScope(*context.profiler, commandList, "pass.AutoExposure");

  shaderinterop::AutoExposureUniforms params{};
  params.srcWidth = srcWidth;
  params.srcHeight = srcHeight;
  params._aePad0 = 0u;
  params._aePad1 = 0u;
  commandList->writeBuffer(_paramsBuffer, &params, sizeof(params));

  const nvrhi::BindingSetHandle bindingSet = GetOrCreateBindingSet(source, stats);
  if (!bindingSet)
    return;

  // linearColor was just UAV-written by RaytracedLightingPass; read it as a
  // shader resource. The stats buffer is the UAV we clear + accumulate into —
  // transition it BEFORE the clear so clearBufferUInt sees UnorderedAccess.
  commandList->setTextureState(source, nvrhi::AllSubresources,
                               nvrhi::ResourceStates::ShaderResource);
  commandList->setBufferState(stats, nvrhi::ResourceStates::UnorderedAccess);
  commandList->commitBarriers();
  commandList->clearBufferUInt(stats, 0u);

  nvrhi::ComputeState state;
  state.pipeline = _pipeline;
  state.bindings = {bindingSet};
  commandList->setComputeState(state);

  const uint32_t groupsX = (srcWidth + 7u) / 8u;
  const uint32_t groupsY = (srcHeight + 7u) / 8u;
  commandList->dispatch(groupsX, groupsY, 1u);
}

}  // namespace pyxis
