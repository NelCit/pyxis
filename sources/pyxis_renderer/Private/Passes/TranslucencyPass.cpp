// Pyxis renderer — TranslucencyPass (RTX-alignment design, WP2-signals).

#include "Passes/TranslucencyPass.h"

#include "Passes/SceneBindings.h"
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
  return LoadSpirvShader(device, path, stage, entry, "TranslucencyPass");
}

struct PipelineVariant {
  nvrhi::rt::PipelineHandle pipeline;
  nvrhi::rt::ShaderTableHandle shaderTable;
};

// This pass reuses the megakernel ShadeSurfaceHit (via ClosestHitMain),
// which fires its own shadow/AO/reflection sub-rays exactly as
// RaytracedLightingPass's closesthit does — same recursion budget (3),
// same MAX_TRANSPARENT_SEGMENTS / REFL_MAX_SEGMENTS / OPENPBR_FEATURE_MASK
// shader-declared DEFAULTS (not overridden here — this pass doesn't
// expose a runtime OpenPBR-feature-mask toggle; see the .h file header).
// Only PROJECTION_MODE is specialized.
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
    log.Error(log::RENDER, "TranslucencyPass: createShaderSpecialization failed (projectionMode="
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
  // Same depth budget as RaytracedLightingPass — ShadeSurfaceHit's own
  // shadow/AO/reflection sub-rays run at every segment.
  pipelineDesc.maxRecursionDepth = 3;
  PipelineVariant variant;
  variant.pipeline = device->createRayTracingPipeline(pipelineDesc);
  if (!variant.pipeline)
  {
    log.Error(log::RENDER, "TranslucencyPass: createRayTracingPipeline failed (projectionMode="
                              + std::to_string(projectionMode) + ")");
    return {};
  }
  variant.shaderTable = variant.pipeline->createShaderTable();
  if (!variant.shaderTable)
  {
    log.Error(log::RENDER, "TranslucencyPass: createShaderTable failed (projectionMode="
                              + std::to_string(projectionMode) + ")");
    return {};
  }
  variant.shaderTable->setRayGenerationShader("RayGenMain");
  variant.shaderTable->addMissShader("MissMain");        // miss-index 0: reflection/continuation rays.
  variant.shaderTable->addMissShader("ShadowMissMain");  // miss-index 1: ShadeSurfaceHit's shadow/AO rays.
  variant.shaderTable->addHitGroup("HitGroupDefault");
  return variant;
}

}  // namespace

TranslucencyPass::TranslucencyPass(nvrhi::IDevice* device, GpuScene& scene,
                                   SceneBindings& sceneBindings)
    : _device(device), _scene(&scene), _sceneBindings(&sceneBindings) {
  if (!_sceneBindings->IsOperational())
  {
    Logging::Get().Error(log::RENDER,
                         "TranslucencyPass: SceneBindings not operational; pass will skip");
    return;
  }

  const AssetLocator locator;
  const Path raygenPath = locator.LocateResource("shaders/translucency_raygen.spv");
  const Path closestHitPath = locator.LocateResource("shaders/translucency_closesthit.spv");
  const Path missPath = locator.LocateResource("shaders/translucency_miss.spv");
  const Path shadowMissPath = locator.LocateResource("shaders/translucency_shadow_miss.spv");
  const Path anyHitPath = locator.LocateResource("shaders/translucency_anyhit.spv");

  _raygenShader = LoadSpirv(_device, raygenPath.View(), nvrhi::ShaderType::RayGeneration, "main");
  _closestHitShader =
      LoadSpirv(_device, closestHitPath.View(), nvrhi::ShaderType::ClosestHit, "main");
  _missShader = LoadSpirv(_device, missPath.View(), nvrhi::ShaderType::Miss, "main");
  _shadowMissShader =
      LoadSpirv(_device, shadowMissPath.View(), nvrhi::ShaderType::Miss, "main");
  _anyHitShader = LoadSpirv(_device, anyHitPath.View(), nvrhi::ShaderType::AnyHit, "main");
  if (!_raygenShader || !_closestHitShader || !_missShader || !_shadowMissShader || !_anyHitShader)
  {
    Logging::Get().Error(log::RENDER, "TranslucencyPass: shader load failed; pass will skip");
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
      nvrhi::BindingLayoutItem::Texture_UAV(1),           // 1 gTranslucency
  };
  _passLayout = _device->createBindingLayout(layoutDesc);
  if (!_passLayout)
  {
    Logging::Get().Error(log::RENDER, "TranslucencyPass: createBindingLayout(Set 1) failed");
    return;
  }

  PipelineVariant perspective = BuildPipelineVariant(
      _device, _sceneBindings->Layout(), _passLayout, _raygenShader, _closestHitShader,
      _missShader, _shadowMissShader, _anyHitShader, /*projectionMode=*/0u);
  if (!perspective.pipeline || !perspective.shaderTable)
    return;
  _pipelines[0] = std::move(perspective.pipeline);
  _shaderTables[0] = std::move(perspective.shaderTable);

  _shadersOk = true;
  Logging::Get().Info(log::RENDER, "TranslucencyPass: initialised (RT pipeline + SBT ready)");
}

