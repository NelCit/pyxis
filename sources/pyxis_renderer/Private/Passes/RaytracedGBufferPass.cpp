// Pyxis renderer — extended G-buffer pass (RTX-alignment design, WP2-core).

#include "Passes/RaytracedGBufferPass.h"

#include "Passes/SceneBindings.h"
#include "RenderGraph/PassContext.h"
#include "RenderGraph/ShaderLoad.h"
#include "Scene/SceneResources.h"  // RFC 0003 — renderer-internal scene view.

#include <Pyxis/Platform/FileSystem/AssetLocator.h>
#include <Pyxis/Platform/Logging/Log.h>
#include <Pyxis/Platform/Logging/LogCategories.h>
#include <Pyxis/Renderer/Descs/RenderSettings.h>
#include <Pyxis/Renderer/Descs/RenderTargets.h>
#include <Pyxis/Renderer/GpuScene.h>
#include <Pyxis/Renderer/Profiler.h>

// The dual-language ShaderInterop header is on the renderer's private
// include path (resources/shaders/). It declares CameraUniforms,
// VisibilityGpu and FrameUiUniforms for the C++ side — same definitions
// the shaders see, kept in lockstep by construction.
#include "ShaderInterop.slang"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <hlsl++.h>
#include <iterator>
#include <string>

