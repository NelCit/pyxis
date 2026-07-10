// Pyxis renderer — CompositePass implementation (RTX-alignment design,
// WP2-final).

#include "Passes/CompositePass.h"

#include "Passes/DenoiseAoPass.h"
#include "Passes/DenoiseAtrousPass.h"
#include "Passes/DenoiseShadowPass.h"
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
  return LoadSpirvShader(device, path, stage, entry, "CompositePass");
}

// One compute pipeline per camera projection mode — the shader's
// PROJECTION_MODE specialization constant (camera_ray.slang, shared with
// every RT pass) drives the BuildCameraRay reconstruction this pass uses
// ONLY for miss-pixel dome backgrounds. Mirrors every other pass's
// BuildPipelineVariant shape.
nvrhi::ComputePipelineHandle BuildPipeline(nvrhi::IDevice* device,
                                          nvrhi::IBindingLayout* sceneLayout,
                                          nvrhi::IBindingLayout* passLayout,
                                          nvrhi::IShader* shader, uint32_t projectionMode,
                                          const char* logContext) noexcept {
  auto& log = Logging::Get();
  const nvrhi::ShaderSpecialization constants[] = {
      nvrhi::ShaderSpecialization::UInt32(shaderinterop::SPEC_ID_PROJECTION_MODE, projectionMode),
  };
  const nvrhi::ShaderHandle specShader = device->createShaderSpecialization(
      shader, constants, static_cast<uint32_t>(std::size(constants)));
  if (!specShader)
  {
    log.Error(log::RENDER, std::string{logContext}
                               + ": createShaderSpecialization failed (projectionMode="
                               + std::to_string(projectionMode) + ")");
    return nullptr;
  }
  nvrhi::ComputePipelineDesc pipelineDesc;
  pipelineDesc.CS = specShader;
  // Set 0 (shared SceneBindings) then this pass's OWN Set 1 — same order
  // every RT pass uses, now on a compute pipeline (SceneBindings' layout
  // visibility mask includes Compute — see SceneBindings.h).
  pipelineDesc.bindingLayouts = {sceneLayout, passLayout};
  nvrhi::ComputePipelineHandle pipeline = device->createComputePipeline(pipelineDesc);
  if (!pipeline)
  {
    log.Error(log::RENDER, std::string{logContext}
                               + ": createComputePipeline failed (projectionMode="
                               + std::to_string(projectionMode) + ")");
    return nullptr;
  }
  return pipeline;
}

}  // namespace

