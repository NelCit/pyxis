// Pyxis renderer — DenoiseTemporalPass (RTX-alignment design, Phase B).

#include "Passes/DenoiseTemporalPass.h"

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

DenoiseTemporalPass::DenoiseTemporalPass(nvrhi::IDevice* device)
    : _device(device), _resources(device) {
  auto& log = Logging::Get();
  const AssetLocator locator;
  const auto spvPath = locator.LocateResource("shaders/denoise_temporal.spv");
  _shader = LoadSpirvShader(_device, spvPath.View(), nvrhi::ShaderType::Compute, "main",
                            "DenoiseTemporalPass");
  if (!_shader)
    return;

  nvrhi::BindingLayoutDesc layoutDesc;
  layoutDesc.visibility = nvrhi::ShaderType::Compute;
  layoutDesc.bindingOffsets.shaderResource = 0;
  layoutDesc.bindingOffsets.sampler = 0;
  layoutDesc.bindingOffsets.constantBuffer = 0;
  layoutDesc.bindingOffsets.unorderedAccess = 0;
  layoutDesc.bindings = {
      nvrhi::BindingLayoutItem::Texture_SRV(0),              // 0  gRawDiffuse
      nvrhi::BindingLayoutItem::Texture_SRV(1),              // 1  gRawSpecular
      nvrhi::BindingLayoutItem::Texture_SRV(2),              // 2  gMotionVector
      nvrhi::BindingLayoutItem::Texture_SRV(3),              // 3  gViewZ
      nvrhi::BindingLayoutItem::Texture_SRV(4),              // 4  gNormalRoughness
      nvrhi::BindingLayoutItem::Texture_SRV(5),              // 5  gPrevDiffuseSlow
      nvrhi::BindingLayoutItem::Texture_SRV(6),              // 6  gPrevSpecularSlow
      nvrhi::BindingLayoutItem::Texture_SRV(7),              // 7  gPrevNormalViewZ
      nvrhi::BindingLayoutItem::Texture_SRV(8),              // 8  gPrevDiffuseFast
      nvrhi::BindingLayoutItem::Texture_SRV(9),              // 9  gPrevSpecularFast
      nvrhi::BindingLayoutItem::Texture_UAV(10),             // 10 gCurrDiffuseSlow
      nvrhi::BindingLayoutItem::Texture_UAV(11),             // 11 gCurrSpecularSlow
      nvrhi::BindingLayoutItem::Texture_UAV(12),             // 12 gCurrNormalViewZ
      nvrhi::BindingLayoutItem::Texture_UAV(13),             // 13 gCurrDiffuseFast
      nvrhi::BindingLayoutItem::Texture_UAV(14),             // 14 gCurrSpecularFast
      nvrhi::BindingLayoutItem::VolatileConstantBuffer(15),  // 15 DenoiseTemporalUniforms
  };
  _bindingLayout = _device->createBindingLayout(layoutDesc);
  if (!_bindingLayout) {
    log.Error(log::RENDER, "DenoiseTemporalPass: createBindingLayout failed");
    return;
  }

  nvrhi::ComputePipelineDesc pipelineDesc;
  pipelineDesc.CS = _shader;
  pipelineDesc.bindingLayouts = {_bindingLayout};
  _pipeline = _device->createComputePipeline(pipelineDesc);
  if (!_pipeline) {
    log.Error(log::RENDER, "DenoiseTemporalPass: createComputePipeline failed");
    return;
  }

  nvrhi::BufferDesc cbDesc;
  cbDesc.byteSize = sizeof(shaderinterop::DenoiseTemporalUniforms);
  cbDesc.isConstantBuffer = true;
  // Volatile: writeBuffer'd once per Execute on the open command list (same
  // finding as TonemapPass.Params / AutoExposurePass.Params). maxVersions=8
  // starved past ~frame 8 of `render.accumulationFrames` (headless has no
  // per-frame GPU wait — VkDeviceManagerHeadless::BeginFrame/EndFrame are
  // no-ops — so many frames' worth of writes can be outstanding at once).
  // Once NVRHI can't find a free/GPU-retired version it logs "maxVersions
  // is insufficient" and SKIPS the write (vulkan-buffer.cpp
  // CommandList::writeVolatileBuffer's `if (!found) { ...; return; }`
  // path) — the next dispatch then binds back to version 0's STALE
  // content, which is this buffer's very first (frame-0, hasHistory=0)
  // write. That silently forced every starved frame to retake the
  // no-history branch, discarding all accumulated history — the root
  // cause of the temporal-accumulation regression this constant fixes.
  // 512 matches Tonemap/AutoExposure's precedent (covers a 512-frame
  // accumulation/bench window at 32 B/version — trivial memory).
  cbDesc.isVolatile = true;
  cbDesc.maxVersions = 512;
  cbDesc.debugName = "DenoiseTemporal.Params";
  _paramsBuffer = _device->createBuffer(cbDesc);
  if (!_paramsBuffer) {
    log.Error(log::RENDER, "DenoiseTemporalPass: createBuffer(params) failed");
    return;
  }

  _ready = true;
  log.Info(log::RENDER, "DenoiseTemporalPass: initialised");
}

