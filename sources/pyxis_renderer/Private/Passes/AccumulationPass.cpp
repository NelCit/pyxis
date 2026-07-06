// Pyxis renderer — AccumulationPass implementation (RTX-alignment design,
// "KEY FINDING (2026-07-06): no true accumulation buffer"). See the
// header's file comment for the full design rationale.

#include "Passes/AccumulationPass.h"

#include "RenderGraph/PassContext.h"
#include "RenderGraph/ShaderLoad.h"
#include "ShaderInterop.slang"

#include <Pyxis/Platform/FileSystem/AssetLocator.h>
#include <Pyxis/Platform/Logging/Log.h>
#include <Pyxis/Platform/Logging/LogCategories.h>
#include <Pyxis/Renderer/Descs/RenderSettings.h>

#include <string>

namespace pyxis {

namespace {

std::uint64_t HashPointers(std::initializer_list<const void*> pointers) {
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

AccumulationPass::AccumulationPass(nvrhi::IDevice* device) : _device(device) {
  auto& log = Logging::Get();
  const AssetLocator locator;
  const auto spvPath = locator.LocateResource("shaders/accumulate.spv");
  _shader = LoadSpirvShader(_device, spvPath.View(), nvrhi::ShaderType::Compute, "main",
                            "AccumulationPass");
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
      nvrhi::BindingLayoutItem::Texture_UAV(1),             // 1 gAccumBuffer
      nvrhi::BindingLayoutItem::VolatileConstantBuffer(2),  // 2 AccumulationUniforms
  };
  _bindingLayout = _device->createBindingLayout(layoutDesc);
  if (!_bindingLayout)
  {
    log.Error(log::RENDER, "AccumulationPass: createBindingLayout failed");
    return;
  }

  nvrhi::ComputePipelineDesc pipelineDesc;
  pipelineDesc.CS = _shader;
  pipelineDesc.bindingLayouts = {_bindingLayout};
  _pipeline = _device->createComputePipeline(pipelineDesc);
  if (!_pipeline)
  {
    log.Error(log::RENDER, "AccumulationPass: createComputePipeline failed");
    return;
  }

  nvrhi::BufferDesc cbDesc;
  cbDesc.byteSize = sizeof(shaderinterop::AccumulationUniforms);
  cbDesc.isConstantBuffer = true;
  // Volatile: writeBuffer'd once per Execute — same maxVersions-starvation
  // finding as TaaPass::_paramsBuffer / DenoiseTemporalPass::Params (see
  // their doc comments): 8 was insufficient for `render.accumulationFrames`
  // past ~frame 8 with no per-frame GPU wait in headless. 512 matches the
  // Tonemap/AutoExposure/Taa precedent.
  cbDesc.isVolatile = true;
  cbDesc.maxVersions = 512;
  cbDesc.debugName = "Accumulation.Params";
  _paramsBuffer = _device->createBuffer(cbDesc);
  if (!_paramsBuffer)
  {
    log.Error(log::RENDER, "AccumulationPass: createBuffer(params) failed");
    return;
  }

  _ready = true;
  log.Info(log::RENDER, "AccumulationPass: initialised");
}

AccumulationPass::~AccumulationPass() = default;

bool AccumulationPass::EnsureBuffer(uint32_t width, uint32_t height) {
  if (width == 0u || height == 0u)
    return false;
  if (_width == width && _height == height && _accumBuffer)
    return true;

  nvrhi::TextureDesc desc;
  desc.width = width;
  desc.height = height;
  // RGBA32F, matching CompositePass::EnsureLinearColor's format exactly —
  // this pass copies its blended running mean straight back into that
  // texture (see Execute()), so a format mismatch would need an implicit
  // (lossy, and NVRHI-unsupported for copyTexture) conversion.
  desc.format = nvrhi::Format::RGBA32_FLOAT;
  desc.dimension = nvrhi::TextureDimension::Texture2D;
  desc.isUAV = true;
  desc.isShaderResource = true;
  desc.debugName = "Accumulation.Buffer";
  desc.initialState = nvrhi::ResourceStates::UnorderedAccess;
  desc.keepInitialState = true;
  nvrhi::TextureHandle buffer = _device->createTexture(desc);
  if (!buffer)
  {
    Logging::Get().Error(log::RENDER, "AccumulationPass: createTexture(accumBuffer, "
                                          + std::to_string(width) + "x" + std::to_string(height)
                                          + " RGBA32F) failed; keeping previous allocation");
    return false;
  }

  _accumBuffer = std::move(buffer);
  _width = width;
  _height = height;
  // A resize invalidates whatever the old buffer held (different
  // dimensions) — start a fresh running mean at the new size.
  _sampleCount = 0;
  _wasActive = false;
  _bindingSetCache.clear();
  return true;
}

nvrhi::BindingSetHandle AccumulationPass::GetOrCreateBindingSet(nvrhi::ITexture* currentColor) {
  const std::uint64_t key = HashPointers({currentColor, _accumBuffer.Get()});
  if (auto cached = _bindingSetCache.find(key); cached != _bindingSetCache.end())
    return cached->second;
  constexpr std::size_t MAX_CACHE_ENTRIES = 4;
  if (_bindingSetCache.size() >= MAX_CACHE_ENTRIES)
    _bindingSetCache.clear();

  nvrhi::BindingSetDesc setDesc;
  setDesc.bindings = {
      nvrhi::BindingSetItem::Texture_SRV(0, currentColor),
      nvrhi::BindingSetItem::Texture_UAV(1, _accumBuffer),
      nvrhi::BindingSetItem::ConstantBuffer(2, _paramsBuffer),
  };
  nvrhi::BindingSetHandle set = _device->createBindingSet(setDesc, _bindingLayout);
  _bindingSetCache.emplace(key, set);
  return set;
}

void AccumulationPass::Execute(nvrhi::ICommandList* commandList, const PassContext& context) {
  if (!_ready || commandList == nullptr || context.settings == nullptr)
    return;

  // PASS_MASK_ACCUMULATE clear (the default — every existing config/
  // golden) => pure passthrough, context.linearColor left exactly as
  // CompositePass wrote it. Byte-identical to pre-existing behaviour.
  const bool active =
      (context.settings->realTimeQuality.passMask & shaderinterop::PASS_MASK_ACCUMULATE) != 0u;
  if (!active)
  {
    // Idle while inactive so a LATER activation always starts fresh
    // (weight 1.0) rather than blending against a stale running mean from
    // a previous activation earlier in the same session.
    _sampleCount = 0;
    _wasActive = false;
    return;
  }

  nvrhi::ITexture* const currentColor = context.linearColor;
  if (currentColor == nullptr)
    return;

  const nvrhi::TextureDesc& desc = currentColor->getDesc();
  const uint32_t width = desc.width;
  const uint32_t height = desc.height;
  if (width == 0u || height == 0u)
    return;
  if (!EnsureBuffer(width, height))
    return;

  // Becoming active this frame (first frame of a session, a resolution
  // change, or a mid-session off->on flip) starts a fresh running mean —
  // sampleWeight below resolves to 1.0, so the buffer is fully overwritten
  // by `currentColor` rather than blended with its previous (stale)
  // contents.
  if (!_wasActive)
  {
    _sampleCount = 0;
    _wasActive = true;
  }

  shaderinterop::AccumulationUniforms params{};
  params.destWidth = width;
  params.destHeight = height;
  params.sampleIndex = _sampleCount;
  params.sampleWeight = 1.0f / static_cast<float>(_sampleCount + 1u);
  commandList->writeBuffer(_paramsBuffer, &params, sizeof(params));

  const nvrhi::BindingSetHandle bindingSet = GetOrCreateBindingSet(currentColor);
  if (!bindingSet)
    return;

  nvrhi::ITexture* const accumBuffer = _accumBuffer.Get();

  commandList->setTextureState(currentColor, nvrhi::AllSubresources,
                               nvrhi::ResourceStates::ShaderResource);
  commandList->setTextureState(accumBuffer, nvrhi::AllSubresources,
                               nvrhi::ResourceStates::UnorderedAccess);
  commandList->commitBarriers();

  nvrhi::ComputeState state;
  state.pipeline = _pipeline;
  state.bindings = {bindingSet};
  commandList->setComputeState(state);

  const uint32_t groupsX = (width + 7u) / 8u;
  const uint32_t groupsY = (height + 7u) / 8u;
  commandList->dispatch(groupsX, groupsY, 1u);

  // Copy this frame's updated running mean back into the shared
  // linearColor texture so every downstream consumer (DlssPass,
  // AutoExposurePass, TaaPass [forced off whenever this pass is active —
  // see PyxisRenderer::RenderFrame], TonemapPass) sees the converged
  // result via the one shared pointer, without PassContext gaining a new
  // field. Explicit CopySource/CopyDest transitions on both sides —
  // copyTexture's auto-tracker has a known gap for UAV-initial-state
  // textures (same defensive pattern TaaPass's history copy-back uses).
  commandList->setTextureState(accumBuffer, nvrhi::AllSubresources,
                               nvrhi::ResourceStates::CopySource);
  commandList->setTextureState(currentColor, nvrhi::AllSubresources,
                               nvrhi::ResourceStates::CopyDest);
  commandList->commitBarriers();
  commandList->copyTexture(currentColor, nvrhi::TextureSlice{}, accumBuffer, nvrhi::TextureSlice{});

  ++_sampleCount;
}

}  // namespace pyxis