CompositePass::CompositePass(nvrhi::IDevice* device, GpuScene& scene, SceneBindings& sceneBindings,
                             DenoiseShadowPass* denoiseShadowPass,
                             DenoiseAtrousPass* denoiseAtrousPass,
                             DenoiseAoPass* denoiseAoPass)
    : _device(device),
      _scene(&scene),
      _sceneBindings(&sceneBindings),
      _denoiseShadowPass(denoiseShadowPass),
      _denoiseAtrousPass(denoiseAtrousPass),
      _denoiseAoPass(denoiseAoPass) {
  if (!_sceneBindings->IsOperational())
  {
    Logging::Get().Error(log::RENDER,
                         "CompositePass: SceneBindings not operational; pass will skip");
    return;
  }

  const AssetLocator locator;
  const Path shaderPath = locator.LocateResource("shaders/composite.spv");
  _shader = LoadSpirv(_device, shaderPath.View(), nvrhi::ShaderType::Compute, "main");
  if (!_shader)
  {
    Logging::Get().Error(log::RENDER, "CompositePass: shader load failed; pass will skip");
    return;
  }

  // Set-1 layout — this pass's OWN I/O (Set 0 is the shared SceneBindings
  // layout). bindingOffsets zeroed so the slot numbers map 1:1 onto
  // composite.slang's `[[vk::binding(N, 1)]]` declarations.
  nvrhi::BindingLayoutDesc layoutDesc;
  layoutDesc.visibility = nvrhi::ShaderType::Compute;
  layoutDesc.bindingOffsets.shaderResource = 0;
  layoutDesc.bindingOffsets.sampler = 0;
  layoutDesc.bindingOffsets.constantBuffer = 0;
  layoutDesc.bindingOffsets.unorderedAccess = 0;
  layoutDesc.bindings = {
      nvrhi::BindingLayoutItem::StructuredBuffer_SRV(0),  // 0  visibility (packed 16 B)
      nvrhi::BindingLayoutItem::Texture_SRV(1),           // 1  gAlbedo
      nvrhi::BindingLayoutItem::Texture_SRV(2),           // 2  gEmissive
      nvrhi::BindingLayoutItem::Texture_SRV(3),           // 3  gAo
      nvrhi::BindingLayoutItem::Texture_SRV(4),           // 4  gDirectDiffuse
      nvrhi::BindingLayoutItem::Texture_SRV(5),           // 5  gDirectSpecular
      nvrhi::BindingLayoutItem::Texture_SRV(6),           // 6  gIndirectDiffuse
      nvrhi::BindingLayoutItem::Texture_SRV(7),           // 7  gReflections
      nvrhi::BindingLayoutItem::Texture_SRV(8),           // 8  gReflectionWeight
      nvrhi::BindingLayoutItem::Texture_SRV(9),           // 9  gTranslucency
      nvrhi::BindingLayoutItem::Texture_UAV(10),          // 10 gLinearColor (RGBA32F)
      nvrhi::BindingLayoutItem::Texture_UAV(11),          // 11 gOutColorHdr (RGBA16F)
      nvrhi::BindingLayoutItem::Texture_UAV(12),          // 12 gOutAlpha (R8_UNORM)
  };
  _passLayout = _device->createBindingLayout(layoutDesc);
  if (!_passLayout)
  {
    Logging::Get().Error(log::RENDER, "CompositePass: createBindingLayout(Set 1) failed");
    return;
  }

  // Perspective (the v1 default) built eagerly; orthographic materializes
  // lazily in EnsureProjectionPipeline the first frame it's requested —
  // same convention as every other multi-variant pass.
  nvrhi::ComputePipelineHandle perspective = BuildPipeline(
      _device, _sceneBindings->Layout(), _passLayout, _shader, 0u, "CompositePass");
  if (!perspective)
    return;
  _pipelines[0] = std::move(perspective);

  // ---- Tiny no-UAV fallbacks for the two AOVs this pass owns (moved
  // here from RaytracedLightingPass) --------------------------------
  auto makeAovFallback = [&](nvrhi::Format fmt, const char* dbgName) -> nvrhi::TextureHandle {
    nvrhi::TextureDesc fbDesc;
    fbDesc.format = fmt;
    fbDesc.width = 1;
    fbDesc.height = 1;
    fbDesc.dimension = nvrhi::TextureDimension::Texture2D;
    fbDesc.isUAV = true;
    fbDesc.debugName = dbgName;
    fbDesc.initialState = nvrhi::ResourceStates::UnorderedAccess;
    fbDesc.keepInitialState = true;
    return _device->createTexture(fbDesc);
  };
  _fallbackColorHdrAov = makeAovFallback(nvrhi::Format::RGBA16_FLOAT, "Composite.FbColorHdrAov");
  _fallbackAlphaAov = makeAovFallback(nvrhi::Format::R8_UNORM, "Composite.FbAlphaAov");
  if (!_fallbackColorHdrAov || !_fallbackAlphaAov)
  {
    Logging::Get().Error(log::RENDER, "CompositePass: AOV fallback create failed");
    return;
  }

  _shadersOk = true;
  Logging::Get().Info(log::RENDER, "CompositePass: initialised (compute recombination ready)");
}

CompositePass::~CompositePass() = default;

bool CompositePass::ReloadShaders() noexcept {
  auto& log = Logging::Get();
  const AssetLocator locator;
  const Path shaderPath = locator.LocateResource("shaders/composite.spv");
  nvrhi::ShaderHandle newShader = LoadSpirv(_device, shaderPath.View(), nvrhi::ShaderType::Compute, "main");
  if (!newShader)
  {
    log.Error(log::RENDER, "CompositePass::ReloadShaders: shader load failed; keeping old pipeline");
    return false;
  }

  nvrhi::ComputePipelineHandle newPerspective = BuildPipeline(
      _device, _sceneBindings->Layout(), _passLayout, newShader, 0u,
      "CompositePass::ReloadShaders");
  if (!newPerspective)
  {
    log.Error(log::RENDER,
              "CompositePass::ReloadShaders: perspective variant rebuild failed; keeping old pipeline");
    return false;
  }
  nvrhi::ComputePipelineHandle newOrthographic;
  if (_pipelines[1])
  {
    newOrthographic = BuildPipeline(_device, _sceneBindings->Layout(), _passLayout, newShader, 1u,
                                    "CompositePass::ReloadShaders");
    if (!newOrthographic)
    {
      log.Error(log::RENDER, "CompositePass::ReloadShaders: orthographic variant rebuild failed; "
                                "keeping old pipeline");
      return false;
    }
  }

  _shader = std::move(newShader);
  _pipelines[0] = std::move(newPerspective);
  _pipelines[1] = std::move(newOrthographic);
  _variantBuildFailed = {};
  _shadersOk = true;
  log.Info(log::RENDER, "CompositePass::ReloadShaders: reload OK");
  return true;
}

