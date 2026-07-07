// Pyxis renderer — IndirectDiffusePass (RTX-alignment design, WP2-signals).

#include "Passes/IndirectDiffusePass.h"

#include "Passes/SceneBindings.h"
#include "Passes/SharcResolvePass.h"
#include "RenderGraph/PassContext.h"
#include "RenderGraph/ShaderLoad.h"
#include "Scene/SceneResources.h"

#include <Pyxis/Platform/FileSystem/AssetLocator.h>
#include <Pyxis/Platform/Logging/Log.h>
#include <Pyxis/Platform/Logging/LogCategories.h>
#include <Pyxis/Renderer/Descs/RenderTargets.h>
#include <Pyxis/Renderer/GpuScene.h>
#include <Pyxis/Renderer/Profiler.h>

#include "ShaderInterop.slang"

#include <cstddef>
#include <string>

namespace pyxis {

namespace {

nvrhi::ShaderHandle LoadSpirv(nvrhi::IDevice* device, std::string_view path,
                              nvrhi::ShaderType stage, const char* entry) noexcept {
  return LoadSpirvShader(device, path, stage, entry, "IndirectDiffusePass");
}

struct PipelineVariant {
  nvrhi::rt::PipelineHandle pipeline;
  nvrhi::rt::ShaderTableHandle shaderTable;
};

// Phase B pipeline: RayGen (bounce ray) + ClosestHit (bounce-hit shading +
// its own NEE shadow ray + the second-bounce continuation ray) + two miss
// shaders (index 0 = bounce-ray dome background, index 1 = NEE shadow-ray
// visibility) + AnyHit (shared by both ray types — see
// indirect_diffuse.slang's AnyHitMain doc comment).
// maxRecursionDepth = 3 (RTX-alignment design, Phase B second
// indirect-diffuse bounce — was 2): the raygen's bounce-ray TraceRay is
// level 1; ClosestHitMain's own shadow-ray TraceRay AND its second-bounce
// continuation TraceRay (both invoked from within the level-1 hit) are
// level 2; the continuation's own hit fires ITS shadow ray at level 3 —
// see indirect_diffuse.slang's ClosestHitMain / MAX_INDIRECT_BOUNCE_DEPTH.
PipelineVariant BuildPipelineVariant(nvrhi::IDevice* device, nvrhi::IBindingLayout* sceneLayout,
                                     nvrhi::IBindingLayout* passLayout, nvrhi::IShader* raygen,
                                     nvrhi::IShader* closestHit, nvrhi::IShader* miss,
                                     nvrhi::IShader* shadowMiss, nvrhi::IShader* anyHit,
                                     uint32_t projectionMode) noexcept {
  auto& log = Logging::Get();
  const nvrhi::ShaderSpecialization raygenConstants[] = {
      nvrhi::ShaderSpecialization::UInt32(shaderinterop::SPEC_ID_PROJECTION_MODE, projectionMode),
  };
  const nvrhi::ShaderHandle specRaygen = device->createShaderSpecialization(
      raygen, raygenConstants, static_cast<uint32_t>(std::size(raygenConstants)));
  if (!specRaygen)
  {
    log.Error(log::RENDER, "IndirectDiffusePass: createShaderSpecialization failed (projectionMode="
                              + std::to_string(projectionMode) + ")");
    return {};
  }

  nvrhi::rt::PipelineDesc pipelineDesc;
  pipelineDesc.shaders = {
      nvrhi::rt::PipelineShaderDesc{}.setExportName("RayGenMain").setShader(specRaygen),
      nvrhi::rt::PipelineShaderDesc{}.setExportName("MissMain").setShader(miss),
      nvrhi::rt::PipelineShaderDesc{}.setExportName("ShadowMissMain").setShader(shadowMiss),
  };
  pipelineDesc.hitGroups = {
      nvrhi::rt::PipelineHitGroupDesc{}
          .setExportName("HitGroupDefault")
          .setClosestHitShader(closestHit)
          .setAnyHitShader(anyHit),
  };
  pipelineDesc.globalBindingLayouts = {sceneLayout, passLayout};
  pipelineDesc.maxRecursionDepth = 3;
  PipelineVariant variant;
  variant.pipeline = device->createRayTracingPipeline(pipelineDesc);
  if (!variant.pipeline)
  {
    log.Error(log::RENDER, "IndirectDiffusePass: createRayTracingPipeline failed (projectionMode="
                              + std::to_string(projectionMode) + ")");
    return {};
  }
  variant.shaderTable = variant.pipeline->createShaderTable();
  if (!variant.shaderTable)
  {
    log.Error(log::RENDER, "IndirectDiffusePass: createShaderTable failed (projectionMode="
                              + std::to_string(projectionMode) + ")");
    return {};
  }
  variant.shaderTable->setRayGenerationShader("RayGenMain");
  variant.shaderTable->addMissShader("MissMain");        // index 0: bounce-ray dome background.
  variant.shaderTable->addMissShader("ShadowMissMain");  // index 1: NEE shadow-ray visibility.
  variant.shaderTable->addHitGroup("HitGroupDefault");
  return variant;
}

}  // namespace

IndirectDiffusePass::IndirectDiffusePass(nvrhi::IDevice* device, GpuScene& scene,
                                         SceneBindings& sceneBindings, SharcResolvePass& sharc)
    : _device(device), _scene(&scene), _sceneBindings(&sceneBindings), _sharc(&sharc) {
  if (!_sceneBindings->IsOperational())
  {
    Logging::Get().Error(log::RENDER,
                         "IndirectDiffusePass: SceneBindings not operational; pass will skip");
    return;
  }

  const AssetLocator locator;
  const Path raygenPath = locator.LocateResource("shaders/indirect_diffuse_raygen.spv");
  const Path closestHitPath = locator.LocateResource("shaders/indirect_diffuse_closesthit.spv");
  const Path missPath = locator.LocateResource("shaders/indirect_diffuse_miss.spv");
  const Path shadowMissPath = locator.LocateResource("shaders/indirect_diffuse_shadow_miss.spv");
  const Path anyHitPath = locator.LocateResource("shaders/indirect_diffuse_anyhit.spv");

  _raygenShader = LoadSpirv(_device, raygenPath.View(), nvrhi::ShaderType::RayGeneration, "main");
  _closestHitShader =
      LoadSpirv(_device, closestHitPath.View(), nvrhi::ShaderType::ClosestHit, "main");
  _missShader = LoadSpirv(_device, missPath.View(), nvrhi::ShaderType::Miss, "main");
  _shadowMissShader =
      LoadSpirv(_device, shadowMissPath.View(), nvrhi::ShaderType::Miss, "main");
  _anyHitShader = LoadSpirv(_device, anyHitPath.View(), nvrhi::ShaderType::AnyHit, "main");
  if (!_raygenShader || !_closestHitShader || !_missShader || !_shadowMissShader || !_anyHitShader)
  {
    Logging::Get().Error(log::RENDER, "IndirectDiffusePass: shader load failed; pass will skip");
    return;
  }

  nvrhi::BindingLayoutDesc layoutDesc;
  layoutDesc.visibility = nvrhi::ShaderType::AllRayTracing;
  layoutDesc.bindingOffsets.shaderResource = 0;
  layoutDesc.bindingOffsets.sampler = 0;
  layoutDesc.bindingOffsets.constantBuffer = 0;
  layoutDesc.bindingOffsets.unorderedAccess = 0;
  layoutDesc.bindings = {
      nvrhi::BindingLayoutItem::StructuredBuffer_SRV(0),  // 0 visibility (packed 16 B)
      nvrhi::BindingLayoutItem::Texture_UAV(1),           // 1 gIndirectDiffuse
      nvrhi::BindingLayoutItem::RawBuffer_UAV(2),         // 2 gShHash (SHARC cache)
      nvrhi::BindingLayoutItem::RawBuffer_UAV(3),         // 3 gShAccum (SHARC cache)
      nvrhi::BindingLayoutItem::RawBuffer_UAV(4),         // 4 gShResolved (SHARC cache)
  };
  _passLayout = _device->createBindingLayout(layoutDesc);
  if (!_passLayout)
  {
    Logging::Get().Error(log::RENDER, "IndirectDiffusePass: createBindingLayout(Set 1) failed");
    return;
  }

  PipelineVariant perspective =
      BuildPipelineVariant(_device, _sceneBindings->Layout(), _passLayout, _raygenShader,
                           _closestHitShader, _missShader, _shadowMissShader, _anyHitShader,
                           /*projectionMode=*/0u);
  if (!perspective.pipeline || !perspective.shaderTable)
    return;
  _pipelines[0] = std::move(perspective.pipeline);
  _shaderTables[0] = std::move(perspective.shaderTable);

  _shadersOk = true;
  Logging::Get().Info(log::RENDER, "IndirectDiffusePass: initialised (RT pipeline + SBT ready)");
}

IndirectDiffusePass::~IndirectDiffusePass() = default;

nvrhi::ITexture* IndirectDiffusePass::EnsureOutput(uint32_t width, uint32_t height) {
  if (width == 0u || height == 0u)
    return nullptr;
  if (_output && _outputW == width && _outputH == height)
    return _output;
  nvrhi::TextureDesc desc;
  desc.width = width;
  desc.height = height;
  desc.format = nvrhi::Format::RGBA16_FLOAT;
  desc.dimension = nvrhi::TextureDimension::Texture2D;
  desc.isUAV = true;
  // WP2-final — CompositePass reads this signal as a Texture_SRV;
  // sampled-image usage is REQUIRED or the SRV silently reads zeros
  // (see RaytracedGBufferPass's makeGuideTexture bug-fix note).
  desc.isShaderResource = true;
  desc.debugName = "IndirectDiffuse.gIndirectDiffuse";
  desc.initialState = nvrhi::ResourceStates::UnorderedAccess;
  desc.keepInitialState = true;
  _output = _device->createTexture(desc);
  _outputW = width;
  _outputH = height;
  _bindingSetCache.clear();
  if (!_output)
  {
    Logging::Get().Error(log::RENDER, "IndirectDiffusePass: createTexture(gIndirectDiffuse, "
                                          + std::to_string(width) + "x" + std::to_string(height)
                                          + ") failed; pass will skip");
    return nullptr;
  }
  return _output;
}

void IndirectDiffusePass::EnsureProjectionPipeline() {
  if (!_shadersOk || _scene == nullptr || !_scene->HasCamera())
    return;
  const std::size_t variant = (_scene->GetCamera().projectionMode == 1u) ? 1u : 0u;
  if (_pipelines[variant] || _variantBuildFailed[variant])
    return;
  PipelineVariant built =
      BuildPipelineVariant(_device, _sceneBindings->Layout(), _passLayout, _raygenShader,
                           _closestHitShader, _missShader, _shadowMissShader, _anyHitShader,
                           static_cast<uint32_t>(variant));
  if (!built.pipeline || !built.shaderTable)
  {
    _variantBuildFailed[variant] = true;
    return;
  }
  _pipelines[variant] = std::move(built.pipeline);
  _shaderTables[variant] = std::move(built.shaderTable);
}

nvrhi::BindingSetHandle IndirectDiffusePass::GetOrCreateBindingSet(nvrhi::IBuffer* visibility) {
  if (auto cached = _bindingSetCache.find(visibility); cached != _bindingSetCache.end())
    return cached->second;
  constexpr std::size_t MAX_CACHE_ENTRIES = 4;
  if (_bindingSetCache.size() >= MAX_CACHE_ENTRIES)
    _bindingSetCache.clear();

  // SHARC cache buffers (owned by SharcResolvePass) — always bound; the shader
  // touches them only when gQuality.giMode != 0.
  nvrhi::IBuffer* const shHash = _sharc != nullptr ? _sharc->HashBuffer() : nullptr;
  nvrhi::IBuffer* const shAccum = _sharc != nullptr ? _sharc->AccumBuffer() : nullptr;
  nvrhi::IBuffer* const shResolved = _sharc != nullptr ? _sharc->ResolvedBuffer() : nullptr;
  if (shHash == nullptr || shAccum == nullptr || shResolved == nullptr)
    return nullptr;  // catastrophic (GPU OOM) — pass skips; see SharcResolvePass ctor.

  nvrhi::BindingSetDesc setDesc;
  setDesc.bindings = {
      nvrhi::BindingSetItem::StructuredBuffer_SRV(0, visibility),
      nvrhi::BindingSetItem::Texture_UAV(1, _output.Get()),
      nvrhi::BindingSetItem::RawBuffer_UAV(2, shHash),
      nvrhi::BindingSetItem::RawBuffer_UAV(3, shAccum),
      nvrhi::BindingSetItem::RawBuffer_UAV(4, shResolved),
  };
  nvrhi::BindingSetHandle set = _device->createBindingSet(setDesc, _passLayout);
  _bindingSetCache[visibility] = set;
  return set;
}

void IndirectDiffusePass::Execute(nvrhi::ICommandList* commandList, const PassContext& context) {
  if (commandList == nullptr || context.targets == nullptr || context.settings == nullptr)
    return;
  // WP2-final — RealTimeQuality.passMask bit 1 gates this signal pass;
  // default 0x1F runs every pass (image-identical to before this gate).
  if ((context.settings->realTimeQuality.passMask & 0x02u) == 0u)
    return;
  nvrhi::IBuffer* const visibility = context.visibility;
  if (visibility == nullptr)
    return;
  if (!_shadersOk || !_output)
    return;
  if (context.sceneBindingSet == nullptr)
    return;

  const SceneResources res = detail::SceneResourcesAccess::Get(*_scene);
  if (res.tlas == nullptr || !_scene->HasCamera())
    return;

  const CameraDesc& camera = _scene->GetCamera();
  const std::size_t projectionVariant = (camera.projectionMode == 1u) ? 1u : 0u;
  if (!_pipelines[projectionVariant] || !_shaderTables[projectionVariant])
    return;

  const nvrhi::BindingSetHandle bindingSet = GetOrCreateBindingSet(visibility);
  if (!bindingSet)
    return;

  commandList->setTextureState(_output.Get(), nvrhi::AllSubresources,
                               nvrhi::ResourceStates::UnorderedAccess);
  commandList->commitBarriers();

  nvrhi::rt::State state;
  state.shaderTable = _shaderTables[projectionVariant];
  state.bindings = {context.sceneBindingSet, bindingSet};
  commandList->setRayTracingState(state);

  nvrhi::rt::DispatchRaysArguments args;
  args.width = _outputW;
  args.height = _outputH;
  args.depth = 1;
  commandList->dispatchRays(args);
}

}  // namespace pyxis
