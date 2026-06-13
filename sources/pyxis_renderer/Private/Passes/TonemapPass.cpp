// Pyxis renderer — display-transform (tonemap + debug-view encode) pass implementation.

#include "Passes/TonemapPass.h"

#include "RenderGraph/PassContext.h"
#include "RenderGraph/ShaderLoad.h"
#include "Scene/SceneResources.h"  // RFC 0003 — TLAS gate mirrors RaytracedLightingPass.

#include <Pyxis/Platform/FileSystem/AssetLocator.h>
#include <Pyxis/Platform/Logging/Log.h>
#include <Pyxis/Platform/Logging/LogCategories.h>
#include <Pyxis/Renderer/Descs/RenderSettings.h>
#include <Pyxis/Renderer/Descs/RenderTargets.h>
#include <Pyxis/Renderer/GpuScene.h>
#include <Pyxis/Renderer/Profiler.h>

// The dual-language ShaderInterop header (renderer private include path)
// declares pyxis::shaderinterop::TonemapUniforms for the C++ side.
#include "ShaderInterop.slang"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace pyxis {

TonemapPass::TonemapPass(nvrhi::IDevice* device, GpuScene& scene)
    : _device(device), _scene(&scene) {
  auto& log = Logging::Get();
  const AssetLocator locator;
  const auto spvPath = locator.LocateResource("shaders/tonemap.spv");

  _shader = LoadSpirvShader(_device, spvPath.View(), nvrhi::ShaderType::Compute, "main",
                            "TonemapPass");
  if (!_shader)
    return;

  nvrhi::BindingLayoutDesc layoutDesc;
  layoutDesc.visibility = nvrhi::ShaderType::Compute;
  // Zero the per-type binding offsets so the layout numbers map 1:1 to the
  // shader's [[vk::binding(N, 0)]] slots (same convention as SsaaResolvePass).
  layoutDesc.bindingOffsets.shaderResource = 0;
  layoutDesc.bindingOffsets.sampler = 0;
  layoutDesc.bindingOffsets.constantBuffer = 0;
  layoutDesc.bindingOffsets.unorderedAccess = 0;
  layoutDesc.bindings = {
      nvrhi::BindingLayoutItem::Texture_SRV(0),              // fp32 linear radiance
      nvrhi::BindingLayoutItem::Texture_SRV(1),              // normal AOV
      nvrhi::BindingLayoutItem::Texture_SRV(2),              // depth AOV (hitT-or-0)
      nvrhi::BindingLayoutItem::Texture_SRV(3),              // primId AOV
      nvrhi::BindingLayoutItem::Texture_SRV(4),              // materialId AOV
      nvrhi::BindingLayoutItem::Texture_SRV(5),              // baseColor AOV
      nvrhi::BindingLayoutItem::Texture_SRV(6),              // worldPos AOV
      nvrhi::BindingLayoutItem::Texture_SRV(7),              // alpha AOV
      nvrhi::BindingLayoutItem::Texture_SRV(8),              // elementId AOV
      nvrhi::BindingLayoutItem::Texture_SRV(9),              // normalEye AOV
      nvrhi::BindingLayoutItem::Texture_SRV(10),             // worldPosEye AOV
      nvrhi::BindingLayoutItem::VolatileConstantBuffer(11),  // TonemapUniforms
      nvrhi::BindingLayoutItem::Texture_UAV(12),             // BGRA8 display output
      nvrhi::BindingLayoutItem::RawBuffer_SRV(13),           // auto-exposure stats (uint2)
  };
  _bindingLayout = _device->createBindingLayout(layoutDesc);
  if (!_bindingLayout) {
    log.Error(log::RENDER, "TonemapPass: createBindingLayout failed");
    return;
  }

  nvrhi::ComputePipelineDesc pipelineDesc;
  pipelineDesc.CS = _shader;
  pipelineDesc.bindingLayouts = {_bindingLayout};
  _pipeline = _device->createComputePipeline(pipelineDesc);
  if (!_pipeline) {
    log.Error(log::RENDER, "TonemapPass: createComputePipeline failed");
    return;
  }

  nvrhi::BufferDesc cbDesc;
  cbDesc.byteSize = sizeof(shaderinterop::TonemapUniforms);
  cbDesc.isConstantBuffer = true;
  // Volatile: writeBuffer'd once per Execute on the open command list (NVRHI
  // auto-versions volatile cbuffers so the dispatch sees the just-written
  // params — same finding as SsaaResolve.Params). maxVersions sizes the
  // version ring PER OPEN COMMAND LIST: the headless --bench-frames loop
  // records hundreds of frames into one list, so 16 versions starved and
  // NVRHI spammed "maxVersions is insufficient" (P5 bench finding). 512
  // covers a 480-frame bench window at 32 B/version (16 KB) — trivial.
  cbDesc.isVolatile = true;
  cbDesc.maxVersions = 512;
  cbDesc.debugName = "Tonemap.Params";
  _paramsBuffer = _device->createBuffer(cbDesc);
  if (!_paramsBuffer) {
    log.Error(log::RENDER, "TonemapPass: createBuffer(params) failed");
    return;
  }

  // 1×1 SRV fallbacks for the 10 debug-view AOVs — table-driven, one row per
  // format/member, same shape as RaytracedLightingPass's AOV fallback table.
  struct FallbackSpec {
    nvrhi::TextureHandle TonemapPass::* member;
    nvrhi::Format format;
    const char* debugName;
  };
  const std::array<FallbackSpec, 10> aovFallbacks{{
      {&TonemapPass::_fallbackNormalAov,      nvrhi::Format::RGBA16_FLOAT, "Tonemap.FbNormalAov"     },
      {&TonemapPass::_fallbackDepthAov,       nvrhi::Format::R32_FLOAT,    "Tonemap.FbDepthAov"      },
      {&TonemapPass::_fallbackPrimIdAov,      nvrhi::Format::R32_UINT,     "Tonemap.FbPrimIdAov"     },
      {&TonemapPass::_fallbackMaterialAov,    nvrhi::Format::R32_UINT,     "Tonemap.FbMaterialAov"   },
      {&TonemapPass::_fallbackBaseColorAov,   nvrhi::Format::RGBA16_FLOAT, "Tonemap.FbBaseColorAov"  },
      {&TonemapPass::_fallbackWorldPosAov,    nvrhi::Format::RGBA32_FLOAT, "Tonemap.FbWorldPosAov"   },
      {&TonemapPass::_fallbackAlphaAov,       nvrhi::Format::R8_UNORM,     "Tonemap.FbAlphaAov"      },
      {&TonemapPass::_fallbackElementIdAov,   nvrhi::Format::R32_UINT,     "Tonemap.FbElementIdAov"  },
      {&TonemapPass::_fallbackNormalEyeAov,   nvrhi::Format::RGBA16_FLOAT, "Tonemap.FbNormalEyeAov"  },
      {&TonemapPass::_fallbackWorldPosEyeAov, nvrhi::Format::RGBA32_FLOAT, "Tonemap.FbWorldPosEyeAov"},
  }};
  for (const FallbackSpec& spec : aovFallbacks) {
    nvrhi::TextureDesc fbDesc;
    fbDesc.format = spec.format;
    fbDesc.width = 1;
    fbDesc.height = 1;
    fbDesc.dimension = nvrhi::TextureDimension::Texture2D;
    fbDesc.isShaderResource = true;  // SRV-only: this pass never writes them.
    fbDesc.debugName = spec.debugName;
    fbDesc.initialState = nvrhi::ResourceStates::ShaderResource;
    fbDesc.keepInitialState = true;
    this->*spec.member = _device->createTexture(fbDesc);
    if (!(this->*spec.member)) {
      log.Error(log::RENDER, std::string{"TonemapPass: AOV fallback create failed: "}
                                 + spec.debugName);
      return;
    }
  }

  // 8-byte zero-filled raw buffer used for binding 13 when the caller doesn't
  // wire PassContext::autoExposureStats. count == 0 → shader uses manual exposure.
  {
    nvrhi::BufferDesc fbBufDesc;
    fbBufDesc.byteSize = 8u;  // uint2: sum, count.
    fbBufDesc.canHaveRawViews = true;
    fbBufDesc.debugName = "Tonemap.FbAutoExposureStats";
    fbBufDesc.initialState = nvrhi::ResourceStates::ShaderResource;
    fbBufDesc.keepInitialState = true;
    _fallbackAutoExposureStats = _device->createBuffer(fbBufDesc);
    if (!_fallbackAutoExposureStats) {
      log.Error(log::RENDER, "TonemapPass: auto-exposure stats fallback create failed");
      return;
    }
  }

  _ready = true;
  log.Info(log::RENDER, "TonemapPass: initialised (display transform extracted from raygen)");
}