DenoiseTemporalPass::~DenoiseTemporalPass() = default;

nvrhi::BindingSetHandle DenoiseTemporalPass::GetOrCreateBindingSet(
    nvrhi::ITexture* rawDiffuse, nvrhi::ITexture* rawSpecular, nvrhi::ITexture* motionVector,
    nvrhi::ITexture* viewZ, nvrhi::ITexture* normalRoughness) {
  // The ping-pong prev/curr textures flip identity every frame (Advance()),
  // so the cache key must fold them in too, not just the caller-owned
  // inputs — otherwise a stale binding set would point curr/prev at last
  // frame's roles.
  const std::uint64_t key = HashPointers(
      {rawDiffuse, rawSpecular, motionVector, viewZ, normalRoughness, _resources.PrevDiffuse(),
       _resources.PrevSpecular(), _resources.PrevNormalViewZ(), _resources.PrevDiffuseFast(),
       _resources.PrevSpecularFast(), _resources.CurrDiffuse(), _resources.CurrSpecular(),
       _resources.CurrNormalViewZ(), _resources.CurrDiffuseFast(), _resources.CurrSpecularFast()});
  if (auto cached = _bindingSetCache.find(key); cached != _bindingSetCache.end())
    return cached->second;
  constexpr std::size_t MAX_CACHE_ENTRIES = 4;
  if (_bindingSetCache.size() >= MAX_CACHE_ENTRIES)
    _bindingSetCache.clear();

  nvrhi::BindingSetDesc setDesc;
  setDesc.bindings = {
      nvrhi::BindingSetItem::Texture_SRV(0, rawDiffuse),
      nvrhi::BindingSetItem::Texture_SRV(1, rawSpecular),
      nvrhi::BindingSetItem::Texture_SRV(2, motionVector),
      nvrhi::BindingSetItem::Texture_SRV(3, viewZ),
      nvrhi::BindingSetItem::Texture_SRV(4, normalRoughness),
      nvrhi::BindingSetItem::Texture_SRV(5, _resources.PrevDiffuse()),
      nvrhi::BindingSetItem::Texture_SRV(6, _resources.PrevSpecular()),
      nvrhi::BindingSetItem::Texture_SRV(7, _resources.PrevNormalViewZ()),
      nvrhi::BindingSetItem::Texture_SRV(8, _resources.PrevDiffuseFast()),
      nvrhi::BindingSetItem::Texture_SRV(9, _resources.PrevSpecularFast()),
      nvrhi::BindingSetItem::Texture_UAV(10, _resources.CurrDiffuse()),
      nvrhi::BindingSetItem::Texture_UAV(11, _resources.CurrSpecular()),
      nvrhi::BindingSetItem::Texture_UAV(12, _resources.CurrNormalViewZ()),
      nvrhi::BindingSetItem::Texture_UAV(13, _resources.CurrDiffuseFast()),
      nvrhi::BindingSetItem::Texture_UAV(14, _resources.CurrSpecularFast()),
      nvrhi::BindingSetItem::ConstantBuffer(15, _paramsBuffer),
  };
  nvrhi::BindingSetHandle set = _device->createBindingSet(setDesc, _bindingLayout);
  _bindingSetCache.emplace(key, set);
  return set;
}

