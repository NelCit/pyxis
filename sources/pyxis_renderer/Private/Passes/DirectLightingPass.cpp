// Pyxis renderer — DirectLightingPass (RTX-alignment design, WP2-signals).

#include "Passes/DirectLightingPass.h"

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
  return LoadSpirvShader(device, path, stage, entry, "DirectLightingPass");
}

struct PipelineVariant {
  nvrhi::rt::PipelineHandle pipeline;
  nvrhi::rt::ShaderTableHandle shaderTable;
};

// The shadow ray is the ONLY TraceRay this pipeline issues, always via
// shading.slang's EvaluateDirectLightSample (hardcoded MissShaderIndex =
// 1, matching that helper's shared convention with ShadeSurfaceHit's own
// loop). The shader table therefore needs a (harmless, never selected)
// index-0 miss slot too — DXR shader tables are contiguous arrays; index
// 1 can't exist without index 0. Registering the same ShadowMissMain
// export at both slots is the simplest correct fill.
PipelineVariant BuildPipelineVariant(nvrhi::IDevice* device, nvrhi::IBindingLayout* sceneLayout,
                                     nvrhi::IBindingLayout* passLayout, nvrhi::IShader* raygen,
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
    log.Error(log::RENDER, "DirectLightingPass: createShaderSpecialization failed (projectionMode="
                              + std::to_string(projectionMode) + ")");
    return {};
  }

  nvrhi::rt::PipelineDesc pipelineDesc;
  pipelineDesc.shaders = {
      nvrhi::rt::PipelineShaderDesc{}.setExportName("RayGenMain").setShader(specRaygen),
      nvrhi::rt::PipelineShaderDesc{}.setExportName("ShadowMissMain").setShader(shadowMiss),
  };
  // Any-hit-only hit group — the shadow ray always carries
  // RAY_FLAG_SKIP_CLOSEST_HIT_SHADER, so closestHitShader stays null.
  pipelineDesc.hitGroups = {
      nvrhi::rt::PipelineHitGroupDesc{}
          .setExportName("HitGroupDefault")
          .setAnyHitShader(anyHit),
  };
  pipelineDesc.globalBindingLayouts = {sceneLayout, passLayout};
  pipelineDesc.maxRecursionDepth = 1;  // primary-depth shadow ray only, no further recursion.
  PipelineVariant variant;
  variant.pipeline = device->createRayTracingPipeline(pipelineDesc);
  if (!variant.pipeline)
  {
    log.Error(log::RENDER, "DirectLightingPass: createRayTracingPipeline failed (projectionMode="
                              + std::to_string(projectionMode) + ")");
    return {};
  }
  variant.shaderTable = variant.pipeline->createShaderTable();
  if (!variant.shaderTable)
  {
    log.Error(log::RENDER, "DirectLightingPass: createShaderTable failed (projectionMode="
                              + std::to_string(projectionMode) + ")");
    return {};
  }
  variant.shaderTable->setRayGenerationShader("RayGenMain");
  variant.shaderTable->addMissShader("ShadowMissMain");  // index 0: unused padding slot.
  variant.shaderTable->addMissShader("ShadowMissMain");  // index 1: the shadow ray's real miss.
  variant.shaderTable->addHitGroup("HitGroupDefault");
  return variant;
}

}  // namespace

DirectLightingPass::DirectLightingPass(nvrhi::IDevice* device, GpuScene& scene,
                                       SceneBindings& sceneBindings)
    : _device(device), _scene(&scene), _sceneBindings(&sceneBindings) {
  if (!_sceneBindings->IsOperational())
  {
    Logging::Get().Error(log::RENDER,
                         "DirectLightingPass: SceneBindings not operational; pass will skip");
    return;
  }

  const AssetLocator locator;
  const Path raygenPath = locator.LocateResource("shaders/direct_lighting_raygen.spv");
  const Path shadowMissPath = locator.LocateResource("shaders/direct_lighting_shadow_miss.spv");
  const Path anyHitPath = locator.LocateResource("shaders/direct_lighting_anyhit.spv");

  _raygenShader = LoadSpirv(_device, raygenPath.View(), nvrhi::ShaderType::RayGeneration, "main");
  _shadowMissShader =
      LoadSpirv(_device, shadowMissPath.View(), nvrhi::ShaderType::Miss, "main");
  _anyHitShader = LoadSpirv(_device, anyHitPath.View(), nvrhi::ShaderType::AnyHit, "main");
  if (!_raygenShader || !_shadowMissShader || !_anyHitShader)
  {
    Logging::Get().Error(log::RENDER, "DirectLightingPass: shader load failed; pass will skip");
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
      nvrhi::BindingLayoutItem::Texture_UAV(1),           // 1 gDirectDiffuse
      nvrhi::BindingLayoutItem::Texture_UAV(2),           // 2 gDirectSpecular
  };
  _passLayout = _device->createBindingLayout(layoutDesc);
  if (!_passLayout)
  {
    Logging::Get().Error(log::RENDER, "DirectLightingPass: createBindingLayout(Set 1) failed");
    return;
  }

  PipelineVariant perspective =
      BuildPipelineVariant(_device, _sceneBindings->Layout(), _passLayout, _raygenShader,
                           _shadowMissShader, _anyHitShader, /*projectionMode=*/0u);
  if (!perspective.pipeline || !perspective.shaderTable)
    return;
  _pipelines[0] = std::move(perspective.pipeline);
  _shaderTables[0] = std::move(perspective.shaderTable);

  _shadersOk = true;
  Logging::Get().Info(log::RENDER, "DirectLightingPass: initialised (RT pipeline + SBT ready)");
}