nvrhi::BindingSetHandle TonemapPass::GetOrCreateBindingSet(
    nvrhi::ITexture* source, nvrhi::ITexture* dest, nvrhi::ITexture* const (&aovs)[10],
    nvrhi::IBuffer* autoExposureStats) {
  // FNV1a-64 over every borrowed pointer the set references — same identity
  // scheme as SsaaResolvePass / BlitToSrgbPass, with the 10 resolved AOV
  // pointers folded in so a caller-side AOV swap invalidates by key change.
  uint64_t key = 1469598103934665603ull;
  auto mix = [&key](const void* ptr) {
    auto bits = reinterpret_cast<std::uintptr_t>(ptr);
    for (int byte = 0; byte < 8; ++byte) {
      key ^= static_cast<uint64_t>(bits & 0xFFu);
      key *= 1099511628211ull;
      bits >>= 8;
    }
  };
  mix(source);
  mix(dest);
  for (nvrhi::ITexture* const aov : aovs)
    mix(aov);
  mix(autoExposureStats);

  if (auto cached = _bindingSetCache.find(key); cached != _bindingSetCache.end())
    return cached->second;

  nvrhi::BindingSetDesc setDesc;
  setDesc.bindings = {
      nvrhi::BindingSetItem::Texture_SRV(0, source),
      nvrhi::BindingSetItem::Texture_SRV(1, aovs[0]),   // normal
      nvrhi::BindingSetItem::Texture_SRV(2, aovs[1]),   // depth
      nvrhi::BindingSetItem::Texture_SRV(3, aovs[2]),   // primId
      nvrhi::BindingSetItem::Texture_SRV(4, aovs[3]),   // materialId
      nvrhi::BindingSetItem::Texture_SRV(5, aovs[4]),   // baseColor
      nvrhi::BindingSetItem::Texture_SRV(6, aovs[5]),   // worldPos
      nvrhi::BindingSetItem::Texture_SRV(7, aovs[6]),   // alpha
      nvrhi::BindingSetItem::Texture_SRV(8, aovs[7]),   // elementId
      nvrhi::BindingSetItem::Texture_SRV(9, aovs[8]),   // normalEye
      nvrhi::BindingSetItem::Texture_SRV(10, aovs[9]),  // worldPosEye
      nvrhi::BindingSetItem::ConstantBuffer(11, _paramsBuffer),
      nvrhi::BindingSetItem::Texture_UAV(12, dest),
      nvrhi::BindingSetItem::RawBuffer_SRV(13, autoExposureStats),
  };
  nvrhi::BindingSetHandle set = _device->createBindingSet(setDesc, _bindingLayout);
  // P6 review — a key miss means at least one bound pointer changed, i.e.
  // a resize / SSAA-factor change retired the previous AOV + linearColor +
  // display-target generation. NVRHI binding sets hold STRONG refs to
  // every bound resource, so keeping the old entries would pin entire
  // retired generations (~190 MB each at 1080p, x4 under SSAA 2x) in
  // VRAM. Clear on miss: steady state keeps exactly 1 live entry, and
  // retired generations are released the frame their replacement is
  // first used.
  if (!_bindingSetCache.empty())
    _bindingSetCache.clear();
  // Bounded cache — kept as belt-and-braces (unreachable after the
  // clear-on-miss above, but harmless).
  if (_bindingSetCache.size() > 8)
    _bindingSetCache.clear();
  _bindingSetCache.emplace(key, set);
  return set;
}

