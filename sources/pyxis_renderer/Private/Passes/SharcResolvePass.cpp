// Pyxis renderer — SharcResolvePass (RTX-alignment design, 2026-07-07).

#include "Passes/SharcResolvePass.h"

#include "RenderGraph/PassContext.h"
#include "RenderGraph/ShaderLoad.h"

#include <Pyxis/Platform/FileSystem/AssetLocator.h>
#include <Pyxis/Platform/Logging/Log.h>
#include <Pyxis/Platform/Logging/LogCategories.h>
#include <Pyxis/Renderer/Descs/RenderSettings.h>

#include "ShaderInterop.slang"

namespace pyxis {

namespace {

nvrhi::BufferHandle MakeCacheBuffer(nvrhi::IDevice* device, uint32_t byteSize, const char* name) {
  nvrhi::BufferDesc desc;
  desc.byteSize = byteSize;
  desc.canHaveUAVs = true;
  desc.canHaveRawViews = true;  // byte-address atomics (InterlockedAdd / CompareExchange)
  desc.initialState = nvrhi::ResourceStates::UnorderedAccess;
  desc.keepInitialState = true;
  desc.debugName = name;
  return device->createBuffer(desc);
}

}  // namespace

SharcResolvePass::SharcResolvePass(nvrhi::IDevice* device) : _device(device) {
  auto& log = Logging::Get();

  // Create the three cache buffers FIRST, unconditionally — IndirectDiffusePass
  // binds them in its Set 1 (whether or not SHARC is enabled), so they must
  // exist even if the resolve shader/pipeline below fails to build. Only a GPU
  // out-of-memory can null them, in which case both passes degrade gracefully.
  _hashBuffer = MakeCacheBuffer(_device, HASH_BYTES, "Sharc.HashEntries");
  _accumBuffer = MakeCacheBuffer(_device, VOXEL_BYTES, "Sharc.Accum");
  _resolvedBuffer = MakeCacheBuffer(_device, VOXEL_BYTES, "Sharc.Resolved");
  if (!_hashBuffer || !_accumBuffer || !_resolvedBuffer) {
    log.Error(log::RENDER, "SharcResolvePass: createBuffer(cache) failed");
    return;
  }

  const AssetLocator locator;
  const auto spvPath = locator.LocateResource("shaders/sharc_resolve.spv");
  _shader = LoadSpirvShader(_device, spvPath.View(), nvrhi::ShaderType::Compute, "main",
                            "SharcResolvePass");
  if (!_shader)
    return;

  nvrhi::BindingLayoutDesc layoutDesc;
  layoutDesc.visibility = nvrhi::ShaderType::Compute;
  layoutDesc.bindingOffsets.shaderResource = 0;
  layoutDesc.bindingOffsets.sampler = 0;
  layoutDesc.bindingOffsets.constantBuffer = 0;
  layoutDesc.bindingOffsets.unorderedAccess = 0;
  layoutDesc.bindings = {
      nvrhi::BindingLayoutItem::RawBuffer_UAV(0),  // 0 gAccum
      nvrhi::BindingLayoutItem::RawBuffer_UAV(1),  // 1 gResolved
  };
  _bindingLayout = _device->createBindingLayout(layoutDesc);
  if (!_bindingLayout) {
    log.Error(log::RENDER, "SharcResolvePass: createBindingLayout failed");
    return;
  }

  nvrhi::ComputePipelineDesc pipelineDesc;
  pipelineDesc.CS = _shader;
  pipelineDesc.bindingLayouts = {_bindingLayout};
  _pipeline = _device->createComputePipeline(pipelineDesc);
  if (!_pipeline) {
    log.Error(log::RENDER, "SharcResolvePass: createComputePipeline failed");
    return;
  }

  nvrhi::BindingSetDesc setDesc;
  setDesc.bindings = {
      nvrhi::BindingSetItem::RawBuffer_UAV(0, _accumBuffer),
      nvrhi::BindingSetItem::RawBuffer_UAV(1, _resolvedBuffer),
  };
  _bindingSet = _device->createBindingSet(setDesc, _bindingLayout);
  if (!_bindingSet) {
    log.Error(log::RENDER, "SharcResolvePass: createBindingSet failed");
    return;
  }

  _ready = true;
  log.Info(log::RENDER, "SharcResolvePass: initialised (SHARC radiance cache, "
                        "2^21 cells, ~75 MiB)");
}

SharcResolvePass::~SharcResolvePass() = default;

void SharcResolvePass::ClearCache(nvrhi::ICommandList* commandList) {
  commandList->clearBufferUInt(_hashBuffer.Get(), 0u);
  commandList->clearBufferUInt(_accumBuffer.Get(), 0u);
  commandList->clearBufferUInt(_resolvedBuffer.Get(), 0u);
  commandList->commitBarriers();
}

void SharcResolvePass::Execute(nvrhi::ICommandList* commandList, const PassContext& context) {
  if (!_ready || commandList == nullptr || context.settings == nullptr)
    return;
  // Gated on PASS_MASK_SHARC_GI — no-op (and cache untouched) when SHARC is off,
  // so the builtin indirect path is byte-identical.
  if ((context.settings->realTimeQuality.passMask & shaderinterop::PASS_MASK_SHARC_GI) == 0u)
    return;

  // The cache persists + converges across the accumulation frames; clear it to
  // zero exactly once, on the first enabled frame, BEFORE IndirectDiffusePass
  // writes the first per-frame accumulation.
  if (!_cleared) {
    ClearCache(commandList);
    _cleared = true;
  }

  // Resolve: fold each cell's per-frame sum into the cross-frame running mean,
  // then clear the per-frame sum. One thread per cell, 256 per group.
  nvrhi::ComputeState state;
  state.pipeline = _pipeline;
  state.bindings = {_bindingSet};
  commandList->setComputeState(state);
  commandList->dispatch((CAPACITY + 255u) / 256u, 1u, 1u);
}

}  // namespace pyxis
