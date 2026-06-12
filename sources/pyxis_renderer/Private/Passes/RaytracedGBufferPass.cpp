// Pyxis renderer — visibility-buffer pass (P4 pass split).

#include "Passes/RaytracedGBufferPass.h"

#include "RenderGraph/PassContext.h"
#include "RenderGraph/ShaderLoad.h"
#include "Scene/SceneResources.h"  // RFC 0003 — renderer-internal scene view.

#include <Pyxis/Platform/FileSystem/AssetLocator.h>
#include <Pyxis/Platform/Logging/Log.h>
#include <Pyxis/Platform/Logging/LogCategories.h>
#include <Pyxis/Renderer/GpuScene.h>
#include <Pyxis/Renderer/Profiler.h>

// The dual-language ShaderInterop header is on the renderer's private
// include path (resources/shaders/). It declares CameraUniforms and
// VisibilityGpu for the C++ side — same definitions the shaders see,
// kept in lockstep by construction.
#include "ShaderInterop.slang"

#include <cstdint>
#include <hlsl++.h>

namespace pyxis {

namespace {

using shaderinterop::CameraUniforms;
static_assert(sizeof(shaderinterop::VisibilityGpu) == 32,
              "VisibilityGpu is 32 bytes (2 rows of 16) — EnsureVisibilityBuffer "
              "allocates width*height*32 with this exact stride; see "
              "resources/shaders/ShaderInterop.slang.");

// Thin wrapper over the shared LoadSpirvShader (RenderGraph/ShaderLoad.h) that
// pins the "RaytracedGBufferPass" log prefix so the call sites below stay terse.
nvrhi::ShaderHandle LoadSpirv(nvrhi::IDevice* device, std::string_view path,
                              nvrhi::ShaderType stage, const char* entry) noexcept {
  return LoadSpirvShader(device, path, stage, entry, "RaytracedGBufferPass");
}

}  // namespace

RaytracedGBufferPass::RaytracedGBufferPass(nvrhi::IDevice* device, GpuScene& scene)
    : _device(device), _scene(&scene) {
  const AssetLocator locator;
  const Path raygenPath = locator.LocateResource("shaders/raytraced_gbuffer_raygen.spv");
  const Path missPath = locator.LocateResource("shaders/raytraced_gbuffer_miss.spv");
  const Path closestHitPath =
      locator.LocateResource("shaders/raytraced_gbuffer_closesthit.spv");
  const Path anyHitPath = locator.LocateResource("shaders/raytraced_gbuffer_anyhit.spv");

  // Slang emits the SPIR-V `OpEntryPoint` name as `"main"` for every
  // [shader(...)]-attributed function regardless of the source-side
  // function name; passing anything else here trips
  // VUID-VkPipelineShaderStageCreateInfo-pName-00707.
  _raygenShader = LoadSpirv(_device, raygenPath.View(), nvrhi::ShaderType::RayGeneration, "main");
  _missShader = LoadSpirv(_device, missPath.View(), nvrhi::ShaderType::Miss, "main");
  _closestHitShader =
      LoadSpirv(_device, closestHitPath.View(), nvrhi::ShaderType::ClosestHit, "main");
  _anyHitShader = LoadSpirv(_device, anyHitPath.View(), nvrhi::ShaderType::AnyHit, "main");
  if (!_raygenShader || !_missShader || !_closestHitShader || !_anyHitShader)
  {
    Logging::Get().Error(log::RENDER, "RaytracedGBufferPass: shader load failed; pass will skip");
    return;
  }

  // Binding layout — visibility=AllRayTracing, bindingOffsets zeroed so
  // the layout's slot numbers map 1:1 onto the shaders'
  // `[[vk::binding(N, 0)]]` declarations (same convention as the
  // lighting pass; NVRHI's default per-type offsets would land the
  // items at 256/0/384 and trip
  // VUID-VkRayTracingPipelineCreateInfoKHR-layout-07988/07990).
  // gMaterials/gInstanceInfo keep the lighting pipeline's slot numbers
  // (3/4) so the shared anyhit module's declarations serve both
  // pipelines unchanged.
  nvrhi::BindingLayoutDesc layoutDesc;
  layoutDesc.visibility = nvrhi::ShaderType::AllRayTracing;
  layoutDesc.bindingOffsets.shaderResource = 0;
  layoutDesc.bindingOffsets.sampler = 0;
  layoutDesc.bindingOffsets.constantBuffer = 0;
  layoutDesc.bindingOffsets.unorderedAccess = 0;
  layoutDesc.bindings = {
      nvrhi::BindingLayoutItem::ConstantBuffer(0),         // binding 0 CameraUniforms
      nvrhi::BindingLayoutItem::RayTracingAccelStruct(1),  // binding 1 TLAS
      nvrhi::BindingLayoutItem::StructuredBuffer_UAV(2),   // binding 2 visibility buffer
      nvrhi::BindingLayoutItem::StructuredBuffer_SRV(3),   // binding 3 materials (anyhit)
      nvrhi::BindingLayoutItem::StructuredBuffer_SRV(4),   // binding 4 instance info (anyhit)
  };
  _bindingLayout = _device->createBindingLayout(layoutDesc);
  if (!_bindingLayout)
  {
    Logging::Get().Error(log::RENDER, "RaytracedGBufferPass: createBindingLayout failed");
    return;
  }

  // Pipeline state — three shader stages + one hit group bundling the
  // thin closesthit with the shared-alpha-test anyhit. maxRecursionDepth
  // = 1: this pipeline traces the primary ray only (no shading, no
  // secondary rays).
  nvrhi::rt::PipelineDesc pipelineDesc;
  pipelineDesc.shaders = {
      nvrhi::rt::PipelineShaderDesc{}.setExportName("RayGenMain").setShader(_raygenShader),
      nvrhi::rt::PipelineShaderDesc{}.setExportName("MissMain").setShader(_missShader),
  };
  pipelineDesc.hitGroups = {
      nvrhi::rt::PipelineHitGroupDesc{}
          .setExportName("HitGroupDefault")
          .setClosestHitShader(_closestHitShader)
          .setAnyHitShader(_anyHitShader),
  };
  pipelineDesc.globalBindingLayouts = {_bindingLayout};
  pipelineDesc.maxRecursionDepth = 1;
  _pipeline = _device->createRayTracingPipeline(pipelineDesc);
  if (!_pipeline)
  {
    Logging::Get().Error(log::RENDER, "RaytracedGBufferPass: createRayTracingPipeline failed");
    return;
  }

  // Shader binding table — one raygen, one miss, one hit group; static
  // for the whole run (rebuilt only by ReloadShaders).
  _shaderTable = _pipeline->createShaderTable();
  if (!_shaderTable)
  {
    Logging::Get().Error(log::RENDER, "RaytracedGBufferPass: createShaderTable failed");
    return;
  }
  _shaderTable->setRayGenerationShader("RayGenMain");
  _shaderTable->addMissShader("MissMain");  // miss-index 0: hitT = -1 sentinel
  _shaderTable->addHitGroup("HitGroupDefault");

  // Camera uniforms constant buffer — same shape + per-frame writeBuffer
  // as the lighting pass's (each pass owns its cbuffer; the uploaded
  // bytes are identical, so the two raygens read the same camera bits).
  nvrhi::BufferDesc cbDesc;
  cbDesc.byteSize = sizeof(CameraUniforms);
  cbDesc.debugName = "RaytracedGBuffer.CameraUniforms";
  cbDesc.isConstantBuffer = true;
  cbDesc.initialState = nvrhi::ResourceStates::ConstantBuffer;
  cbDesc.keepInitialState = true;
  _cameraUniformsBuffer = _device->createBuffer(cbDesc);
  if (!_cameraUniformsBuffer)
  {
    Logging::Get().Error(log::RENDER,
                         "RaytracedGBufferPass: createBuffer(CameraUniforms) failed");
    return;
  }

  // 1-element fallbacks for the anyhit's alpha-test chain (bindings
  // 3/4) while GpuScene hasn't allocated the real tables — same shape
  // as the lighting pass's per-stride fallbacks; contents written every
  // Execute while the matching scene buffer is null.
  auto makeStructuredFallback = [&](std::size_t stride, const char* debugName) {
    nvrhi::BufferDesc desc;
    desc.byteSize = stride;
    desc.structStride = stride;
    desc.canHaveRawViews = false;
    desc.canHaveTypedViews = false;
    desc.format = nvrhi::Format::UNKNOWN;
    desc.debugName = debugName;
    desc.initialState = nvrhi::ResourceStates::ShaderResource;
    desc.keepInitialState = true;
    return _device->createBuffer(desc);
  };
  _fallbackMaterialBuffer = makeStructuredFallback(sizeof(shaderinterop::OpenPBRMaterialGPU),
                                                   "RaytracedGBuffer.FallbackMaterial");
  _fallbackInstanceInfoBuffer = makeStructuredFallback(
      sizeof(shaderinterop::InstanceInfoGpu), "RaytracedGBuffer.FallbackInstanceInfo");
  if (!_fallbackMaterialBuffer || !_fallbackInstanceInfoBuffer)
  {
    Logging::Get().Error(log::RENDER,
                         "RaytracedGBufferPass: createBuffer(scene fallbacks) failed");
    return;
  }

  _shadersOk = true;
  Logging::Get().Info(log::RENDER,
                      "RaytracedGBufferPass: initialised (RT pipeline + SBT ready)");
}

RaytracedGBufferPass::~RaytracedGBufferPass() = default;

nvrhi::IBuffer* RaytracedGBufferPass::EnsureVisibilityBuffer(uint32_t width, uint32_t height) {
  if (width == 0u || height == 0u)
    return nullptr;
  if (_visibility && _visibilityW == width && _visibilityH == height)
    return _visibility;
  nvrhi::BufferDesc desc;
  desc.byteSize = static_cast<uint64_t>(width) * static_cast<uint64_t>(height)
                  * sizeof(shaderinterop::VisibilityGpu);
  desc.structStride = sizeof(shaderinterop::VisibilityGpu);
  desc.canHaveUAVs = true;  // raygen writes gVisibility (binding 2).
  desc.canHaveRawViews = false;
  desc.canHaveTypedViews = false;
  desc.format = nvrhi::Format::UNKNOWN;
  desc.debugName = "RaytracedGBuffer.visibility";
  desc.initialState = nvrhi::ResourceStates::UnorderedAccess;
  desc.keepInitialState = true;
  _visibility = _device->createBuffer(desc);
  _visibilityW = width;
  _visibilityH = height;
  return _visibility;
}

bool RaytracedGBufferPass::ReloadShaders() noexcept {
  // Editor-driven reload — same workflow + failure-handling contract as
  // RaytracedLightingPass::ReloadShaders: re-read the .spv currently on
  // disk, rebuild pipeline + SBT, keep the old handles on any failure.
  auto& log = Logging::Get();
  const AssetLocator locator;
  const Path raygenPath = locator.LocateResource("shaders/raytraced_gbuffer_raygen.spv");
  const Path missPath = locator.LocateResource("shaders/raytraced_gbuffer_miss.spv");
  const Path closestHitPath =
      locator.LocateResource("shaders/raytraced_gbuffer_closesthit.spv");
  const Path anyHitPath = locator.LocateResource("shaders/raytraced_gbuffer_anyhit.spv");

  nvrhi::ShaderHandle newRaygen =
      LoadSpirv(_device, raygenPath.View(), nvrhi::ShaderType::RayGeneration, "main");
  nvrhi::ShaderHandle newMiss =
      LoadSpirv(_device, missPath.View(), nvrhi::ShaderType::Miss, "main");
  nvrhi::ShaderHandle newClosestHit =
      LoadSpirv(_device, closestHitPath.View(), nvrhi::ShaderType::ClosestHit, "main");
  nvrhi::ShaderHandle newAnyHit =
      LoadSpirv(_device, anyHitPath.View(), nvrhi::ShaderType::AnyHit, "main");
  if (!newRaygen || !newMiss || !newClosestHit || !newAnyHit)
  {
    log.Error(log::RENDER,
              "RaytracedGBufferPass::ReloadShaders: shader load failed; keeping old pipeline");
    return false;
  }

  nvrhi::rt::PipelineDesc pipelineDesc;
  pipelineDesc.shaders = {
      nvrhi::rt::PipelineShaderDesc{}.setExportName("RayGenMain").setShader(newRaygen),
      nvrhi::rt::PipelineShaderDesc{}.setExportName("MissMain").setShader(newMiss),
  };
  pipelineDesc.hitGroups = {
      nvrhi::rt::PipelineHitGroupDesc{}
          .setExportName("HitGroupDefault")
          .setClosestHitShader(newClosestHit)
          .setAnyHitShader(newAnyHit),
  };
  pipelineDesc.globalBindingLayouts = {_bindingLayout};
  pipelineDesc.maxRecursionDepth = 1;
  nvrhi::rt::PipelineHandle newPipeline = _device->createRayTracingPipeline(pipelineDesc);
  if (!newPipeline)
  {
    log.Error(log::RENDER,
              "RaytracedGBufferPass::ReloadShaders: createRayTracingPipeline failed; "
              "keeping old pipeline");
    return false;
  }

  nvrhi::rt::ShaderTableHandle newShaderTable = newPipeline->createShaderTable();
  if (!newShaderTable)
  {
    log.Error(log::RENDER,
              "RaytracedGBufferPass::ReloadShaders: createShaderTable failed; "
              "keeping old pipeline");
    return false;
  }
  newShaderTable->setRayGenerationShader("RayGenMain");
  newShaderTable->addMissShader("MissMain");
  newShaderTable->addHitGroup("HitGroupDefault");

  // Atomic-ish swap — render thread only, same contract as the
  // lighting pass: once all handles flip together the next Execute
  // picks up the new pipeline + table.
  _raygenShader     = std::move(newRaygen);
  _missShader       = std::move(newMiss);
  _closestHitShader = std::move(newClosestHit);
  _anyHitShader     = std::move(newAnyHit);
  _pipeline         = std::move(newPipeline);
  _shaderTable      = std::move(newShaderTable);
  _shadersOk = true;
  log.Info(log::RENDER, "RaytracedGBufferPass::ReloadShaders: reload OK");
  return true;
}

nvrhi::BindingSetHandle RaytracedGBufferPass::GetOrCreateBindingSet(
    nvrhi::IBuffer* visibility, const SceneResources& res) {
  // Snapshot the borrowed scene pointers; any flip (lazy material /
  // instance allocation, TLAS rebuild) invalidates every cached set —
  // same scheme as the lighting pass's BindingsSnapshot.
  auto slot = [](BindingSlot index) constexpr noexcept { return static_cast<std::size_t>(index); };
  BindingsSnapshot current{};
  current[slot(BindingSlot::Tlas)]         = res.tlas;
  current[slot(BindingSlot::Materials)]    = res.materialBuffer;
  current[slot(BindingSlot::InstanceInfo)] = res.instanceInfoBuffer;
  if (current != _lastBindings)
  {
    _bindingSetCache.clear();
    _lastBindings = current;
  }

  if (auto cached = _bindingSetCache.find(visibility); cached != _bindingSetCache.end())
  {
    return cached->second;
  }
  constexpr std::size_t MAX_CACHE_ENTRIES = 6;
  if (_bindingSetCache.size() >= MAX_CACHE_ENTRIES)
  {
    _bindingSetCache.clear();
  }

  // Scene buffers fall back to the 1-element scratch records when the
  // scene hasn't allocated yet (same defaults the lighting pass binds —
  // a rogue InstanceID() resolves to the sentinel grey material with no
  // ALPHA_TESTED bit, so the anyhit accepts the hit exactly as before).
  nvrhi::IBuffer* materialBuffer = res.materialBuffer;
  if (materialBuffer == nullptr)
    materialBuffer = _fallbackMaterialBuffer.Get();
  nvrhi::IBuffer* instanceInfoBuffer = res.instanceInfoBuffer;
  if (instanceInfoBuffer == nullptr)
    instanceInfoBuffer = _fallbackInstanceInfoBuffer.Get();

  nvrhi::BindingSetDesc setDesc;
  setDesc.bindings = {
      nvrhi::BindingSetItem::ConstantBuffer(0, _cameraUniformsBuffer),
      nvrhi::BindingSetItem::RayTracingAccelStruct(1, res.tlas),
      nvrhi::BindingSetItem::StructuredBuffer_UAV(2, visibility),
      nvrhi::BindingSetItem::StructuredBuffer_SRV(3, materialBuffer),
      nvrhi::BindingSetItem::StructuredBuffer_SRV(4, instanceInfoBuffer),
  };
  nvrhi::BindingSetHandle set = _device->createBindingSet(setDesc, _bindingLayout);
  _bindingSetCache[visibility] = set;
  return set;
}

void RaytracedGBufferPass::Execute(nvrhi::ICommandList* commandList,
                                   const PassContext& context) {
  if (!_shadersOk)
    return;
  if (commandList == nullptr || context.targets == nullptr)
    return;
  // P4 — the visibility buffer this pass owns, threaded back through the
  // context by PyxisRenderer (null when no display target is bound — the
  // same "nothing to render into" early-out the lighting pass takes).
  // The identity check pairs the buffer with the cached dims the
  // dispatch below uses; PyxisRenderer is the only producer, so a
  // mismatch only happens on a stale context.
  nvrhi::IBuffer* const visibility = context.visibility;
  if (visibility == nullptr || visibility != _visibility.Get())
    return;

  // RFC 0003 — snapshot the scene's borrowed NVRHI resources once per Execute.
  // Safe here: single-writer §31 — CommitResources ran earlier this frame on
  // this thread.
  const SceneResources res = detail::SceneResourcesAccess::Get(*_scene);

  // Need a TLAS + camera before we can trace anything — same degenerate
  // "scene with no instances or camera" early-out as the lighting pass
  // (which leaves the visibility buffer untouched; the lighting pass
  // early-outs on the same gates so the stale records are never read).
  if (res.tlas == nullptr || !_scene->HasCamera())
    return;

  const Profiler::GpuScope gpuScope(*context.profiler, commandList, "pass.RaytracedGBuffer");

  // ---- Upload camera uniforms ----------------------------------------
  // Identical derivation to the lighting pass's upload (the same
  // CameraDesc inputs produce the same bytes, so BuildCameraRay reads
  // bit-identical matrices in both raygens). The GBuffer raygen only
  // consumes worldFromView / viewFromClip / projectionMode; the rest
  // rides along to keep this code a mirror of the lighting pass's.
  const CameraDesc& camera = _scene->GetCamera();
  CameraUniforms cameraUniforms{};
  cameraUniforms.worldFromView = hlslpp::inverse(camera.viewFromWorld);
  cameraUniforms.viewFromClip = hlslpp::inverse(camera.projFromView);
  cameraUniforms.viewFromWorld = camera.viewFromWorld;
  cameraUniforms.exposure  = camera.exposure;
  // pixelSpreadRadians: same expression as the lighting pass, with the
  // dispatch height standing in for the output texture's height (they
  // are the same value — both are sized off targets->color).
  const auto outputHeight = static_cast<float>(_visibilityH);
  float projRow1[16];
  hlslpp::store(projRow1, camera.projFromView);
  const float projYY = projRow1[5];  // row 1, col 1 in row-major float[16]
  const float tanHalfFov = (projYY > 1e-6f) ? (1.0f / projYY) : 0.0f;
  cameraUniforms.pixelSpreadRadians = (outputHeight > 0.0f && tanHalfFov > 0.0f)
                                          ? (2.0f * tanHalfFov / outputHeight)
                                          : 0.0f;
  cameraUniforms.projectionMode = camera.projectionMode;
  cameraUniforms._camPad1 = 0.0f;
  commandList->writeBuffer(_cameraUniformsBuffer.Get(), &cameraUniforms, sizeof(cameraUniforms));

  // ---- Scene-buffer fallback uploads --------------------------------
  // Same defaults the lighting pass writes (grey material / zero
  // instance record) while the scene buffers are null.
  static const shaderinterop::OpenPBRMaterialGPU FALLBACK_MATERIAL_GREY = []() {
    shaderinterop::OpenPBRMaterialGPU material{};
    material.baseColorR = 0.8f;
    material.baseColorG = 0.8f;
    material.baseColorB = 0.8f;
    material.flags = 0u;
    material.baseColorTex = shaderinterop::INVALID_BINDLESS_TEXTURE;
    material.normalTex = shaderinterop::INVALID_BINDLESS_TEXTURE;
    material.metallicTex = shaderinterop::INVALID_BINDLESS_TEXTURE;
    material.roughnessTex = shaderinterop::INVALID_BINDLESS_TEXTURE;
    material.roughness = 0.5f;
    material.metalness = 0.0f;
    material.opacity = 1.0f;
    material.specularIor = 1.5f;
    material.coatWeight = 0.0f;
    material.coatRoughness = 0.0f;
    material.emissionLuminance = 0.0f;
    material.emissionTex = shaderinterop::INVALID_BINDLESS_TEXTURE;
    material.opacityTex = shaderinterop::INVALID_BINDLESS_TEXTURE;
    material.transmissionTex = shaderinterop::INVALID_BINDLESS_TEXTURE;
    material.coatRoughnessTex = shaderinterop::INVALID_BINDLESS_TEXTURE;
    return material;
  }();
  static const shaderinterop::InstanceInfoGpu FALLBACK_INSTANCE_INFO_ZERO{};
  if (res.materialBuffer == nullptr)
    commandList->writeBuffer(_fallbackMaterialBuffer.Get(), &FALLBACK_MATERIAL_GREY,
                             sizeof(FALLBACK_MATERIAL_GREY));
  if (res.instanceInfoBuffer == nullptr)
    commandList->writeBuffer(_fallbackInstanceInfoBuffer.Get(), &FALLBACK_INSTANCE_INFO_ZERO,
                             sizeof(FALLBACK_INSTANCE_INFO_ZERO));

  // ---- Bind + dispatch ----------------------------------------------
  const nvrhi::BindingSetHandle bindingSet = GetOrCreateBindingSet(visibility, res);
  if (!bindingSet)
    return;

  // The visibility buffer must be in UnorderedAccess for the raygen's
  // structured-buffer writes; the lighting pass transitions it to
  // ShaderResource when it reads the records back (explicit cross-pass
  // barrier — same finding as SsaaResolvePass).
  commandList->setBufferState(visibility, nvrhi::ResourceStates::UnorderedAccess);
  commandList->commitBarriers();

  nvrhi::rt::State state;
  state.shaderTable = _shaderTable;
  state.bindings = {bindingSet};
  commandList->setRayTracingState(state);

  // Dispatch dims = the visibility buffer's dims (cached at
  // EnsureVisibilityBuffer) = the display target's dims — identical
  // extents to the lighting dispatch so `vis[y*width+x]` lines up.
  nvrhi::rt::DispatchRaysArguments args;
  args.width = _visibilityW;
  args.height = _visibilityH;
  args.depth = 1;
  commandList->dispatchRays(args);
}

}  // namespace pyxis
