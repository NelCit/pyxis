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
static_assert(sizeof(CameraUniforms) == 208,
              "CameraUniforms is three float4x4s + 16-byte exposure row = 208 bytes "
              "(worldFromView / viewFromClip / viewFromWorld + exposure stops, "
              "see resources/shaders/ShaderInterop.slang).");
static_assert(sizeof(shaderinterop::FrameUiUniforms) == 32,
              "FrameUiUniforms is 32 bytes (2 cbuffer rows): picker + display "
              "selector on row 0, per-AOV knobs (worldPosPeriod + reserved) on row 1.");

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
  const Path shadowMissPath = locator.LocateResource("shaders/shadow_miss.spv");
  const Path closestHitPath = locator.LocateResource("shaders/closesthit.spv");
  const Path anyHitPath = locator.LocateResource("shaders/anyhit.spv");

  // Slang emits the SPIR-V `OpEntryPoint` name as `"main"` for every
  // [shader(...)]-attributed function regardless of the source-side
  // function name (verified via spirv-dis on the .spv); the .slang
  // files have been renamed to `void main(...)` to keep both sides
  // aligned. Passing anything else here trips
  // VUID-VkPipelineShaderStageCreateInfo-pName-00707.
  _raygenShader = LoadSpirv(_device, raygenPath.View(), nvrhi::ShaderType::RayGeneration, "main");
  _missShader = LoadSpirv(_device, missPath.View(), nvrhi::ShaderType::Miss, "main");
  _shadowMissShader =
      LoadSpirv(_device, shadowMissPath.View(), nvrhi::ShaderType::Miss, "main");
  _closestHitShader =
      LoadSpirv(_device, closestHitPath.View(), nvrhi::ShaderType::ClosestHit, "main");
  _anyHitShader = LoadSpirv(_device, anyHitPath.View(), nvrhi::ShaderType::AnyHit, "main");
  if (!_raygenShader || !_missShader || !_shadowMissShader || !_closestHitShader || !_anyHitShader)
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
      nvrhi::BindingLayoutItem::Texture_UAV(16),             // binding 16 materialId AOV
      nvrhi::BindingLayoutItem::Texture_UAV(17),             // binding 17 baseColor AOV
      nvrhi::BindingLayoutItem::Texture_UAV(18),             // binding 18 worldPos AOV
      nvrhi::BindingLayoutItem::ConstantBuffer(19),          // binding 19 FrameUiUniforms
      // Tier 1 Hydra-canonical AOVs (alpha, elementId, Neye, Peye).
      nvrhi::BindingLayoutItem::Texture_UAV(20),             // binding 20 alpha AOV
      nvrhi::BindingLayoutItem::Texture_UAV(21),             // binding 21 elementId AOV
      nvrhi::BindingLayoutItem::Texture_UAV(22),             // binding 22 normalEye AOV
      nvrhi::BindingLayoutItem::Texture_UAV(23),             // binding 23 worldPosEye AOV
      // M8a UV pipeline + bindless materials.
      nvrhi::BindingLayoutItem::StructuredBuffer_SRV(24),    // binding 24 mesh UVs
      nvrhi::BindingLayoutItem::StructuredBuffer_SRV(25),    // binding 25 mesh UV offsets
      nvrhi::BindingLayoutItem::StructuredBuffer_SRV(26),    // binding 26 mesh indices
      nvrhi::BindingLayoutItem::StructuredBuffer_SRV(27),    // binding 27 mesh index offsets
      // Bindless material textures. NVRHI's vulkan backend applies
      // ePartiallyBound to every layout entry (vulkan-resource-bindings
      // .cpp:184), so unbound array slots are safe as long as the
      // closesthit's `mat.flags & MATERIAL_FLAG_HAS_BASE_COLOR_MAP`
      // gate prevents access to them. Cap mirrors BINDLESS_TEXTURES_CAP
      // in ShaderInterop.slang. True createBindlessLayout (plan §5
      // ~80K capacity) is a post-v1 sweep — 4096 covers Bistro + every
      // v1 production scene.
      nvrhi::BindingLayoutItem::Texture_SRV(28).setSize(
          shaderinterop::BINDLESS_TEXTURES_CAP),             // binding 28 bindless textures
      // M9 smooth shading — per-vertex normals + per-mesh start offsets.
      nvrhi::BindingLayoutItem::StructuredBuffer_SRV(29),    // binding 29 mesh vertex normals
      nvrhi::BindingLayoutItem::StructuredBuffer_SRV(30),    // binding 30 mesh vertex-normal offsets
      // M9 normal mapping — per-vertex tangents (float4: xyz tangent + w sign).
      nvrhi::BindingLayoutItem::StructuredBuffer_SRV(31),    // binding 31 mesh tangents
      nvrhi::BindingLayoutItem::StructuredBuffer_SRV(32),    // binding 32 mesh tangent offsets
      // M9-fidelity per-role samplers — dome (Wrap-Clamp-Wrap) at 33;
      // bindlessSampler at 10 stays as material sampler (Wrap-Wrap-Wrap).
      nvrhi::BindingLayoutItem::Sampler(33),                 // binding 33 dome HDRI sampler
  };
  _bindingLayout = _device->createBindingLayout(layoutDesc);
  if (!_bindingLayout)
  {
    Logging::Get().Error(log::RENDER, "PathTracePass: createBindingLayout failed");
    return;
  }

  // Pipeline state — four shader stages registered by `exportName`,
  // one hit group bundling closesthit + anyhit. M9-fidelity:
  // maxRecursionDepth=2 (raygen→primary closesthit + closesthit→
  // shadow ray which only invokes anyhit/miss, but RT pipeline
  // accounting still counts it as a recursion level).
  nvrhi::rt::PipelineDesc pipelineDesc;
  pipelineDesc.shaders = {
      nvrhi::rt::PipelineShaderDesc{}.setExportName("RayGenMain").setShader(_raygenShader),
      nvrhi::rt::PipelineShaderDesc{}.setExportName("MissMain").setShader(_missShader),
      nvrhi::rt::PipelineShaderDesc{}.setExportName("ShadowMissMain").setShader(_shadowMissShader),
  };
  pipelineDesc.hitGroups = {
      nvrhi::rt::PipelineHitGroupDesc{}
          .setExportName("HitGroupDefault")
          .setClosestHitShader(_closestHitShader)
          .setAnyHitShader(_anyHitShader),
  };
  pipelineDesc.globalBindingLayouts = {_bindingLayout};
  pipelineDesc.maxRecursionDepth = 2;
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
  _shaderTable->addMissShader("MissMain");        // miss-index 0: primary rays (sky / dome)
  _shaderTable->addMissShader("ShadowMissMain");  // miss-index 1: shadow rays (visibility)
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

  // M7 follow-up — viewer-only per-frame UI cbuffer at binding 19.
  // Same shape / lifetime as CameraUniforms, just smaller (16 bytes).
  // See ShaderInterop.slang's FrameUiUniforms for the field layout.
  nvrhi::BufferDesc uiCbDesc;
  uiCbDesc.byteSize = sizeof(shaderinterop::FrameUiUniforms);
  uiCbDesc.debugName = "PathTrace.FrameUiUniforms";
  uiCbDesc.isConstantBuffer = true;
  uiCbDesc.initialState = nvrhi::ResourceStates::ConstantBuffer;
  uiCbDesc.keepInitialState = true;
  _frameUiBuffer = _device->createBuffer(uiCbDesc);
  if (!_frameUiBuffer)
  {
    Logging::Get().Error(log::RENDER, "PathTracePass: createBuffer(FrameUiUniforms) failed");
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

  // M8a UV pipeline fallbacks. Same uint-stride shape for the index
  // and offset buffers; the UV fallback is a single zero float2.
  _fallbackMeshUvOffsetsBuffer    = makeUintFallback("PathTrace.FallbackMeshUvOffsets");
  _fallbackMeshIndicesBuffer      = makeUintFallback("PathTrace.FallbackMeshIndices");
  _fallbackMeshIndexOffsetsBuffer = makeUintFallback("PathTrace.FallbackMeshIndexOffsets");
  if (!_fallbackMeshUvOffsetsBuffer || !_fallbackMeshIndicesBuffer
      || !_fallbackMeshIndexOffsetsBuffer)
  {
    Logging::Get().Error(log::RENDER,
                         "PathTracePass: createBuffer(Fallback{MeshUvOffsets,MeshIndices,"
                         "MeshIndexOffsets}) failed");
    return;
  }
  {
    nvrhi::BufferDesc uvFallbackDesc;
    uvFallbackDesc.byteSize = sizeof(hlslpp::float2);
    uvFallbackDesc.structStride = sizeof(hlslpp::float2);
    uvFallbackDesc.canHaveRawViews = false;
    uvFallbackDesc.canHaveTypedViews = false;
    uvFallbackDesc.format = nvrhi::Format::UNKNOWN;
    uvFallbackDesc.debugName = "PathTrace.FallbackMeshUvs";
    uvFallbackDesc.initialState = nvrhi::ResourceStates::ShaderResource;
    uvFallbackDesc.keepInitialState = true;
    _fallbackMeshUvsBuffer = _device->createBuffer(uvFallbackDesc);
  }
  if (!_fallbackMeshUvsBuffer)
  {
    Logging::Get().Error(log::RENDER, "PathTracePass: createBuffer(FallbackMeshUvs) failed");
    return;
  }

  // M9 smooth-shading fallbacks. Vertex-normal buffer is one zero
  // float4; the offset table fallback shares the uint-stride helper
  // with the index/UV-offset fallbacks above.
  _fallbackMeshVertexNormalOffsetsBuffer =
      makeUintFallback("PathTrace.FallbackMeshVertexNormalOffsets");
  if (!_fallbackMeshVertexNormalOffsetsBuffer)
  {
    Logging::Get().Error(log::RENDER,
                         "PathTracePass: createBuffer(FallbackMeshVertexNormalOffsets) failed");
    return;
  }
  {
    nvrhi::BufferDesc nFallbackDesc;
    nFallbackDesc.byteSize = sizeof(hlslpp::float4);
    nFallbackDesc.structStride = sizeof(hlslpp::float4);
    nFallbackDesc.canHaveRawViews = false;
    nFallbackDesc.canHaveTypedViews = false;
    nFallbackDesc.format = nvrhi::Format::UNKNOWN;
    nFallbackDesc.debugName = "PathTrace.FallbackMeshVertexNormals";
    nFallbackDesc.initialState = nvrhi::ResourceStates::ShaderResource;
    nFallbackDesc.keepInitialState = true;
    _fallbackMeshVertexNormalsBuffer = _device->createBuffer(nFallbackDesc);
  }
  if (!_fallbackMeshVertexNormalsBuffer)
  {
    Logging::Get().Error(log::RENDER,
                         "PathTracePass: createBuffer(FallbackMeshVertexNormals) failed");
    return;
  }

  // M9 tangent fallbacks. Same shape as the vertex-normal fallbacks
  // above; closesthit detects zero-magnitude tangent and skips the
  // TBN sample.
  _fallbackMeshTangentOffsetsBuffer =
      makeUintFallback("PathTrace.FallbackMeshTangentOffsets");
  if (!_fallbackMeshTangentOffsetsBuffer)
  {
    Logging::Get().Error(log::RENDER,
                         "PathTracePass: createBuffer(FallbackMeshTangentOffsets) failed");
    return;
  }
  {
    nvrhi::BufferDesc tFallbackDesc;
    tFallbackDesc.byteSize = sizeof(hlslpp::float4);
    tFallbackDesc.structStride = sizeof(hlslpp::float4);
    tFallbackDesc.canHaveRawViews = false;
    tFallbackDesc.canHaveTypedViews = false;
    tFallbackDesc.format = nvrhi::Format::UNKNOWN;
    tFallbackDesc.debugName = "PathTrace.FallbackMeshTangents";
    tFallbackDesc.initialState = nvrhi::ResourceStates::ShaderResource;
    tFallbackDesc.keepInitialState = true;
    _fallbackMeshTangentsBuffer = _device->createBuffer(tFallbackDesc);
  }
  if (!_fallbackMeshTangentsBuffer)
  {
    Logging::Get().Error(log::RENDER,
                         "PathTracePass: createBuffer(FallbackMeshTangents) failed");
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
  // Iterate over a static fallback-spec table — adding a new AOV is
  // one row in this table + matching member in PathTracePass.h. Pre-
  // refactor the seven createTexture calls were spelled out verbatim
  // and the existence check at the bottom was easy to miss when adding
  // a new format.
  struct FallbackSpec {
    nvrhi::TextureHandle PathTracePass::* member;
    nvrhi::Format format;
    const char* debugName;
  };
  const std::array<FallbackSpec, 11> aovFallbacks{{
      {&PathTracePass::_fallbackColorHdrAov,    nvrhi::Format::RGBA16_FLOAT, "PathTrace.FbColorHdrAov"   },
      {&PathTracePass::_fallbackNormalAov,      nvrhi::Format::RGBA16_FLOAT, "PathTrace.FbNormalAov"     },
      {&PathTracePass::_fallbackDepthAov,       nvrhi::Format::R32_FLOAT,    "PathTrace.FbDepthAov"      },
      {&PathTracePass::_fallbackPrimIdAov,      nvrhi::Format::R32_UINT,     "PathTrace.FbPrimIdAov"     },
      {&PathTracePass::_fallbackMaterialAov,    nvrhi::Format::R32_UINT,     "PathTrace.FbMaterialAov"   },
      {&PathTracePass::_fallbackBaseColorAov,   nvrhi::Format::RGBA16_FLOAT, "PathTrace.FbBaseColorAov"  },
      {&PathTracePass::_fallbackWorldPosAov,    nvrhi::Format::RGBA32_FLOAT, "PathTrace.FbWorldPosAov"   },
      // Tier 1 Hydra-canonical fallbacks.
      {&PathTracePass::_fallbackAlphaAov,       nvrhi::Format::R8_UNORM,     "PathTrace.FbAlphaAov"      },
      {&PathTracePass::_fallbackElementIdAov,   nvrhi::Format::R32_UINT,     "PathTrace.FbElementIdAov"  },
      {&PathTracePass::_fallbackNormalEyeAov,   nvrhi::Format::RGBA16_FLOAT, "PathTrace.FbNormalEyeAov"  },
      {&PathTracePass::_fallbackWorldPosEyeAov, nvrhi::Format::RGBA32_FLOAT, "PathTrace.FbWorldPosEyeAov"},
  }};
  for (const FallbackSpec& spec : aovFallbacks)
  {
    this->*spec.member = makeAovFallback(spec.format, spec.debugName);
    if (!(this->*spec.member))
    {
      Logging::Get().Error(log::RENDER, std::string{"PathTracePass: AOV fallback create failed: "}
                                            + spec.debugName);
      return;
    }
  }

  // Pick-result fallback — 1-element RWStructuredBuffer, never read
  // back. Mirrors the AovTextures::pickResult layout.
  nvrhi::BufferDesc pickFbDesc;
  pickFbDesc.byteSize = sizeof(pyxis::shaderinterop::PickResult);
  pickFbDesc.structStride = sizeof(pyxis::shaderinterop::PickResult);
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
  const Path shadowMissPath = locator.LocateResource("shaders/shadow_miss.spv");
  const Path closestHitPath = locator.LocateResource("shaders/closesthit.spv");
  const Path anyHitPath     = locator.LocateResource("shaders/anyhit.spv");

  nvrhi::ShaderHandle newRaygen =
      LoadSpirv(_device, raygenPath.View(), nvrhi::ShaderType::RayGeneration, "main");
  nvrhi::ShaderHandle newMiss =
      LoadSpirv(_device, missPath.View(), nvrhi::ShaderType::Miss, "main");
  nvrhi::ShaderHandle newShadowMiss =
      LoadSpirv(_device, shadowMissPath.View(), nvrhi::ShaderType::Miss, "main");
  nvrhi::ShaderHandle newClosestHit =
      LoadSpirv(_device, closestHitPath.View(), nvrhi::ShaderType::ClosestHit, "main");
  nvrhi::ShaderHandle newAnyHit =
      LoadSpirv(_device, anyHitPath.View(), nvrhi::ShaderType::AnyHit, "main");
  if (!newRaygen || !newMiss || !newShadowMiss || !newClosestHit || !newAnyHit)
  {
    log.Error(log::RENDER, "PathTracePass::ReloadShaders: shader load failed; keeping old pipeline");
    return false;
  }

  nvrhi::rt::PipelineDesc pipelineDesc;
  pipelineDesc.shaders = {
      nvrhi::rt::PipelineShaderDesc{}.setExportName("RayGenMain").setShader(newRaygen),
      nvrhi::rt::PipelineShaderDesc{}.setExportName("MissMain").setShader(newMiss),
      nvrhi::rt::PipelineShaderDesc{}.setExportName("ShadowMissMain").setShader(newShadowMiss),
  };
  pipelineDesc.hitGroups = {
      nvrhi::rt::PipelineHitGroupDesc{}
          .setExportName("HitGroupDefault")
          .setClosestHitShader(newClosestHit)
          .setAnyHitShader(newAnyHit),
  };
  pipelineDesc.globalBindingLayouts = {_bindingLayout};
  pipelineDesc.maxRecursionDepth = 2;
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
  newShaderTable->addMissShader("MissMain");        // miss-index 0: primary rays
  newShaderTable->addMissShader("ShadowMissMain");  // miss-index 1: shadow rays
  newShaderTable->addHitGroup("HitGroupDefault");

  // Atomic-ish swap: every reference taken in Execute reads from the
  // member handles, so once we overwrite all four together (single-
  // threaded — render thread only, gated by waitForIdle on the caller
  // side) the next Execute picks up the new pipeline + table.
  _raygenShader     = std::move(newRaygen);
  _missShader       = std::move(newMiss);
  _shadowMissShader = std::move(newShadowMiss);
  _closestHitShader = std::move(newClosestHit);
  _anyHitShader     = std::move(newAnyHit);
  _pipeline         = std::move(newPipeline);
  _shaderTable      = std::move(newShaderTable);
  _shadersOk = true;
  log.Info(log::RENDER, "PathTracePass::ReloadShaders: reload OK");
  return true;
}

nvrhi::BindingSetHandle PathTracePass::GetOrCreateBindingSet(RenderTargets const& targets) {
  nvrhi::ITexture* output = targets.color;
  // Capture every borrowed-pointer that participates in a binding into
  // one snapshot, then compare against last frame's. A mismatch on
  // ANY field invalidates the cached binding sets — covers the
  // scene-side lazy-allocation flips (GpuScene's first AcquireMaterial
  // / AppendInstance / AddLight that creates a real buffer where a
  // 1×1 fallback used to live), the caller-side AOV swaps on resize,
  // and the dome-texture flip when a USD dome's env-map resolves.
  // Indexed init keeps slot order in lockstep with the BindingSlot
  // enum; std::array's element-wise operator== and operator= keep the
  // compare + assign idiomatic without the multi-level pointer cast
  // memcmp/memcpy would need.
  auto slot = [](BindingSlot index) constexpr noexcept { return static_cast<std::size_t>(index); };
  BindingsSnapshot current{};
  current[slot(BindingSlot::Materials)]        = _scene->GetMaterialBuffer();
  current[slot(BindingSlot::InstanceMaterial)] = _scene->GetInstanceMaterialBuffer();
  current[slot(BindingSlot::Lights)]           = _scene->GetLightBuffer();
  current[slot(BindingSlot::InstanceMesh)]     = _scene->GetInstanceMeshBuffer();
  current[slot(BindingSlot::MeshFaceNormals)]  = _scene->GetMeshFaceNormalsBuffer();
  current[slot(BindingSlot::MeshFaceOffsets)]  = _scene->GetMeshFaceOffsetsBuffer();
  current[slot(BindingSlot::DomeTexture)]      = _scene->GetDomeEnvMapTexture();
  current[slot(BindingSlot::BindlessSampler)]  = _scene->GetBindlessSampler();
  current[slot(BindingSlot::MeshUvs)]                = _scene->GetMeshUvsBuffer();
  current[slot(BindingSlot::MeshUvOffsets)]          = _scene->GetMeshUvOffsetsBuffer();
  current[slot(BindingSlot::MeshIndices)]            = _scene->GetMeshIndicesBuffer();
  current[slot(BindingSlot::MeshIndexOffsets)]       = _scene->GetMeshIndexOffsetsBuffer();
  current[slot(BindingSlot::MeshVertexNormals)]      = _scene->GetMeshVertexNormalsBuffer();
  current[slot(BindingSlot::MeshVertexNormalOffsets)]= _scene->GetMeshVertexNormalOffsetsBuffer();
  current[slot(BindingSlot::MeshTangents)]           = _scene->GetMeshTangentsBuffer();
  current[slot(BindingSlot::MeshTangentOffsets)]     = _scene->GetMeshTangentOffsetsBuffer();
  current[slot(BindingSlot::DomeSampler)]            = _scene->GetDomeSampler();
  current[slot(BindingSlot::ColorHdrAov)]      = targets.colorHdr;
  current[slot(BindingSlot::NormalAov)]        = targets.normalAov;
  current[slot(BindingSlot::DepthAov)]         = targets.depthAov;
  current[slot(BindingSlot::PrimIdAov)]        = targets.primIdAov;
  current[slot(BindingSlot::MaterialAov)]      = targets.materialIdAov;
  current[slot(BindingSlot::BaseColorAov)]     = targets.baseColorAov;
  current[slot(BindingSlot::WorldPosAov)]      = targets.worldPosAov;
  current[slot(BindingSlot::PickResult)]       = targets.pickResult;
  current[slot(BindingSlot::AlphaAov)]         = targets.alphaAov;
  current[slot(BindingSlot::ElementIdAov)]     = targets.elementIdAov;
  current[slot(BindingSlot::NormalEyeAov)]     = targets.normalEyeAov;
  current[slot(BindingSlot::WorldPosEyeAov)]   = targets.worldPosEyeAov;
  // Bindless-texture cache invalidation. We need to detect BOTH
  // grow/shrink of the array AND slot-pointer churn (the §M8a
  // free-list slot-recycle path lets DestroyTexture + later
  // AcquireTexture reuse a slot with a fresh ITexture*, leaving the
  // count unchanged). Fingerprint = FNV1a-64 over (count, every live
  // ITexture*); on mismatch with the prior frame's, invalidate.
  const uint32_t bindlessTextureCount = _scene->GetBindlessTextureCount();
  std::uint64_t bindlessFingerprint = 0xcbf29ce484222325ULL;
  bindlessFingerprint ^= bindlessTextureCount;
  bindlessFingerprint *= 0x100000001b3ULL;
  for (uint32_t bindlessSlot = 0; bindlessSlot < bindlessTextureCount; ++bindlessSlot)
  {
    const auto ptrAsInt =
        reinterpret_cast<std::uintptr_t>(_scene->GetBindlessTextureAt(bindlessSlot));
    bindlessFingerprint ^= static_cast<std::uint64_t>(ptrAsInt);
    bindlessFingerprint *= 0x100000001b3ULL;
  }
  if (current != _lastBindings || bindlessFingerprint != _lastBindlessTextureFingerprint)
  {
    _bindingSetCache.clear();
    _lastBindings = current;
    _lastBindlessTextureFingerprint = bindlessFingerprint;
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
  // M8a UV pipeline — same fallback shape as the M7-NdotL buffers.
  nvrhi::IBuffer* meshUvsBuffer = _scene->GetMeshUvsBuffer();
  if (meshUvsBuffer == nullptr)
    meshUvsBuffer = _fallbackMeshUvsBuffer.Get();
  nvrhi::IBuffer* meshUvOffsetsBuffer = _scene->GetMeshUvOffsetsBuffer();
  if (meshUvOffsetsBuffer == nullptr)
    meshUvOffsetsBuffer = _fallbackMeshUvOffsetsBuffer.Get();
  nvrhi::IBuffer* meshIndicesBuffer = _scene->GetMeshIndicesBuffer();
  if (meshIndicesBuffer == nullptr)
    meshIndicesBuffer = _fallbackMeshIndicesBuffer.Get();
  nvrhi::IBuffer* meshIndexOffsetsBuffer = _scene->GetMeshIndexOffsetsBuffer();
  if (meshIndexOffsetsBuffer == nullptr)
    meshIndexOffsetsBuffer = _fallbackMeshIndexOffsetsBuffer.Get();
  // M9 smooth shading — per-vertex normals + their offset table.
  nvrhi::IBuffer* meshVertexNormalsBuffer = _scene->GetMeshVertexNormalsBuffer();
  if (meshVertexNormalsBuffer == nullptr)
    meshVertexNormalsBuffer = _fallbackMeshVertexNormalsBuffer.Get();
  nvrhi::IBuffer* meshVertexNormalOffsetsBuffer = _scene->GetMeshVertexNormalOffsetsBuffer();
  if (meshVertexNormalOffsetsBuffer == nullptr)
    meshVertexNormalOffsetsBuffer = _fallbackMeshVertexNormalOffsetsBuffer.Get();
  // M9 normal mapping — per-vertex tangents + their offset table.
  nvrhi::IBuffer* meshTangentsBuffer = _scene->GetMeshTangentsBuffer();
  if (meshTangentsBuffer == nullptr)
    meshTangentsBuffer = _fallbackMeshTangentsBuffer.Get();
  nvrhi::IBuffer* meshTangentOffsetsBuffer = _scene->GetMeshTangentOffsetsBuffer();
  if (meshTangentOffsetsBuffer == nullptr)
    meshTangentOffsetsBuffer = _fallbackMeshTangentOffsetsBuffer.Get();
  // M9-fidelity per-role samplers — dome sampler at binding 33.
  // Falls back to the material sampler when GpuScene hasn't created
  // the dome sampler yet (truly empty scene); identical filter
  // settings, just different addressing modes.
  nvrhi::ISampler* domeSampler = _scene->GetDomeSampler();
  if (domeSampler == nullptr)
    domeSampler = _fallbackDomeSampler.Get();
  // M7-IBL: dome HDRI texture + the M9-fidelity material sampler at
  // binding 10 (now used for material textures only; dome sampler
  // moved to binding 33 above). Scene's first live dome wins; the
  // 1×1 black fallback texture handles "no dome" — miss shader's
  // "use authored color" branch fires when sampling that all-black.
  nvrhi::ITexture* domeTexture = _scene->GetDomeEnvMapTexture();
  if (domeTexture == nullptr)
    domeTexture = _fallbackDomeTexture.Get();
  nvrhi::ISampler* materialSampler = _scene->GetBindlessSampler();
  if (materialSampler == nullptr)
    materialSampler = _fallbackDomeSampler.Get();

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
  nvrhi::ITexture* primIdAov = targets.primIdAov;
  if (primIdAov == nullptr) primIdAov = _fallbackPrimIdAov.Get();
  nvrhi::IBuffer*  pickBuffer  = targets.pickResult;
  if (pickBuffer == nullptr)  pickBuffer  = _fallbackPickResult.Get();
  nvrhi::ITexture* materialAov = targets.materialIdAov;
  if (materialAov == nullptr) materialAov = _fallbackMaterialAov.Get();
  nvrhi::ITexture* baseColorAov = targets.baseColorAov;
  if (baseColorAov == nullptr) baseColorAov = _fallbackBaseColorAov.Get();
  nvrhi::ITexture* worldPosAov = targets.worldPosAov;
  if (worldPosAov == nullptr) worldPosAov = _fallbackWorldPosAov.Get();
  // Tier 1 Hydra-canonical AOVs.
  nvrhi::ITexture* alphaAov = targets.alphaAov;
  if (alphaAov == nullptr) alphaAov = _fallbackAlphaAov.Get();
  nvrhi::ITexture* elementIdAov = targets.elementIdAov;
  if (elementIdAov == nullptr) elementIdAov = _fallbackElementIdAov.Get();
  nvrhi::ITexture* normalEyeAov = targets.normalEyeAov;
  if (normalEyeAov == nullptr) normalEyeAov = _fallbackNormalEyeAov.Get();
  nvrhi::ITexture* worldPosEyeAov = targets.worldPosEyeAov;
  if (worldPosEyeAov == nullptr) worldPosEyeAov = _fallbackWorldPosEyeAov.Get();

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
      nvrhi::BindingSetItem::Sampler(10, materialSampler),
      nvrhi::BindingSetItem::Texture_UAV(11, colorHdrAov),
      nvrhi::BindingSetItem::Texture_UAV(12, normalAov),
      nvrhi::BindingSetItem::Texture_UAV(13, depthAov),
      nvrhi::BindingSetItem::Texture_UAV(14, primIdAov),
      nvrhi::BindingSetItem::StructuredBuffer_UAV(15, pickBuffer),
      nvrhi::BindingSetItem::Texture_UAV(16, materialAov),
      nvrhi::BindingSetItem::Texture_UAV(17, baseColorAov),
      nvrhi::BindingSetItem::Texture_UAV(18, worldPosAov),
      nvrhi::BindingSetItem::ConstantBuffer(19, _frameUiBuffer),
      nvrhi::BindingSetItem::Texture_UAV(20, alphaAov),
      nvrhi::BindingSetItem::Texture_UAV(21, elementIdAov),
      nvrhi::BindingSetItem::Texture_UAV(22, normalEyeAov),
      nvrhi::BindingSetItem::Texture_UAV(23, worldPosEyeAov),
      nvrhi::BindingSetItem::StructuredBuffer_SRV(24, meshUvsBuffer),
      nvrhi::BindingSetItem::StructuredBuffer_SRV(25, meshUvOffsetsBuffer),
      nvrhi::BindingSetItem::StructuredBuffer_SRV(26, meshIndicesBuffer),
      nvrhi::BindingSetItem::StructuredBuffer_SRV(27, meshIndexOffsetsBuffer),
      // M9 smooth shading.
      nvrhi::BindingSetItem::StructuredBuffer_SRV(29, meshVertexNormalsBuffer),
      nvrhi::BindingSetItem::StructuredBuffer_SRV(30, meshVertexNormalOffsetsBuffer),
      // M9 normal mapping.
      nvrhi::BindingSetItem::StructuredBuffer_SRV(31, meshTangentsBuffer),
      nvrhi::BindingSetItem::StructuredBuffer_SRV(32, meshTangentOffsetsBuffer),
      // M9-fidelity per-role samplers — dome at binding 33.
      nvrhi::BindingSetItem::Sampler(33, domeSampler),
  };

  // ---- Bindless texture array (binding 28) -----------------------------
  // Walk the scene's texture table and emit one Texture_SRV(28,
  // arrayElement=slot, tex) per live entry. Slot 0 is bound to the
  // 4×4 magenta missingTexture as a defence-in-depth fallback for
  // texture-decode failures (texture entry's bindlessSlot is reset
  // to 0 in Commit.cpp's stb_image / tinyexr error paths). The
  // shader's MATERIAL_FLAG_HAS_BASE_COLOR_MAP gate normally prevents
  // any sample of slot 0 — INVALID_BINDLESS_TEXTURE (=0xFFFFFFFF)
  // fails the cap check before sampling — so the magenta only fires
  // when a translation succeeded but the decode then failed. NVRHI
  // vulkan applies ePartiallyBound to every layout (vulkan-resource-
  // bindings.cpp:184) so unbound array slots are safe.
  nvrhi::ITexture* const missingTexture = _scene->GetMissingTexture();
  if (missingTexture != nullptr)
  {
    auto missingItem = nvrhi::BindingSetItem::Texture_SRV(28, missingTexture);
    missingItem.arrayElement = 0;
    setDesc.bindings.push_back(missingItem);
  }
  for (uint32_t bindlessSlot = 1;
       bindlessSlot < bindlessTextureCount && bindlessSlot < shaderinterop::BINDLESS_TEXTURES_CAP;
       ++bindlessSlot)
  {
    nvrhi::ITexture* const sceneTex = _scene->GetBindlessTextureAt(bindlessSlot);
    if (sceneTex == nullptr)
      continue;
    auto item = nvrhi::BindingSetItem::Texture_SRV(28, sceneTex);
    item.arrayElement = bindlessSlot;
    setDesc.bindings.push_back(item);
  }

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
  // pickResultStaging on the same queue our renderer submits on.
  //
  // Correctness contract (load-bearing — read carefully before
  // changing FIF or the picker race becomes silent + intermittent):
  //   * mapBuffer(CpuAccessMode::Read) does NOT itself fence on the
  //     GPU under the Vulkan backend (it's vkInvalidateMappedMemory
  //     + memcpy). The data we read here is whatever the staging
  //     buffer currently holds.
  //   * The previous frame's executeCommandList (which submitted the
  //     copy) MUST have retired before this map runs. With
  //     framesInFlight = 1 the viewer / headless waits on the prior
  //     submit's fence inside deviceManager->BeginFrame before we
  //     reach this point, so the copy IS done. The `framesInFlight
  //     == 1` assert below pins that contract; if a future RFC bumps
  //     it (M11+ frame-pacing knobs) the picker readback needs an
  //     explicit nvrhi::EventQuery between the submit and the next
  //     Execute, OR a fallback to "skip the map this frame and report
  //     last-known-good value" (a one-extra-frame stale picker).
  // Skipped on the very first Execute too (no copy was issued yet,
  // staging holds default-init garbage).
  // Picker readback contract — load-bearing, read PyxisRenderer's
  // RendererCreateDesc::framesInFlight comment before changing FIF:
  // the mapBuffer-without-fence path here only holds at FIF == 1
  // because deviceManager->BeginFrame waits the prior submit's fence
  // before the next Execute reaches this map. At FIF > 1 (e.g. the
  // headless path, which raises FIF to 3 for §33.7 byte-equal EXR)
  // the drain silently no-ops — the editor sees the last-known
  // _lastPickResult value, which is exactly what callers without a
  // live picker want. A future RFC bumping the viewer's FIF needs an
  // nvrhi::EventQuery between the submit and the map; that's the
  // only way to make this safe past FIF=1.
  if (_pickStagingHasFrame
      && context.targets->pickResultStaging != nullptr
      && context.framesInFlight == 1)
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
  const CameraDesc& camera = _scene->GetCamera();
  CameraUniforms cameraUniforms{};
  cameraUniforms.worldFromView = hlslpp::inverse(camera.viewFromWorld);
  cameraUniforms.viewFromClip = hlslpp::inverse(camera.projFromView);
  // Forward viewFromWorld is straight off the CameraDesc — no inverse
  // needed; raygen uses it to transform world-space hit position +
  // normal into eye space for the Hydra-canonical Neye / Peye AOVs.
  cameraUniforms.viewFromWorld = camera.viewFromWorld;
  // Photographic exposure (stops). Raygen multiplies post-shading
  // radiance by 2^exposure before the ACES tonemap fires. Pads stay
  // zero — the cbuffer's size assert pins their presence.
  cameraUniforms.exposure  = camera.exposure;
  // M9-fidelity: per-pixel angular spread for ray-cone-footprint LOD
  // selection in the closesthit. Standard pinhole derivation:
  //   pixelSpread = 2 × tan(fovY/2) / imageHeight
  // We extract fovY from the projection matrix's [1][1] entry
  // (= 1/tan(fovY/2) for a column-vector / row-major projection
  // matrix). imageHeight comes from the bound output texture.
  const auto outputHeight = static_cast<float>(output->getDesc().height);
  // hlslpp::float4x4 stores rows as float[4]; row-1 column-1 is
  // the canonical cot(fovY/2) entry of a column-vector / row-major
  // perspective projection matrix.
  float projRow1[16];
  hlslpp::store(projRow1, camera.projFromView);
  const float projYY = projRow1[5];  // row 1, col 1 in row-major float[16]
  const float tanHalfFov = (projYY > 1e-6f) ? (1.0f / projYY) : 0.0f;
  cameraUniforms.pixelSpreadRadians = (outputHeight > 0.0f && tanHalfFov > 0.0f)
                                          ? (2.0f * tanHalfFov / outputHeight)
                                          : 0.0f;
  cameraUniforms._camPad0  = 0.0f;
  cameraUniforms._camPad1  = 0.0f;
  commandList->writeBuffer(_cameraUniformsBuffer.Get(), &cameraUniforms, sizeof(cameraUniforms));

  // ---- Upload viewer-only per-frame UI state -------------------------
  // M7 follow-up — split out of CameraUniforms after the audit. Mouse
  // pixel + AOV-inspector debug-view mode live at binding 19 so future
  // CameraUniforms growth doesn't collide with editor-driven UI knobs.
  shaderinterop::FrameUiUniforms frameUi{};
  frameUi.mousePixelX = (context.settings != nullptr)
                            ? context.settings->mousePixelX
                            : RenderSettings::MOUSE_PIXEL_NONE;
  frameUi.mousePixelY = (context.settings != nullptr)
                            ? context.settings->mousePixelY
                            : RenderSettings::MOUSE_PIXEL_NONE;
  frameUi.debugViewMode = (context.settings != nullptr)
                              ? static_cast<uint32_t>(context.settings->debugView)
                              : 0u;
  frameUi._reservedUi0 = 0u;
  // Per-AOV knobs (row 1). worldPosPeriod default of 10 m matches the
  // pre-slider behaviour; the editor's WorldPos display can crank
  // this up for Bistro-scale scenes (~50 m) without touching shader.
  frameUi.worldPosPeriod = (context.settings != nullptr
                            && context.settings->worldPosPeriod > 0.0f)
                               ? context.settings->worldPosPeriod
                               : 10.0f;
  frameUi._reservedUi1 = 0u;
  frameUi._reservedUi2 = 0u;
  frameUi._reservedUi3 = 0u;
  commandList->writeBuffer(_frameUiBuffer.Get(), &frameUi, sizeof(frameUi));

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
  // ---- Scene-buffer fallback uploads --------------------------------
  // 5 lazy-allocated scene-side buffers (materials / instance-material
  // / lights / instance-mesh / mesh-face-offsets / mesh-face-normals)
  // share the same shape: "if the scene hasn't allocated yet, write
  // a tiny default into the 1-element fallback buffer so the binding
  // point sees something safe." Pre-refactor the five blocks were
  // spelled out verbatim ~80 lines; one table + loop replaces them.
  // The default-grey OpenPBR material (baseColor 0.8 grey, roughness
  // 0.5, IoR 1.5, all texture slots = INVALID_BINDLESS_TEXTURE) is
  // built on first call and cached in a function-local static.
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
  static const shaderinterop::LightGpu FALLBACK_LIGHT_DISABLED{};
  static const std::uint32_t           FALLBACK_UINT_ZERO = 0u;
  static const float                   FALLBACK_FLOAT4_ZERO[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  static const float                   FALLBACK_FLOAT2_ZERO[2] = {0.0f, 0.0f};

  struct BufferFallbackSpec {
    nvrhi::IBuffer* (GpuScene::*sceneGetter)() const noexcept;
    nvrhi::BufferHandle PathTracePass::* fallbackMember;
    const void*  defaultBytes;
    std::size_t  defaultByteSize;
  };
  const std::array<BufferFallbackSpec, 14> bufferFallbacks{{
      {&GpuScene::GetMaterialBuffer,         &PathTracePass::_fallbackMaterialBuffer,
       &FALLBACK_MATERIAL_GREY,    sizeof(FALLBACK_MATERIAL_GREY)   },
      {&GpuScene::GetInstanceMaterialBuffer, &PathTracePass::_fallbackInstanceMaterialBuffer,
       &FALLBACK_UINT_ZERO,        sizeof(FALLBACK_UINT_ZERO)       },
      {&GpuScene::GetLightBuffer,            &PathTracePass::_fallbackLightBuffer,
       &FALLBACK_LIGHT_DISABLED,   sizeof(FALLBACK_LIGHT_DISABLED)  },
      {&GpuScene::GetInstanceMeshBuffer,     &PathTracePass::_fallbackInstanceMeshBuffer,
       &FALLBACK_UINT_ZERO,        sizeof(FALLBACK_UINT_ZERO)       },
      {&GpuScene::GetMeshFaceOffsetsBuffer,  &PathTracePass::_fallbackMeshFaceOffsetsBuffer,
       &FALLBACK_UINT_ZERO,        sizeof(FALLBACK_UINT_ZERO)       },
      {&GpuScene::GetMeshFaceNormalsBuffer,  &PathTracePass::_fallbackMeshFaceNormalsBuffer,
       FALLBACK_FLOAT4_ZERO,       sizeof(FALLBACK_FLOAT4_ZERO)     },
      // M8a UV pipeline.
      {&GpuScene::GetMeshUvsBuffer,          &PathTracePass::_fallbackMeshUvsBuffer,
       FALLBACK_FLOAT2_ZERO,       sizeof(FALLBACK_FLOAT2_ZERO)     },
      {&GpuScene::GetMeshUvOffsetsBuffer,    &PathTracePass::_fallbackMeshUvOffsetsBuffer,
       &FALLBACK_UINT_ZERO,        sizeof(FALLBACK_UINT_ZERO)       },
      {&GpuScene::GetMeshIndicesBuffer,      &PathTracePass::_fallbackMeshIndicesBuffer,
       &FALLBACK_UINT_ZERO,        sizeof(FALLBACK_UINT_ZERO)       },
      {&GpuScene::GetMeshIndexOffsetsBuffer, &PathTracePass::_fallbackMeshIndexOffsetsBuffer,
       &FALLBACK_UINT_ZERO,        sizeof(FALLBACK_UINT_ZERO)       },
      // M9 smooth shading.
      {&GpuScene::GetMeshVertexNormalsBuffer,        &PathTracePass::_fallbackMeshVertexNormalsBuffer,
       FALLBACK_FLOAT4_ZERO,       sizeof(FALLBACK_FLOAT4_ZERO)     },
      {&GpuScene::GetMeshVertexNormalOffsetsBuffer,  &PathTracePass::_fallbackMeshVertexNormalOffsetsBuffer,
       &FALLBACK_UINT_ZERO,        sizeof(FALLBACK_UINT_ZERO)       },
      // M9 normal mapping.
      {&GpuScene::GetMeshTangentsBuffer,             &PathTracePass::_fallbackMeshTangentsBuffer,
       FALLBACK_FLOAT4_ZERO,       sizeof(FALLBACK_FLOAT4_ZERO)     },
      {&GpuScene::GetMeshTangentOffsetsBuffer,       &PathTracePass::_fallbackMeshTangentOffsetsBuffer,
       &FALLBACK_UINT_ZERO,        sizeof(FALLBACK_UINT_ZERO)       },
  }};
  for (const BufferFallbackSpec& spec : bufferFallbacks)
  {
    if ((_scene->*spec.sceneGetter)() == nullptr)
    {
      commandList->writeBuffer((this->*spec.fallbackMember).Get(),
                               spec.defaultBytes, spec.defaultByteSize);
    }
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
  if (context.targets->primIdAov != nullptr)
    commandList->setTextureState(context.targets->primIdAov,
                                 nvrhi::AllSubresources,
                                 nvrhi::ResourceStates::UnorderedAccess);
  if (context.targets->materialIdAov != nullptr)
    commandList->setTextureState(context.targets->materialIdAov,
                                 nvrhi::AllSubresources,
                                 nvrhi::ResourceStates::UnorderedAccess);
  if (context.targets->baseColorAov != nullptr)
    commandList->setTextureState(context.targets->baseColorAov,
                                 nvrhi::AllSubresources,
                                 nvrhi::ResourceStates::UnorderedAccess);
  if (context.targets->worldPosAov != nullptr)
    commandList->setTextureState(context.targets->worldPosAov,
                                 nvrhi::AllSubresources,
                                 nvrhi::ResourceStates::UnorderedAccess);
  if (context.targets->alphaAov != nullptr)
    commandList->setTextureState(context.targets->alphaAov,
                                 nvrhi::AllSubresources,
                                 nvrhi::ResourceStates::UnorderedAccess);
  if (context.targets->elementIdAov != nullptr)
    commandList->setTextureState(context.targets->elementIdAov,
                                 nvrhi::AllSubresources,
                                 nvrhi::ResourceStates::UnorderedAccess);
  if (context.targets->normalEyeAov != nullptr)
    commandList->setTextureState(context.targets->normalEyeAov,
                                 nvrhi::AllSubresources,
                                 nvrhi::ResourceStates::UnorderedAccess);
  if (context.targets->worldPosEyeAov != nullptr)
    commandList->setTextureState(context.targets->worldPosEyeAov,
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
