// Pyxis renderer — DlssPass (DLSS Stage 2a). See DlssPass.h for the full
// role/gating contract.

#include "Passes/DlssPass.h"

#include "Dlss/DlssProvider.h"
#include "Passes/CameraJitter.h"
#include "Passes/ExposureMath.h"
#include "Passes/SceneBindings.h"
#include "RenderGraph/PassContext.h"
#include "RenderGraph/ShaderLoad.h"

#include <Pyxis/Platform/FileSystem/AssetLocator.h>
#include <Pyxis/Platform/Logging/Log.h>
#include <Pyxis/Platform/Logging/LogCategories.h>
#include <Pyxis/Renderer/Descs/CameraDesc.h>
#include <Pyxis/Renderer/Descs/RenderSettings.h>
#include <Pyxis/Renderer/Descs/RenderTargets.h>
#include <Pyxis/Renderer/GpuScene.h>
#include <Pyxis/Renderer/Profiler.h>

#include "ShaderInterop.slang"

#include <hlsl++.h>

#include <nvrhi/vulkan.h>
#include <vulkan/vulkan.h>

#include <cstdint>
#include <initializer_list>
#include <string>

namespace pyxis {

namespace {

// Extracts a world-space basis vector / position from a row-major-
// flattened worldFromView (hlslpp::store's own layout — column j is
// {flat[j], flat[4+j], flat[8+j], flat[12+j]} for a row-major matrix).
// camera_ray.slang's BuildCameraRay confirms the convention this mirrors:
// mul(worldFromView, float4(0,0,-1,0)) is world-space FORWARD (view space
// looks down -Z), mul(..., float4(1,0,0,0)) / (0,1,0,0) are world-space
// right/up, and mul(..., float4(0,0,0,1)) is world-space camera position
// (the translation column).
void ExtractCameraBasis(const float* worldFromViewFlat, float outPos[3], float outRight[3],
                        float outUp[3], float outFwd[3]) noexcept {
  outPos[0] = worldFromViewFlat[3];
  outPos[1] = worldFromViewFlat[7];
  outPos[2] = worldFromViewFlat[11];
  outRight[0] = worldFromViewFlat[0];
  outRight[1] = worldFromViewFlat[4];
  outRight[2] = worldFromViewFlat[8];
  outUp[0] = worldFromViewFlat[1];
  outUp[1] = worldFromViewFlat[5];
  outUp[2] = worldFromViewFlat[9];
  outFwd[0] = -worldFromViewFlat[2];
  outFwd[1] = -worldFromViewFlat[6];
  outFwd[2] = -worldFromViewFlat[10];
}

DlssProvider::TaggedImage ToTaggedImage(nvrhi::ITexture* texture) noexcept {
  DlssProvider::TaggedImage image{};
  if (texture == nullptr)
    return image;
  const nvrhi::TextureDesc& desc = texture->getDesc();
  image.image = texture->getNativeObject(nvrhi::ObjectTypes::VK_Image);
  image.imageView =
      texture->getNativeView(nvrhi::ObjectTypes::VK_ImageView, nvrhi::Format::UNKNOWN,
                             nvrhi::AllSubresources, nvrhi::TextureDimension::Texture2D,
                             /*isReadOnlyDSV*/ false);
  image.format = static_cast<uint32_t>(nvrhi::vulkan::convertFormat(desc.format));
  image.width = desc.width;
  image.height = desc.height;
  // Every tagged texture here is one of Pyxis's own UAV compute
  // read/write targets, kept in ResourceStates::UnorderedAccess by every
  // producing pass (CompositePass's linearColor, RaytracedGBufferPass's
  // gViewZ/gMotionVector) and by this pass's own EnsureOutput() — which
  // NVRHI's Vulkan backend maps 1:1 to VK_IMAGE_LAYOUT_GENERAL
  // (vulkan-constants.cpp's g_ResourceStateMap). Execute() below
  // explicitly (re)asserts that state on every tagged texture immediately
  // before this call, so reporting GENERAL here is never a stale guess.
  image.layout = static_cast<uint32_t>(VK_IMAGE_LAYOUT_GENERAL);
  return image;
}

nvrhi::TextureHandle MakeDisplayOutput(nvrhi::IDevice* device, uint32_t width, uint32_t height) {
  nvrhi::TextureDesc desc;
  desc.width = width;
  desc.height = height;
  // Matches CompositePass::EnsureLinearColor's format — AutoExposurePass /
  // TonemapPass read whichever texture PassContext::linearColor points at
  // without caring which pass produced it.
  desc.format = nvrhi::Format::RGBA32_FLOAT;
  desc.dimension = nvrhi::TextureDimension::Texture2D;
  desc.isUAV = true;
  desc.isShaderResource = true;
  desc.debugName = "Dlss.Output";
  desc.initialState = nvrhi::ResourceStates::UnorderedAccess;
  desc.keepInitialState = true;
  return device->createTexture(desc);
}

// 1x1 R32F exposure buffer (kBufferTypeExposure). UAV + shader-resource so
// ToTaggedImage's GENERAL-layout report is valid; written each frame via
// commandList->writeTexture.
nvrhi::TextureHandle MakeExposureTexture(nvrhi::IDevice* device) {
  nvrhi::TextureDesc desc;
  desc.width = 1;
  desc.height = 1;
  desc.format = nvrhi::Format::R32_FLOAT;
  desc.dimension = nvrhi::TextureDimension::Texture2D;
  desc.isUAV = true;
  desc.isShaderResource = true;
  desc.debugName = "Dlss.Exposure";
  desc.initialState = nvrhi::ResourceStates::UnorderedAccess;
  desc.keepInitialState = true;
  return device->createTexture(desc);
}

nvrhi::TextureHandle MakeDepthScratch(nvrhi::IDevice* device, uint32_t width, uint32_t height) {
  nvrhi::TextureDesc desc;
  desc.width = width;
  desc.height = height;
  desc.format = nvrhi::Format::R32_FLOAT;
  desc.dimension = nvrhi::TextureDimension::Texture2D;
  desc.isUAV = true;
  desc.isShaderResource = true;
  desc.debugName = "Dlss.DepthScratch";
  desc.initialState = nvrhi::ResourceStates::UnorderedAccess;
  desc.keepInitialState = true;
  return device->createTexture(desc);
}

// Byte-for-byte lockstep with dlss_expose.slang's own DlssExposeParams
// (16 bytes) — see that shader's file comment for the RR pre-exposure
// rationale.
struct DlssExposeParams {
  uint32_t width;
  uint32_t height;
  float scale;
  float pad0;
};

std::uint64_t HashPointers(std::initializer_list<const void*> pointers) noexcept {
  std::uint64_t key = 1469598103934665603ull;
  for (const void* ptr : pointers)
  {
    auto bits = reinterpret_cast<std::uintptr_t>(ptr);
    for (int byte = 0; byte < 8; ++byte)
    {
      key ^= static_cast<std::uint64_t>(bits & 0xFFu);
      key *= 1099511628211ull;
      bits >>= 8;
    }
  }
  return key;
}

}  // namespace

DlssPass::DlssPass(nvrhi::IDevice* device, DlssProvider& dlssProvider, GpuScene& scene,
                   SceneBindings& sceneBindings)
    : _device(device), _dlssProvider(&dlssProvider), _scene(&scene),
      _sceneBindings(&sceneBindings) {
  // Depth-conversion compute pipeline (dlss_depth_convert.slang) — built
  // eagerly here, same convention as every other compute pass in this
  // codebase (TaaPass/AutoExposurePass/the denoiser chain), so a missing/
  // corrupt .spv is caught and logged at construction, not silently
  // discovered on the first DLSS-active frame.
  auto& log = Logging::Get();
  const AssetLocator locator;
  const auto spvPath = locator.LocateResource("shaders/dlss_depth_convert.spv");
  _depthConvertShader = LoadSpirvShader(_device, spvPath.View(), nvrhi::ShaderType::Compute, "main",
                                        "DlssPass.DepthConvert");
  if (!_depthConvertShader)
    return;

  nvrhi::BindingLayoutDesc layoutDesc;
  layoutDesc.visibility = nvrhi::ShaderType::Compute;
  layoutDesc.bindingOffsets.shaderResource = 0;
  layoutDesc.bindingOffsets.sampler = 0;
  layoutDesc.bindingOffsets.constantBuffer = 0;
  layoutDesc.bindingOffsets.unorderedAccess = 0;
  layoutDesc.bindings = {
      nvrhi::BindingLayoutItem::Texture_SRV(0),             // 0 gViewZ
      nvrhi::BindingLayoutItem::Texture_UAV(1),             // 1 gDlssDepth
      nvrhi::BindingLayoutItem::VolatileConstantBuffer(2),  // 2 DlssDepthUniforms
  };
  _depthConvertLayout = _device->createBindingLayout(layoutDesc);
  if (!_depthConvertLayout)
  {
    log.Error(log::RENDER, "DlssPass: createBindingLayout(depthConvert) failed");
    return;
  }

  nvrhi::ComputePipelineDesc pipelineDesc;
  pipelineDesc.CS = _depthConvertShader;
  pipelineDesc.bindingLayouts = {_depthConvertLayout};
  _depthConvertPipeline = _device->createComputePipeline(pipelineDesc);
  if (!_depthConvertPipeline)
  {
    log.Error(log::RENDER, "DlssPass: createComputePipeline(depthConvert) failed");
    return;
  }

  nvrhi::BufferDesc cbDesc;
  cbDesc.byteSize = sizeof(shaderinterop::DlssDepthUniforms);
  cbDesc.isConstantBuffer = true;
  cbDesc.isVolatile = true;
  // Matches TaaPass.Params / DenoiseTemporalPass.Params's own
  // maxVersions=512 precedent — see either's doc comment for the
  // maxVersions-starvation finding this avoids.
  cbDesc.maxVersions = 512;
  cbDesc.debugName = "Dlss.DepthConvertParams";
  _depthConvertParamsBuffer = _device->createBuffer(cbDesc);
  if (!_depthConvertParamsBuffer)
  {
    log.Error(log::RENDER, "DlssPass: createBuffer(depthConvertParams) failed");
    return;
  }

  // 1x1 exposure buffer (kBufferTypeExposure). Non-fatal on failure — Evaluate
  // falls back to DLSS auto-exposure when inputs.exposure has a null image.
  _exposureTexture = MakeExposureTexture(_device);
  if (!_exposureTexture)
    log.Error(log::RENDER, "DlssPass: createTexture(Exposure) failed — DLSS falls back to auto-exposure");

  // RR pre-exposure compute (dlss_expose.slang — see its file comment).
  // Non-fatal on any failure: _exposeReady stays false and the RR path
  // simply runs un-pre-exposed, exactly as before this change.
  do
  {
    const auto exposeSpvPath = locator.LocateResource("shaders/dlss_expose.spv");
    _exposeShader = LoadSpirvShader(_device, exposeSpvPath.View(), nvrhi::ShaderType::Compute,
                                    "main", "DlssPass.Expose");
    if (!_exposeShader)
      break;
    nvrhi::BindingLayoutDesc exposeLayoutDesc;
    exposeLayoutDesc.visibility = nvrhi::ShaderType::Compute;
    exposeLayoutDesc.bindingOffsets.shaderResource = 0;
    exposeLayoutDesc.bindingOffsets.sampler = 0;
    exposeLayoutDesc.bindingOffsets.constantBuffer = 0;
    exposeLayoutDesc.bindingOffsets.unorderedAccess = 0;
    exposeLayoutDesc.bindings = {
        nvrhi::BindingLayoutItem::Texture_UAV(0),             // 0 gColor (in-place)
        nvrhi::BindingLayoutItem::VolatileConstantBuffer(1),  // 1 DlssExposeParams
    };
    _exposeLayout = _device->createBindingLayout(exposeLayoutDesc);
    if (!_exposeLayout)
    {
      log.Error(log::RENDER, "DlssPass: createBindingLayout(expose) failed");
      break;
    }
    nvrhi::ComputePipelineDesc exposePipelineDesc;
    exposePipelineDesc.CS = _exposeShader;
    exposePipelineDesc.bindingLayouts = {_exposeLayout};
    _exposePipeline = _device->createComputePipeline(exposePipelineDesc);
    if (!_exposePipeline)
    {
      log.Error(log::RENDER, "DlssPass: createComputePipeline(expose) failed");
      break;
    }
    nvrhi::BufferDesc exposeCbDesc;
    exposeCbDesc.byteSize = 16;  // DlssExposeParams — see Execute's local mirror.
    exposeCbDesc.isConstantBuffer = true;
    exposeCbDesc.isVolatile = true;
    // TWO writes per RR frame (scale + inverse) — 512 still leaves the
    // same starvation headroom as the depthConvertParams precedent above.
    exposeCbDesc.maxVersions = 512;
    exposeCbDesc.debugName = "Dlss.ExposeParams";
    _exposeParamsBuffer = _device->createBuffer(exposeCbDesc);
    if (!_exposeParamsBuffer)
    {
      log.Error(log::RENDER, "DlssPass: createBuffer(exposeParams) failed");
      break;
    }
    _exposeReady = true;
  } while (false);

  _depthConvertReady = true;
  log.Info(log::RENDER, "DlssPass: initialised (depth-conversion compute pipeline ready"
                        + std::string(_exposeReady ? "; RR pre-exposure ready)" : ")"));
}

DlssPass::~DlssPass() {
  // ProgrammingGuideDLSS.md 5.0 note: "To turn off DLSS set mode to eOff,
  // note that this does NOT release any resources, for that please use
  // slFreeResources" — called here (before the device tears down) rather
  // than leaving it to DlssProvider's own dtor, which has no NVRHI/device
  // lifetime ordering guarantee relative to this pass. No-op if DLSS was
  // never actually evaluated.
  if (_dlssProvider != nullptr)
    _dlssProvider->ReleaseResources();
}

nvrhi::ITexture* DlssPass::EnsureOutput(uint32_t width, uint32_t height) {
  if (width == 0u || height == 0u)
    return nullptr;
  if (_outputW == width && _outputH == height && _output)
    return _output.Get();

  nvrhi::TextureHandle output = MakeDisplayOutput(_device, width, height);
  if (!output)
  {
    if (!_outputCreateFailedLogged)
    {
      Logging::Get().Error(log::RENDER, "DlssPass: createTexture(output, " + std::to_string(width)
                                            + "x" + std::to_string(height)
                                            + ") failed; keeping previous allocation");
      _outputCreateFailedLogged = true;
    }
    return _output.Get();
  }
  _output = output;
  _outputW = width;
  _outputH = height;
  _outputCreateFailedLogged = false;
  return _output.Get();
}

nvrhi::ITexture* DlssPass::EnsureDepthScratch(uint32_t width, uint32_t height) {
  if (width == 0u || height == 0u)
    return nullptr;
  if (_dlssDepthW == width && _dlssDepthH == height && _dlssDepth)
    return _dlssDepth.Get();

  nvrhi::TextureHandle depthScratch = MakeDepthScratch(_device, width, height);
  if (!depthScratch)
  {
    if (!_dlssDepthCreateFailedLogged)
    {
      Logging::Get().Error(log::RENDER, "DlssPass: createTexture(depthScratch, "
                                            + std::to_string(width) + "x" + std::to_string(height)
                                            + ") failed; keeping previous allocation");
      _dlssDepthCreateFailedLogged = true;
    }
    return _dlssDepth.Get();
  }
  _dlssDepth = depthScratch;
  _dlssDepthW = width;
  _dlssDepthH = height;
  _dlssDepthCreateFailedLogged = false;
  _depthConvertBindingSetCache.clear();  // stale keys reference the old texture.
  return _dlssDepth.Get();
}

nvrhi::BindingSetHandle DlssPass::GetOrCreateDepthConvertBindingSet(nvrhi::ITexture* viewZ,
                                                                    nvrhi::ITexture* dlssDepth) {
  const std::uint64_t key = HashPointers({viewZ, dlssDepth});
  if (auto cached = _depthConvertBindingSetCache.find(key);
      cached != _depthConvertBindingSetCache.end())
    return cached->second;
  constexpr std::size_t MAX_CACHE_ENTRIES = 4;
  if (_depthConvertBindingSetCache.size() >= MAX_CACHE_ENTRIES)
    _depthConvertBindingSetCache.clear();

  nvrhi::BindingSetDesc setDesc;
  setDesc.bindings = {
      nvrhi::BindingSetItem::Texture_SRV(0, viewZ),
      nvrhi::BindingSetItem::Texture_UAV(1, dlssDepth),
      nvrhi::BindingSetItem::ConstantBuffer(2, _depthConvertParamsBuffer),
  };
  nvrhi::BindingSetHandle set = _device->createBindingSet(setDesc, _depthConvertLayout);
  _depthConvertBindingSetCache.emplace(key, set);
  return set;
}

nvrhi::BindingSetHandle DlssPass::GetOrCreateExposeBindingSet(nvrhi::ITexture* color) {
  if (auto cached = _exposeBindingSetCache.find(color); cached != _exposeBindingSetCache.end())
    return cached->second;
  constexpr std::size_t MAX_EXPOSE_CACHE_ENTRIES = 4;
  if (_exposeBindingSetCache.size() >= MAX_EXPOSE_CACHE_ENTRIES)
    _exposeBindingSetCache.clear();
  nvrhi::BindingSetDesc setDesc;
  setDesc.bindings = {
      nvrhi::BindingSetItem::Texture_UAV(0, color),
      nvrhi::BindingSetItem::ConstantBuffer(1, _exposeParamsBuffer),
  };
  nvrhi::BindingSetHandle set = _device->createBindingSet(setDesc, _exposeLayout);
  _exposeBindingSetCache[color] = set;
  return set;
}

void DlssPass::Execute(nvrhi::ICommandList* commandList, const PassContext& context) {
  if (commandList == nullptr || context.settings == nullptr || context.targets == nullptr
      || _dlssProvider == nullptr)
    return;

  // Defensive re-check — PyxisRenderer::RenderFrame's {requested,
  // effective} denoiser resolution already guarantees these line up (see
  // DlssPass.h's file comment), but this pass has no other way to know
  // "am I even supposed to run this frame" since (unlike passMask-gated
  // passes) there's no dedicated bit for it.
  if (context.settings->realTimeQuality.denoiser != DENOISER_DLSS || !_dlssProvider->IsUsable())
    return;

  nvrhi::ITexture* const colorIn = context.linearColor;
  nvrhi::ITexture* const depth = context.gViewZ;
  nvrhi::ITexture* const mvec = context.gMotionVector;
  nvrhi::ITexture* const displayTarget = context.targets->color;
  if (colorIn == nullptr || depth == nullptr || mvec == nullptr || displayTarget == nullptr
      || _scene == nullptr || !_scene->HasCamera() || _sceneBindings == nullptr)
    return;

  // DLSS Stage 2b — Ray Reconstruction's guide buffers (ProgrammingGuideDLSS_RR.md
  // §4.1): gAlbedo (diffuse), gSpecularAlbedo (new — raytraced_gbuffer.slang's
  // EnvBRDFApprox2/OpenPBRSpecularF0), gNormalRoughness (ePacked — roughness
  // already lives in .a), and the RAW (never denoised — see
  // PyxisRenderer::RenderFrame's passMask forcing) ReflectionsPass output
  // for kBufferTypeSpecularHitDistance (.a = mean hitT). `useRR` degrades
  // to the proven SR path below whenever the provider itself can't do RR
  // OR any one of these guides isn't ready yet (first frame / a signal
  // pass failed to initialise) — same defensive-recheck-not-second-source-
  // of-truth reasoning as the `denoiser != DENOISER_DLSS` gate above.
  nvrhi::ITexture* const albedo = context.gAlbedo;
  nvrhi::ITexture* const specularAlbedo = context.gSpecularAlbedo;
  nvrhi::ITexture* const normalRoughness = context.gNormalRoughness;
  nvrhi::ITexture* const specularHitDistance = context.gReflections;
  // DLAA (dlssExecMode == 4) is the DLSS-SR feature's native-resolution AA
  // mode — route it through the SR path (Evaluate), NOT Ray Reconstruction.
  // RR is a distinct denoiser-replacing feature; the user asked for DLAA, and
  // only the SR path carries the kBufferTypeExposure buffer the SR options
  // consume (RTX-alignment 2026-07-08). RR stays the default for the upscaling
  // modes (Auto/Quality/…) where it's the intended path.
  const bool wantDlaa =
      context.settings->realTimeQuality.dlssExecMode == DLSS_EXEC_MODE_DLAA;
  const bool useRR = !wantDlaa && _dlssProvider->IsRRUsable() && albedo != nullptr
                    && specularAlbedo != nullptr && normalRoughness != nullptr
                    && specularHitDistance != nullptr;

  const nvrhi::TextureDesc& renderDesc = colorIn->getDesc();
  const nvrhi::TextureDesc& displayDesc = displayTarget->getDesc();
  // §30.10 — no allocations inside Execute; PyxisRenderer::RenderFrame
  // already called EnsureOutput(displayWidth, displayHeight) +
  // EnsureDepthScratch(renderWidth, renderHeight) on the CPU frame path
  // (same convention as every other pass's Ensure* call).
  nvrhi::ITexture* const output = _output.Get();
  nvrhi::ITexture* const dlssDepth = _dlssDepth.Get();
  if (output == nullptr || _outputW != displayDesc.width || _outputH != displayDesc.height
      || !_depthConvertReady || dlssDepth == nullptr || _dlssDepthW != renderDesc.width
      || _dlssDepthH != renderDesc.height)
    return;

  // ---- Depth conversion (dlss_depth_convert.slang) ---------------------
  // Streamline's DLSS-SR plugin requires the standard kBufferTypeDepth tag
  // (verified via slGetFeatureRequirements — see that shader's own file
  // comment); gViewZ is already-linear view-space distance, not a
  // projective depth, so it needs converting first.
  {
    const Profiler::CpuScope depthConvertScope(*context.profiler, "pass.DlssDepthConvert.cpu");
    commandList->setTextureState(depth, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
    commandList->setTextureState(dlssDepth, nvrhi::AllSubresources,
                                 nvrhi::ResourceStates::UnorderedAccess);
    commandList->commitBarriers();

    shaderinterop::DlssDepthUniforms depthParams{};
    depthParams.width = renderDesc.width;
    depthParams.height = renderDesc.height;
    depthParams.nearClip = _scene->GetCamera().nearClip;
    depthParams.farClip = _scene->GetCamera().farClip;
    commandList->writeBuffer(_depthConvertParamsBuffer, &depthParams, sizeof(depthParams));

    const nvrhi::BindingSetHandle depthConvertBindingSet =
        GetOrCreateDepthConvertBindingSet(depth, dlssDepth);
    if (!depthConvertBindingSet)
      return;

    nvrhi::ComputeState depthConvertState;
    depthConvertState.pipeline = _depthConvertPipeline;
    depthConvertState.bindings = {depthConvertBindingSet};
    commandList->setComputeState(depthConvertState);
    const uint32_t groupsX = (renderDesc.width + 7u) / 8u;
    const uint32_t groupsY = (renderDesc.height + 7u) / 8u;
    commandList->dispatch(groupsX, groupsY, 1u);
  }

  // ---- Resource-state transitions -------------------------------------
  // Every tagged texture (colorIn/dlssDepth/mvec/output) is asserted into
  // UnorderedAccess (-> VK_IMAGE_LAYOUT_GENERAL, see ToTaggedImage's own
  // comment) immediately before slEvaluateFeature — ProgrammingGuide
  // ManualHooking.md 6.2's "IMPORTANT: Image layout needs to be correct
  // when tagged resource is used by SL" note. colorIn/mvec are already in
  // this state from their producing passes in the common case; this is a
  // defensive re-assert (NVRHI no-ops a transition to a texture's current
  // tracked state). dlssDepth was just written above (UnorderedAccess
  // already) — re-asserted here too for a single consistent barrier point.
  commandList->setTextureState(colorIn, nvrhi::AllSubresources, nvrhi::ResourceStates::UnorderedAccess);
  commandList->setTextureState(dlssDepth, nvrhi::AllSubresources, nvrhi::ResourceStates::UnorderedAccess);
  commandList->setTextureState(mvec, nvrhi::AllSubresources, nvrhi::ResourceStates::UnorderedAccess);
  commandList->setTextureState(output, nvrhi::AllSubresources, nvrhi::ResourceStates::UnorderedAccess);
  if (useRR)
  {
    commandList->setTextureState(albedo, nvrhi::AllSubresources, nvrhi::ResourceStates::UnorderedAccess);
    commandList->setTextureState(specularAlbedo, nvrhi::AllSubresources,
                                 nvrhi::ResourceStates::UnorderedAccess);
    commandList->setTextureState(normalRoughness, nvrhi::AllSubresources,
                                 nvrhi::ResourceStates::UnorderedAccess);
    commandList->setTextureState(specularHitDistance, nvrhi::AllSubresources,
                                 nvrhi::ResourceStates::UnorderedAccess);
  }
  commandList->commitBarriers();

  // ---- Camera matrices (CPU-side, from SceneBindings' snapshot of THIS
  // frame's conformed matrices — see SceneBindings.h's Last*() accessors
  // doc comments for why these are the ones DLSS must see, not a re-
  // derivation from GpuScene's raw camera). ---------------------------
  const CameraDesc& camera = _scene->GetCamera();
  float viewToClipFlat[16];
  float clipToViewFlat[16];
  float worldFromViewFlat[16];
  float prevClipFromWorldFlat[16];
  hlslpp::store(viewToClipFlat, _sceneBindings->LastCameraViewToClip());
  hlslpp::store(clipToViewFlat, _sceneBindings->LastClipToCameraView());
  hlslpp::store(worldFromViewFlat, _sceneBindings->LastWorldFromView());
  hlslpp::store(prevClipFromWorldFlat, _sceneBindings->LastPrevClipFromWorldForMotionVectors());

  // clipToPrevClip = prevClipFromWorld * worldFromView * viewFromClip
  // (apply viewFromClip first: currentClip -> currentView -> world ->
  // prevClip). See DlssPass.cpp's own commit / DlssProvider.h's file
  // comment for the row-major/column-vector derivation.
  const hlslpp::float4x4 clipToPrevClip =
      hlslpp::mul(_sceneBindings->LastPrevClipFromWorldForMotionVectors(),
                 hlslpp::mul(_sceneBindings->LastWorldFromView(),
                            _sceneBindings->LastClipToCameraView()));
  const hlslpp::float4x4 prevClipToClip = hlslpp::inverse(clipToPrevClip);
  float clipToPrevClipFlat[16];
  float prevClipToClipFlat[16];
  hlslpp::store(clipToPrevClipFlat, clipToPrevClip);
  hlslpp::store(prevClipToClipFlat, prevClipToClip);

  float cameraPos[3];
  float cameraRight[3];
  float cameraUp[3];
  float cameraFwd[3];
  ExtractCameraBasis(worldFromViewFlat, cameraPos, cameraRight, cameraUp, cameraFwd);

  // ---- Jitter — the EXACT SAME per-frame offset SceneBindings uploaded
  // for the raygen this frame (Passes/CameraJitter.h; see that header's
  // file comment for why a second, independent formula here would be a
  // correctness bug, not just a style nit). DLSS must be jittered even
  // though PASS_MASK_TAA is masked off when effective denoiser == Dlss
  // (PyxisRenderer::RenderFrame) — TaaPass and DLSS never run in the same
  // frame, but the SceneBindings jitter upload is gated on
  // `taaJitterEnabled`, which PyxisRenderer also sets true whenever the
  // effective denoiser is Dlss (see its own RenderFrame comment).
  const shaderinterop::float2 jitter = ComputeHaltonJitter(context.frameIndex);
  const uint32_t dlssExecMode = context.settings->realTimeQuality.dlssExecMode;
  const bool orthographic = camera.projectionMode == 1u;

  bool evaluateOk = false;
  // RR pre-exposure bookkeeping — set inside the RR branch, consumed by the
  // post-evaluate inverse/undo dispatches at the tail of this function.
  float rrExposureGain = 1.0f;
  bool rrPreExposed = false;
  if (useRR)
  {
    // DLSS Stage 2b — DLSSDOptions::worldToCameraView / cameraViewToWorld
    // (ProgrammingGuideDLSS_RR.md §4.1.9, kBufferTypeSpecularHitDistance's
    // reprojection contract). cameraViewToWorld IS worldFromViewFlat
    // (already computed above for ExtractCameraBasis); worldToCameraView
    // is its inverse — same hlslpp::inverse pattern this function already
    // uses for clipToPrevClip/prevClipToClip below.
    const hlslpp::float4x4 viewFromWorld =
        hlslpp::inverse(_sceneBindings->LastWorldFromView());
    float viewFromWorldFlat[16];
    hlslpp::store(viewFromWorldFlat, viewFromWorld);

    DlssProvider::FrameInputsRR inputsRR{};
    inputsRR.frameIndex = context.frameIndex;
    inputsRR.renderWidth = renderDesc.width;
    inputsRR.renderHeight = renderDesc.height;
    inputsRR.displayWidth = displayDesc.width;
    inputsRR.displayHeight = displayDesc.height;
    inputsRR.dlssExecMode = dlssExecMode;
    inputsRR.orthographic = orthographic;
    inputsRR.jitterX = jitter.x;
    inputsRR.jitterY = jitter.y;
    inputsRR.cameraViewToClip = viewToClipFlat;
    inputsRR.clipToCameraView = clipToViewFlat;
    inputsRR.clipToPrevClip = clipToPrevClipFlat;
    inputsRR.prevClipToClip = prevClipToClipFlat;
    inputsRR.worldToCameraView = viewFromWorldFlat;
    inputsRR.cameraViewToWorld = worldFromViewFlat;
    inputsRR.cameraPos[0] = cameraPos[0];
    inputsRR.cameraPos[1] = cameraPos[1];
    inputsRR.cameraPos[2] = cameraPos[2];
    inputsRR.cameraUp[0] = cameraUp[0];
    inputsRR.cameraUp[1] = cameraUp[1];
    inputsRR.cameraUp[2] = cameraUp[2];
    inputsRR.cameraRight[0] = cameraRight[0];
    inputsRR.cameraRight[1] = cameraRight[1];
    inputsRR.cameraRight[2] = cameraRight[2];
    inputsRR.cameraFwd[0] = cameraFwd[0];
    inputsRR.cameraFwd[1] = cameraFwd[1];
    inputsRR.cameraFwd[2] = cameraFwd[2];
    inputsRR.cameraNear = camera.nearClip;
    inputsRR.cameraFar = camera.farClip;
    inputsRR.colorIn = ToTaggedImage(colorIn);
    inputsRR.colorOut = ToTaggedImage(output);
    inputsRR.depth = ToTaggedImage(dlssDepth);
    inputsRR.mvec = ToTaggedImage(mvec);
    inputsRR.albedo = ToTaggedImage(albedo);
    inputsRR.specularAlbedo = ToTaggedImage(specularAlbedo);
    inputsRR.normalRoughness = ToTaggedImage(normalRoughness);
    inputsRR.specularHitDistance = ToTaggedImage(specularHitDistance);
    // RR pre-exposure (dlss_expose.slang): DLSSDOptions has no
    // useAutoExposure/kBufferTypeExposure hook, so scale colorIn's RGB by
    // the SAME display gain TonemapPass applies downstream — RR's internal
    // tone handling then sees display-range input (its trained domain)
    // instead of the World Lobby's raw 0-12000 range. Inverse-scaled after
    // evaluate (tail of this function) so downstream exposure math is
    // untouched. Matches ovrtx's exposed-buffer denoiser convention.
    if (_exposeReady)
    {
      rrExposureGain = ComputeEffectiveExposureScale(*context.settings, camera);
      if (rrExposureGain > 0.0f && rrExposureGain != 1.0f)
      {
        const nvrhi::BindingSetHandle exposeSet = GetOrCreateExposeBindingSet(colorIn);
        if (exposeSet)
        {
          DlssExposeParams exposeParams{};
          exposeParams.width = renderDesc.width;
          exposeParams.height = renderDesc.height;
          exposeParams.scale = rrExposureGain;
          commandList->writeBuffer(_exposeParamsBuffer, &exposeParams, sizeof(exposeParams));
          nvrhi::ComputeState exposeState;
          exposeState.pipeline = _exposePipeline;
          exposeState.bindings = {exposeSet};
          commandList->setComputeState(exposeState);
          commandList->dispatch((renderDesc.width + 7u) / 8u, (renderDesc.height + 7u) / 8u,
                                1u);
          rrPreExposed = true;
        }
      }
    }

    inputsRR.vkCommandBuffer = commandList->getNativeObject(nvrhi::ObjectTypes::VK_CommandBuffer);
    evaluateOk = _dlssProvider->EvaluateRR(inputsRR);
  }
  else
  {
    DlssProvider::FrameInputs inputs{};
    inputs.frameIndex = context.frameIndex;
    inputs.renderWidth = renderDesc.width;
    inputs.renderHeight = renderDesc.height;
    inputs.displayWidth = displayDesc.width;
    inputs.displayHeight = displayDesc.height;
    inputs.dlssExecMode = dlssExecMode;
    inputs.reset = false;  // DlssProvider::Evaluate derives first-frame/resolution-change resets itself.
    inputs.orthographic = orthographic;
    inputs.jitterX = jitter.x;
    inputs.jitterY = jitter.y;
    inputs.cameraViewToClip = viewToClipFlat;
    inputs.clipToCameraView = clipToViewFlat;
    inputs.clipToPrevClip = clipToPrevClipFlat;
    inputs.prevClipToClip = prevClipToClipFlat;
    inputs.cameraPos[0] = cameraPos[0];
    inputs.cameraPos[1] = cameraPos[1];
    inputs.cameraPos[2] = cameraPos[2];
    inputs.cameraUp[0] = cameraUp[0];
    inputs.cameraUp[1] = cameraUp[1];
    inputs.cameraUp[2] = cameraUp[2];
    inputs.cameraRight[0] = cameraRight[0];
    inputs.cameraRight[1] = cameraRight[1];
    inputs.cameraRight[2] = cameraRight[2];
    inputs.cameraFwd[0] = cameraFwd[0];
    inputs.cameraFwd[1] = cameraFwd[1];
    inputs.cameraFwd[2] = cameraFwd[2];
    inputs.cameraNear = camera.nearClip;
    inputs.cameraFar = camera.farClip;
    inputs.colorIn = ToTaggedImage(colorIn);
    inputs.colorOut = ToTaggedImage(output);
    inputs.depth = ToTaggedImage(dlssDepth);  // converted NDC depth, NOT raw gViewZ — see above.
    inputs.mvec = ToTaggedImage(mvec);
    // Exposure buffer (kBufferTypeExposure): write the SAME display gain
    // TonemapPass applies downstream (ComputeEffectiveExposureScale) into the
    // 1x1 texture, so DLSS normalizes with PYX's exposure rather than its own
    // auto-exposure estimate (which diverged on the 12000-nit dome and
    // over-brightened ~1.5x). Not an allocation — the texture is ctor-made.
    if (_exposureTexture)
    {
      // The SAME display gain TonemapPass applies downstream. NOTE (measured
      // 2026-07-08): this correctly turns off DLSS's own auto-exposure but only
      // WEAKLY counters the DLAA path's residual over-brightening — DLSS-SR in
      // HDR mode does its own tone handling on the World Lobby's 0-12000 linear
      // range (out of its trained input domain), leaving the frame ~0.10 mean
      // too bright regardless of this value. Fully matching would need feeding
      // DLSS pre-exposed (display-range) color, a deeper pipeline change
      // tracked as debt; DLSS/DLAA does not improve the CONVERGED offline
      // metric anyway (it's a real-time/few-sample AA feature).
      const float exposureGain =
          ComputeEffectiveExposureScale(*context.settings, camera);
      commandList->setTextureState(_exposureTexture, nvrhi::AllSubresources,
                                   nvrhi::ResourceStates::CopyDest);
      commandList->commitBarriers();
      commandList->writeTexture(_exposureTexture, /*arraySlice*/ 0, /*mipLevel*/ 0,
                                &exposureGain, /*rowPitch*/ sizeof(float), /*depthPitch*/ 0);
      commandList->setTextureState(_exposureTexture, nvrhi::AllSubresources,
                                   nvrhi::ResourceStates::UnorderedAccess);
      commandList->commitBarriers();
      inputs.exposure = ToTaggedImage(_exposureTexture);
    }
    inputs.vkCommandBuffer = commandList->getNativeObject(nvrhi::ObjectTypes::VK_CommandBuffer);
    evaluateOk = _dlssProvider->Evaluate(inputs);
  }

  if (!evaluateOk)
  {
    // Graceful degradation, same shape as every other pass's shader-
    // load-failure gate: leave `output` whatever it held last (garbage on
    // the very first failed frame, stale-but-plausible thereafter) rather
    // than crash. The "denoiser: requested=Dlss effective=..." log line
    // (PyxisRenderer.cpp) doesn't re-resolve per-Evaluate-failure — this
    // is a distinct, rarer failure mode (device interop succeeded at
    // startup, then a specific frame's tag/evaluate call failed), so it
    // gets its own log line instead.
    Logging::Get().Error(log::RENDER, std::string{"DlssPass: DlssProvider::"}
                                          + (useRR ? "EvaluateRR" : "Evaluate")
                                          + " failed this frame; output stale");
    // Undo the RR pre-exposure on colorIn — the redirect below never runs
    // on this path, so downstream AutoExposure/Tonemap still read colorIn
    // and must not see a double-exposed image.
    if (rrPreExposed)
    {
      const nvrhi::BindingSetHandle undoSet = GetOrCreateExposeBindingSet(colorIn);
      if (undoSet)
      {
        DlssExposeParams undoParams{};
        undoParams.width = renderDesc.width;
        undoParams.height = renderDesc.height;
        undoParams.scale = 1.0f / rrExposureGain;
        commandList->writeBuffer(_exposeParamsBuffer, &undoParams, sizeof(undoParams));
        nvrhi::ComputeState undoState;
        undoState.pipeline = _exposePipeline;
        undoState.bindings = {undoSet};
        commandList->setComputeState(undoState);
        commandList->dispatch((renderDesc.width + 7u) / 8u, (renderDesc.height + 7u) / 8u, 1u);
      }
    }
    return;
  }

  // Per ProgrammingGuideManualHooking.md 7.0, `output` may now be in a
  // Streamline-internal state; re-assert UnorderedAccess before any
  // downstream NVRHI pass (AutoExposurePass/TonemapPass) reads it, same
  // defensive-reassert reasoning as the pre-evaluate transitions above.
  commandList->setTextureState(output, nvrhi::AllSubresources, nvrhi::ResourceStates::UnorderedAccess);
  commandList->commitBarriers();

  // Inverse of the RR pre-exposure, applied to the reconstructed output at
  // DISPLAY resolution — hands downstream (AutoExposure/Tonemap) the same
  // unexposed-linear convention every other producer of linearColor uses.
  if (rrPreExposed)
  {
    const nvrhi::BindingSetHandle inverseSet = GetOrCreateExposeBindingSet(output);
    if (inverseSet)
    {
      DlssExposeParams inverseParams{};
      inverseParams.width = displayDesc.width;
      inverseParams.height = displayDesc.height;
      inverseParams.scale = 1.0f / rrExposureGain;
      commandList->writeBuffer(_exposeParamsBuffer, &inverseParams, sizeof(inverseParams));
      nvrhi::ComputeState inverseState;
      inverseState.pipeline = _exposePipeline;
      inverseState.bindings = {inverseSet};
      commandList->setComputeState(inverseState);
      commandList->dispatch((displayDesc.width + 7u) / 8u, (displayDesc.height + 7u) / 8u, 1u);
    }
  }

  // Redirect the shared cursor to THIS pass's display-resolution output —
  // see PassContext::linearColor's own doc comment for why this field is
  // `mutable` and why only this pass (not TaaPass) needs to reassign the
  // pointer rather than copy back in place.
  context.linearColor = output;
}

}  // namespace pyxis
