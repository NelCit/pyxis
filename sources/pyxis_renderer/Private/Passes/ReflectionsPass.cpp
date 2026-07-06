// Pyxis renderer — ReflectionsPass (RTX-alignment design, WP2-signals).

#include "Passes/ReflectionsPass.h"

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
  return LoadSpirvShader(device, path, stage, entry, "ReflectionsPass");
}

struct PipelineVariant {
  nvrhi::rt::PipelineHandle pipeline;
  nvrhi::rt::ShaderTableHandle shaderTable;
};

PipelineVariant BuildPipelineVariant(nvrhi::IDevice* device, nvrhi::IBindingLayout* sceneLayout,
                                     nvrhi::IBindingLayout* passLayout, nvrhi::IShader* raygen,
                                     nvrhi::IShader* closestHit, nvrhi::IShader* miss,
                                     nvrhi::IShader* aoAnyHit, nvrhi::IShader* aoMiss,
                                     uint32_t projectionMode) noexcept {
  auto& log = Logging::Get();
  const nvrhi::ShaderSpecialization raygenConstants[] = {
      nvrhi::ShaderSpecialization::UInt32(shaderinterop::SPEC_ID_PROJECTION_MODE, projectionMode),
  };
  const nvrhi::ShaderHandle specRaygen = device->createShaderSpecialization(
      raygen, raygenConstants, static_cast<uint32_t>(std::size(raygenConstants)));
  if (!specRaygen)
  {
    log.Error(log::RENDER, "ReflectionsPass: createShaderSpecialization failed (projectionMode="
                              + std::to_string(projectionMode) + ")");
    return {};
  }

  nvrhi::rt::PipelineDesc pipelineDesc;
  pipelineDesc.shaders = {
      nvrhi::rt::PipelineShaderDesc{}.setExportName("RayGenMain").setShader(specRaygen),
      nvrhi::rt::PipelineShaderDesc{}.setExportName("MissMain").setShader(miss),
      // Occlusion-aware-ambient follow-up — the AO ray ClosestHitMain
      // fires at the reflection hit (reflections.slang) uses this as its
      // MissShaderIndex 1.
      nvrhi::rt::PipelineShaderDesc{}.setExportName("AoMissMain").setShader(aoMiss),
  };
  // HitGroupDefault: no any-hit — alpha-tested cutout geometry reads as
  // solid in Phase A reflections (documented approximation — the WP2
  // contract table lists only RayGen/ClosestHit/Miss for this pass).
  // HitGroupAo: any-hit only (no closest-hit — the AO ray always carries
  // RAY_FLAG_SKIP_CLOSEST_HIT_SHADER, same "never invoked" legality
  // AmbientOcclusionPass's own any-hit-only hit group relies on),
  // SEPARATE from HitGroupDefault above so the primary reflection ray's
  // "reads as solid" behavior is unaffected by this any-hit gate.
  pipelineDesc.hitGroups = {
      nvrhi::rt::PipelineHitGroupDesc{}
          .setExportName("HitGroupDefault")
          .setClosestHitShader(closestHit),
      nvrhi::rt::PipelineHitGroupDesc{}
          .setExportName("HitGroupAo")
          .setAnyHitShader(aoAnyHit),
  };
  pipelineDesc.globalBindingLayouts = {sceneLayout, passLayout};
  // Occlusion-aware-ambient follow-up — was 1 (one mirror-direction
  // bounce, no further recursion). Now 2: RayGen's reflection TraceRay is
  // level 1; ClosestHitMain's own AO TraceRay (the occlusion-aware-ambient
  // gate, reflections.slang) is level 2. No further recursion from there
  // (the AO ray's any-hit never calls TraceRay).
  pipelineDesc.maxRecursionDepth = 2;
  PipelineVariant variant;
  variant.pipeline = device->createRayTracingPipeline(pipelineDesc);
  if (!variant.pipeline)
  {
    log.Error(log::RENDER, "ReflectionsPass: createRayTracingPipeline failed (projectionMode="
                              + std::to_string(projectionMode) + ")");
    return {};
  }
  variant.shaderTable = variant.pipeline->createShaderTable();
  if (!variant.shaderTable)
  {
    log.Error(log::RENDER, "ReflectionsPass: createShaderTable failed (projectionMode="
                              + std::to_string(projectionMode) + ")");
    return {};
  }
  variant.shaderTable->setRayGenerationShader("RayGenMain");
  variant.shaderTable->addMissShader("MissMain");    // index 0: primary reflection ray.
  variant.shaderTable->addMissShader("AoMissMain");  // index 1: occlusion-aware-ambient AO ray.
  variant.shaderTable->addHitGroup("HitGroupDefault");  // index 0: primary reflection ray.
  variant.shaderTable->addHitGroup("HitGroupAo");       // index 1: occlusion-aware-ambient AO ray.
  return variant;
}

}  // namespace

