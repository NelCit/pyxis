// Pyxis renderer — primary-ray path-trace pass.

#include "Passes/PathTracePass.h"

#include "RenderGraph/PassContext.h"

#include <Pyxis/Platform/FileSystem/AssetLocator.h>
#include <Pyxis/Platform/Logging/Log.h>
#include <Pyxis/Platform/Logging/LogCategories.h>
#include <Pyxis/Renderer/GpuScene.h>
#include <Pyxis/Renderer/Profiler.h>

// The dual-language ShaderInterop header is on the renderer's
// private include path (resources/shaders/). It declares
// `pyxis::shaderinterop::CameraUniforms` (and `HitInfo`) for the
// C++ side and the same structs at file scope for slangc — same
// definitions, kept in lockstep by construction.
#include "ShaderInterop.slang"

#include <cstdint>
#include <cstring>
#include <fstream>
#include <hlsl++.h>
#include <ios>
#include <string>
#include <vector>

namespace pyxis {

namespace {

// CameraUniforms comes straight from the shared ShaderInterop header,
// keeping the C++ side and the SPIR-V side guaranteed-in-lockstep by
// construction. The static_assert remains as a tripwire: if someone
// adds a field to the shader-side struct without bumping the cbuffer
// size on the C++ allocator below, the build fails here rather than
// at runtime via a confusing validation error.
using shaderinterop::CameraUniforms;
static_assert(sizeof(CameraUniforms) == 144,
              "CameraUniforms is two float4x4s + 16-byte UI tail (mousePixel + "
              "debugViewMode + pad). See resources/shaders/ShaderInterop.slang.");

std::vector<char> ReadBinaryFile(std::string_view path) noexcept {
  std::ifstream stream(std::string{path}, std::ios::binary | std::ios::ate);
  if (!stream.is_open())
    return {};
  const auto fileSize = stream.tellg();
  if (fileSize <= 0)
    return {};
  std::vector<char> bytes(static_cast<std::size_t>(fileSize));
  stream.seekg(0, std::ios::beg);
  stream.read(bytes.data(), fileSize);
  return bytes;
}

nvrhi::ShaderHandle LoadSpirv(nvrhi::IDevice* device, std::string_view path,
                              nvrhi::ShaderType stage, const char* entry) noexcept {
  auto& log = Logging::Get();
  auto bytes = ReadBinaryFile(path);
  if (bytes.empty())
  {
    std::string msg = "PathTracePass: failed to load shader ";
    msg.append(path);
    log.Error(log::RENDER, msg);
    return nullptr;
  }
  nvrhi::ShaderDesc shaderDesc{};
  shaderDesc.shaderType = stage;
  shaderDesc.entryName = entry;
  shaderDesc.debugName = std::string{path};
  return device->createShader(shaderDesc, bytes.data(), bytes.size());
}

}  // namespace

PathTracePass::PathTracePass(nvrhi::IDevice* device, GpuScene& scene)
    : _device(device), _scene(&scene) {
  const AssetLocator locator;
  const Path raygenPath = locator.LocateResource("shaders/raygen.spv");
  const Path missPath = locator.LocateResource("shaders/miss.spv");
  const Path closestHitPath = locator.LocateResource("shaders/closesthit.spv");

  // Slang emits the SPIR-V `OpEntryPoint` name as `"main"` for every
  // [shader(...)]-attributed function regardless of the source-side
  // function name (verified via spirv-dis on the .spv); the .slang
  // files have been renamed to `void main(...)` to keep both sides
  // aligned. Passing anything else here trips
  // VUID-VkPipelineShaderStageCreateInfo-pName-00707.
  _raygenShader = LoadSpirv(_device, raygenPath.View(), nvrhi::ShaderType::RayGeneration, "main");
  _missShader = LoadSpirv(_device, missPath.View(), nvrhi::ShaderType::Miss, "main");
  _closestHitShader =
      LoadSpirv(_device, closestHitPath.View(), nvrhi::ShaderType::ClosestHit, "main");
  if (!_raygenShader || !_missShader || !_closestHitShader)
  {
    Logging::Get().Error(log::RENDER, "PathTracePass: shader load failed; pass will skip");
    return;
  }

  // Binding layout — visibility=AllRayTracing covers all five RT
  // stages so we don't have to re-author this for shadow-trace etc.
  // additions later. Slot indices match the raygen.slang register
  // assignments (b0 / t0 / u0).
  // Non-volatile ConstantBuffer (= VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER)
  // matches Slang's `ConstantBuffer<T>` declaration in the shader.
  //
  // bindingOffsets are zeroed: NVRHI's default offsets
  // (shaderResource=0, constantBuffer=256, unorderedAccess=384)
  // would emit Vulkan binding numbers 256 / 0 / 384 for our three
  // items, which doesn't match the shader's `[[vk::binding(0/1/2,
  // 0)]]` declarations and trips
  // VUID-VkRayTracingPipelineCreateInfoKHR-layout-07988/07990.
  // With offsets zero, items collapse to one binding space and the
  // distinct slot numbers (0 / 1 / 2) we pass below produce
  // bindings 0 / 1 / 2 — matching what Slang emitted in the SPIR-V.
  nvrhi::BindingLayoutDesc layoutDesc;
  layoutDesc.visibility = nvrhi::ShaderType::AllRayTracing;
  layoutDesc.bindingOffsets.shaderResource = 0;
  layoutDesc.bindingOffsets.sampler = 0;
  layoutDesc.bindingOffsets.constantBuffer = 0;
  layoutDesc.bindingOffsets.unorderedAccess = 0;
  layoutDesc.bindings = {
      nvrhi::BindingLayoutItem::ConstantBuffer(0),           // binding 0 CameraUniforms
      nvrhi::BindingLayoutItem::RayTracingAccelStruct(1),    // binding 1 TLAS
      nvrhi::BindingLayoutItem::Texture_UAV(2),              // binding 2 output (BGRA8 display)
      nvrhi::BindingLayoutItem::StructuredBuffer_SRV(3),     // binding 3 materials (M5)
      nvrhi::BindingLayoutItem::StructuredBuffer_SRV(4),     // binding 4 instance→material (M6)
      nvrhi::BindingLayoutItem::StructuredBuffer_SRV(5),     // binding 5 lights (M7)
      nvrhi::BindingLayoutItem::StructuredBuffer_SRV(6),     // binding 6 instance→mesh (M7 NdotL)
      nvrhi::BindingLayoutItem::StructuredBuffer_SRV(7),     // binding 7 mesh face normals (M7 NdotL)
      nvrhi::BindingLayoutItem::StructuredBuffer_SRV(8),     // binding 8 mesh face offsets (M7 NdotL)
      nvrhi::BindingLayoutItem::Texture_SRV(9),              // binding 9 dome env-map (M7-IBL)
      nvrhi::BindingLayoutItem::Sampler(10),                 // binding 10 dome sampler (M7-IBL)
      // M7 follow-up — AOV inspector + picker.
      nvrhi::BindingLayoutItem::Texture_UAV(11),             // binding 11 colorHdr AOV
      nvrhi::BindingLayoutItem::Texture_UAV(12),             // binding 12 normal AOV
      nvrhi::BindingLayoutItem::Texture_UAV(13),             // binding 13 depth AOV
      nvrhi::BindingLayoutItem::Texture_UAV(14),             // binding 14 instanceId AOV
      nvrhi::BindingLayoutItem::StructuredBuffer_UAV(15),    // binding 15 pickResult
  };
  _bindingLayout = _device->createBindingLayout(layoutDesc);
  if (!_bindingLayout)
  {
    Logging::Get().Error(log::RENDER, "PathTracePass: createBindingLayout failed");
    return;
  }

  // Pipeline state — three shader stages registered by `exportName`,
  // one hit group bundling closesthit (no anyhit / intersection in
  // M3), maxRecursionDepth=1 because we only TraceRay from raygen
  // and the closesthit doesn't recurse.
  nvrhi::rt::PipelineDesc pipelineDesc;
  pipelineDesc.shaders = {
      nvrhi::rt::PipelineShaderDesc{}.setExportName("RayGenMain").setShader(_raygenShader),
      nvrhi::rt::PipelineShaderDesc{}.setExportName("MissMain").setShader(_missShader),
  };
  pipelineDesc.hitGroups = {
      nvrhi::rt::PipelineHitGroupDesc{}
          .setExportName("HitGroupDefault")
          .setClosestHitShader(_closestHitShader),
  };
  pipelineDesc.globalBindingLayouts = {_bindingLayout};
  pipelineDesc.maxRecursionDepth = 1;
  _pipeline = _device->createRayTracingPipeline(pipelineDesc);
  if (!_pipeline)
  {
    Logging::Get().Error(log::RENDER, "PathTracePass: createRayTracingPipeline failed");
    return;
  }

  // Shader binding table — one raygen, one miss, one hit group.
  // The pass dispatches with this single SBT every frame; the
  // entries are static across the run.
  _shaderTable = _pipeline->createShaderTable();
  if (!_shaderTable)
  {
    Logging::Get().Error(log::RENDER, "PathTracePass: createShaderTable failed");
    return;
  }
  _shaderTable->setRayGenerationShader("RayGenMain");
  _shaderTable->addMissShader("MissMain");
  _shaderTable->addHitGroup("HitGroupDefault");

  // Camera uniforms constant buffer — sized for one CameraUniforms
  // struct; rewritten every frame from GpuScene's CameraDesc via
  // commandList->writeBuffer (non-volatile path).
  nvrhi::BufferDesc cbDesc;
  cbDesc.byteSize = sizeof(CameraUniforms);
  cbDesc.debugName = "PathTrace.CameraUniforms";
  cbDesc.isConstantBuffer = true;
  cbDesc.initialState = nvrhi::ResourceStates::ConstantBuffer;
  cbDesc.keepInitialState = true;
  _cameraUniformsBuffer = _device->createBuffer(cbDesc);
  if (!_cameraUniformsBuffer)
  {
    Logging::Get().Error(log::RENDER, "PathTracePass: createBuffer(CameraUniforms) failed");
    return;
  }

  // M5: 1-element fallback material buffer. The closesthit binding
  // 3 must point at a non-null structured buffer even when the
  // scene has no materials yet (e.g. the M3 hardcoded cube). The
  // contents are written every Execute() to a default-init
  // OpenPBRMaterialGPU (baseColor 0.8 grey) — see the writeBuffer in
  // Execute below. That keeps the cube-without-materials path
  // visually consistent with the GpuScene sentinel slot 0 (also a
  // default OpenPBRMaterialDesc → grey 0.8).
  nvrhi::BufferDesc fallbackDesc;
  fallbackDesc.byteSize = sizeof(shaderinterop::OpenPBRMaterialGPU);
  fallbackDesc.structStride = sizeof(shaderinterop::OpenPBRMaterialGPU);
  fallbackDesc.canHaveRawViews = false;
  fallbackDesc.canHaveTypedViews = false;
  fallbackDesc.format = nvrhi::Format::UNKNOWN;
  fallbackDesc.debugName = "PathTrace.FallbackMaterial";
  fallbackDesc.initialState = nvrhi::ResourceStates::ShaderResource;
  fallbackDesc.keepInitialState = true;
  _fallbackMaterialBuffer = _device->createBuffer(fallbackDesc);
  if (!_fallbackMaterialBuffer)
  {
    Logging::Get().Error(log::RENDER, "PathTracePass: createBuffer(FallbackMaterial) failed");
    return;
  }

  // M6: 1-element fallback instance→material side-table. Same
  // contract as the material fallback — binding 4 must point at a
  // non-null structured buffer even when the scene has no instances
  // of its own (the M3 hardcoded cube path runs through the
  // hardcoded-cube scene which DOES populate instances, so this
  // fallback only matters in the truly-empty-scene degenerate case).
  // Holds a single zero-uint, so any rogue InstanceID() lookup
  // resolves to material slot 0 = sentinel grey.
  nvrhi::BufferDesc instanceMatFallbackDesc;
  instanceMatFallbackDesc.byteSize = sizeof(std::uint32_t);
  instanceMatFallbackDesc.structStride = sizeof(std::uint32_t);
  instanceMatFallbackDesc.canHaveRawViews = false;
  instanceMatFallbackDesc.canHaveTypedViews = false;
  instanceMatFallbackDesc.format = nvrhi::Format::UNKNOWN;
  instanceMatFallbackDesc.debugName = "PathTrace.FallbackInstanceMaterial";
  instanceMatFallbackDesc.initialState = nvrhi::ResourceStates::ShaderResource;
  instanceMatFallbackDesc.keepInitialState = true;
  _fallbackInstanceMaterialBuffer = _device->createBuffer(instanceMatFallbackDesc);
  if (!_fallbackInstanceMaterialBuffer)
  {
    Logging::Get().Error(log::RENDER,
                         "PathTracePass: createBuffer(FallbackInstanceMaterial) failed");
    return;
  }

  // M7: 1-element fallback lights buffer. Same shape as the material
  // fallback — one disabled (intensity=0) LightGpu so the closesthit
  // per-light loop sees one entry that contributes nothing → falls
  // through to the M5/M6 baseColor-only path.
  nvrhi::BufferDesc lightFallbackDesc;
  lightFallbackDesc.byteSize = sizeof(shaderinterop::LightGpu);
  lightFallbackDesc.structStride = sizeof(shaderinterop::LightGpu);
  lightFallbackDesc.canHaveRawViews = false;
  lightFallbackDesc.canHaveTypedViews = false;
  lightFallbackDesc.format = nvrhi::Format::UNKNOWN;
  lightFallbackDesc.debugName = "PathTrace.FallbackLights";
  lightFallbackDesc.initialState = nvrhi::ResourceStates::ShaderResource;
  lightFallbackDesc.keepInitialState = true;
  _fallbackLightBuffer = _device->createBuffer(lightFallbackDesc);
  if (!_fallbackLightBuffer)
  {
    Logging::Get().Error(log::RENDER, "PathTracePass: createBuffer(FallbackLights) failed");
    return;
  }

  // M7 NdotL: 1-element fallbacks for the three normal-lookup
  // buffers. instanceMesh + meshFaceOffsets are uint stride; the
  // face-normals fallback is one float4 of zero (which the closesthit
  // reads as nLocal=(0,0,0) → NdotL=0 for any light direction →
  // unlit, but no OOB).
  auto makeUintFallback = [&](const char* debugName) {
    nvrhi::BufferDesc desc;
    desc.byteSize = sizeof(std::uint32_t);
    desc.structStride = sizeof(std::uint32_t);
    desc.canHaveRawViews = false;
    desc.canHaveTypedViews = false;
    desc.format = nvrhi::Format::UNKNOWN;
    desc.debugName = debugName;
    desc.initialState = nvrhi::ResourceStates::ShaderResource;
    desc.keepInitialState = true;
    return _device->createBuffer(desc);
  };
  _fallbackInstanceMeshBuffer = makeUintFallback("PathTrace.FallbackInstanceMesh");
  _fallbackMeshFaceOffsetsBuffer = makeUintFallback("PathTrace.FallbackMeshFaceOffsets");
  if (!_fallbackInstanceMeshBuffer || !_fallbackMeshFaceOffsetsBuffer)
  {
    Logging::Get().Error(log::RENDER,
                         "PathTracePass: createBuffer(Fallback{InstanceMesh,MeshFaceOffsets}) "
                         "failed");
    return;
  }

  nvrhi::BufferDesc faceNormalsFallbackDesc;
  faceNormalsFallbackDesc.byteSize = sizeof(hlslpp::float4);
  faceNormalsFallbackDesc.structStride = sizeof(hlslpp::float4);
  faceNormalsFallbackDesc.canHaveRawViews = false;
  faceNormalsFallbackDesc.canHaveTypedViews = false;
  faceNormalsFallbackDesc.format = nvrhi::Format::UNKNOWN;
  faceNormalsFallbackDesc.debugName = "PathTrace.FallbackMeshFaceNormals";
  faceNormalsFallbackDesc.initialState = nvrhi::ResourceStates::ShaderResource;
  faceNormalsFallbackDesc.keepInitialState = true;
  _fallbackMeshFaceNormalsBuffer = _device->createBuffer(faceNormalsFallbackDesc);
  if (!_fallbackMeshFaceNormalsBuffer)
  {
    Logging::Get().Error(log::RENDER,
                         "PathTracePass: createBuffer(FallbackMeshFaceNormals) failed");
    return;
  }

  // M7-IBL: 1×1 black RGBA32F dome fallback texture + a default
  // linear-clamp sampler. Used by the miss shader's lat-long sample
  // when no dome with a resolved env-map exists; texture sampled as
  // (0,0,0,0) so the miss shader's "use authored color" branch
  // continues to fire as the visible result. The fallback texture
  // gets its zero pixel written on the first Execute() — same shape
  // as the other M5/M6/M7 fallbacks.
  nvrhi::TextureDesc domeFallbackDesc;
  domeFallbackDesc.width = 1;
  domeFallbackDesc.height = 1;
  domeFallbackDesc.format = nvrhi::Format::RGBA32_FLOAT;
  domeFallbackDesc.dimension = nvrhi::TextureDimension::Texture2D;
  domeFallbackDesc.debugName = "PathTrace.FallbackDomeTexture";
  domeFallbackDesc.initialState = nvrhi::ResourceStates::ShaderResource;
  domeFallbackDesc.keepInitialState = true;
  _fallbackDomeTexture = _device->createTexture(domeFallbackDesc);
  if (!_fallbackDomeTexture)
  {
    Logging::Get().Error(log::RENDER, "PathTracePass: createTexture(FallbackDomeTexture) failed");
    return;
  }
  // Default linear-clamp sampler — used as fallback when GpuScene
  // hasn't created its bindless sampler yet (i.e. first frame /
  // empty scene). The HDRI lat-long sample wraps in U (azimuth) and
  // clamps in V (elevation); a single sampler covers both — sample
  // wrap mode at the GLSL/HLSL site rather than baking it into a
  // per-axis sampler.
  nvrhi::SamplerDesc samplerDesc;
  samplerDesc.minFilter = true;
  samplerDesc.magFilter = true;
  samplerDesc.mipFilter = true;
  samplerDesc.addressU = nvrhi::SamplerAddressMode::Wrap;
  samplerDesc.addressV = nvrhi::SamplerAddressMode::Clamp;
  samplerDesc.addressW = nvrhi::SamplerAddressMode::Wrap;
  _fallbackDomeSampler = _device->createSampler(samplerDesc);
  if (!_fallbackDomeSampler)
  {
    Logging::Get().Error(log::RENDER, "PathTracePass: createSampler(FallbackDomeSampler) failed");
    return;
  }

  // ---- M7 follow-up: tiny no-write fallbacks for the AOV inspector --
  // The shader writes unconditionally to bindings 11..14 + 15, so when
  // the caller doesn't supply RAW AOV textures (headless mode), bind a
  // 1×1 UAV-capable scratch texture per format. Same trick as the
  // dome fallback above: cheap, never read, just keeps the binding
  // valid.
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
  _fallbackColorHdrAov = makeAovFallback(nvrhi::Format::RGBA16_FLOAT, "PathTrace.FbColorHdrAov");
  _fallbackNormalAov   = makeAovFallback(nvrhi::Format::RGBA16_FLOAT, "PathTrace.FbNormalAov");
  _fallbackDepthAov    = makeAovFallback(nvrhi::Format::R32_FLOAT,    "PathTrace.FbDepthAov");
  _fallbackInstanceAov = makeAovFallback(nvrhi::Format::R32_UINT,     "PathTrace.FbInstanceAov");
  if (!_fallbackColorHdrAov || !_fallbackNormalAov
      || !_fallbackDepthAov || !_fallbackInstanceAov)
  {
    Logging::Get().Error(log::RENDER, "PathTracePass: AOV fallback texture create failed");
    return;
  }

  // Pick-result fallback — 1-element RWStructuredBuffer, never read
  // back. Mirrors the AovTextures::pickResult layout.
  nvrhi::BufferDesc pickFbDesc;
  pickFbDesc.byteSize = 32;        // sizeof(shaderinterop::PickResult)
  pickFbDesc.structStride = 32;
  pickFbDesc.canHaveUAVs = true;
  pickFbDesc.debugName = "PathTrace.FbPickResult";
  pickFbDesc.initialState = nvrhi::ResourceStates::UnorderedAccess;
  pickFbDesc.keepInitialState = true;
  _fallbackPickResult = _device->createBuffer(pickFbDesc);
  if (!_fallbackPickResult)
  {
    Logging::Get().Error(log::RENDER, "PathTracePass: createBuffer(FbPickResult) failed");
    return;
  }

  _shadersOk = true;
  Logging::Get().Info(log::RENDER, "PathTracePass: initialised (RT pipeline + SBT ready)");
}

PathTracePass::~PathTracePass() = default;

bool PathTracePass::ReloadShaders() noexcept {
  // Editor-driven reload (M7 follow-up). Re-translates Slang -> SPIR-V
  // is NOT done here — the Slang compiler isn't linked into the
  // runtime; the .spv files are produced by ShaderMake at CMake
  // build time. Click effect is therefore: re-read the .spv files
  // currently on disk + rebuild the pipeline. Workflow: edit the
  // .slang, run `cmake --build --target pyxis_renderer_shaders`
  // in another terminal, then click Reload.
  //
  // Failure-handling: on any single step failing (file read,
  // createShader, createRayTracingPipeline, createShaderTable) we
  // restore the previous handles and return false so the editor
  // can log it. Without this, a broken .spv would brick the viewer.
  auto& log = Logging::Get();
  const AssetLocator locator;
  const Path raygenPath     = locator.LocateResource("shaders/raygen.spv");
  const Path missPath       = locator.LocateResource("shaders/miss.spv");
  const Path closestHitPath = locator.LocateResource("shaders/closesthit.spv");

  nvrhi::ShaderHandle newRaygen =
      LoadSpirv(_device, raygenPath.View(), nvrhi::ShaderType::RayGeneration, "main");
  nvrhi::ShaderHandle newMiss =
      LoadSpirv(_device, missPath.View(), nvrhi::ShaderType::Miss, "main");
  nvrhi::ShaderHandle newClosestHit =
      LoadSpirv(_device, closestHitPath.View(), nvrhi::ShaderType::ClosestHit, "main");
  if (!newRaygen || !newMiss || !newClosestHit)
  {
    log.Error(log::RENDER, "PathTracePass::ReloadShaders: shader load failed; keeping old pipeline");
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
          .setClosestHitShader(newClosestHit),
  };
  pipelineDesc.globalBindingLayouts = {_bindingLayout};
  pipelineDesc.maxRecursionDepth = 1;
  nvrhi::rt::PipelineHandle newPipeline = _device->createRayTracingPipeline(pipelineDesc);
  if (!newPipeline)
  {
    log.Error(log::RENDER,
              "PathTracePass::ReloadShaders: createRayTracingPipeline failed; keeping old pipeline");
    return false;
  }

  nvrhi::rt::ShaderTableHandle newShaderTable = newPipeline->createShaderTable();
  if (!newShaderTable)
  {
    log.Error(log::RENDER,
              "PathTracePass::ReloadShaders: createShaderTable failed; keeping old pipeline");
    return false;
  }
  newShaderTable->setRayGenerationShader("RayGenMain");
  newShaderTable->addMissShader("MissMain");
  newShaderTable->addHitGroup("HitGroupDefault");

  // Atomic-ish swap: every reference taken in Execute reads from the
  // member handles, so once we overwrite all four together (single-
  // threaded — render thread only, gated by waitForIdle on the caller
  // side) the next Execute picks up the new pipeline + table.
  _raygenShader     = std::move(newRaygen);
  _missShader       = std::move(newMiss);
  _closestHitShader = std::move(newClosestHit);
  _pipeline         = std::move(newPipeline);
  _shaderTable      = std::move(newShaderTable);
  _shadersOk = true;
  log.Info(log::RENDER, "PathTracePass::ReloadShaders: reload OK");
  return true;
}

nvrhi::BindingSetHandle PathTracePass::GetOrCreateBindingSet(RenderTargets const& targets) {
  nvrhi::ITexture* output = targets.color;
  // M5/M6/M7: invalidate cache when ANY of the scene's lazy buffers
  // flips. Each is lazy-allocated by GpuScene on the first relevant
  // verb, so a scene that started empty but gained its first
  // material / instance / light / mesh between frames needs the
  // cached binding sets thrown out.
  // M7 follow-up: same for the caller-owned raw AOV textures + pick
  // buffer — a resize swaps them out from under us.
  nvrhi::IBuffer* sceneMaterials = _scene->GetMaterialBuffer();
  nvrhi::IBuffer* sceneInstanceMaterial = _scene->GetInstanceMaterialBuffer();
  nvrhi::IBuffer* sceneLights = _scene->GetLightBuffer();
  nvrhi::IBuffer* sceneInstanceMesh = _scene->GetInstanceMeshBuffer();
  nvrhi::IBuffer* sceneMeshFaceNormals = _scene->GetMeshFaceNormalsBuffer();
  nvrhi::IBuffer* sceneMeshFaceOffsets = _scene->GetMeshFaceOffsetsBuffer();
  nvrhi::ITexture* sceneDomeTexture = _scene->GetDomeEnvMapTexture();
  nvrhi::ISampler* sceneBindlessSampler = _scene->GetBindlessSampler();
  if (sceneMaterials != _lastSeenMaterialBuffer
      || sceneInstanceMaterial != _lastSeenInstanceMaterialBuffer
      || sceneLights != _lastSeenLightBuffer
      || sceneInstanceMesh != _lastSeenInstanceMeshBuffer
      || sceneMeshFaceNormals != _lastSeenMeshFaceNormalsBuffer
      || sceneMeshFaceOffsets != _lastSeenMeshFaceOffsetsBuffer
      || sceneDomeTexture != _lastSeenDomeTexture
      || sceneBindlessSampler != _lastSeenBindlessSampler
      || targets.colorHdr      != _lastSeenColorHdrAov
      || targets.normalAov     != _lastSeenNormalAov
      || targets.depthAov      != _lastSeenDepthAov
      || targets.instanceIdAov != _lastSeenInstanceAov
      || targets.pickResult    != _lastSeenPickResult)
  {
    _bindingSetCache.clear();
    _lastSeenMaterialBuffer = sceneMaterials;
    _lastSeenInstanceMaterialBuffer = sceneInstanceMaterial;
    _lastSeenLightBuffer = sceneLights;
    _lastSeenInstanceMeshBuffer = sceneInstanceMesh;
    _lastSeenMeshFaceNormalsBuffer = sceneMeshFaceNormals;
    _lastSeenMeshFaceOffsetsBuffer = sceneMeshFaceOffsets;
    _lastSeenDomeTexture = sceneDomeTexture;
    _lastSeenBindlessSampler = sceneBindlessSampler;
    _lastSeenColorHdrAov = targets.colorHdr;
    _lastSeenNormalAov = targets.normalAov;
    _lastSeenDepthAov = targets.depthAov;
    _lastSeenInstanceAov = targets.instanceIdAov;
    _lastSeenPickResult = targets.pickResult;
  }

  if (auto cached = _bindingSetCache.find(output); cached != _bindingSetCache.end())
  {
    return cached->second;
  }
  constexpr std::size_t MAX_CACHE_ENTRIES = 6;
  if (_bindingSetCache.size() >= MAX_CACHE_ENTRIES)
  {
    _bindingSetCache.clear();
  }

  // Slot numbers match the BindingLayoutDesc above (0..5 with zero
  // offsets) so NVRHI emits Vulkan bindings 0..5 to match the
  // shaders' `[[vk::binding(N, 0)]]` declarations.
  // M5: binding 3 is the materials structured buffer — scene's own
  // (when AcquireMaterial has been called + CommitResources has
  // run) or the 1-element fallback grey (used by cube fixtures
  // that have no materials).
  // M6: binding 4 is the instance→material side-table — scene's
  // own (when CommitResources has built a TLAS) or the 1-element
  // fallback (used by truly-empty scenes).
  // M7: binding 5 is the lights buffer — scene's own (when AddLight
  // has been called + CommitResources has run) or the 1-element
  // intensity=0 sentinel fallback (used by unlit M5/M6 fixtures so
  // the closesthit per-light loop falls through to baseColor-only).
  nvrhi::IBuffer* materialBuffer = _scene->GetMaterialBuffer();
  if (materialBuffer == nullptr)
    materialBuffer = _fallbackMaterialBuffer.Get();
  nvrhi::IBuffer* instanceMaterialBuffer = _scene->GetInstanceMaterialBuffer();
  if (instanceMaterialBuffer == nullptr)
    instanceMaterialBuffer = _fallbackInstanceMaterialBuffer.Get();
  nvrhi::IBuffer* lightBuffer = _scene->GetLightBuffer();
  if (lightBuffer == nullptr)
    lightBuffer = _fallbackLightBuffer.Get();
  nvrhi::IBuffer* instanceMeshBuffer = _scene->GetInstanceMeshBuffer();
  if (instanceMeshBuffer == nullptr)
    instanceMeshBuffer = _fallbackInstanceMeshBuffer.Get();
  nvrhi::IBuffer* meshFaceNormalsBuffer = _scene->GetMeshFaceNormalsBuffer();
  if (meshFaceNormalsBuffer == nullptr)
    meshFaceNormalsBuffer = _fallbackMeshFaceNormalsBuffer.Get();
  nvrhi::IBuffer* meshFaceOffsetsBuffer = _scene->GetMeshFaceOffsetsBuffer();
  if (meshFaceOffsetsBuffer == nullptr)
    meshFaceOffsetsBuffer = _fallbackMeshFaceOffsetsBuffer.Get();
  // M7-IBL: dome HDRI texture + sampler. Scene's first live dome
  // wins; falls back to the 1×1 black texture + the local linear-
  // clamp sampler when no dome with a resolved env-map exists. The
  // miss shader handles the all-black case via the M7-simple
  // "use authored color" branch.
  nvrhi::ITexture* domeTexture = _scene->GetDomeEnvMapTexture();
  if (domeTexture == nullptr)
    domeTexture = _fallbackDomeTexture.Get();
  nvrhi::ISampler* domeSampler = _scene->GetBindlessSampler();
  if (domeSampler == nullptr)
    domeSampler = _fallbackDomeSampler.Get();

  // M7 follow-up — caller-owned raw AOVs + pick buffer. Each falls
  // back to the 1×1 / 1-element scratch resource when the caller
  // doesn't supply one (headless mode), so the shader's binding
  // remains valid while the writes go to a discarded resource.
  nvrhi::ITexture* colorHdrAov = targets.colorHdr;
  if (colorHdrAov == nullptr) colorHdrAov = _fallbackColorHdrAov.Get();
  nvrhi::ITexture* normalAov   = targets.normalAov;
  if (normalAov == nullptr)   normalAov   = _fallbackNormalAov.Get();
  nvrhi::ITexture* depthAov    = targets.depthAov;
  if (depthAov == nullptr)    depthAov    = _fallbackDepthAov.Get();
  nvrhi::ITexture* instanceAov = targets.instanceIdAov;
  if (instanceAov == nullptr) instanceAov = _fallbackInstanceAov.Get();
  nvrhi::IBuffer*  pickBuffer  = targets.pickResult;
  if (pickBuffer == nullptr)  pickBuffer  = _fallbackPickResult.Get();

  nvrhi::BindingSetDesc setDesc;
  setDesc.bindings = {
      nvrhi::BindingSetItem::ConstantBuffer(0, _cameraUniformsBuffer),
      nvrhi::BindingSetItem::RayTracingAccelStruct(1, _scene->GetTlas()),
      nvrhi::BindingSetItem::Texture_UAV(2, output),
      nvrhi::BindingSetItem::StructuredBuffer_SRV(3, materialBuffer),
      nvrhi::BindingSetItem::StructuredBuffer_SRV(4, instanceMaterialBuffer),
      nvrhi::BindingSetItem::StructuredBuffer_SRV(5, lightBuffer),
      nvrhi::BindingSetItem::StructuredBuffer_SRV(6, instanceMeshBuffer),
      nvrhi::BindingSetItem::StructuredBuffer_SRV(7, meshFaceNormalsBuffer),
      nvrhi::BindingSetItem::StructuredBuffer_SRV(8, meshFaceOffsetsBuffer),
      nvrhi::BindingSetItem::Texture_SRV(9, domeTexture),
      nvrhi::BindingSetItem::Sampler(10, domeSampler),
      nvrhi::BindingSetItem::Texture_UAV(11, colorHdrAov),
      nvrhi::BindingSetItem::Texture_UAV(12, normalAov),
      nvrhi::BindingSetItem::Texture_UAV(13, depthAov),
      nvrhi::BindingSetItem::Texture_UAV(14, instanceAov),
      nvrhi::BindingSetItem::StructuredBuffer_UAV(15, pickBuffer),
  };
  nvrhi::BindingSetHandle set = _device->createBindingSet(setDesc, _bindingLayout);
  _bindingSetCache[output] = set;
  return set;
}

void PathTracePass::Execute(nvrhi::ICommandList* commandList, const PassContext& context) {
  if (!_shadersOk)
    return;
  if (commandList == nullptr || context.targets == nullptr)
    return;
  nvrhi::ITexture* const output = context.targets->color;
  if (output == nullptr)
    return;

  // Need a TLAS + camera before we can trace anything. M3 callers
  // (HeadlessMode / ViewerMode) populate both at startup and call
  // CommitResources before RenderFrame, so the early-out only
  // fires in degenerate "scene with no instances or camera"
  // configurations.
  const nvrhi::rt::IAccelStruct* const tlas = _scene->GetTlas();
  if (tlas == nullptr || !_scene->HasCamera())
    return;

  const Profiler::GpuScope gpuScope(*context.profiler, commandList, "pass.PathTrace");

  // ---- Drain prior-frame pick staging -------------------------------
  // M7 follow-up. The previous Execute() copied pickResult ->
  // pickResultStaging on the same queue our renderer submits on; by
  // now the GPU has retired that copy, so the host-mapped read here
  // gets the prior frame's value. Skipped on the first Execute (no
  // copy was issued yet, staging holds garbage).
  if (_pickStagingHasFrame
      && context.targets->pickResultStaging != nullptr)
  {
    const void* mapped = _device->mapBuffer(context.targets->pickResultStaging,
                                            nvrhi::CpuAccessMode::Read);
    if (mapped != nullptr)
    {
      std::memcpy(&_lastPickResult, mapped, sizeof(_lastPickResult));
      _device->unmapBuffer(context.targets->pickResultStaging);
    }
  }

  // ---- Upload camera uniforms ----------------------------------------
  // Inverses are computed on the CPU each frame because the shader
  // wants worldFromView + viewFromClip, while GpuScene stores the
  // forward viewFromWorld + projFromView (matching CameraDesc's
  // §18.4 contract). hlslpp::inverse handles the row-vector
  // convention correctly because it's a pure linear-algebra
  // operation that doesn't care about row-vector vs column-vector
  // semantics.
  // M7 follow-up: also pack the per-frame UI tail (mouse pixel +
  // debug-view mode) into the same cbuffer so the AOV inspector
  // doesn't need a second binding for one struct's worth of data.
  const CameraDesc& camera = _scene->GetCamera();
  CameraUniforms cameraUniforms{};
  cameraUniforms.worldFromView = hlslpp::inverse(camera.viewFromWorld);
  cameraUniforms.viewFromClip = hlslpp::inverse(camera.projFromView);
  cameraUniforms.mousePixelX = (context.settings != nullptr)
                                   ? context.settings->mousePixelX
                                   : RenderSettings::MOUSE_PIXEL_NONE;
  cameraUniforms.mousePixelY = (context.settings != nullptr)
                                   ? context.settings->mousePixelY
                                   : RenderSettings::MOUSE_PIXEL_NONE;
  cameraUniforms.debugViewMode = (context.settings != nullptr)
                                     ? static_cast<uint32_t>(context.settings->debugView)
                                     : 0u;
  cameraUniforms._reservedDbg0 = 0u;
  commandList->writeBuffer(_cameraUniformsBuffer.Get(), &cameraUniforms, sizeof(cameraUniforms));

  // M5: write the fallback material if the scene has no materials of
  // its own. Only matters for the M3-cube path — scenes that
  // AcquireMaterial bind their own buffer instead. The fallback packs
  // the same default OpenPBRMaterialDesc that GpuScene reserves at
  // slot 0 (baseColor 0.8 grey, roughness 0.5, opacity 1, IoR 1.5),
  // so an instance with `material = MaterialHandle::Invalid` renders
  // a recognisable visible grey instead of black — and the fallback
  // path stays color-consistent with the real material buffer's
  // sentinel slot 0. Cheap (one writeBuffer of 80 bytes per frame
  // when active).
  if (_scene->GetMaterialBuffer() == nullptr)
  {
    static const shaderinterop::OpenPBRMaterialGPU FALLBACK_MATERIAL_DEFAULT = []() {
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
    commandList->writeBuffer(_fallbackMaterialBuffer.Get(), &FALLBACK_MATERIAL_DEFAULT,
                             sizeof(FALLBACK_MATERIAL_DEFAULT));
  }

  // M6: same shape for the instance→material side-table fallback.
  // Holds a single zero-uint so any rogue InstanceID resolves to
  // material slot 0 (which the material fallback above maps to grey).
  if (_scene->GetInstanceMaterialBuffer() == nullptr)
  {
    static const std::uint32_t FALLBACK_INSTANCE_MATERIAL_ZERO = 0u;
    commandList->writeBuffer(_fallbackInstanceMaterialBuffer.Get(),
                             &FALLBACK_INSTANCE_MATERIAL_ZERO,
                             sizeof(FALLBACK_INSTANCE_MATERIAL_ZERO));
  }

  // M7: same shape for the lights fallback. One zero-init LightGpu
  // (intensity=0) so the closesthit per-light loop sees one disabled
  // entry and falls through to the M5/M6 baseColor-only path —
  // preserving byte-equal across M5 + M6 fixtures that don't author
  // lights.
  if (_scene->GetLightBuffer() == nullptr)
  {
    static const shaderinterop::LightGpu FALLBACK_LIGHT_DISABLED{};
    commandList->writeBuffer(_fallbackLightBuffer.Get(),
                             &FALLBACK_LIGHT_DISABLED,
                             sizeof(FALLBACK_LIGHT_DISABLED));
  }

  // M7 NdotL: zero-init the three normal-lookup fallbacks if the
  // scene hasn't built them. instanceMesh + meshFaceOffsets resolve
  // to slot 0 (offset 0), and meshFaceNormals[0] = (0,0,0,0) — so
  // the closesthit reads nLocal=(0,0,0) → NdotL=0, which combined
  // with the lights fallback (intensity=0 above) routes to the
  // baseColor-only path anyway.
  if (_scene->GetInstanceMeshBuffer() == nullptr)
  {
    static const std::uint32_t FALLBACK_INSTANCE_MESH_ZERO = 0u;
    commandList->writeBuffer(_fallbackInstanceMeshBuffer.Get(),
                             &FALLBACK_INSTANCE_MESH_ZERO,
                             sizeof(FALLBACK_INSTANCE_MESH_ZERO));
  }
  if (_scene->GetMeshFaceOffsetsBuffer() == nullptr)
  {
    static const std::uint32_t FALLBACK_MESH_FACE_OFFSETS_ZERO = 0u;
    commandList->writeBuffer(_fallbackMeshFaceOffsetsBuffer.Get(),
                             &FALLBACK_MESH_FACE_OFFSETS_ZERO,
                             sizeof(FALLBACK_MESH_FACE_OFFSETS_ZERO));
  }
  if (_scene->GetMeshFaceNormalsBuffer() == nullptr)
  {
    static const float FALLBACK_MESH_FACE_NORMALS_ZERO[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    commandList->writeBuffer(_fallbackMeshFaceNormalsBuffer.Get(),
                             FALLBACK_MESH_FACE_NORMALS_ZERO,
                             sizeof(FALLBACK_MESH_FACE_NORMALS_ZERO));
  }

  // M7-IBL: WHITE-init the 1×1 dome fallback texture if the scene
  // hasn't bound a real env-map. RGBA32_FLOAT, 16 bytes — sample
  // returns (1,1,1,1) so the miss shader's `hdri × tint × scale`
  // collapses to `tint × scale` (the dome's authored color), which
  // is the right answer when the dome is color-only or when EXR
  // decode failed.
  //
  // White (not black) on purpose. A zero fallback combined with the
  // miss shader's `hdri × tint × scale` branch turns any color-only
  // dome / failed-HDRI into a black sky, dropping the author's
  // intent — see the M7 audit closeout for the exact reproducer.
  if (_scene->GetDomeEnvMapTexture() == nullptr)
  {
    static const float FALLBACK_DOME_PIXEL[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    commandList->writeTexture(_fallbackDomeTexture.Get(), 0, 0, FALLBACK_DOME_PIXEL,
                              sizeof(FALLBACK_DOME_PIXEL));
  }

  // ---- Bind + dispatch ----------------------------------------------
  const nvrhi::BindingSetHandle bindingSet = GetOrCreateBindingSet(*context.targets);
  if (!bindingSet)
    return;

  // Output image must be in UnorderedAccess so the shader can write
  // it. The caller (or RenderGraph) is responsible for transitioning
  // it to a presentable / copy-source state afterward; that happens
  // in the viewer / headless paths.
  commandList->setTextureState(output, nvrhi::AllSubresources,
                               nvrhi::ResourceStates::UnorderedAccess);
  // Same transition for the M7 raw AOV outputs the raygen writes.
  // keepInitialState on the AovTextures side means NVRHI re-syncs to
  // UnorderedAccess automatically — but a no-op explicit barrier here
  // documents intent and shields against future format / usage flips.
  if (context.targets->colorHdr != nullptr)
    commandList->setTextureState(context.targets->colorHdr,
                                 nvrhi::AllSubresources,
                                 nvrhi::ResourceStates::UnorderedAccess);
  if (context.targets->normalAov != nullptr)
    commandList->setTextureState(context.targets->normalAov,
                                 nvrhi::AllSubresources,
                                 nvrhi::ResourceStates::UnorderedAccess);
  if (context.targets->depthAov != nullptr)
    commandList->setTextureState(context.targets->depthAov,
                                 nvrhi::AllSubresources,
                                 nvrhi::ResourceStates::UnorderedAccess);
  if (context.targets->instanceIdAov != nullptr)
    commandList->setTextureState(context.targets->instanceIdAov,
                                 nvrhi::AllSubresources,
                                 nvrhi::ResourceStates::UnorderedAccess);
  commandList->commitBarriers();

  nvrhi::rt::State state;
  state.shaderTable = _shaderTable;
  state.bindings = {bindingSet};
  commandList->setRayTracingState(state);

  nvrhi::rt::DispatchRaysArguments args;
  args.width = output->getDesc().width;
  args.height = output->getDesc().height;
  args.depth = 1;
  commandList->dispatchRays(args);

  // ---- Submit pick-result staging copy -------------------------------
  // M7 follow-up. After dispatchRays retires, copy the device pick
  // buffer (just written by the raygen if mouse was over a pixel)
  // into the host-readable staging buffer. The next Execute()'s
  // top-of-frame map (gated on _pickStagingHasFrame) reads what we
  // just submitted here.
  if (context.targets->pickResult != nullptr
      && context.targets->pickResultStaging != nullptr)
  {
    commandList->copyBuffer(context.targets->pickResultStaging, 0,
                            context.targets->pickResult, 0,
                            sizeof(PickResult));
    _pickStagingHasFrame = true;
  }
}

}  // namespace pyxis