void TonemapPass::Execute(nvrhi::ICommandList* commandList, const PassContext& context) {
  if (!_ready || commandList == nullptr || context.settings == nullptr
      || context.targets == nullptr)
    return;

  // Display target + the fp32 radiance RaytracedLightingPass wrote this frame.
  nvrhi::ITexture* const dest = context.targets->color;
  nvrhi::ITexture* const source = context.linearColor;
  if (dest == nullptr || source == nullptr)
    return;

  // Mirror RaytracedLightingPass's early-outs: when the trace skipped (no TLAS / no
  // camera) the linearColor scratch holds stale or undefined data and the old
  // inline display branch never ran — leave the display output untouched
  // exactly as before the split.
  const SceneResources res = detail::SceneResourcesAccess::Get(*_scene);
  if (res.tlas == nullptr || !_scene->HasCamera())
    return;

  // Output dims = the display target's (full render resolution — super-res
  // under viewer SSAA, exactly where the inline raygen branch ran).
  const nvrhi::TextureDesc& destDesc = dest->getDesc();
  const uint32_t destWidth = destDesc.width;
  const uint32_t destHeight = destDesc.height;
  if (destWidth == 0u || destHeight == 0u)
    return;

  const Profiler::GpuScope gpuScope(*context.profiler, commandList, "pass.Tonemap");

  // Exposure from the same GpuScene camera RaytracedLightingPass uploads into
  // CameraUniforms.exposure (identical float, identical 2^exposure gain);
  // debugViewMode / worldPosPeriod from RenderSettings exactly as
  // RaytracedLightingPass fills FrameUiUniforms (10 m default period).
  shaderinterop::TonemapUniforms params{};
  params.exposure = _scene->GetCamera().exposure;
  params.debugViewMode = static_cast<uint32_t>(context.settings->debugView);
  params.worldPosPeriod = (context.settings->worldPosPeriod > 0.0f)
                              ? context.settings->worldPosPeriod
                              : 10.0f;
  params.destWidth = destWidth;
  params.destHeight = destHeight;
  // Auto-exposure (AutoExposurePass populated context.autoExposureStats this
  // frame when enabled); the shader's COLOR path reads the stats only when
  // autoExposureEnabled != 0, otherwise it uses the manual `exposure`.
  params.autoExposureEnabled = context.settings->autoExposure;
  params.autoExposureKey = (context.settings->autoExposureKey > 0.0f)
                               ? context.settings->autoExposureKey
                               : 0.18f;
  params._pad2 = 0u;
  commandList->writeBuffer(_paramsBuffer, &params, sizeof(params));

  // Resolve the 10 debug-view AOVs — caller's texture or the 1×1 fallback.
  // Order matches bindings 1..10 in tonemap.slang.
  nvrhi::ITexture* const aovs[10] = {
      context.targets->normalAov      ? context.targets->normalAov      : _fallbackNormalAov.Get(),
      context.targets->depthAov       ? context.targets->depthAov       : _fallbackDepthAov.Get(),
      context.targets->primIdAov      ? context.targets->primIdAov      : _fallbackPrimIdAov.Get(),
      context.targets->materialIdAov  ? context.targets->materialIdAov  : _fallbackMaterialAov.Get(),
      context.targets->baseColorAov   ? context.targets->baseColorAov   : _fallbackBaseColorAov.Get(),
      context.targets->worldPosAov    ? context.targets->worldPosAov    : _fallbackWorldPosAov.Get(),
      context.targets->alphaAov       ? context.targets->alphaAov       : _fallbackAlphaAov.Get(),
      context.targets->elementIdAov   ? context.targets->elementIdAov   : _fallbackElementIdAov.Get(),
      context.targets->normalEyeAov   ? context.targets->normalEyeAov   : _fallbackNormalEyeAov.Get(),
      context.targets->worldPosEyeAov ? context.targets->worldPosEyeAov : _fallbackWorldPosEyeAov.Get(),
  };

  // Auto-exposure stats: the renderer-owned buffer AutoExposurePass wrote this
  // frame, or the zero-filled fallback (count 0 → manual exposure) for callers
  // that don't wire it.
  nvrhi::IBuffer* const autoExposureStats = context.autoExposureStats != nullptr
                                                ? context.autoExposureStats
                                                : _fallbackAutoExposureStats.Get();

  const nvrhi::BindingSetHandle bindingSet =
      GetOrCreateBindingSet(source, dest, aovs, autoExposureStats);
  if (!bindingSet)
    return;

  // Explicit barriers: linearColor + the AOVs were just UAV-written by
  // RaytracedLightingPass and must be readable as shader resources; the display
  // target must be in UnorderedAccess for the compute write. The cross-pass
  // UAV→SRV hazard needs the explicit transition to be reliable (same
  // finding as SsaaResolvePass on the shared color AOV).
  commandList->setTextureState(source, nvrhi::AllSubresources,
                               nvrhi::ResourceStates::ShaderResource);
  for (nvrhi::ITexture* const aov : aovs)
    commandList->setTextureState(aov, nvrhi::AllSubresources,
                                 nvrhi::ResourceStates::ShaderResource);
  commandList->setTextureState(dest, nvrhi::AllSubresources,
                               nvrhi::ResourceStates::UnorderedAccess);
  // The stats buffer was UAV-written by AutoExposurePass (when enabled); read it
  // as a shader resource here. No-op when auto-exposure is off (fallback stays
  // ShaderResource via keepInitialState).
  commandList->setBufferState(autoExposureStats, nvrhi::ResourceStates::ShaderResource);
  commandList->commitBarriers();

  nvrhi::ComputeState state;
  state.pipeline = _pipeline;
  state.bindings = {bindingSet};
  commandList->setComputeState(state);

  // 8×8 thread group (matches [numthreads(8,8,1)] in the shader);
  // ceil-divide the destination dims into groups.
  const uint32_t groupsX = (destWidth + 7u) / 8u;
  const uint32_t groupsY = (destHeight + 7u) / 8u;
  commandList->dispatch(groupsX, groupsY, 1u);
}

}  // namespace pyxis