void DenoiseTemporalPass::Execute(nvrhi::ICommandList* commandList, const PassContext& context) {
  if (!_ready || commandList == nullptr || context.settings == nullptr
      || context.targets == nullptr)
    return;
  if ((context.settings->realTimeQuality.passMask & shaderinterop::PASS_MASK_DENOISE) == 0u)
    return;

  nvrhi::ITexture* const rawDiffuse = context.gIndirectDiffuse;
  nvrhi::ITexture* const rawSpecular = context.gReflections;
  nvrhi::ITexture* const motionVector = context.gMotionVector;
  nvrhi::ITexture* const viewZ = context.gViewZ;
  nvrhi::ITexture* const normalRoughness = context.gNormalRoughness;
  if (rawDiffuse == nullptr || rawSpecular == nullptr || motionVector == nullptr
      || viewZ == nullptr || normalRoughness == nullptr)
    return;

  const nvrhi::TextureDesc& desc = rawDiffuse->getDesc();
  const uint32_t width = desc.width;
  const uint32_t height = desc.height;
  if (width == 0u || height == 0u)
    return;
  if (!_resources.Ensure(width, height))
    return;

  shaderinterop::DenoiseTemporalUniforms params{};
  params.destWidth = width;
  params.destHeight = height;
  params.hasHistory = _resources.HasHistory() ? 1u : 0u;
  params.maxAccumFrames = 30u;      // NRD default target.
  params.fastHistoryFrames = 6u;    // NRD default target — work item 2 dual history.
  params.clampSigma = 2.0f;         // NRD default target — fast-history luminance clamp.
  params._pad1 = 0u;
  params._pad2 = 0u;
  commandList->writeBuffer(_paramsBuffer, &params, sizeof(params));

  const nvrhi::BindingSetHandle bindingSet =
      GetOrCreateBindingSet(rawDiffuse, rawSpecular, motionVector, viewZ, normalRoughness);
  if (!bindingSet)
    return;

  commandList->setTextureState(rawDiffuse, nvrhi::AllSubresources,
                               nvrhi::ResourceStates::ShaderResource);
  commandList->setTextureState(rawSpecular, nvrhi::AllSubresources,
                               nvrhi::ResourceStates::ShaderResource);
  commandList->setTextureState(motionVector, nvrhi::AllSubresources,
                               nvrhi::ResourceStates::ShaderResource);
  commandList->setTextureState(viewZ, nvrhi::AllSubresources,
                               nvrhi::ResourceStates::ShaderResource);
  commandList->setTextureState(normalRoughness, nvrhi::AllSubresources,
                               nvrhi::ResourceStates::ShaderResource);
  // Previous-frame ping-pong textures are only ever read here; NVRHI
  // still wants an explicit state each frame since their role (prev vs
  // curr) flips.
  commandList->setTextureState(_resources.PrevDiffuse(), nvrhi::AllSubresources,
                               nvrhi::ResourceStates::ShaderResource);
  commandList->setTextureState(_resources.PrevSpecular(), nvrhi::AllSubresources,
                               nvrhi::ResourceStates::ShaderResource);
  commandList->setTextureState(_resources.PrevNormalViewZ(), nvrhi::AllSubresources,
                               nvrhi::ResourceStates::ShaderResource);
  commandList->setTextureState(_resources.PrevDiffuseFast(), nvrhi::AllSubresources,
                               nvrhi::ResourceStates::ShaderResource);
  commandList->setTextureState(_resources.PrevSpecularFast(), nvrhi::AllSubresources,
                               nvrhi::ResourceStates::ShaderResource);
  commandList->setTextureState(_resources.CurrDiffuse(), nvrhi::AllSubresources,
                               nvrhi::ResourceStates::UnorderedAccess);
  commandList->setTextureState(_resources.CurrSpecular(), nvrhi::AllSubresources,
                               nvrhi::ResourceStates::UnorderedAccess);
  commandList->setTextureState(_resources.CurrNormalViewZ(), nvrhi::AllSubresources,
                               nvrhi::ResourceStates::UnorderedAccess);
  commandList->setTextureState(_resources.CurrDiffuseFast(), nvrhi::AllSubresources,
                               nvrhi::ResourceStates::UnorderedAccess);
  commandList->setTextureState(_resources.CurrSpecularFast(), nvrhi::AllSubresources,
                               nvrhi::ResourceStates::UnorderedAccess);
  commandList->commitBarriers();

  nvrhi::ComputeState state;
  state.pipeline = _pipeline;
  state.bindings = {bindingSet};
  commandList->setComputeState(state);

  const uint32_t groupsX = (width + 7u) / 8u;
  const uint32_t groupsY = (height + 7u) / 8u;
  commandList->dispatch(groupsX, groupsY, 1u);

  // Snapshot the JUST-WRITTEN textures for Diffuse()/Specular() BEFORE
  // flipping the ping-pong roles below — see those accessors' doc comment
  // in the header for why reading through `_resources.CurrDiffuse()`
  // AFTER Advance() would return the wrong (stale) slot to a same-frame
  // downstream reader (DenoiseHistoryFixPass).
  _lastWrittenDiffuse = _resources.CurrDiffuse();
  _lastWrittenSpecular = _resources.CurrSpecular();
  _lastWrittenDiffuseFast = _resources.CurrDiffuseFast();
  _lastWrittenSpecularFast = _resources.CurrSpecularFast();

  // Flip for next frame — the ping-pong role swap this WHOLE class exists
  // for. MUST happen after the dispatch above (the dispatch just wrote
  // this frame's data into what was `Curr*`; that data becomes `Prev*`
  // for the next Execute() call).
  _resources.Advance();
}

}  // namespace pyxis