ReflectionsPass::ReflectionsPass(nvrhi::IDevice* device, GpuScene& scene,
                                 SceneBindings& sceneBindings)
    : _device(device), _scene(&scene), _sceneBindings(&sceneBindings) {
  if (!_sceneBindings->IsOperational())
  {
    Logging::Get().Error(log::RENDER,
                         "ReflectionsPass: SceneBindings not operational; pass will skip");
    return;
  }

  const AssetLocator locator;
  const Path raygenPath = locator.LocateResource("shaders/reflections_raygen.spv");
  const Path closestHitPath = locator.LocateResource("shaders/reflections_closesthit.spv");
  const Path missPath = locator.LocateResource("shaders/reflections_miss.spv");
  // Occlusion-aware-ambient follow-up (rtx-realtime-alignment-design.md,
  // 2026-07-06) — the short AO ray ClosestHitMain fires at the reflection
  // hit; see this pass's own header / reflections.slang's file header.
  const Path aoAnyHitPath = locator.LocateResource("shaders/reflections_ao_anyhit.spv");
  const Path aoMissPath = locator.LocateResource("shaders/reflections_ao_miss.spv");

  _raygenShader = LoadSpirv(_device, raygenPath.View(), nvrhi::ShaderType::RayGeneration, "main");
  _closestHitShader =
      LoadSpirv(_device, closestHitPath.View(), nvrhi::ShaderType::ClosestHit, "main");
  _missShader = LoadSpirv(_device, missPath.View(), nvrhi::ShaderType::Miss, "main");
  _aoAnyHitShader = LoadSpirv(_device, aoAnyHitPath.View(), nvrhi::ShaderType::AnyHit, "main");
  _aoMissShader = LoadSpirv(_device, aoMissPath.View(), nvrhi::ShaderType::Miss, "main");
  if (!_raygenShader || !_closestHitShader || !_missShader || !_aoAnyHitShader || !_aoMissShader)
  {
    Logging::Get().Error(log::RENDER, "ReflectionsPass: shader load failed; pass will skip");
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
      nvrhi::BindingLayoutItem::Texture_SRV(1),           // 1 gNormalRoughness
      nvrhi::BindingLayoutItem::Texture_UAV(2),           // 2 gReflections
      nvrhi::BindingLayoutItem::Texture_UAV(3),           // 3 gReflectionWeight (WP2-final)
  };
  _passLayout = _device->createBindingLayout(layoutDesc);
  if (!_passLayout)
  {
    Logging::Get().Error(log::RENDER, "ReflectionsPass: createBindingLayout(Set 1) failed");
    return;
  }

  PipelineVariant perspective =
      BuildPipelineVariant(_device, _sceneBindings->Layout(), _passLayout, _raygenShader,
                           _closestHitShader, _missShader, _aoAnyHitShader, _aoMissShader,
                           /*projectionMode=*/0u);
  if (!perspective.pipeline || !perspective.shaderTable)
    return;
  _pipelines[0] = std::move(perspective.pipeline);
  _shaderTables[0] = std::move(perspective.shaderTable);

  _shadersOk = true;
  Logging::Get().Info(log::RENDER, "ReflectionsPass: initialised (RT pipeline + SBT ready)");
}

ReflectionsPass::~ReflectionsPass() = default;

