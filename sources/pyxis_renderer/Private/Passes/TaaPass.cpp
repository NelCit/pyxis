// Pyxis renderer — TaaPass (RTX-alignment design, Phase B).

#include "Passes/TaaPass.h"

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

nvrhi::TextureHandle MakeHistoryTexture(nvrhi::IDevice* device, uint32_t width, uint32_t height,
                                        const char* debugName) {
  nvrhi::TextureDesc desc;
  desc.width = width;
  desc.height = height;
  // Matches CompositePass::EnsureLinearColor's format exactly — TaaPass
  // copies its blended history straight back into that texture, so a
  // format mismatch would need an implicit (lossy, and NVRHI-unsupported
  // for copyTexture) conversion.
  desc.format = nvrhi::Format::RGBA32_FLOAT;
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

TaaPass::TaaPass(nvrhi::IDevice* device) : _device(device) {
  auto& log = Logging::Get();
  const AssetLocator locator;
  const auto spvPath = locator.LocateResource("shaders/taa.spv");
  _shader = LoadSpirvShader(_device, spvPath.View(), nvrhi::ShaderType::Compute, "main", "TaaPass");
  if (!_shader)
    return;

  nvrhi::BindingLayoutDesc layoutDesc;
  layoutDesc.visibility = nvrhi::ShaderType::Compute;
  layoutDesc.bindingOffsets.shaderResource = 0;
  layoutDesc.bindingOffsets.sampler = 0;
  layoutDesc.bindingOffsets.constantBuffer = 0;
  layoutDesc.bindingOffsets.unorderedAccess = 0;
  layoutDesc.bindings = {
      nvrhi::BindingLayoutItem::Texture_SRV(0),             // 0 gCurrentColor
      nvrhi::BindingLayoutItem::Texture_SRV(1),             // 1 gPrevHistory
      nvrhi::BindingLayoutItem::Texture_SRV(2),             // 2 gMotionVector
      nvrhi::BindingLayoutItem::Texture_UAV(3),             // 3 gOutHistory
      nvrhi::BindingLayoutItem::VolatileConstantBuffer(4),  // 4 TaaUniforms
  };
  _bindingLayout = _device->createBindingLayout(layoutDesc);
  if (!_bindingLayout) {
    log.Error(log::RENDER, "TaaPass: createBindingLayout failed");
    return;
  }

  nvrhi::ComputePipelineDesc pipelineDesc;
  pipelineDesc.CS = _shader;
  pipelineDesc.bindingLayouts = {_bindingLayout};
  _pipeline = _device->createComputePipeline(pipelineDesc);
  if (!_pipeline) {
    log.Error(log::RENDER, "TaaPass: createComputePipeline failed");
    return;
  }

  nvrhi::BufferDesc cbDesc;
  cbDesc.byteSize = sizeof(shaderinterop::TaaUniforms);
  cbDesc.isConstantBuffer = true;
  // Volatile: writeBuffer'd once per Execute — same maxVersions-starvation
  // finding as DenoiseTemporalPass.Params (see its doc comment): 8 was
  // insufficient for `render.accumulationFrames` past ~frame 8 with no
  // per-frame GPU wait in headless, silently forcing TaaUniforms.hasHistory
  // back to its stale frame-0 (=0) value most frames and defeating the
  // history blend. 512 matches the Tonemap/AutoExposure precedent.
  cbDesc.isVolatile = true;
  cbDesc.maxVersions = 512;
  cbDesc.debugName = "Taa.Params";
  _paramsBuffer = _device->createBuffer(cbDesc);
  if (!_paramsBuffer) {
    log.Error(log::RENDER, "TaaPass: createBuffer(params) failed");
    return;
  }

  _ready = true;
  log.Info(log::RENDER, "TaaPass: initialised");
}

TaaPass::~TaaPass() = default;

bool TaaPass::EnsureHistory(uint32_t width, uint32_t height) {
  if (width == 0u || height == 0u)
    return false;
  if (_width == width && _height == height && _history[0] && _history[1])
    return true;

  std::array<nvrhi::TextureHandle, 2> history;
  history[0] = MakeHistoryTexture(_device, width, height, "Taa.History0");
  history[1] = MakeHistoryTexture(_device, width, height, "Taa.History1");
  if (!history[0] || !history[1]) {
    Logging::Get().Error(log::RENDER, "TaaPass: createTexture(history, " + std::to_string(width)
                                          + "x" + std::to_string(height)
                                          + ") failed; keeping previous allocation");
    return false;
  }

  _history = history;
  _width = width;
  _height = height;
  _prevIndex = 0u;
  _currIndex = 1u;
  _hasHistory = false;
  _bindingSetCache.clear();
  return true;
}

nvrhi::BindingSetHandle TaaPass::GetOrCreateBindingSet(nvrhi::ITexture* currentColor,
                                                       nvrhi::ITexture* motionVector) {
  const std::uint64_t key =
      HashPointers({currentColor, motionVector, _history[_prevIndex].Get(),
                    _history[_currIndex].Get()});
  if (auto cached = _bindingSetCache.find(key); cached != _bindingSetCache.end())
    return cached->second;
  constexpr std::size_t MAX_CACHE_ENTRIES = 4;
  if (_bindingSetCache.size() >= MAX_CACHE_ENTRIES)
    _bindingSetCache.clear();

  nvrhi::BindingSetDesc setDesc;
  setDesc.bindings = {
      nvrhi::BindingSetItem::Texture_SRV(0, currentColor),
      nvrhi::BindingSetItem::Texture_SRV(1, _history[_prevIndex]),
      nvrhi::BindingSetItem::Texture_SRV(2, motionVector),
      nvrhi::BindingSetItem::Texture_UAV(3, _history[_currIndex]),
      nvrhi::BindingSetItem::ConstantBuffer(4, _paramsBuffer),
  };
  nvrhi::BindingSetHandle set = _device->createBindingSet(setDesc, _bindingLayout);
  _bindingSetCache.emplace(key, set);
  return set;
}

void TaaPass::Execute(nvrhi::ICommandList* commandList, const PassContext& context) {
  if (!_ready || commandList == nullptr || context.settings == nullptr
      || context.targets == nullptr)
    return;
  // Phase B — passMask bit 6 (PASS_MASK_TAA). Default OFF in headless (the
  // §33.7 byte-equal contract needs a fixed, non-temporally-blended
  // image); viewer default flip is Phase C's calibration work. No-op
  // leaves context.linearColor exactly as CompositePass wrote it.
  if ((context.settings->realTimeQuality.passMask & shaderinterop::PASS_MASK_TAA) == 0u)
    return;

  nvrhi::ITexture* const currentColor = context.linearColor;
  nvrhi::ITexture* const motionVector = context.gMotionVector;
  if (currentColor == nullptr || motionVector == nullptr)
    return;

  const nvrhi::TextureDesc& desc = currentColor->getDesc();
  const uint32_t width = desc.width;
  const uint32_t height = desc.height;
  if (width == 0u || height == 0u)
    return;
  if (!EnsureHistory(width, height))
    return;

  shaderinterop::TaaUniforms params{};
  params.destWidth = width;
  params.destHeight = height;
  params.hasHistory = _hasHistory ? 1u : 0u;
  // Noise-floor + vegetation spec (rtx-realtime-alignment-design.md,
  // 2026-07-06), work item 3 — adaptive blend: this is now the FLOOR
  // alpha converges toward as history accumulates (taa.slang derives the
  // actual per-pixel alpha from a confidence/history-length signal), not
  // a flat per-frame blend. NRD-default-derived (~1/32, ReLAX's own
  // "fast" antilag alpha floor).
  params.alphaMin = 1.0f / 32.0f;
  commandList->writeBuffer(_paramsBuffer, &params, sizeof(params));

  const nvrhi::BindingSetHandle bindingSet = GetOrCreateBindingSet(currentColor, motionVector);
  if (!bindingSet)
    return;

  nvrhi::ITexture* const currHistory = _history[_currIndex].Get();

  commandList->setTextureState(currentColor, nvrhi::AllSubresources,
                               nvrhi::ResourceStates::ShaderResource);
  commandList->setTextureState(motionVector, nvrhi::AllSubresources,
                               nvrhi::ResourceStates::ShaderResource);
  commandList->setTextureState(_history[_prevIndex].Get(), nvrhi::AllSubresources,
                               nvrhi::ResourceStates::ShaderResource);
  commandList->setTextureState(currHistory, nvrhi::AllSubresources,
                               nvrhi::ResourceStates::UnorderedAccess);
  commandList->commitBarriers();

  nvrhi::ComputeState state;
  state.pipeline = _pipeline;
  state.bindings = {bindingSet};
  commandList->setComputeState(state);

  const uint32_t groupsX = (width + 7u) / 8u;
  const uint32_t groupsY = (height + 7u) / 8u;
  commandList->dispatch(groupsX, groupsY, 1u);

  // Copy this frame's blended history back into the shared linearColor
  // texture so TonemapPass (which reads context.linearColor, unaware of
  // TaaPass's own history buffers) sees the anti-aliased result. Explicit
  // CopySource/CopyDest transitions on both sides — copyTexture's
  // auto-tracker has a known gap for UAV-initial-state textures (see
  // ViewerMode.cpp's present-blit copy for the same defensive pattern).
  commandList->setTextureState(currHistory, nvrhi::AllSubresources,
                               nvrhi::ResourceStates::CopySource);
  commandList->setTextureState(currentColor, nvrhi::AllSubresources,
                               nvrhi::ResourceStates::CopyDest);
  commandList->commitBarriers();
  commandList->copyTexture(currentColor, nvrhi::TextureSlice{}, currHistory,
                           nvrhi::TextureSlice{});

  // Flip for next frame — MUST happen after the dispatch + copy above.
  _prevIndex ^= 1u;
  _currIndex ^= 1u;
  _hasHistory = true;
}

}  // namespace pyxis