namespace pyxis {

namespace {

static_assert(sizeof(shaderinterop::VisibilityGpu) == 16,
              "VisibilityGpu is 16 bytes (RTX-alignment WP2 pack) — EnsureVisibilityBuffer "
              "allocates width*height*16 with this exact stride; see "
              "resources/shaders/ShaderInterop.slang.");
static_assert(sizeof(shaderinterop::FrameUiUniforms) == 32,
              "FrameUiUniforms is 32 bytes (2 cbuffer rows). Only the row-0 picker pixel is "
              "live here; the debugViewMode / worldPosPeriod slots are TonemapUniforms' "
              "domain (kept for the frozen layout, written 0).");

// Thin wrapper over the shared LoadSpirvShader (RenderGraph/ShaderLoad.h) that
// pins the "RaytracedGBufferPass" log prefix so the call sites below stay terse.
nvrhi::ShaderHandle LoadSpirv(nvrhi::IDevice* device, std::string_view path,
                              nvrhi::ShaderType stage, const char* entry) noexcept {
  return LoadSpirvShader(device, path, stage, entry, "RaytracedGBufferPass");
}

// P5 (design D2) — one RT pipeline + SBT per camera projection mode.
// Both default-null on a failed build; created together because the
// SBT derives from its pipeline.
struct PipelineVariant {
  nvrhi::rt::PipelineHandle pipeline;
  nvrhi::rt::ShaderTableHandle shaderTable;
};

// Build one projection-mode variant from the supplied BASE shader
// handles. Specialization map: only the raygen's module declares a
// spec constant (PROJECTION_MODE id 0 via its camera_ray.slang
// include); the thin closesthit / miss / anyhit include neither
// camera_ray.slang's specialized branch nor shading.slang's lighting
// path, so they pass through unspecialized. maxRecursionDepth stays 1
// — this pipeline traces the primary ray only, so the lighting pass's
// recursion budget doesn't apply here.
// §30.5 — more than five inputs, so they ride in a Desc struct (the
// nvrhi::*Desc idiom this function itself uses internally). No
// shadowMiss: this pipeline has no shadow rays.
struct PipelineVariantDesc {
  nvrhi::IDevice* device = nullptr;
  // WP2-core — TWO binding layouts now: Set 0 is the shared
  // SceneBindings layout (owned by PyxisRenderer, immutable across
  // every RT pass); Set 1 is this pass's OWN I/O layout.
  nvrhi::IBindingLayout* sceneLayout = nullptr;
  nvrhi::IBindingLayout* passLayout = nullptr;
  nvrhi::IShader* raygen = nullptr;
  nvrhi::IShader* miss = nullptr;
  nvrhi::IShader* closestHit = nullptr;
  nvrhi::IShader* anyHit = nullptr;
  uint32_t projectionMode = 0;
  const char* logContext = "";
};

PipelineVariant BuildPipelineVariant(const PipelineVariantDesc& desc) noexcept {
  nvrhi::IDevice* const device = desc.device;
  nvrhi::IShader* const raygen = desc.raygen;
  nvrhi::IShader* const miss = desc.miss;
  nvrhi::IShader* const closestHit = desc.closestHit;
  nvrhi::IShader* const anyHit = desc.anyHit;
  const uint32_t projectionMode = desc.projectionMode;
  const char* const logContext = desc.logContext;
  auto& log = Logging::Get();
  const nvrhi::ShaderSpecialization raygenConstants[] = {
      nvrhi::ShaderSpecialization::UInt32(shaderinterop::SPEC_ID_PROJECTION_MODE,
                                          projectionMode),
  };
  const nvrhi::ShaderHandle specRaygen = device->createShaderSpecialization(
      raygen, raygenConstants, static_cast<uint32_t>(std::size(raygenConstants)));
  if (!specRaygen)
  {
    log.Error(log::RENDER, std::string{logContext}
                               + ": createShaderSpecialization failed (projectionMode="
                               + std::to_string(projectionMode) + ")");
    return {};
  }

  // Pipeline state — three shader stages + one hit group bundling the
  // thin closesthit with the shared-alpha-test anyhit.
  nvrhi::rt::PipelineDesc pipelineDesc;
  pipelineDesc.shaders = {
      nvrhi::rt::PipelineShaderDesc{}.setExportName("RayGenMain").setShader(specRaygen),
      nvrhi::rt::PipelineShaderDesc{}.setExportName("MissMain").setShader(miss),
  };
  pipelineDesc.hitGroups = {
      nvrhi::rt::PipelineHitGroupDesc{}
          .setExportName("HitGroupDefault")
          .setClosestHitShader(closestHit)
          .setAnyHitShader(anyHit),
  };
  // WP2-core — Set 0 (SceneBindings, shared verbatim with
  // RaytracedLightingPass) first, this pass's OWN Set 1 second. NVRHI
  // binds one descriptor set per layout, index-matched at trace time
  // (see Execute()'s `state.bindings = {sceneSet, passSet}`).
  pipelineDesc.globalBindingLayouts = {desc.sceneLayout, desc.passLayout};
  pipelineDesc.maxRecursionDepth = 1;
  PipelineVariant variant;
  variant.pipeline = device->createRayTracingPipeline(pipelineDesc);
  if (!variant.pipeline)
  {
    log.Error(log::RENDER, std::string{logContext}
                               + ": createRayTracingPipeline failed (projectionMode="
                               + std::to_string(projectionMode) + ")");
    return {};
  }

  // Shader binding table — one raygen, one miss, one hit group; static
  // for the whole run (rebuilt only by ReloadShaders).
  variant.shaderTable = variant.pipeline->createShaderTable();
  if (!variant.shaderTable)
  {
    log.Error(log::RENDER, std::string{logContext}
                               + ": createShaderTable failed (projectionMode="
                               + std::to_string(projectionMode) + ")");
    return {};
  }
  variant.shaderTable->setRayGenerationShader("RayGenMain");
  variant.shaderTable->addMissShader("MissMain");  // miss-index 0: hitT = -1 sentinel
  variant.shaderTable->addHitGroup("HitGroupDefault");
  return variant;
}

}  // namespace

RaytracedGBufferPass::RaytracedGBufferPass(nvrhi::IDevice* device, GpuScene& scene,
                                           SceneBindings& sceneBindings)
    : _device(device), _scene(&scene), _sceneBindings(&sceneBindings) {
  if (!_sceneBindings->IsOperational())
  {
    Logging::Get().Error(log::RENDER,
                         "RaytracedGBufferPass: SceneBindings not operational; pass will skip");
    return;
  }

  const AssetLocator locator;
  const Path raygenPath = locator.LocateResource("shaders/raytraced_gbuffer_raygen.spv");
  const Path missPath = locator.LocateResource("shaders/raytraced_gbuffer_miss.spv");
  const Path closestHitPath =
      locator.LocateResource("shaders/raytraced_gbuffer_closesthit.spv");
  const Path anyHitPath = locator.LocateResource("shaders/raytraced_gbuffer_anyhit.spv");

  // Slang emits the SPIR-V `OpEntryPoint` name as `"main"` for every
  // [shader(...)]-attributed function regardless of the source-side
  // function name (verified via spirv-dis) — even though WP2-core's
  // raytraced_gbuffer.slang now names its four functions RayGenMain /
  // ClosestHitMain / MissMain / AnyHitMain (distinct names are required
  // WITHIN the one source file; slangc's `-entry <name>` selects which
  // one to compile per invocation, and each invocation's SPIR-V still
  // reports its single entry point as "main"). Passing anything else
  // here trips VUID-VkPipelineShaderStageCreateInfo-pName-00707.
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

  // Set-1 layout — this pass's OWN I/O (Set 0 is the shared
  // SceneBindings layout). bindingOffsets zeroed so the slot numbers
  // below map 1:1 onto raytraced_gbuffer.slang's
  // `[[vk::binding(N, 1)]]` declarations (same convention every layout
  // in this codebase uses).
  nvrhi::BindingLayoutDesc layoutDesc;
  layoutDesc.visibility = nvrhi::ShaderType::AllRayTracing;
  layoutDesc.bindingOffsets.shaderResource = 0;
  layoutDesc.bindingOffsets.sampler = 0;
  layoutDesc.bindingOffsets.constantBuffer = 0;
  layoutDesc.bindingOffsets.unorderedAccess = 0;
  layoutDesc.bindings = {
      nvrhi::BindingLayoutItem::StructuredBuffer_UAV(0),  // 0  visibility (packed 16 B)
      nvrhi::BindingLayoutItem::Texture_UAV(1),           // 1  gAlbedo
      nvrhi::BindingLayoutItem::Texture_UAV(2),           // 2  gNormalRoughness
      nvrhi::BindingLayoutItem::Texture_UAV(3),           // 3  gEmissive
      nvrhi::BindingLayoutItem::Texture_UAV(4),           // 4  gViewZ
      nvrhi::BindingLayoutItem::Texture_UAV(5),           // 5  gMotionVector
      nvrhi::BindingLayoutItem::Texture_UAV(6),           // 6  gOutNormal
      nvrhi::BindingLayoutItem::Texture_UAV(7),           // 7  gOutDepth
      nvrhi::BindingLayoutItem::Texture_UAV(8),           // 8  gOutPrimId
      nvrhi::BindingLayoutItem::Texture_UAV(9),           // 9  gOutMaterialId
      nvrhi::BindingLayoutItem::Texture_UAV(10),          // 10 gOutBaseColor
      nvrhi::BindingLayoutItem::Texture_UAV(11),          // 11 gOutWorldPos
      nvrhi::BindingLayoutItem::Texture_UAV(12),          // 12 gOutElementId
      nvrhi::BindingLayoutItem::Texture_UAV(13),          // 13 gOutNormalEye
      nvrhi::BindingLayoutItem::Texture_UAV(14),          // 14 gOutWorldPosEye
      nvrhi::BindingLayoutItem::StructuredBuffer_UAV(15), // 15 pickResult
      nvrhi::BindingLayoutItem::ConstantBuffer(16),       // 16 FrameUiUniforms
      nvrhi::BindingLayoutItem::Texture_UAV(17),          // 17 gSpecularAlbedo (DLSS Stage 2b)
  };
  _passLayout = _device->createBindingLayout(layoutDesc);
  if (!_passLayout)
  {
    Logging::Get().Error(log::RENDER, "RaytracedGBufferPass: createBindingLayout(Set 1) failed");
    return;
  }

  // P5 (design D2) — the PERSPECTIVE pipeline variant (projectionMode
  // 0, the v1 default) is built eagerly here; the ORTHOGRAPHIC variant
  // is built lazily by EnsureProjectionPipeline the first time the
  // camera reports it (PyxisRenderer's CPU frame path — never inside
  // Execute). maxRecursionDepth = 1: this pipeline traces the primary
  // ray only (no shading, no secondary rays).
  PipelineVariant perspective =
      BuildPipelineVariant({.device = _device,
                            .sceneLayout = _sceneBindings->Layout(),
                            .passLayout = _passLayout,
                            .raygen = _raygenShader,
                            .miss = _missShader,
                            .closestHit = _closestHitShader,
                            .anyHit = _anyHitShader,
                            .projectionMode = 0u,
                            .logContext = "RaytracedGBufferPass"});
  if (!perspective.pipeline || !perspective.shaderTable)
    return;  // BuildPipelineVariant already logged the failing step.
  _pipelines[0]    = std::move(perspective.pipeline);
  _shaderTables[0] = std::move(perspective.shaderTable);

  // M7 follow-up — viewer-only per-frame UI cbuffer at Set-1 binding 16
  // (the picker's mousePixel gate). Moved here from RaytracedLightingPass
  // (WP2-core — the id/geometry AOVs + pick latch moved with it).
  nvrhi::BufferDesc uiCbDesc;
  uiCbDesc.byteSize = sizeof(shaderinterop::FrameUiUniforms);
  uiCbDesc.debugName = "RaytracedGBuffer.FrameUiUniforms";
  uiCbDesc.isConstantBuffer = true;
  uiCbDesc.initialState = nvrhi::ResourceStates::ConstantBuffer;
  uiCbDesc.keepInitialState = true;
  _frameUiBuffer = _device->createBuffer(uiCbDesc);
  if (!_frameUiBuffer)
  {
    Logging::Get().Error(log::RENDER, "RaytracedGBufferPass: createBuffer(FrameUiUniforms) failed");
    return;
  }

  // ---- Tiny no-UAV fallbacks for the id/geometry AOVs + pick buffer
  // (moved here from RaytracedLightingPass, WP2-core). Bound when the
  // caller doesn't supply that AOV (headless mode + the M2-era
  // color-only paths). --------------------------------------------
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
  struct FallbackSpec {
    nvrhi::TextureHandle RaytracedGBufferPass::* member;
    nvrhi::Format format;
    const char* debugName;
  };
  const std::array<FallbackSpec, 11> aovFallbacks{{
      {&RaytracedGBufferPass::_fallbackNormalAov,      nvrhi::Format::RGBA16_FLOAT, "RaytracedGBuffer.FbNormalAov"     },
      {&RaytracedGBufferPass::_fallbackDepthAov,       nvrhi::Format::R32_FLOAT,    "RaytracedGBuffer.FbDepthAov"      },
      {&RaytracedGBufferPass::_fallbackPrimIdAov,      nvrhi::Format::R32_UINT,     "RaytracedGBuffer.FbPrimIdAov"     },
      {&RaytracedGBufferPass::_fallbackMaterialAov,    nvrhi::Format::R32_UINT,     "RaytracedGBuffer.FbMaterialAov"   },
      {&RaytracedGBufferPass::_fallbackBaseColorAov,   nvrhi::Format::RGBA16_FLOAT, "RaytracedGBuffer.FbBaseColorAov"  },
      {&RaytracedGBufferPass::_fallbackWorldPosAov,    nvrhi::Format::RGBA32_FLOAT, "RaytracedGBuffer.FbWorldPosAov"   },
      {&RaytracedGBufferPass::_fallbackElementIdAov,   nvrhi::Format::R32_UINT,     "RaytracedGBuffer.FbElementIdAov"  },
      {&RaytracedGBufferPass::_fallbackNormalEyeAov,   nvrhi::Format::RGBA16_FLOAT, "RaytracedGBuffer.FbNormalEyeAov"  },
      {&RaytracedGBufferPass::_fallbackWorldPosEyeAov, nvrhi::Format::RGBA32_FLOAT, "RaytracedGBuffer.FbWorldPosEyeAov"},
      {&RaytracedGBufferPass::_fallbackViewZAov,       nvrhi::Format::R32_FLOAT,    "RaytracedGBuffer.FbViewZAov"      },
      {&RaytracedGBufferPass::_fallbackMotionVectorAov,nvrhi::Format::RG16_FLOAT,   "RaytracedGBuffer.FbMotionVectorAov"},
  }};
  for (const FallbackSpec& spec : aovFallbacks)
  {
    this->*spec.member = makeAovFallback(spec.format, spec.debugName);
    if (!(this->*spec.member))
    {
      Logging::Get().Error(log::RENDER, std::string{"RaytracedGBufferPass: AOV fallback create failed: "}
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
  pickFbDesc.debugName = "RaytracedGBuffer.FbPickResult";
  pickFbDesc.initialState = nvrhi::ResourceStates::UnorderedAccess;
  pickFbDesc.keepInitialState = true;
  _fallbackPickResult = _device->createBuffer(pickFbDesc);
  if (!_fallbackPickResult)
  {
    Logging::Get().Error(log::RENDER, "RaytracedGBufferPass: createBuffer(FbPickResult) failed");
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
  desc.canHaveUAVs = true;  // raygen writes gVisibility (Set 1 binding 0).
  desc.canHaveRawViews = false;
  desc.canHaveTypedViews = false;
  desc.format = nvrhi::Format::UNKNOWN;
  desc.debugName = "RaytracedGBuffer.visibility";
  desc.initialState = nvrhi::ResourceStates::UnorderedAccess;
  desc.keepInitialState = true;
  _visibility = _device->createBuffer(desc);
  _visibilityW = width;
  _visibilityH = height;
  // P6 review — the binding-set cache keys on the visibility pointer, and
  // NVRHI binding sets hold STRONG refs to every bound resource: without
  // this clear, each resize would leave a cached set pinning the retired
  // buffers in VRAM (up to MAX_CACHE_ENTRIES-1 stale buffers). CPU frame
  // path (PyxisRenderer calls this before the graph walks), so dropping
  // descriptor sets here is legal.
  _bindingSetCache.clear();
  _lastBindings = {};
  if (!_visibility)
  {
    // §30.6 — no silent failures: a null handle here degrades the whole
    // frame chain (both RT passes early-out on null visibility). Latched
    // so the per-frame Ensure call doesn't spam.
    if (!_visibilityCreateFailedLogged)
    {
      Logging::Get().Error(log::RENDER,
                           "RaytracedGBufferPass: createBuffer(visibility, "
                               + std::to_string(desc.byteSize)
                               + " bytes) failed; RT passes will skip");
      _visibilityCreateFailedLogged = true;
    }
    return nullptr;
  }
  _visibilityCreateFailedLogged = false;
  // Fresh buffer ⇒ contents undefined; Execute clears it to the miss
  // pattern before anything else can read it (belt-and-braces for
  // asymmetric pass failures — see IsOperational).
  _visibilityNeedsClear = true;

  // WP2-core — (re)create the material G-buffer / denoiser-guide
  // textures at the SAME dims, in lockstep with the visibility buffer
  // (one resize cadence for this pass's four owned per-pixel scratch
  // resources). Unconsumed by any pass yet (the Phase B signal passes
  // are out of this WP's scope) — written every frame as the
  // foundation those passes build on.
  auto makeGuideTexture = [&](nvrhi::Format fmt, const char* debugName) {
    nvrhi::TextureDesc guideDesc;
    guideDesc.width = width;
    guideDesc.height = height;
    guideDesc.format = fmt;
    guideDesc.dimension = nvrhi::TextureDimension::Texture2D;
    guideDesc.isUAV = true;
    // RTX-alignment design, WP2-final — ALL THREE guides are read as
    // Texture_SRVs now (gNormalRoughness by AmbientOcclusionPass /
    // ReflectionsPass since WP2-signals; gAlbedo + gEmissive by
    // CompositePass since WP2-final), so every guide needs sampled-image
    // usage in addition to the UAV write this raygen does. WP2-final
    // BUG-FIX NOTE: gAlbedo/gEmissive were UAV-only in WP2-signals
    // ("unconsumed"); binding a texture without sampled-image usage as an
    // SRV is invalid Vulkan usage that silently read ZEROS on the lab
    // driver — CompositePass's whole diffuse re-modulation collapsed to
    // black until this flag was set.
    guideDesc.isShaderResource = true;
    guideDesc.debugName = debugName;
    guideDesc.initialState = nvrhi::ResourceStates::UnorderedAccess;
    guideDesc.keepInitialState = true;
    return _device->createTexture(guideDesc);
  };
  _gAlbedo = makeGuideTexture(nvrhi::Format::RGBA16_FLOAT, "RaytracedGBuffer.gAlbedo");
  _gNormalRoughness = makeGuideTexture(nvrhi::Format::RGBA16_FLOAT,
                                       "RaytracedGBuffer.gNormalRoughness");
  _gEmissive = makeGuideTexture(nvrhi::Format::RGBA16_FLOAT, "RaytracedGBuffer.gEmissive");
  // DLSS Stage 2b — RR's kBufferTypeSpecularAlbedo guide, same resize
  // cadence as the three guides above.
  _gSpecularAlbedo =
      makeGuideTexture(nvrhi::Format::RGBA16_FLOAT, "RaytracedGBuffer.gSpecularAlbedo");
  if (!_gAlbedo || !_gNormalRoughness || !_gEmissive || !_gSpecularAlbedo)
  {
    Logging::Get().Error(log::RENDER,
                         "RaytracedGBufferPass: createTexture(material G-buffer) failed; "
                         "pass will skip");
    return nullptr;
  }
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

  // P5 — rebuild base + specialized raygen + pipelines + SBTs TOGETHER.
  // Variant 0 (perspective) always exists; variant 1 (orthographic) is
  // rebuilt only if it had been materialized — otherwise
  // EnsureProjectionPipeline lazily rebuilds it from the NEW base
  // shaders on demand.
  PipelineVariant newPerspective =
      BuildPipelineVariant({.device = _device,
                            .sceneLayout = _sceneBindings->Layout(),
                            .passLayout = _passLayout,
                            .raygen = newRaygen,
                            .miss = newMiss,
                            .closestHit = newClosestHit,
                            .anyHit = newAnyHit,
                            .projectionMode = 0u,
                            .logContext = "RaytracedGBufferPass::ReloadShaders"});
  if (!newPerspective.pipeline || !newPerspective.shaderTable)
  {
    log.Error(log::RENDER,
              "RaytracedGBufferPass::ReloadShaders: perspective variant rebuild failed; "
              "keeping old pipeline");
    return false;
  }
  PipelineVariant newOrthographic;
  if (_pipelines[1])
  {
    newOrthographic =
        BuildPipelineVariant({.device = _device,
                              .sceneLayout = _sceneBindings->Layout(),
                              .passLayout = _passLayout,
                              .raygen = newRaygen,
                              .miss = newMiss,
                              .closestHit = newClosestHit,
                              .anyHit = newAnyHit,
                              .projectionMode = 1u,
                              .logContext = "RaytracedGBufferPass::ReloadShaders"});
    if (!newOrthographic.pipeline || !newOrthographic.shaderTable)
    {
      log.Error(log::RENDER,
                "RaytracedGBufferPass::ReloadShaders: orthographic variant rebuild failed; "
                "keeping old pipeline");
      return false;
    }
  }

  // Atomic-ish swap — render thread only, same contract as the
  // lighting pass: once all handles flip together the next Execute
  // picks up the new pipelines + tables.
  _raygenShader     = std::move(newRaygen);
  _missShader       = std::move(newMiss);
  _closestHitShader = std::move(newClosestHit);
  _anyHitShader     = std::move(newAnyHit);
  _pipelines[0]     = std::move(newPerspective.pipeline);
  _shaderTables[0]  = std::move(newPerspective.shaderTable);
  // Null when the orthographic variant wasn't materialized pre-reload.
  _pipelines[1]     = std::move(newOrthographic.pipeline);
  _shaderTables[1]  = std::move(newOrthographic.shaderTable);
  _variantBuildFailed = {};
  _shadersOk = true;
  log.Info(log::RENDER, "RaytracedGBufferPass::ReloadShaders: reload OK");
  return true;
}

void RaytracedGBufferPass::EnsureProjectionPipeline() {
  if (!_shadersOk || _scene == nullptr || !_scene->HasCamera())
    return;
  // Same selection Execute uses: anything but 1 (including out-of-range
  // garbage) falls to perspective — matching the shader branch's
  // `PROJECTION_MODE == 1u` shape exactly.
  const std::size_t variant = (_scene->GetCamera().projectionMode == 1u) ? 1u : 0u;
  if (_pipelines[variant] || _variantBuildFailed[variant])
    return;
  // CPU-frame-path creation (PyxisRenderer calls this before the graph
  // walks) — pipeline variants are NEVER created inside Execute
  // (§30.10 no allocations in the per-frame pass body).
  PipelineVariant built = BuildPipelineVariant(
      {.device = _device,
       .sceneLayout = _sceneBindings->Layout(),
       .passLayout = _passLayout,
       .raygen = _raygenShader,
       .miss = _missShader,
       .closestHit = _closestHitShader,
       .anyHit = _anyHitShader,
       .projectionMode = static_cast<uint32_t>(variant),
       .logContext = "RaytracedGBufferPass::EnsureProjectionPipeline"});
  if (!built.pipeline || !built.shaderTable)
  {
    // Latched so the per-frame hook doesn't retry (and re-log) forever;
    // ReloadShaders resets the latch.
    _variantBuildFailed[variant] = true;
    return;
  }
  _pipelines[variant]    = std::move(built.pipeline);
  _shaderTables[variant] = std::move(built.shaderTable);
}

nvrhi::BindingSetHandle RaytracedGBufferPass::GetOrCreateBindingSet(
    nvrhi::IBuffer* visibility, RenderTargets const& targets) {
  // Snapshot the caller-owned AOV target pointers; any flip (swapchain
  // rebuild, caller re-pointing an AOV) invalidates every cached set —
  // same scheme RaytracedLightingPass's (pre-WP2) BindingsSnapshot used.
  auto slot = [](BindingSlot index) constexpr noexcept { return static_cast<std::size_t>(index); };
  BindingsSnapshot current{};
  current[slot(BindingSlot::NormalAov)]       = targets.normalAov;
  current[slot(BindingSlot::DepthAov)]        = targets.depthAov;
  current[slot(BindingSlot::PrimIdAov)]       = targets.primIdAov;
  current[slot(BindingSlot::MaterialIdAov)]   = targets.materialIdAov;
  current[slot(BindingSlot::BaseColorAov)]    = targets.baseColorAov;
  current[slot(BindingSlot::WorldPosAov)]     = targets.worldPosAov;
  current[slot(BindingSlot::ElementIdAov)]    = targets.elementIdAov;
  current[slot(BindingSlot::NormalEyeAov)]    = targets.normalEyeAov;
  current[slot(BindingSlot::WorldPosEyeAov)]  = targets.worldPosEyeAov;
  current[slot(BindingSlot::ViewZAov)]        = targets.viewZAov;
  current[slot(BindingSlot::MotionVectorAov)] = targets.motionVector;
  current[slot(BindingSlot::PickResult)]      = targets.pickResult;
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

  nvrhi::ITexture* normalAov = targets.normalAov;
  if (normalAov == nullptr) normalAov = _fallbackNormalAov.Get();
  nvrhi::ITexture* depthAov = targets.depthAov;
  if (depthAov == nullptr) depthAov = _fallbackDepthAov.Get();
  nvrhi::ITexture* primIdAov = targets.primIdAov;
  if (primIdAov == nullptr) primIdAov = _fallbackPrimIdAov.Get();
  nvrhi::ITexture* materialAov = targets.materialIdAov;
  if (materialAov == nullptr) materialAov = _fallbackMaterialAov.Get();
  nvrhi::ITexture* baseColorAov = targets.baseColorAov;
  if (baseColorAov == nullptr) baseColorAov = _fallbackBaseColorAov.Get();
  nvrhi::ITexture* worldPosAov = targets.worldPosAov;
  if (worldPosAov == nullptr) worldPosAov = _fallbackWorldPosAov.Get();
  nvrhi::ITexture* elementIdAov = targets.elementIdAov;
  if (elementIdAov == nullptr) elementIdAov = _fallbackElementIdAov.Get();
  nvrhi::ITexture* normalEyeAov = targets.normalEyeAov;
  if (normalEyeAov == nullptr) normalEyeAov = _fallbackNormalEyeAov.Get();
  nvrhi::ITexture* worldPosEyeAov = targets.worldPosEyeAov;
  if (worldPosEyeAov == nullptr) worldPosEyeAov = _fallbackWorldPosEyeAov.Get();
  nvrhi::ITexture* viewZAov = targets.viewZAov;
  if (viewZAov == nullptr) viewZAov = _fallbackViewZAov.Get();
  nvrhi::ITexture* motionVectorAov = targets.motionVector;
  if (motionVectorAov == nullptr) motionVectorAov = _fallbackMotionVectorAov.Get();
  nvrhi::IBuffer* pickBuffer = targets.pickResult;
  if (pickBuffer == nullptr) pickBuffer = _fallbackPickResult.Get();

  nvrhi::BindingSetDesc setDesc;
  setDesc.bindings = {
      nvrhi::BindingSetItem::StructuredBuffer_UAV(0, visibility),
      nvrhi::BindingSetItem::Texture_UAV(1, _gAlbedo.Get()),
      nvrhi::BindingSetItem::Texture_UAV(2, _gNormalRoughness.Get()),
      nvrhi::BindingSetItem::Texture_UAV(3, _gEmissive.Get()),
      nvrhi::BindingSetItem::Texture_UAV(4, viewZAov),
      nvrhi::BindingSetItem::Texture_UAV(5, motionVectorAov),
      nvrhi::BindingSetItem::Texture_UAV(6, normalAov),
      nvrhi::BindingSetItem::Texture_UAV(7, depthAov),
      nvrhi::BindingSetItem::Texture_UAV(8, primIdAov),
      nvrhi::BindingSetItem::Texture_UAV(9, materialAov),
      nvrhi::BindingSetItem::Texture_UAV(10, baseColorAov),
      nvrhi::BindingSetItem::Texture_UAV(11, worldPosAov),
      nvrhi::BindingSetItem::Texture_UAV(12, elementIdAov),
      nvrhi::BindingSetItem::Texture_UAV(13, normalEyeAov),
      nvrhi::BindingSetItem::Texture_UAV(14, worldPosEyeAov),
      nvrhi::BindingSetItem::StructuredBuffer_UAV(15, pickBuffer),
      nvrhi::BindingSetItem::ConstantBuffer(16, _frameUiBuffer),
      nvrhi::BindingSetItem::Texture_UAV(17, _gSpecularAlbedo.Get()),
  };
  nvrhi::BindingSetHandle set = _device->createBindingSet(setDesc, _passLayout);
  _bindingSetCache[visibility] = set;
  return set;
}

void RaytracedGBufferPass::Execute(nvrhi::ICommandList* commandList,
                                   const PassContext& context) {
  if (commandList == nullptr || context.targets == nullptr)
    return;
  // The visibility buffer this pass owns, threaded back through the
  // context by PyxisRenderer (null when no display target is bound — the
  // same "nothing to render into" early-out the lighting pass takes).
  nvrhi::IBuffer* const visibility = context.visibility;
  if (visibility == nullptr || visibility != _visibility.Get())
    return;

  // P6 review — consume the fresh-buffer clear BEFORE any pipeline/
  // shader early-out below: a freshly created buffer has undefined
  // contents, and the gates from here on are pass-LOCAL (_shadersOk,
  // variant pipeline), so this pass can skip its dispatch while the
  // lighting pass still runs. 0xBF800000 is the bit pattern of
  // float -1.0f — every unwritten VisibilityGpu record reads
  // hitT = -1.0, which the lighting raygen treats as a miss instead of
  // shading from garbage. hitT sits at word 0 of every 4-word (16 B,
  // WP2-packed) record, so the uniform fill still produces a valid
  // miss pattern (see ShaderInterop.slang's VisibilityGpu doc comment).
  if (_visibilityNeedsClear)
  {
    commandList->clearBufferUInt(visibility, 0xBF800000u);
    _visibilityNeedsClear = false;
  }

  if (!_shadersOk || !_gAlbedo || !_gNormalRoughness || !_gEmissive || !_gSpecularAlbedo)
    return;
  // WP2-core — the shared Set-0 binding set, built ONCE per frame by
  // PyxisRenderer::RenderFrame (before the graph walks) via
  // SceneBindings::Update. Null exactly when there's nothing to trace
  // yet (no TLAS / SceneBindings ctor failure) — mirrors the pre-WP2
  // `res.tlas == nullptr` early-out below.
  if (context.sceneBindingSet == nullptr)
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

  // P5 — select the projection-mode pipeline variant. Pure array
  // lookup (§30.10 — no creation here): EnsureProjectionPipeline ran
  // on PyxisRenderer's CPU frame path BEFORE the graph walked. Reads
  // the same CameraDesc::projectionMode the lighting pass selects by,
  // so both RT passes run the same variant every frame.
  const CameraDesc& camera = _scene->GetCamera();
  const std::size_t projectionVariant = (camera.projectionMode == 1u) ? 1u : 0u;
  if (!_pipelines[projectionVariant] || !_shaderTables[projectionVariant])
    return;

  // ---- Drain prior-frame pick staging (moved here from
  // RaytracedLightingPass, WP2-core — see that pass's pre-WP2 history
  // for the full correctness contract; unchanged). --------------------
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

  // ---- Upload viewer-only per-frame UI state (mousePixel gate) -------
  // Moved here from RaytracedLightingPass (WP2-core).
  shaderinterop::FrameUiUniforms frameUi{};
  frameUi.mousePixelX = (context.settings != nullptr)
                            ? context.settings->mousePixelX
                            : RenderSettings::MOUSE_PIXEL_NONE;
  frameUi.mousePixelY = (context.settings != nullptr)
                            ? context.settings->mousePixelY
                            : RenderSettings::MOUSE_PIXEL_NONE;
  frameUi.debugViewMode = 0u;   // Tonemap's domain; dead here.
  frameUi._reservedUi0 = 0u;
  frameUi.worldPosPeriod = 0.0f;  // Tonemap's domain; dead here.
  frameUi._reservedUi1 = 0u;
  frameUi._reservedUi2 = 0u;
  frameUi._reservedUi3 = 0u;
  commandList->writeBuffer(_frameUiBuffer.Get(), &frameUi, sizeof(frameUi));

  // ---- Bind + dispatch ----------------------------------------------
  const nvrhi::BindingSetHandle bindingSet = GetOrCreateBindingSet(visibility, *context.targets);
  if (!bindingSet)
    return;

  // The visibility buffer + this pass's owned G-buffer textures must be
  // in UnorderedAccess for the raygen's writes; the lighting pass
  // transitions the visibility buffer to ShaderResource when it reads
  // the records back (explicit cross-pass barrier — same finding as
  // SsaaResolvePass).
  commandList->setBufferState(visibility, nvrhi::ResourceStates::UnorderedAccess);
  commandList->setTextureState(_gAlbedo.Get(), nvrhi::AllSubresources,
                               nvrhi::ResourceStates::UnorderedAccess);
  commandList->setTextureState(_gNormalRoughness.Get(), nvrhi::AllSubresources,
                               nvrhi::ResourceStates::UnorderedAccess);
  commandList->setTextureState(_gEmissive.Get(), nvrhi::AllSubresources,
                               nvrhi::ResourceStates::UnorderedAccess);
  commandList->setTextureState(_gSpecularAlbedo.Get(), nvrhi::AllSubresources,
                               nvrhi::ResourceStates::UnorderedAccess);
  if (context.targets->normalAov != nullptr)
    commandList->setTextureState(context.targets->normalAov, nvrhi::AllSubresources,
                                 nvrhi::ResourceStates::UnorderedAccess);
  if (context.targets->depthAov != nullptr)
    commandList->setTextureState(context.targets->depthAov, nvrhi::AllSubresources,
                                 nvrhi::ResourceStates::UnorderedAccess);
  if (context.targets->primIdAov != nullptr)
    commandList->setTextureState(context.targets->primIdAov, nvrhi::AllSubresources,
                                 nvrhi::ResourceStates::UnorderedAccess);
  if (context.targets->materialIdAov != nullptr)
    commandList->setTextureState(context.targets->materialIdAov, nvrhi::AllSubresources,
                                 nvrhi::ResourceStates::UnorderedAccess);
  if (context.targets->baseColorAov != nullptr)
    commandList->setTextureState(context.targets->baseColorAov, nvrhi::AllSubresources,
                                 nvrhi::ResourceStates::UnorderedAccess);
  if (context.targets->worldPosAov != nullptr)
    commandList->setTextureState(context.targets->worldPosAov, nvrhi::AllSubresources,
                                 nvrhi::ResourceStates::UnorderedAccess);
  if (context.targets->elementIdAov != nullptr)
    commandList->setTextureState(context.targets->elementIdAov, nvrhi::AllSubresources,
                                 nvrhi::ResourceStates::UnorderedAccess);
  if (context.targets->normalEyeAov != nullptr)
    commandList->setTextureState(context.targets->normalEyeAov, nvrhi::AllSubresources,
                                 nvrhi::ResourceStates::UnorderedAccess);
  if (context.targets->worldPosEyeAov != nullptr)
    commandList->setTextureState(context.targets->worldPosEyeAov, nvrhi::AllSubresources,
                                 nvrhi::ResourceStates::UnorderedAccess);
  if (context.targets->viewZAov != nullptr)
    commandList->setTextureState(context.targets->viewZAov, nvrhi::AllSubresources,
                                 nvrhi::ResourceStates::UnorderedAccess);
  if (context.targets->motionVector != nullptr)
    commandList->setTextureState(context.targets->motionVector, nvrhi::AllSubresources,
                                 nvrhi::ResourceStates::UnorderedAccess);
  commandList->commitBarriers();

  nvrhi::rt::State state;
  state.shaderTable = _shaderTables[projectionVariant];
  // WP2-core — Set 0 (shared SceneBindings, built once per frame by
  // PyxisRenderer) then this pass's OWN Set 1, index-matched to
  // pipelineDesc.globalBindingLayouts.
  state.bindings = {context.sceneBindingSet, bindingSet};
  commandList->setRayTracingState(state);

  // Dispatch dims = the visibility buffer's dims (cached at
  // EnsureVisibilityBuffer) = the display target's dims — identical
  // extents to the lighting dispatch so `vis[y*width+x]` lines up.
  nvrhi::rt::DispatchRaysArguments args;
  args.width = _visibilityW;
  args.height = _visibilityH;
  args.depth = 1;
  commandList->dispatchRays(args);

  // ---- Submit pick-result staging copy (moved here from
  // RaytracedLightingPass, WP2-core). ----------------------------------
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