nvrhi::ITexture* ReflectionsPass::EnsureOutput(uint32_t width, uint32_t height) {
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
  // WP2-final — CompositePass reads this signal (and gReflectionWeight,
  // which shares this desc below) as Texture_SRVs; sampled-image usage is
  // REQUIRED or the SRV silently reads zeros (see RaytracedGBufferPass's
  // makeGuideTexture bug-fix note).
  desc.isShaderResource = true;
  desc.debugName = "Reflections.gReflections";
  desc.initialState = nvrhi::ResourceStates::UnorderedAccess;
  desc.keepInitialState = true;
  _output = _device->createTexture(desc);
  // WP2-final — the SpecWeight sibling texture, same dims/lifetime as _output.
  desc.debugName = "Reflections.gReflectionWeight";
  _reflectionWeight = _device->createTexture(desc);
  _outputW = width;
  _outputH = height;
  _bindingSetCache.clear();
  _lastBindings = {};
  if (!_output || !_reflectionWeight)
  {
    Logging::Get().Error(log::RENDER, "ReflectionsPass: createTexture(gReflections/"
                                          "gReflectionWeight, " + std::to_string(width) + "x"
                                          + std::to_string(height) + ") failed; pass will skip");
    return nullptr;
  }
  return _output;
}

void ReflectionsPass::EnsureProjectionPipeline() {
  if (!_shadersOk || _scene == nullptr || !_scene->HasCamera())
    return;
  const std::size_t variant = (_scene->GetCamera().projectionMode == 1u) ? 1u : 0u;
  if (_pipelines[variant] || _variantBuildFailed[variant])
    return;
  PipelineVariant built =
      BuildPipelineVariant(_device, _sceneBindings->Layout(), _passLayout, _raygenShader,
                           _closestHitShader, _missShader, _aoAnyHitShader, _aoMissShader,
                           static_cast<uint32_t>(variant));
  if (!built.pipeline || !built.shaderTable)
  {
    _variantBuildFailed[variant] = true;
    return;
  }
  _pipelines[variant] = std::move(built.pipeline);
  _shaderTables[variant] = std::move(built.shaderTable);
}

nvrhi::BindingSetHandle ReflectionsPass::GetOrCreateBindingSet(nvrhi::IBuffer* visibility,
                                                               nvrhi::ITexture* normalRoughness) {
  const std::array<const void*, 2> current{visibility, normalRoughness};
  if (current != _lastBindings)
  {
    _bindingSetCache.clear();
    _lastBindings = current;
  }
  if (auto cached = _bindingSetCache.find(visibility); cached != _bindingSetCache.end())
    return cached->second;
  constexpr std::size_t MAX_CACHE_ENTRIES = 4;
  if (_bindingSetCache.size() >= MAX_CACHE_ENTRIES)
    _bindingSetCache.clear();

  nvrhi::BindingSetDesc setDesc;
  setDesc.bindings = {
      nvrhi::BindingSetItem::StructuredBuffer_SRV(0, visibility),
      nvrhi::BindingSetItem::Texture_SRV(1, normalRoughness),
      nvrhi::BindingSetItem::Texture_UAV(2, _output.Get()),
      nvrhi::BindingSetItem::Texture_UAV(3, _reflectionWeight.Get()),
  };
  nvrhi::BindingSetHandle set = _device->createBindingSet(setDesc, _passLayout);
  _bindingSetCache[visibility] = set;
  return set;
}

void ReflectionsPass::Execute(nvrhi::ICommandList* commandList, const PassContext& context) {
  if (commandList == nullptr || context.targets == nullptr || context.settings == nullptr)
    return;
  // WP2-final — RealTimeQuality.passMask bit 3 gates this signal pass
  // entirely: when clear, Execute() no-ops and gReflections /
  // gReflectionWeight simply keep whatever they held (last frame's
  // contents, or the untouched post-creation state). Default 0x1F runs
  // every pass, so this is a no-op change for the image-identical
  // default path.
  if ((context.settings->realTimeQuality.passMask & 0x08u) == 0u)
    return;
  nvrhi::IBuffer* const visibility = context.visibility;
  if (visibility == nullptr)
    return;
  nvrhi::ITexture* const normalRoughness = context.gNormalRoughness;
  if (normalRoughness == nullptr)
    return;
  if (!_shadersOk || !_output || !_reflectionWeight)
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

  const nvrhi::BindingSetHandle bindingSet = GetOrCreateBindingSet(visibility, normalRoughness);
  if (!bindingSet)
    return;

  commandList->setTextureState(normalRoughness, nvrhi::AllSubresources,
                               nvrhi::ResourceStates::ShaderResource);
  commandList->setTextureState(_output.Get(), nvrhi::AllSubresources,
                               nvrhi::ResourceStates::UnorderedAccess);
  commandList->setTextureState(_reflectionWeight.Get(), nvrhi::AllSubresources,
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