DirectLightingPass::~DirectLightingPass() = default;

nvrhi::ITexture* DirectLightingPass::EnsureOutputs(uint32_t width, uint32_t height) {
  if (width == 0u || height == 0u)
    return nullptr;
  if (_diffuse && _specular && _outputW == width && _outputH == height)
    return _diffuse;
  auto makeOutput = [&](const char* debugName) {
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
    desc.debugName = debugName;
    desc.initialState = nvrhi::ResourceStates::UnorderedAccess;
    desc.keepInitialState = true;
    return _device->createTexture(desc);
  };
  _diffuse = makeOutput("DirectLighting.gDirectDiffuse");
  _specular = makeOutput("DirectLighting.gDirectSpecular");
  _outputW = width;
  _outputH = height;
  _bindingSetCache.clear();
  if (!_diffuse || !_specular)
  {
    Logging::Get().Error(log::RENDER, "DirectLightingPass: createTexture(gDirectDiffuse/Specular, "
                                          + std::to_string(width) + "x" + std::to_string(height)
                                          + ") failed; pass will skip");
    return nullptr;
  }
  return _diffuse;
}

void DirectLightingPass::EnsureProjectionPipeline() {
  if (!_shadersOk || _scene == nullptr || !_scene->HasCamera())
    return;
  const std::size_t variant = (_scene->GetCamera().projectionMode == 1u) ? 1u : 0u;
  if (_pipelines[variant] || _variantBuildFailed[variant])
    return;
  PipelineVariant built =
      BuildPipelineVariant(_device, _sceneBindings->Layout(), _passLayout, _raygenShader,
                           _shadowMissShader, _anyHitShader, static_cast<uint32_t>(variant));
  if (!built.pipeline || !built.shaderTable)
  {
    _variantBuildFailed[variant] = true;
    return;
  }
  _pipelines[variant] = std::move(built.pipeline);
  _shaderTables[variant] = std::move(built.shaderTable);
}

nvrhi::BindingSetHandle DirectLightingPass::GetOrCreateBindingSet(nvrhi::IBuffer* visibility) {
  if (auto cached = _bindingSetCache.find(visibility); cached != _bindingSetCache.end())
    return cached->second;
  constexpr std::size_t MAX_CACHE_ENTRIES = 4;
  if (_bindingSetCache.size() >= MAX_CACHE_ENTRIES)
    _bindingSetCache.clear();

  nvrhi::BindingSetDesc setDesc;
  setDesc.bindings = {
      nvrhi::BindingSetItem::StructuredBuffer_SRV(0, visibility),
      nvrhi::BindingSetItem::Texture_UAV(1, _diffuse.Get()),
      nvrhi::BindingSetItem::Texture_UAV(2, _specular.Get()),
  };
  nvrhi::BindingSetHandle set = _device->createBindingSet(setDesc, _passLayout);
  _bindingSetCache[visibility] = set;
  return set;
}

void DirectLightingPass::Execute(nvrhi::ICommandList* commandList, const PassContext& context) {
  if (commandList == nullptr || context.targets == nullptr || context.settings == nullptr)
    return;
  // WP2-final — RealTimeQuality.passMask bit 0 gates this signal pass;
  // default 0x1F runs every pass (image-identical to before this gate).
  if ((context.settings->realTimeQuality.passMask & 0x01u) == 0u)
    return;
  nvrhi::IBuffer* const visibility = context.visibility;
  if (visibility == nullptr)
    return;
  if (!_shadersOk || !_diffuse || !_specular)
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

  commandList->setTextureState(_diffuse.Get(), nvrhi::AllSubresources,
                               nvrhi::ResourceStates::UnorderedAccess);
  commandList->setTextureState(_specular.Get(), nvrhi::AllSubresources,
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
