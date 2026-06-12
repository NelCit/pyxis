// Pyxis renderer — primary-ray path-trace pass.

#include "Passes/PathTracePass.h"

#include "RenderGraph/PassContext.h"
#include "RenderGraph/ShaderLoad.h"
#include "Scene/SceneResources.h"  // RFC 0003 — replaces the public GpuScene getters.

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

// Thin wrapper over the shared LoadSpirvShader (RenderGraph/ShaderLoad.h) that
// pins the "PathTracePass" log prefix so the call sites below stay terse.
nvrhi::ShaderHandle LoadSpirv(nvrhi::IDevice* device, std::string_view path,
                              nvrhi::ShaderType stage, const char* entry) noexcept {
  return LoadSpirvShader(device, path, stage, entry, "PathTracePass");
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
  // P2 buffer packing — bindings 8/25/27/30/31/32 are RETIRED (the five per-mesh
  // offset tables folded into the MeshInfoGpu record at 6; the per-vertex
  // normal/tangent streams interleaved into VertexAttribGpu at 29). Vulkan allows
  // sparse binding numbers, so the survivors keep their numbers exactly.
  layoutDesc.bindings = {
      nvrhi::BindingLayoutItem::ConstantBuffer(0),           // binding 0 CameraUniforms
      nvrhi::BindingLayoutItem::RayTracingAccelStruct(1),    // binding 1 TLAS
      nvrhi::BindingLayoutItem::Texture_UAV(2),              // binding 2 output (BGRA8 display)
      nvrhi::BindingLayoutItem::StructuredBuffer_SRV(3),     // binding 3 materials (M5)
      nvrhi::BindingLayoutItem::StructuredBuffer_SRV(4),     // binding 4 instance info (P2: InstanceInfoGpu)
      nvrhi::BindingLayoutItem::StructuredBuffer_SRV(5),     // binding 5 lights (M7)
      nvrhi::BindingLayoutItem::StructuredBuffer_SRV(6),     // binding 6 mesh info (P2: MeshInfoGpu)
      nvrhi::BindingLayoutItem::StructuredBuffer_SRV(7),     // binding 7 mesh face normals (M7 NdotL)
      nvrhi::BindingLayoutItem::Texture_SRV(9),              // binding 9 dome env-map (M7-IBL)
      nvrhi::BindingLayoutItem::Sampler(10),                 // binding 10 material sampler
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
      nvrhi::BindingLayoutItem::StructuredBuffer_SRV(26),    // binding 26 mesh indices (§14.5 index-pool view)
      // Bindless material textures. NVRHI's vulkan backend applies
      // ePartiallyBound to every layout entry (vulkan-resource-bindings
      // .cpp:184), so unbound array slots are safe as long as the
      // closesthit's `mat.flags & MATERIAL_FLAG_HAS_BASE_COLOR_MAP`
      // gate prevents access to them. Cap mirrors BINDLESS_TEXTURES_CAP
      // in ShaderInterop.slang. True createBindlessLayout (plan §5
      // ~80K capacity) is a post-v1 sweep — 4096 covers World Lobby + every
      // v1 production scene.
      nvrhi::BindingLayoutItem::Texture_SRV(28).setSize(
          shaderinterop::BINDLESS_TEXTURES_CAP),             // binding 28 bindless textures
      // P2 — interleaved per-vertex normal+tangent attributes (VertexAttribGpu).
      nvrhi::BindingLayoutItem::StructuredBuffer_SRV(29),    // binding 29 mesh vertex attribs
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
  // V2.B (plan §V2.B.1/B.3) — depth 3: raygen→primary closesthit (1) →
  // reflection ray (2) → reflection's shadow/AO ray (3). Opacity
  // transmission no longer recurses (raygen composites it front-to-back
  // at depth 0 via its transmission loop), so stacked translucent layers
  // don't inflate this — the cap stays at the documented 3.
  pipelineDesc.maxRecursionDepth = 3;
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

  // 1-element fallback structured buffers — P2 collapse: ONE buffer per
  // distinct element stride instead of one per binding (see the member
  // comment in PathTracePass.h for the binding ↔ buffer map). Each scene-
  // side structured-buffer binding must point at a non-null buffer whose
  // stride matches the shader's element type even when GpuScene hasn't
  // allocated the real one yet; contents are written every Execute while
  // the matching scene buffer is null. The material fallback packs the
  // same default grey GpuScene reserves at sentinel slot 0, so the
  // no-materials path stays colour-consistent.
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
                                                   "PathTrace.FallbackMaterial");
  _fallbackLightBuffer = makeStructuredFallback(sizeof(shaderinterop::LightGpu),
                                                "PathTrace.FallbackLights");
  _fallbackInstanceInfoBuffer = makeStructuredFallback(sizeof(shaderinterop::InstanceInfoGpu),
                                                       "PathTrace.FallbackInstanceInfo");
  static_assert(sizeof(shaderinterop::MeshInfoGpu) == sizeof(shaderinterop::VertexAttribGpu),
                "bindings 6 + 29 share the 32-byte fallback buffer");
  _fallbackStride32Buffer = makeStructuredFallback(sizeof(shaderinterop::MeshInfoGpu),
                                                   "PathTrace.FallbackStride32");
  _fallbackFloat4Buffer = makeStructuredFallback(sizeof(hlslpp::float4),
                                                 "PathTrace.FallbackFloat4");
  // gMeshUvs is a TIGHT 8-byte float2 stride (see MeshUvPack.h) — NOT the
  // 16-byte hlslpp::float2.
  _fallbackUvBuffer = makeStructuredFallback(2u * sizeof(float), "PathTrace.FallbackMeshUvs");
  _fallbackUintBuffer = makeStructuredFallback(sizeof(std::uint32_t),
                                               "PathTrace.FallbackUint");
  if (!_fallbackMaterialBuffer || !_fallbackLightBuffer || !_fallbackInstanceInfoBuffer
      || !_fallbackStride32Buffer || !_fallbackFloat4Buffer || !_fallbackUvBuffer
      || !_fallbackUintBuffer)
  {
    Logging::Get().Error(log::RENDER,
                         "PathTracePass: createBuffer(per-stride scene fallbacks) failed");
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
  // V2.B (plan §V2.B.1/B.3) — depth 3: raygen→primary closesthit (1) →
  // reflection ray (2) → reflection's shadow/AO ray (3). Opacity
  // transmission no longer recurses (raygen composites it front-to-back
  // at depth 0 via its transmission loop), so stacked translucent layers
  // don't inflate this — the cap stays at the documented 3.
  pipelineDesc.maxRecursionDepth = 3;
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

nvrhi::BindingSetHandle PathTracePass::GetOrCreateBindingSet(RenderTargets const& targets,
                                                             const SceneResources& res) {
  nvrhi::ITexture* output = targets.color;
  // Capture every borrowed-pointer that participates in a binding into
  // one snapshot, then compare against last frame's. A mismatch on
  // ANY field invalidates the cached binding sets — covers the
  // scene-side lazy-allocation flips (GpuScene's first AcquireMaterial
  // / AppendInstance / AddLight that creates a real buffer where a
  // 1×1 fallback used to live), the §14.5 pool-growth handle swaps,
  // the caller-side AOV swaps on resize, and the dome-texture flip
  // when a USD dome's env-map resolves. Indexed init keeps slot order
  // in lockstep with the BindingSlot enum; std::array's element-wise
  // operator== and operator= keep the compare + assign idiomatic
  // without the multi-level pointer cast memcmp/memcpy would need.
  auto slot = [](BindingSlot index) constexpr noexcept { return static_cast<std::size_t>(index); };
  BindingsSnapshot current{};
  current[slot(BindingSlot::Materials)]         = res.materialBuffer;
  current[slot(BindingSlot::InstanceInfo)]      = res.instanceInfoBuffer;
  current[slot(BindingSlot::Lights)]            = res.lightBuffer;
  current[slot(BindingSlot::MeshInfo)]          = res.meshInfoBuffer;
  current[slot(BindingSlot::MeshFaceNormals)]   = res.meshFaceNormalsBuffer;
  current[slot(BindingSlot::DomeTexture)]       = res.domeEnvMapTexture;
  current[slot(BindingSlot::BindlessSampler)]   = res.bindlessSampler;
  current[slot(BindingSlot::MeshUvs)]           = res.meshUvsBuffer;
  current[slot(BindingSlot::MeshIndices)]       = res.meshIndicesBuffer;
  current[slot(BindingSlot::MeshVertexAttribs)] = res.meshVertexAttribsBuffer;
  current[slot(BindingSlot::DomeSampler)]       = res.domeSampler;
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
  const uint32_t bindlessTextureCount = res.bindlessTextureCount;
  std::uint64_t bindlessFingerprint = 0xcbf29ce484222325ULL;
  bindlessFingerprint ^= bindlessTextureCount;
  bindlessFingerprint *= 0x100000001b3ULL;
  for (uint32_t bindlessSlot = 0; bindlessSlot < bindlessTextureCount; ++bindlessSlot)
  {
    const auto ptrAsInt =
        reinterpret_cast<std::uintptr_t>(res.bindlessTextures[bindlessSlot]);
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

  // Slot numbers match the BindingLayoutDesc above (zero offsets) so
  // NVRHI emits Vulkan binding numbers matching the shaders'
  // `[[vk::binding(N, 0)]]` declarations.
  // M5: binding 3 is the materials structured buffer — scene's own
  // (when AcquireMaterial has been called + CommitResources has
  // run) or the 1-element fallback grey (used by cube fixtures
  // that have no materials).
  // M6/P2: binding 4 is the packed instance-info table — scene's
  // own (when CommitResources has built a TLAS) or the 1-element
  // zero fallback (used by truly-empty scenes).
  // M7: binding 5 is the lights buffer — scene's own (when AddLight
  // has been called + CommitResources has run) or the 1-element
  // intensity=0 sentinel fallback (used by unlit M5/M6 fixtures so
  // the closesthit per-light loop falls through to baseColor-only).
  nvrhi::IBuffer* materialBuffer = res.materialBuffer;
  if (materialBuffer == nullptr)
    materialBuffer = _fallbackMaterialBuffer.Get();
  nvrhi::IBuffer* instanceInfoBuffer = res.instanceInfoBuffer;
  if (instanceInfoBuffer == nullptr)
    instanceInfoBuffer = _fallbackInstanceInfoBuffer.Get();
  nvrhi::IBuffer* lightBuffer = res.lightBuffer;
  if (lightBuffer == nullptr)
    lightBuffer = _fallbackLightBuffer.Get();
  // P2 — packed per-mesh info (binding 6) + the element side tables. The
  // 32-byte-stride fallback covers both MeshInfoGpu and VertexAttribGpu.
  nvrhi::IBuffer* meshInfoBuffer = res.meshInfoBuffer;
  if (meshInfoBuffer == nullptr)
    meshInfoBuffer = _fallbackStride32Buffer.Get();
  nvrhi::IBuffer* meshFaceNormalsBuffer = res.meshFaceNormalsBuffer;
  if (meshFaceNormalsBuffer == nullptr)
    meshFaceNormalsBuffer = _fallbackFloat4Buffer.Get();
  nvrhi::IBuffer* meshUvsBuffer = res.meshUvsBuffer;
  if (meshUvsBuffer == nullptr)
    meshUvsBuffer = _fallbackUvBuffer.Get();
  nvrhi::IBuffer* meshIndicesBuffer = res.meshIndicesBuffer;
  if (meshIndicesBuffer == nullptr)
    meshIndicesBuffer = _fallbackUintBuffer.Get();
  nvrhi::IBuffer* meshVertexAttribsBuffer = res.meshVertexAttribsBuffer;
  if (meshVertexAttribsBuffer == nullptr)
    meshVertexAttribsBuffer = _fallbackStride32Buffer.Get();
  // M9-fidelity per-role samplers — dome sampler at binding 33.
  // Falls back to the material sampler when GpuScene hasn't created
  // the dome sampler yet (truly empty scene); identical filter
  // settings, just different addressing modes.
  nvrhi::ISampler* domeSampler = res.domeSampler;
  if (domeSampler == nullptr)
    domeSampler = _fallbackDomeSampler.Get();
  // M7-IBL: dome HDRI texture + the M9-fidelity material sampler at
  // binding 10 (now used for material textures only; dome sampler
  // moved to binding 33 above). Scene's first live dome wins; the
  // 1×1 black fallback texture handles "no dome" — miss shader's
  // "use authored color" branch fires when sampling that all-black.
  nvrhi::ITexture* domeTexture = res.domeEnvMapTexture;
  if (domeTexture == nullptr)
    domeTexture = _fallbackDomeTexture.Get();
  nvrhi::ISampler* materialSampler = res.bindlessSampler;
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
      nvrhi::BindingSetItem::RayTracingAccelStruct(1, res.tlas),
      nvrhi::BindingSetItem::Texture_UAV(2, output),
      nvrhi::BindingSetItem::StructuredBuffer_SRV(3, materialBuffer),
      nvrhi::BindingSetItem::StructuredBuffer_SRV(4, instanceInfoBuffer),
      nvrhi::BindingSetItem::StructuredBuffer_SRV(5, lightBuffer),
      nvrhi::BindingSetItem::StructuredBuffer_SRV(6, meshInfoBuffer),
      nvrhi::BindingSetItem::StructuredBuffer_SRV(7, meshFaceNormalsBuffer),
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
      // §14.5 — the index pool's structured view.
      nvrhi::BindingSetItem::StructuredBuffer_SRV(26, meshIndicesBuffer),
      // P2 — interleaved per-vertex normal+tangent attributes.
      nvrhi::BindingSetItem::StructuredBuffer_SRV(29, meshVertexAttribsBuffer),
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
  nvrhi::ITexture* const missingTexture = res.missingTexture;
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
    nvrhi::ITexture* const sceneTex = res.bindlessTextures[bindlessSlot];
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

  // RFC 0003 — snapshot the scene's borrowed NVRHI resources once per Execute via
  // the renderer-internal accessor (the public Get* getters are gone). Safe here:
  // single-writer §31 — CommitResources ran earlier this frame on this thread.
  const SceneResources res = detail::SceneResourcesAccess::Get(*_scene);

  // Need a TLAS + camera before we can trace anything. M3 callers
  // (HeadlessMode / ViewerMode) populate both at startup and call
  // CommitResources before RenderFrame, so the early-out only
  // fires in degenerate "scene with no instances or camera"
  // configurations.
  if (res.tlas == nullptr || !_scene->HasCamera())
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
  // V2.A.20 — projection mode (0 = perspective, 1 = orthographic).
  // Raygen branches on this to choose between the
  // ray-from-origin-through-NDC (perspective) and
  // per-pixel-origin-with-constant-direction (orthographic) primary
  // ray constructions.
  cameraUniforms.projectionMode = camera.projectionMode;
  cameraUniforms._camPad1 = 0.0f;
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
  // this up for World Lobby-scale scenes (~50 m) without touching shader.
  frameUi.worldPosPeriod = (context.settings != nullptr
                            && context.settings->worldPosPeriod > 0.0f)
                               ? context.settings->worldPosPeriod
                               : 10.0f;
  frameUi._reservedUi1 = 0u;
  frameUi._reservedUi2 = 0u;
  frameUi._reservedUi3 = 0u;
  commandList->writeBuffer(_frameUiBuffer.Get(), &frameUi, sizeof(frameUi));

  // ---- Scene-buffer fallback uploads --------------------------------
  // Lazy-allocated scene-side buffers share the same shape: "if the
  // scene hasn't allocated yet, write a tiny default into the matching
  // per-stride 1-element fallback buffer so the binding point sees
  // something safe." P2 collapsed the old 14 per-binding fallbacks to
  // one per distinct stride; the table below maps each scene resource
  // to its fallback + default bytes. The default-grey OpenPBR material
  // (baseColor 0.8 grey, roughness 0.5, IoR 1.5, all texture slots =
  // INVALID_BINDLESS_TEXTURE) packs the same default GpuScene reserves
  // at sentinel slot 0, so an instance with `material = Invalid`
  // renders a recognisable visible grey instead of black. Built on
  // first call and cached in a function-local static.
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
  static const shaderinterop::LightGpu        FALLBACK_LIGHT_DISABLED{};
  static const shaderinterop::InstanceInfoGpu FALLBACK_INSTANCE_INFO_ZERO{};
  static const shaderinterop::MeshInfoGpu     FALLBACK_STRIDE32_ZERO{};
  static const std::uint32_t                  FALLBACK_UINT_ZERO = 0u;
  static const float                          FALLBACK_FLOAT4_ZERO[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  static const float                          FALLBACK_FLOAT2_ZERO[2] = {0.0f, 0.0f};

  struct BufferFallbackSpec {
    const void*     sceneResource;  // null ⇒ the fallback must carry the default.
    nvrhi::IBuffer* fallback;
    const void*     defaultBytes;
    std::size_t     defaultByteSize;
  };
  const std::array<BufferFallbackSpec, 7> bufferFallbacks{{
      {res.materialBuffer, _fallbackMaterialBuffer.Get(),
       &FALLBACK_MATERIAL_GREY, sizeof(FALLBACK_MATERIAL_GREY)},
      {res.lightBuffer, _fallbackLightBuffer.Get(),
       &FALLBACK_LIGHT_DISABLED, sizeof(FALLBACK_LIGHT_DISABLED)},
      {res.instanceInfoBuffer, _fallbackInstanceInfoBuffer.Get(),
       &FALLBACK_INSTANCE_INFO_ZERO, sizeof(FALLBACK_INSTANCE_INFO_ZERO)},
      // Bindings 6 (MeshInfoGpu) + 29 (VertexAttribGpu) share the 32-byte zero
      // fallback — two table rows so EITHER scene buffer being null re-arms it.
      {res.meshInfoBuffer, _fallbackStride32Buffer.Get(),
       &FALLBACK_STRIDE32_ZERO, sizeof(FALLBACK_STRIDE32_ZERO)},
      {res.meshVertexAttribsBuffer, _fallbackStride32Buffer.Get(),
       &FALLBACK_STRIDE32_ZERO, sizeof(FALLBACK_STRIDE32_ZERO)},
      {res.meshFaceNormalsBuffer, _fallbackFloat4Buffer.Get(),
       FALLBACK_FLOAT4_ZERO, sizeof(FALLBACK_FLOAT4_ZERO)},
      {res.meshUvsBuffer, _fallbackUvBuffer.Get(),
       FALLBACK_FLOAT2_ZERO, sizeof(FALLBACK_FLOAT2_ZERO)},
  }};
  for (const BufferFallbackSpec& spec : bufferFallbacks)
  {
    if (spec.sceneResource == nullptr)
      commandList->writeBuffer(spec.fallback, spec.defaultBytes, spec.defaultByteSize);
  }
  // Index-pool fallback (binding 26): one zero uint.
  if (res.meshIndicesBuffer == nullptr)
    commandList->writeBuffer(_fallbackUintBuffer.Get(), &FALLBACK_UINT_ZERO,
                             sizeof(FALLBACK_UINT_ZERO));

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
  if (res.domeEnvMapTexture == nullptr)
  {
    static const float FALLBACK_DOME_PIXEL[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    commandList->writeTexture(_fallbackDomeTexture.Get(), 0, 0, FALLBACK_DOME_PIXEL,
                              sizeof(FALLBACK_DOME_PIXEL));
  }

  // ---- Bind + dispatch ----------------------------------------------
  const nvrhi::BindingSetHandle bindingSet = GetOrCreateBindingSet(*context.targets, res);
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