nvrhi::ITexture* CompositePass::EnsureLinearColor(uint32_t width, uint32_t height) {
  if (width == 0u || height == 0u)
    return nullptr;
  if (_linearColor && _linearColorW == width && _linearColorH == height)
    return _linearColor;
  nvrhi::TextureDesc desc;
  desc.width = width;
  desc.height = height;
  // RGBA32F — full fp32 so the display transform reads the exact bits the
  // old in-kernel megakernel payload.color carried (RGBA16F would quantize
  // the display path; the colorHdr AOV stays the quantized inspector copy).
  desc.format = nvrhi::Format::RGBA32_FLOAT;
  desc.dimension = nvrhi::TextureDimension::Texture2D;
  desc.isUAV = true;             // this pass writes gLinearColor (Set 1 binding 10).
  desc.isShaderResource = true;  // sampled by TonemapPass / AutoExposurePass.
  desc.debugName = "Composite.linearColor";
  desc.initialState = nvrhi::ResourceStates::UnorderedAccess;
  desc.keepInitialState = true;
  _linearColor = _device->createTexture(desc);
  _linearColorW = width;
  _linearColorH = height;
  _bindingSetCache.clear();
  if (!_linearColor)
  {
    if (!_linearColorCreateFailedLogged)
    {
      Logging::Get().Error(log::RENDER,
                           "CompositePass: createTexture(linearColor, " + std::to_string(width)
                               + "x" + std::to_string(height)
                               + " RGBA32F) failed; composite/tonemap will skip");
      _linearColorCreateFailedLogged = true;
    }
    return nullptr;
  }
  _linearColorCreateFailedLogged = false;
  return _linearColor;
}

void CompositePass::EnsureProjectionPipeline() {
  if (!_shadersOk || _scene == nullptr || !_scene->HasCamera())
    return;
  const std::size_t variant = (_scene->GetCamera().projectionMode == 1u) ? 1u : 0u;
  if (_pipelines[variant] || _variantBuildFailed[variant])
    return;
  nvrhi::ComputePipelineHandle built =
      BuildPipeline(_device, _sceneBindings->Layout(), _passLayout, _shader,
                    static_cast<uint32_t>(variant), "CompositePass::EnsureProjectionPipeline");
  if (!built)
  {
    _variantBuildFailed[variant] = true;
    return;
  }
  _pipelines[variant] = std::move(built);
}