TranslucencyPass::~TranslucencyPass() = default;

nvrhi::ITexture* TranslucencyPass::EnsureOutput(uint32_t width, uint32_t height) {
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
  desc.debugName = "Translucency.gTranslucency";
  desc.initialState = nvrhi::ResourceStates::UnorderedAccess;
  desc.keepInitialState = true;
  _output = _device->createTexture(desc);
  _outputW = width;
  _outputH = height;
  _bindingSetCache.clear();
  if (!_output)
  {
    Logging::Get().Error(log::RENDER, "TranslucencyPass: createTexture(gTranslucency, "
                                          + std::to_string(width) + "x" + std::to_string(height)
                                          + ") failed; pass will skip");
    return nullptr;
  }
  return _output;
}

void TranslucencyPass::EnsureProjectionPipeline() {
  if (!_shadersOk || _scene == nullptr || !_scene->HasCamera())
    return;
  const std::size_t variant = (_scene->GetCamera().projectionMode == 1u) ? 1u : 0u;
  if (_pipelines[variant] || _variantBuildFailed[variant])
    return;
  PipelineVariant built = BuildPipelineVariant(
      _device, _sceneBindings->Layout(), _passLayout, _raygenShader, _closestHitShader,
      _missShader, _shadowMissShader, _anyHitShader, static_cast<uint32_t>(variant));
  if (!built.pipeline || !built.shaderTable)
  {
    _variantBuildFailed[variant] = true;
    return;
  }
  _pipelines[variant] = std::move(built.pipeline);
  _shaderTables[variant] = std::move(built.shaderTable);
}

nvrhi::BindingSetHandle TranslucencyPass::GetOrCreateBindingSet(nvrhi::IBuffer* visibility) {
  if (auto cached = _bindingSetCache.find(visibility); cached != _bindingSetCache.end())
    return cached->second;
  constexpr std::size_t MAX_CACHE_ENTRIES = 4;
  if (_bindingSetCache.size() >= MAX_CACHE_ENTRIES)
    _bindingSetCache.clear();

  nvrhi::BindingSetDesc setDesc;
  setDesc.bindings = {
      nvrhi::BindingSetItem::StructuredBuffer_SRV(0, visibility),
      nvrhi::BindingSetItem::Texture_UAV(1, _output.Get()),
  };
  nvrhi::BindingSetHandle set = _device->createBindingSet(setDesc, _passLayout);
  _bindingSetCache[visibility] = set;
  return set;
}

void TranslucencyPass::Execute(nvrhi::ICommandList* commandList, const PassContext& context) {
  if (commandList == nullptr || context.targets == nullptr || context.settings == nullptr)
    return;
  // WP2-final — RealTimeQuality.passMask bit 4 gates this signal pass;
  // default 0x1F runs every pass (image-identical to before this gate).
  if ((context.settings->realTimeQuality.passMask & 0x10u) == 0u)
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
