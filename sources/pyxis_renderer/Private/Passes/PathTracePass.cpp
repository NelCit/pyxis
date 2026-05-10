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
      nvrhi::BindingLayoutItem::Texture_UAV(16),             // binding 16 materialId AOV
      nvrhi::BindingLayoutItem::Texture_UAV(17),             // binding 17 baseColor AOV
      nvrhi::BindingLayoutItem::Texture_UAV(18),             // binding 18 worldPos AOV
      nvrhi::BindingLayoutItem::ConstantBuffer(19),          // binding 19 FrameUiUniforms
      // Tier 1 Hydra-canonical AOVs (alpha, elementId, Neye, Peye).
      nvrhi::BindingLayoutItem::Texture_UAV(20),             // binding 20 alpha AOV
      nvrhi::BindingLayoutItem::Texture_UAV(21),             // binding 21 elementId AOV
      nvrhi::BindingLayoutItem::Texture_UAV(22),             // binding 22 normalEye AOV
      nvrhi::BindingLayoutItem::Texture_UAV(23),             // binding 23 worldPosEye AOV
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
  if (current != _lastBindings)
  {
    _bindingSetCache.clear();
    _lastBindings = current;
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
      nvrhi::BindingSetItem::Sampler(10, domeSampler),
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
  cameraUniforms._camPad0  = 0.0f;
  cameraUniforms._camPad1  = 0.0f;
  cameraUniforms._camPad2  = 0.0f;
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

  struct BufferFallbackSpec {
    nvrhi::IBuffer* (GpuScene::*sceneGetter)() const noexcept;
    nvrhi::BufferHandle PathTracePass::* fallbackMember;
    const void*  defaultBytes;
    std::size_t  defaultByteSize;
  };
  const std::array<BufferFallbackSpec, 6> bufferFallbacks{{
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