nvrhi::BindingSetHandle CompositePass::GetOrCreateBindingSet(
    nvrhi::IBuffer* visibility, nvrhi::ITexture* albedo, nvrhi::ITexture* emissive,
    nvrhi::ITexture* ambientOcclusion, nvrhi::ITexture* directDiffuse,
    nvrhi::ITexture* directSpecular, nvrhi::ITexture* indirectDiffuse,
    nvrhi::ITexture* reflections, nvrhi::ITexture* reflectionWeight,
    nvrhi::ITexture* translucency, nvrhi::ITexture* linearColor, nvrhi::ITexture* colorHdr,
    nvrhi::ITexture* alphaAov) {
  // FNV1a-64 over every borrowed pointer this set references — same
  // identity scheme TonemapPass uses (too many bound resources for a
  // fixed-size snapshot array to stay readable).
  uint64_t key = 1469598103934665603ull;
  auto mix = [&key](const void* ptr) {
    auto bits = reinterpret_cast<std::uintptr_t>(ptr);
    for (int byte = 0; byte < 8; ++byte)
    {
      key ^= static_cast<uint64_t>(bits & 0xFFu);
      key *= 1099511628211ull;
      bits >>= 8;
    }
  };
  mix(visibility);
  mix(albedo);
  mix(emissive);
  mix(ambientOcclusion);
  mix(directDiffuse);
  mix(directSpecular);
  mix(indirectDiffuse);
  mix(reflections);
  mix(reflectionWeight);
  mix(translucency);
  mix(linearColor);
  mix(colorHdr);
  mix(alphaAov);

  if (auto cached = _bindingSetCache.find(key); cached != _bindingSetCache.end())
    return cached->second;

  nvrhi::BindingSetDesc setDesc;
  setDesc.bindings = {
      nvrhi::BindingSetItem::StructuredBuffer_SRV(0, visibility),
      nvrhi::BindingSetItem::Texture_SRV(1, albedo),
      nvrhi::BindingSetItem::Texture_SRV(2, emissive),
      nvrhi::BindingSetItem::Texture_SRV(3, ambientOcclusion),
      nvrhi::BindingSetItem::Texture_SRV(4, directDiffuse),
      nvrhi::BindingSetItem::Texture_SRV(5, directSpecular),
      nvrhi::BindingSetItem::Texture_SRV(6, indirectDiffuse),
      nvrhi::BindingSetItem::Texture_SRV(7, reflections),
      nvrhi::BindingSetItem::Texture_SRV(8, reflectionWeight),
      nvrhi::BindingSetItem::Texture_SRV(9, translucency),
      nvrhi::BindingSetItem::Texture_UAV(10, linearColor),
      nvrhi::BindingSetItem::Texture_UAV(11, colorHdr),
      nvrhi::BindingSetItem::Texture_UAV(12, alphaAov),
  };
  nvrhi::BindingSetHandle set = _device->createBindingSet(setDesc, _passLayout);
  // Bounded cache — bound resources churn on resize / AOV re-point;
  // clear-on-overflow keeps stale dangling pointers from accumulating.
  constexpr std::size_t MAX_CACHE_ENTRIES = 6;
  if (_bindingSetCache.size() >= MAX_CACHE_ENTRIES)
    _bindingSetCache.clear();
  _bindingSetCache.emplace(key, set);
  return set;
}

