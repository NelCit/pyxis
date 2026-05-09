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
static_assert(sizeof(CameraUniforms) == 128,
              "CameraUniforms is two float4x4s = 128 bytes "
              "(see resources/shaders/ShaderInterop.slang).");

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
      nvrhi::BindingLayoutItem::Texture_UAV(2),              // binding 2 output
      nvrhi::BindingLayoutItem::StructuredBuffer_SRV(3),     // binding 3 materials (M5)
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

  _shadersOk = true;
  Logging::Get().Info(log::RENDER, "PathTracePass: initialised (RT pipeline + SBT ready)");
}

PathTracePass::~PathTracePass() = default;

nvrhi::BindingSetHandle PathTracePass::GetOrCreateBindingSet(nvrhi::ITexture* output) {
  // M5: invalidate cache when the scene's material-buffer pointer
  // flips. Lazy-allocated by GpuScene on the first
  // AcquireMaterial → CommitResources, so a scene that started
  // empty but gained a material between frames needs the cached
  // binding sets thrown out.
  nvrhi::IBuffer* sceneMaterials = _scene->GetMaterialBuffer();
  if (sceneMaterials != _lastSeenMaterialBuffer)
  {
    _bindingSetCache.clear();
    _lastSeenMaterialBuffer = sceneMaterials;
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

  // Slot numbers match the BindingLayoutDesc above (0 / 1 / 2 / 3
  // with zero offsets) so NVRHI emits Vulkan bindings 0..3 to match
  // the shaders' `[[vk::binding(N, 0)]]` declarations.
  // M5: binding 3 is the materials structured buffer — scene's own
  // (when AcquireMaterial has been called + CommitResources has
  // run) or the 1-element fallback (zero baseColor, used by the M3
  // cube fixture which has no materials).
  nvrhi::IBuffer* materialBuffer = _scene->GetMaterialBuffer();
  if (materialBuffer == nullptr)
    materialBuffer = _fallbackMaterialBuffer.Get();

  nvrhi::BindingSetDesc setDesc;
  setDesc.bindings = {
      nvrhi::BindingSetItem::ConstantBuffer(0, _cameraUniformsBuffer),
      nvrhi::BindingSetItem::RayTracingAccelStruct(1, _scene->GetTlas()),
      nvrhi::BindingSetItem::Texture_UAV(2, output),
      nvrhi::BindingSetItem::StructuredBuffer_SRV(3, materialBuffer),
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

  // ---- Upload camera uniforms ----------------------------------------
  // Inverses are computed on the CPU each frame because the shader
  // wants worldFromView + viewFromClip, while GpuScene stores the
  // forward viewFromWorld + projFromView (matching CameraDesc's
  // §18.4 contract). hlslpp::inverse handles the row-vector
  // convention correctly because it's a pure linear-algebra
  // operation that doesn't care about row-vector vs column-vector
  // semantics.
  const CameraDesc& camera = _scene->GetCamera();
  CameraUniforms cameraUniforms{};
  cameraUniforms.worldFromView = hlslpp::inverse(camera.viewFromWorld);
  cameraUniforms.viewFromClip = hlslpp::inverse(camera.projFromView);
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

  // ---- Bind + dispatch ----------------------------------------------
  const nvrhi::BindingSetHandle bindingSet = GetOrCreateBindingSet(output);
  if (!bindingSet)
    return;

  // Output image must be in UnorderedAccess so the shader can write
  // it. The caller (or RenderGraph) is responsible for transitioning
  // it to a presentable / copy-source state afterward; that happens
  // in the viewer / headless paths.
  commandList->setTextureState(output, nvrhi::AllSubresources,
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
}

}  // namespace pyxis