void CompositePass::Execute(nvrhi::ICommandList* commandList, const PassContext& context) {
  if (!_shadersOk || commandList == nullptr || context.targets == nullptr)
    return;

  nvrhi::ITexture* const linearColor = context.linearColor;
  if (linearColor == nullptr)
    return;
  nvrhi::IBuffer* const visibility = context.visibility;
  if (visibility == nullptr)
    return;
  if (context.sceneBindingSet == nullptr)
    return;
  // The five signal passes' outputs + the G-buffer guides — all
  // required; a missing one means an upstream pass isn't operational
  // this frame (mirrors the pre-composite behaviour of the whole chain
  // degrading to "nothing new displayed").
  nvrhi::ITexture* const albedo = context.gAlbedo;
  nvrhi::ITexture* const emissive = context.gEmissive;
  if (albedo == nullptr || emissive == nullptr)
    return;
  nvrhi::ITexture* ambientOcclusion = context.gAo;
  // RTX-alignment design (rtx-realtime-alignment-design.md), Phase B —
  // when the denoiser chain is enabled, read the DENOISED diffuse/
  // specular signals instead of the raw (noisy) signal-pass outputs
  // PassContext carries. PassContext's own gDirectDiffuse/gDirectSpecular/
  // gIndirectDiffuse/gReflections stay the raw signals throughout (the
  // denoiser chain's OWN inputs) — see PyxisRenderer::RenderFrame's doc
  // comment for why they can't double as "the composited-from" source.
  // Falls back to raw whenever the denoiser pointer is null (chain not
  // constructed) or its output isn't ready yet (defensive; shouldn't
  // happen once EnsureOutputs has run once).
  const bool denoiseEnabled = context.settings != nullptr
                            && (context.settings->realTimeQuality.passMask
                                & shaderinterop::PASS_MASK_DENOISE)
                                   != 0u;
  nvrhi::ITexture* directDiffuse = context.gDirectDiffuse;
  nvrhi::ITexture* directSpecular = context.gDirectSpecular;
  nvrhi::ITexture* indirectDiffuse = context.gIndirectDiffuse;
  nvrhi::ITexture* reflections = context.gReflections;
  if (denoiseEnabled) {
    if (_denoiseShadowPass != nullptr) {
      if (nvrhi::ITexture* const denoised = _denoiseShadowPass->Diffuse())
        directDiffuse = denoised;
      if (nvrhi::ITexture* const denoised = _denoiseShadowPass->Specular())
        directSpecular = denoised;
    }
    if (_denoiseAtrousPass != nullptr) {
      if (nvrhi::ITexture* const denoised = _denoiseAtrousPass->Diffuse())
        indirectDiffuse = denoised;
      if (nvrhi::ITexture* const denoised = _denoiseAtrousPass->Specular())
        reflections = denoised;
    }
    // NRD stage 3 (RTX-alignment 2026-07-10): when NrdDenoisePass ran this
    // frame (PYXIS_WITH_NRD builds, effective denoiser == Nrd) it publishes
    // its denoised diffuse/specular through the shared context — prefer
    // them over the builtin à-trous outputs above. Null in every other
    // configuration, so the builtin path is untouched byte-for-byte.
    if (context.nrdDenoisedDiffuse != nullptr)
      indirectDiffuse = context.nrdDenoisedDiffuse;
    if (context.nrdDenoisedSpecular != nullptr)
      reflections = context.nrdDenoisedSpecular;
    // Noise-floor + vegetation spec (rtx-realtime-alignment-design.md,
    // 2026-07-06), work item 1 — same raw-vs-denoised swap for AO.
    if (_denoiseAoPass != nullptr) {
      if (nvrhi::ITexture* const denoised = _denoiseAoPass->Output())
        ambientOcclusion = denoised;
    }
  }
  nvrhi::ITexture* const reflectionWeight = context.gReflectionWeight;
  nvrhi::ITexture* const translucency = context.gTranslucency;
  if (ambientOcclusion == nullptr || directDiffuse == nullptr || directSpecular == nullptr
      || indirectDiffuse == nullptr || reflections == nullptr || reflectionWeight == nullptr
      || translucency == nullptr)
    return;

  const SceneResources res = detail::SceneResourcesAccess::Get(*_scene);
  if (res.tlas == nullptr || !_scene->HasCamera())
    return;

  const CameraDesc& camera = _scene->GetCamera();
  const std::size_t projectionVariant = (camera.projectionMode == 1u) ? 1u : 0u;
  nvrhi::IComputePipeline* const pipeline = _pipelines[projectionVariant];
  if (pipeline == nullptr)
    return;

  nvrhi::ITexture* colorHdrAov = context.targets->colorHdr;
  if (colorHdrAov == nullptr)
    colorHdrAov = _fallbackColorHdrAov.Get();
  nvrhi::ITexture* alphaAov = context.targets->alphaAov;
  if (alphaAov == nullptr)
    alphaAov = _fallbackAlphaAov.Get();

  const nvrhi::BindingSetHandle bindingSet = GetOrCreateBindingSet(
      visibility, albedo, emissive, ambientOcclusion, directDiffuse, directSpecular,
      indirectDiffuse, reflections, reflectionWeight, translucency, linearColor, colorHdrAov,
      alphaAov);
  if (!bindingSet)
    return;

  // Explicit barriers — every input was UAV-written by an earlier pass in
  // this same command list and must be readable as shader resources; the
  // outputs must be in UnorderedAccess for this compute write. The
  // cross-pass UAV->SRV hazard needs the explicit transition to be
  // reliable (same finding as every other pass in this graph).
  commandList->setBufferState(visibility, nvrhi::ResourceStates::ShaderResource);
  nvrhi::ITexture* const srvInputs[] = {
      albedo, emissive, ambientOcclusion, directDiffuse, directSpecular, indirectDiffuse,
      reflections, reflectionWeight, translucency,
  };
  for (nvrhi::ITexture* const tex : srvInputs)
    commandList->setTextureState(tex, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
  commandList->setTextureState(linearColor, nvrhi::AllSubresources,
                               nvrhi::ResourceStates::UnorderedAccess);
  commandList->setTextureState(colorHdrAov, nvrhi::AllSubresources,
                               nvrhi::ResourceStates::UnorderedAccess);
  commandList->setTextureState(alphaAov, nvrhi::AllSubresources,
                               nvrhi::ResourceStates::UnorderedAccess);
  commandList->commitBarriers();

  nvrhi::ComputeState state;
  state.pipeline = pipeline;
  // Set 0 (shared SceneBindings, for the camera + lights + dome the
  // miss-pixel background needs) then this pass's OWN Set 1.
  state.bindings = {context.sceneBindingSet, bindingSet};
  commandList->setComputeState(state);

  const nvrhi::TextureDesc& outDesc = linearColor->getDesc();
  const uint32_t groupsX = (outDesc.width + 7u) / 8u;
  const uint32_t groupsY = (outDesc.height + 7u) / 8u;
  commandList->dispatch(groupsX, groupsY, 1u);
}

}  // namespace pyxis
