// Pyxis renderer — SceneBindings (RTX-alignment design, WP2-core).

#include "Passes/SceneBindings.h"

#include "Passes/CameraJitter.h"  // DLSS Stage 2a — shared with DlssPass, see its own file comment.
#include "Passes/ExposureMath.h"  // RTX-alignment design — shared exposureScale/clamp descale.
#include "Scene/SceneResources.h"  // RFC 0003 — renderer-internal scene view.

#include <Pyxis/Platform/Logging/Log.h>
#include <Pyxis/Platform/Logging/LogCategories.h>

#include "ShaderInterop.slang"

#include <algorithm>
#include <bit>
#include <cstdint>
#include <cstring>
#include <hlsl++.h>

namespace pyxis {

namespace {

using shaderinterop::CameraUniforms;
using shaderinterop::MotionVectorCameraUniforms;
using shaderinterop::RealTimeQualityUniforms;
using shaderinterop::RtxSamplingUniforms;
static_assert(sizeof(CameraUniforms) == 208,
              "CameraUniforms is three float4x4s + 16-byte exposure row = 208 bytes; "
              "see resources/shaders/ShaderInterop.slang.");
static_assert(sizeof(MotionVectorCameraUniforms) == 80,
              "MotionVectorCameraUniforms is one float4x4 + a 16-byte row = 80 bytes; "
              "see resources/shaders/ShaderInterop.slang.");
static_assert(sizeof(RealTimeQualityUniforms) == 48,
              "RealTimeQualityUniforms is 3 rows of 16 = 48 bytes; "
              "see resources/shaders/ShaderInterop.slang.");
static_assert(sizeof(RtxSamplingUniforms) == 16,
              "RtxSamplingUniforms is one cbuffer row = 16 bytes; "
              "see resources/shaders/ShaderInterop.slang.");

// Plan §12.1 — the fixed default PCG32 seed every stochastic estimator's
// SeedPcg32(...) call folds in via gSampling.rngSeed. NOT yet wired to
// Configuration::render.seed (RenderSettings.h — the public RenderSettings
// POD — is frozen this phase for concurrent work on its RealTimeQuality
// block); see RtxSamplingUniforms's doc comment in ShaderInterop.slang.
// Frame-to-frame variation still comes from `frameIndex`, so headless
// determinism (same seed+frame => same image) holds with this fixed
// default exactly as it would with a configured one. Mirrors
// Configuration::RenderConfig::seed's own default (1) for parity.
constexpr std::uint32_t DEFAULT_RNG_SEED = 1u;

// ComputeHaltonJitter moved to Passes/CameraJitter.h (DLSS Stage 2a) so
// DlssPass can feed Streamline the EXACT SAME per-frame pixel offset this
// file uploads into RtxSamplingUniforms — see that header's file comment.

nvrhi::BufferHandle MakeStructuredFallback(nvrhi::IDevice* device, std::size_t stride,
                                           const char* debugName) {
  nvrhi::BufferDesc desc;
  desc.byteSize = stride;
  desc.structStride = stride;
  desc.canHaveRawViews = false;
  desc.canHaveTypedViews = false;
  desc.format = nvrhi::Format::UNKNOWN;
  desc.debugName = debugName;
  desc.initialState = nvrhi::ResourceStates::ShaderResource;
  desc.keepInitialState = true;
  return device->createBuffer(desc);
}

}  // namespace

SceneBindings::SceneBindings(nvrhi::IDevice* device) : _device(device) {
  // Set-0 layout — bindingOffsets zeroed so the slot numbers below map
  // 1:1 onto every RT shader's `[[vk::binding(N, 0)]]` declarations
  // (same convention every pass in this codebase uses; NVRHI's default
  // per-type offsets would desync the emitted Vulkan binding numbers —
  // see any pass ctor's identical comment for the VUID this avoids).
  nvrhi::BindingLayoutDesc layoutDesc;
  // WP2-final — broadened from AllRayTracing-only so CompositePass (a
  // compute pass) can also bind this Set-0 layout/set. Purely additive on
  // the Vulkan descriptor-set-layout stage mask; every existing RT pass's
  // pipeline is unaffected (see this class's header doc comment).
  layoutDesc.visibility = nvrhi::ShaderType::AllRayTracing | nvrhi::ShaderType::Compute;
  layoutDesc.bindingOffsets.shaderResource = 0;
  layoutDesc.bindingOffsets.sampler = 0;
  layoutDesc.bindingOffsets.constantBuffer = 0;
  layoutDesc.bindingOffsets.unorderedAccess = 0;
  layoutDesc.bindings = {
      nvrhi::BindingLayoutItem::ConstantBuffer(0),         // 0  CameraUniforms
      nvrhi::BindingLayoutItem::RayTracingAccelStruct(1),  // 1  TLAS
      nvrhi::BindingLayoutItem::StructuredBuffer_SRV(2),   // 2  materials
      nvrhi::BindingLayoutItem::StructuredBuffer_SRV(3),   // 3  instance info
      nvrhi::BindingLayoutItem::StructuredBuffer_SRV(4),   // 4  mesh info
      nvrhi::BindingLayoutItem::StructuredBuffer_SRV(5),   // 5  lights
      nvrhi::BindingLayoutItem::StructuredBuffer_SRV(6),   // 6  mesh index pool
      nvrhi::BindingLayoutItem::StructuredBuffer_SRV(7),   // 7  mesh UVs
      nvrhi::BindingLayoutItem::StructuredBuffer_SRV(8),   // 8  mesh vertex attribs
      nvrhi::BindingLayoutItem::StructuredBuffer_SRV(9),   // 9  mesh face normals
      nvrhi::BindingLayoutItem::Texture_SRV(10),           // 10 dome env map
      nvrhi::BindingLayoutItem::Sampler(11),                // 11 dome sampler
      nvrhi::BindingLayoutItem::Sampler(12),                // 12 material sampler
      nvrhi::BindingLayoutItem::Texture_SRV(13).setSize(
          shaderinterop::BINDLESS_TEXTURES_CAP),            // 13 bindless textures
      nvrhi::BindingLayoutItem::ConstantBuffer(14),         // 14 MotionVectorCameraUniforms
      nvrhi::BindingLayoutItem::ConstantBuffer(15),         // 15 RealTimeQualityUniforms (WP2-final)
      nvrhi::BindingLayoutItem::ConstantBuffer(16),         // 16 RtxSamplingUniforms (Phase B)
  };
  _layout = _device->createBindingLayout(layoutDesc);
  if (!_layout)
  {
    Logging::Get().Error(log::RENDER, "SceneBindings: createBindingLayout failed");
    return;
  }

  nvrhi::BufferDesc cbDesc;
  cbDesc.byteSize = sizeof(CameraUniforms);
  cbDesc.debugName = "SceneBindings.CameraUniforms";
  cbDesc.isConstantBuffer = true;
  cbDesc.initialState = nvrhi::ResourceStates::ConstantBuffer;
  cbDesc.keepInitialState = true;
  _cameraUniformsBuffer = _device->createBuffer(cbDesc);
  if (!_cameraUniformsBuffer)
  {
    Logging::Get().Error(log::RENDER, "SceneBindings: createBuffer(CameraUniforms) failed");
    return;
  }

  nvrhi::BufferDesc mvCbDesc;
  mvCbDesc.byteSize = sizeof(MotionVectorCameraUniforms);
  mvCbDesc.debugName = "SceneBindings.MotionVectorCameraUniforms";
  mvCbDesc.isConstantBuffer = true;
  mvCbDesc.initialState = nvrhi::ResourceStates::ConstantBuffer;
  mvCbDesc.keepInitialState = true;
  _motionVectorCameraBuffer = _device->createBuffer(mvCbDesc);
  if (!_motionVectorCameraBuffer)
  {
    Logging::Get().Error(log::RENDER,
                         "SceneBindings: createBuffer(MotionVectorCameraUniforms) failed");
    return;
  }

  // WP2-final — RealTimeQualityUniforms (binding 15), same non-volatile
  // writeBuffer-every-Update() shape as the two cbuffers above.
  nvrhi::BufferDesc qualityCbDesc;
  qualityCbDesc.byteSize = sizeof(RealTimeQualityUniforms);
  qualityCbDesc.debugName = "SceneBindings.RealTimeQualityUniforms";
  qualityCbDesc.isConstantBuffer = true;
  qualityCbDesc.initialState = nvrhi::ResourceStates::ConstantBuffer;
  qualityCbDesc.keepInitialState = true;
  _realTimeQualityBuffer = _device->createBuffer(qualityCbDesc);
  if (!_realTimeQualityBuffer)
  {
    Logging::Get().Error(log::RENDER,
                         "SceneBindings: createBuffer(RealTimeQualityUniforms) failed");
    return;
  }

  // Phase B — RtxSamplingUniforms (binding 16), same non-volatile
  // writeBuffer-every-Update() shape as the two cbuffers above.
  nvrhi::BufferDesc samplingCbDesc;
  samplingCbDesc.byteSize = sizeof(RtxSamplingUniforms);
  samplingCbDesc.debugName = "SceneBindings.RtxSamplingUniforms";
  samplingCbDesc.isConstantBuffer = true;
  samplingCbDesc.initialState = nvrhi::ResourceStates::ConstantBuffer;
  samplingCbDesc.keepInitialState = true;
  _rtxSamplingBuffer = _device->createBuffer(samplingCbDesc);
  if (!_rtxSamplingBuffer)
  {
    Logging::Get().Error(log::RENDER, "SceneBindings: createBuffer(RtxSamplingUniforms) failed");
    return;
  }

  _fallbackMaterialBuffer = MakeStructuredFallback(
      _device, sizeof(shaderinterop::OpenPBRMaterialGPU), "SceneBindings.FallbackMaterial");
  _fallbackInstanceInfoBuffer = MakeStructuredFallback(
      _device, sizeof(shaderinterop::InstanceInfoGpu), "SceneBindings.FallbackInstanceInfo");
  _fallbackLightBuffer = MakeStructuredFallback(_device, sizeof(shaderinterop::LightGpu),
                                                "SceneBindings.FallbackLights");
  static_assert(sizeof(shaderinterop::MeshInfoGpu) == sizeof(shaderinterop::VertexAttribGpu),
                "bindings 4 + 8 share the 32-byte fallback buffer");
  _fallbackStride32Buffer = MakeStructuredFallback(_device, sizeof(shaderinterop::MeshInfoGpu),
                                                   "SceneBindings.FallbackStride32");
  _fallbackMeshIndicesBuffer =
      MakeStructuredFallback(_device, sizeof(std::uint32_t), "SceneBindings.FallbackMeshIndices");
  // gMeshUvs is a TIGHT 8-byte float2 stride (see MeshUvPack.h) — NOT the
  // 16-byte hlslpp::float2.
  _fallbackMeshUvsBuffer =
      MakeStructuredFallback(_device, 2u * sizeof(float), "SceneBindings.FallbackMeshUvs");
  _fallbackMeshFaceNormalsBuffer = MakeStructuredFallback(_device, sizeof(hlslpp::float4),
                                                          "SceneBindings.FallbackMeshFaceNormals");
  if (!_fallbackMaterialBuffer || !_fallbackInstanceInfoBuffer || !_fallbackLightBuffer
      || !_fallbackStride32Buffer || !_fallbackMeshIndicesBuffer || !_fallbackMeshUvsBuffer
      || !_fallbackMeshFaceNormalsBuffer)
  {
    Logging::Get().Error(log::RENDER, "SceneBindings: createBuffer(scene fallbacks) failed");
    return;
  }

  // M7-IBL: 1x1 dome fallback texture, WHITE-initialised on the first
  // Update() call that finds no scene dome (see the Update() body for
  // why white, not black). Default linear-clamp-ish sampler reused as
  // the fallback for BOTH the dome sampler (11) and the material
  // sampler (12).
  nvrhi::TextureDesc domeFallbackDesc;
  domeFallbackDesc.width = 1;
  domeFallbackDesc.height = 1;
  domeFallbackDesc.format = nvrhi::Format::RGBA32_FLOAT;
  domeFallbackDesc.dimension = nvrhi::TextureDimension::Texture2D;
  domeFallbackDesc.debugName = "SceneBindings.FallbackDomeTexture";
  domeFallbackDesc.initialState = nvrhi::ResourceStates::ShaderResource;
  domeFallbackDesc.keepInitialState = true;
  _fallbackDomeTexture = _device->createTexture(domeFallbackDesc);
  if (!_fallbackDomeTexture)
  {
    Logging::Get().Error(log::RENDER, "SceneBindings: createTexture(FallbackDomeTexture) failed");
    return;
  }
  nvrhi::SamplerDesc samplerDesc;
  samplerDesc.minFilter = true;
  samplerDesc.magFilter = true;
  samplerDesc.mipFilter = true;
  samplerDesc.addressU = nvrhi::SamplerAddressMode::Wrap;
  samplerDesc.addressV = nvrhi::SamplerAddressMode::Clamp;
  samplerDesc.addressW = nvrhi::SamplerAddressMode::Wrap;
  _fallbackSampler = _device->createSampler(samplerDesc);
  if (!_fallbackSampler)
  {
    Logging::Get().Error(log::RENDER, "SceneBindings: createSampler(FallbackSampler) failed");
    return;
  }

  _ok = true;
  Logging::Get().Info(log::RENDER, "SceneBindings: initialised (Set-0 layout ready)");
}

SceneBindings::~SceneBindings() = default;

nvrhi::BindingSetHandle SceneBindings::Update(nvrhi::ICommandList* commandList,
                                              const CameraDesc& camera, uint32_t outputHeight,
                                              const SceneResources& res,
                                              const RenderSettings& settings,
                                              uint64_t frameIndex, bool taaJitterEnabled,
                                              uint32_t seed) {
  if (!_ok || commandList == nullptr || res.tlas == nullptr)
    return nullptr;

  const RenderSettings::RealTimeQuality& quality = settings.realTimeQuality;

  // ---- CameraUniforms — the ONE derivation both RT passes duplicated
  // pre-WP2 (see git history of RaytracedGBufferPass::Execute /
  // RaytracedLightingPass::Execute for the byte-identical expressions
  // this replaces). ------------------------------------------------
  // RTX-alignment (rtx-realtime-alignment-design.md): conform the
  // ingest-baked projection to the render-target aspect the way the
  // Omniverse RTX renderer does — the HORIZONTAL aperture wins and the
  // vertical FOV derives from the aspect (crop/expand vertically). The
  // World Lobby cameras author a 1.37:1 film back (20.955 × 15.291)
  // rendered at 16:9; unconformed, Pyxis showed 30% more vertical FOV
  // than ovrtx (measured: depth-edge cross-correlation peaks at
  // sy = 1.30, sx = 1.00 — exactly the 15.2908/11.787 crop-vertical
  // prediction). Scaling row 1 of projFromView rescales the projected
  // y for perspective AND orthographic alike; a no-op when the authored
  // aperture aspect already matches the render aspect.
  float projRaw[16];
  hlslpp::store(projRaw, camera.projFromView);
  hlslpp::float4x4 projFromView = camera.projFromView;
  float projYY = projRaw[5];  // row 1, col 1 in row-major float[16]
  const float renderAspect = (settings.height > 0u)
                                 ? static_cast<float>(settings.width)
                                       / static_cast<float>(settings.height)
                                 : 0.0f;
  const float projYYConformed = projRaw[0] * renderAspect;
  if (renderAspect > 0.0f && std::fabs(projYY) > 1e-6f
      && std::fabs(projYYConformed - projYY) > 1e-6f * std::fabs(projYY))
  {
    const hlslpp::float4x4 conformY(1.0f, 0.0f, 0.0f, 0.0f,
                                    0.0f, projYYConformed / projYY, 0.0f, 0.0f,
                                    0.0f, 0.0f, 1.0f, 0.0f,
                                    0.0f, 0.0f, 0.0f, 1.0f);
    projFromView = hlslpp::mul(conformY, projFromView);
    projYY = projYYConformed;
  }

  CameraUniforms cameraUniforms{};
  cameraUniforms.worldFromView = hlslpp::inverse(camera.viewFromWorld);
  cameraUniforms.viewFromClip = hlslpp::inverse(projFromView);
  cameraUniforms.viewFromWorld = camera.viewFromWorld;
  cameraUniforms.exposure = camera.exposure;
  const auto outputHeightF = static_cast<float>(outputHeight);
  const float tanHalfFov = (projYY > 1e-6f) ? (1.0f / projYY) : 0.0f;
  cameraUniforms.pixelSpreadRadians = (outputHeightF > 0.0f && tanHalfFov > 0.0f)
                                          ? (2.0f * tanHalfFov / outputHeightF)
                                          : 0.0f;
  cameraUniforms.projectionMode = camera.projectionMode;
  cameraUniforms._camPad1 = 0.0f;
  commandList->writeBuffer(_cameraUniformsBuffer.Get(), &cameraUniforms, sizeof(cameraUniforms));

  // ---- MotionVectorCameraUniforms — bit-exact cameraStatic gate,
  // moved here from RaytracedLightingPass (WP2-core: this is now the
  // ONLY upload point, so there is exactly one "previous frame" to
  // track instead of two independent, possibly-diverging histories).
  // See ShaderInterop.slang's MotionVectorCameraUniforms doc comment
  // for why the gate is a CPU-side bit-pattern compare, not a
  // shader-side float comparison. -----------------------------------
  const shaderinterop::float4x4 currentClipFromWorld =
      hlslpp::mul(projFromView, camera.viewFromWorld);
  bool cameraStatic = false;
  if (!_hasPrevCamera)
  {
    _prevClipFromWorld = currentClipFromWorld;
    _hasPrevCamera = true;
    cameraStatic = true;
  }
  else
  {
    float currentRaw[16];
    float prevRaw[16];
    hlslpp::store(currentRaw, currentClipFromWorld);
    hlslpp::store(prevRaw, _prevClipFromWorld);
    cameraStatic = true;
    for (int i = 0; i < 16; ++i)
    {
      if (std::bit_cast<uint32_t>(currentRaw[i]) != std::bit_cast<uint32_t>(prevRaw[i]))
      {
        cameraStatic = false;
        break;
      }
    }
  }
  MotionVectorCameraUniforms motionVectorCameraUniforms{};
  motionVectorCameraUniforms.prevClipFromWorld = _prevClipFromWorld;
  motionVectorCameraUniforms.cameraStatic = cameraStatic ? 1u : 0u;
  motionVectorCameraUniforms._mvPad0 = 0u;
  motionVectorCameraUniforms._mvPad1 = 0u;
  motionVectorCameraUniforms._mvPad2 = 0u;
  commandList->writeBuffer(_motionVectorCameraBuffer.Get(), &motionVectorCameraUniforms,
                           sizeof(motionVectorCameraUniforms));
  // DLSS Stage 2a — snapshot the CONFORMED matrices + the prevClipFromWorld
  // value THIS frame's gMotionVector was actually reprojected against,
  // BEFORE the end-of-function overwrite below rolls _prevClipFromWorld
  // forward to the current frame (see LastPrevClipFromWorldForMotionVectors's
  // doc comment).
  _lastCameraViewToClip = projFromView;
  _lastClipToCameraView = cameraUniforms.viewFromClip;
  _lastWorldFromView = cameraUniforms.worldFromView;
  _lastPrevClipFromWorldForMotionVectors = _prevClipFromWorld;
  _prevClipFromWorld = currentClipFromWorld;

  // ---- RealTimeQualityUniforms (WP2-final) — the ONE upload point every
  // RT/compute pass's gQuality cbuffer read reflects; see
  // ShaderInterop.slang's RealTimeQualityUniforms doc comment for which
  // fields are live vs. Phase-B-reserved no-ops today. ------------------
  RealTimeQualityUniforms qualityUniforms{};
  qualityUniforms.aoRayLength = quality.aoRayLength;
  qualityUniforms.reflectionMaxRoughness = quality.reflectionMaxRoughness;
  qualityUniforms.directSamples = quality.directSamples;
  qualityUniforms.indirectSamples = quality.indirectSamples;
  qualityUniforms.indirectMaxBounces = quality.indirectMaxBounces;
  qualityUniforms.reflectionSamples = quality.reflectionSamples;
  qualityUniforms.refractionMaxBounces = quality.refractionMaxBounces;
  // RTX-alignment design, "unexposed intensity" clamp fix — ovrtx's own
  // schema documents maxRayUnexposedIntensity as "automatically scaled with
  // the exposure": the authored setting (6400/6400/19200) caps the EXPOSED
  // intensity (radiance x exposureScale), so the equivalent cap in Pyxis's
  // own UNEXPOSED-radiance clamp space (where gQuality.maxRayIntensity* is
  // actually consumed — direct_lighting.slang / indirect_diffuse.slang /
  // reflections.slang) is `setting / exposureScale`. ONE shared helper
  // (ExposureMath.h) computes the SAME exposureScale TonemapPass's display
  // path applies, so this can never drift from what the pixel actually
  // shows. Legacy mode with an unauthored (0-stop) camera exposure yields
  // exposureScale == 1.0 exactly, so radiometric fixtures with no exposure
  // stay byte-identical; a strongly-negative authored exposure (World
  // Lobby: -10 stops) inflates the clamp by ~2^10, effectively removing it
  // for a dome that would otherwise be clamped mid-scene. A tiny epsilon
  // floor guards the divide against a pathological near-zero scale
  // producing +inf instead of merely "very large".
  const float exposureScale =
      std::max(ComputeEffectiveExposureScale(settings, camera), 1e-12f);
  qualityUniforms.maxRayIntensityDirect = quality.maxRayIntensityDirect / exposureScale;
  qualityUniforms.maxRayIntensityIndirect = quality.maxRayIntensityIndirect / exposureScale;
  qualityUniforms.maxRayIntensityReflections = quality.maxRayIntensityReflections / exposureScale;
  qualityUniforms._rtqPad0 = 0u;
  qualityUniforms._rtqPad1 = 0u;
  commandList->writeBuffer(_realTimeQualityBuffer.Get(), &qualityUniforms,
                           sizeof(qualityUniforms));

  // ---- RtxSamplingUniforms (Phase B) — the ONE upload point every
  // stochastic estimator's gSampling cbuffer read reflects. jitterX/Y stay
  // exactly 0.0 when TAA is disabled (headless default) — see
  // ComputeHaltonJitter's doc comment + camera_ray.slang's BuildCameraRay
  // for why that keeps the byte-equal path untouched. -------------------
  RtxSamplingUniforms samplingUniforms{};
  samplingUniforms.frameIndex = static_cast<uint32_t>(frameIndex & 0xFFFFFFFFu);
  // RTX-alignment design, "interior illumination" follow-up — wire
  // RenderSettings::seed (Configuration::render.seed, threaded in by
  // PyxisRenderer::RenderFrame) through instead of the fixed
  // DEFAULT_RNG_SEED every stochastic pass used until now. 0 means
  // "caller didn't set it" (a default-constructed RenderSettings used
  // pre-this-change) — fall back to DEFAULT_RNG_SEED rather than seeding
  // PCG32 with zero.
  samplingUniforms.rngSeed = (seed != 0u) ? seed : DEFAULT_RNG_SEED;
  if (taaJitterEnabled)
  {
    const shaderinterop::float2 jitter = ComputeHaltonJitter(frameIndex);
    samplingUniforms.jitterX = jitter.x;
    samplingUniforms.jitterY = jitter.y;
  }
  else
  {
    samplingUniforms.jitterX = 0.0f;
    samplingUniforms.jitterY = 0.0f;
  }
  commandList->writeBuffer(_rtxSamplingBuffer.Get(), &samplingUniforms, sizeof(samplingUniforms));

  // ---- Scene-buffer fallback uploads — same defaults every pass wrote
  // independently pre-WP2. --------------------------------------------
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
    material.specularColorR = 1.0f;
    material.specularColorG = 1.0f;
    material.specularColorB = 1.0f;
    material.specularWeight = 1.0f;
    material.coatColorR = 1.0f;
    material.coatColorG = 1.0f;
    material.coatColorB = 1.0f;
    material.coatIor = 1.6f;
    material.coatDarkening = 1.0f;
    material.fuzzWeight = 0.0f;
    material.fuzzRoughness = 0.5f;
    material.baseDiffuseRoughness = 0.0f;
    material.fuzzColorR = 1.0f;
    material.fuzzColorG = 1.0f;
    material.fuzzColorB = 1.0f;
    material.specularRoughnessAnisotropy = 0.0f;
    material.transmissionColorR = 1.0f;
    material.transmissionColorG = 1.0f;
    material.transmissionColorB = 1.0f;
    material.subsurfaceWeight = 0.0f;
    material.subsurfaceColorR = 0.8f;
    material.subsurfaceColorG = 0.8f;
    material.subsurfaceColorB = 0.8f;
    material.baseWeight = 1.0f;
    material.transmissionWeight = 0.0f;
    return material;
  }();
  static const shaderinterop::LightGpu FALLBACK_LIGHT_DISABLED{};
  static const shaderinterop::InstanceInfoGpu FALLBACK_INSTANCE_INFO_ZERO{};
  static const shaderinterop::MeshInfoGpu FALLBACK_STRIDE32_ZERO{};
  static const std::uint32_t FALLBACK_UINT_ZERO = 0u;
  static const float FALLBACK_FLOAT4_ZERO[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  static const float FALLBACK_FLOAT2_ZERO[2] = {0.0f, 0.0f};

  struct BufferFallbackSpec {
    const void* sceneResource;
    nvrhi::IBuffer* fallback;
    const void* defaultBytes;
    std::size_t defaultByteSize;
  };
  const std::array<BufferFallbackSpec, 6> bufferFallbacks{{
      {res.materialBuffer, _fallbackMaterialBuffer.Get(), &FALLBACK_MATERIAL_GREY,
       sizeof(FALLBACK_MATERIAL_GREY)},
      {res.lightBuffer, _fallbackLightBuffer.Get(), &FALLBACK_LIGHT_DISABLED,
       sizeof(FALLBACK_LIGHT_DISABLED)},
      {res.instanceInfoBuffer, _fallbackInstanceInfoBuffer.Get(), &FALLBACK_INSTANCE_INFO_ZERO,
       sizeof(FALLBACK_INSTANCE_INFO_ZERO)},
      // Bindings 4 (MeshInfoGpu) + 8 (VertexAttribGpu) share the 32-byte
      // zero fallback — two table rows so EITHER scene buffer being
      // null re-arms it.
      {res.meshInfoBuffer, _fallbackStride32Buffer.Get(), &FALLBACK_STRIDE32_ZERO,
       sizeof(FALLBACK_STRIDE32_ZERO)},
      {res.meshVertexAttribsBuffer, _fallbackStride32Buffer.Get(), &FALLBACK_STRIDE32_ZERO,
       sizeof(FALLBACK_STRIDE32_ZERO)},
      {res.meshFaceNormalsBuffer, _fallbackMeshFaceNormalsBuffer.Get(), FALLBACK_FLOAT4_ZERO,
       sizeof(FALLBACK_FLOAT4_ZERO)},
  }};
  for (const BufferFallbackSpec& spec : bufferFallbacks)
  {
    if (spec.sceneResource == nullptr)
      commandList->writeBuffer(spec.fallback, spec.defaultBytes, spec.defaultByteSize);
  }
  if (res.meshUvsBuffer == nullptr)
    commandList->writeBuffer(_fallbackMeshUvsBuffer.Get(), FALLBACK_FLOAT2_ZERO,
                             sizeof(FALLBACK_FLOAT2_ZERO));
  if (res.meshIndicesBuffer == nullptr)
    commandList->writeBuffer(_fallbackMeshIndicesBuffer.Get(), &FALLBACK_UINT_ZERO,
                             sizeof(FALLBACK_UINT_ZERO));

  // M7-IBL: WHITE-init the 1x1 dome fallback when the scene hasn't bound
  // a real env-map — sample returns (1,1,1,1) so `hdri x tint x scale`
  // collapses to `tint x scale` (the dome's authored color / a failed
  // EXR decode), matching the pre-WP2 lighting pass's fallback exactly.
  if (res.domeEnvMapTexture == nullptr)
  {
    static const float FALLBACK_DOME_PIXEL[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    commandList->writeTexture(_fallbackDomeTexture.Get(), 0, 0, FALLBACK_DOME_PIXEL,
                              sizeof(FALLBACK_DOME_PIXEL));
  }

  // ---- Snapshot + rebuild-on-change ----------------------------------
  auto slot = [](BindingSlot index) constexpr noexcept { return static_cast<std::size_t>(index); };
  BindingsSnapshot current{};
  current[slot(BindingSlot::Tlas)] = res.tlas;
  current[slot(BindingSlot::Materials)] = res.materialBuffer;
  current[slot(BindingSlot::InstanceInfo)] = res.instanceInfoBuffer;
  current[slot(BindingSlot::MeshInfo)] = res.meshInfoBuffer;
  current[slot(BindingSlot::Lights)] = res.lightBuffer;
  current[slot(BindingSlot::MeshIndices)] = res.meshIndicesBuffer;
  current[slot(BindingSlot::MeshUvs)] = res.meshUvsBuffer;
  current[slot(BindingSlot::MeshVertexAttribs)] = res.meshVertexAttribsBuffer;
  current[slot(BindingSlot::MeshFaceNormals)] = res.meshFaceNormalsBuffer;
  current[slot(BindingSlot::DomeTexture)] = res.domeEnvMapTexture;
  current[slot(BindingSlot::DomeSampler)] = res.domeSampler;
  current[slot(BindingSlot::MaterialSampler)] = res.bindlessSampler;

  const uint32_t bindlessTextureCount = res.bindlessTextureCount;
  std::uint64_t bindlessFingerprint = 0xcbf29ce484222325ULL;
  bindlessFingerprint ^= bindlessTextureCount;
  bindlessFingerprint *= 0x100000001b3ULL;
  for (uint32_t bindlessSlot = 0; bindlessSlot < bindlessTextureCount; ++bindlessSlot)
  {
    const auto ptrAsInt = reinterpret_cast<std::uintptr_t>(res.bindlessTextures[bindlessSlot]);
    bindlessFingerprint ^= static_cast<std::uint64_t>(ptrAsInt);
    bindlessFingerprint *= 0x100000001b3ULL;
  }

  const bool needsRebuild =
      !_set || current != _lastBindings || bindlessFingerprint != _lastBindlessTextureFingerprint;
  if (!needsRebuild)
    return _set;

  _lastBindings = current;
  _lastBindlessTextureFingerprint = bindlessFingerprint;

  nvrhi::IBuffer* materialBuffer = res.materialBuffer;
  if (materialBuffer == nullptr)
    materialBuffer = _fallbackMaterialBuffer.Get();
  nvrhi::IBuffer* instanceInfoBuffer = res.instanceInfoBuffer;
  if (instanceInfoBuffer == nullptr)
    instanceInfoBuffer = _fallbackInstanceInfoBuffer.Get();
  nvrhi::IBuffer* meshInfoBuffer = res.meshInfoBuffer;
  if (meshInfoBuffer == nullptr)
    meshInfoBuffer = _fallbackStride32Buffer.Get();
  nvrhi::IBuffer* lightBuffer = res.lightBuffer;
  if (lightBuffer == nullptr)
    lightBuffer = _fallbackLightBuffer.Get();
  nvrhi::IBuffer* meshIndicesBuffer = res.meshIndicesBuffer;
  if (meshIndicesBuffer == nullptr)
    meshIndicesBuffer = _fallbackMeshIndicesBuffer.Get();
  nvrhi::IBuffer* meshUvsBuffer = res.meshUvsBuffer;
  if (meshUvsBuffer == nullptr)
    meshUvsBuffer = _fallbackMeshUvsBuffer.Get();
  nvrhi::IBuffer* meshVertexAttribsBuffer = res.meshVertexAttribsBuffer;
  if (meshVertexAttribsBuffer == nullptr)
    meshVertexAttribsBuffer = _fallbackStride32Buffer.Get();
  nvrhi::IBuffer* meshFaceNormalsBuffer = res.meshFaceNormalsBuffer;
  if (meshFaceNormalsBuffer == nullptr)
    meshFaceNormalsBuffer = _fallbackMeshFaceNormalsBuffer.Get();
  nvrhi::ITexture* domeTexture = res.domeEnvMapTexture;
  if (domeTexture == nullptr)
    domeTexture = _fallbackDomeTexture.Get();
  nvrhi::ISampler* domeSampler = res.domeSampler;
  if (domeSampler == nullptr)
    domeSampler = _fallbackSampler.Get();
  nvrhi::ISampler* materialSampler = res.bindlessSampler;
  if (materialSampler == nullptr)
    materialSampler = _fallbackSampler.Get();

  nvrhi::BindingSetDesc setDesc;
  setDesc.bindings = {
      nvrhi::BindingSetItem::ConstantBuffer(0, _cameraUniformsBuffer),
      nvrhi::BindingSetItem::RayTracingAccelStruct(1, res.tlas),
      nvrhi::BindingSetItem::StructuredBuffer_SRV(2, materialBuffer),
      nvrhi::BindingSetItem::StructuredBuffer_SRV(3, instanceInfoBuffer),
      nvrhi::BindingSetItem::StructuredBuffer_SRV(4, meshInfoBuffer),
      nvrhi::BindingSetItem::StructuredBuffer_SRV(5, lightBuffer),
      nvrhi::BindingSetItem::StructuredBuffer_SRV(6, meshIndicesBuffer),
      nvrhi::BindingSetItem::StructuredBuffer_SRV(7, meshUvsBuffer),
      nvrhi::BindingSetItem::StructuredBuffer_SRV(8, meshVertexAttribsBuffer),
      nvrhi::BindingSetItem::StructuredBuffer_SRV(9, meshFaceNormalsBuffer),
      nvrhi::BindingSetItem::Texture_SRV(10, domeTexture),
      nvrhi::BindingSetItem::Sampler(11, domeSampler),
      nvrhi::BindingSetItem::Sampler(12, materialSampler),
      nvrhi::BindingSetItem::ConstantBuffer(14, _motionVectorCameraBuffer),
      nvrhi::BindingSetItem::ConstantBuffer(15, _realTimeQualityBuffer),
      nvrhi::BindingSetItem::ConstantBuffer(16, _rtxSamplingBuffer),
  };

  // ---- Bindless texture array (binding 13) ---------------------------
  // Same walk every pass used pre-WP2: slot 0 = the 4x4 magenta
  // missingTexture defence-in-depth fallback, live slots 1..count from
  // the scene's texture table.
  nvrhi::ITexture* const missingTexture = res.missingTexture;
  if (missingTexture != nullptr)
  {
    auto missingItem = nvrhi::BindingSetItem::Texture_SRV(13, missingTexture);
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
    auto item = nvrhi::BindingSetItem::Texture_SRV(13, sceneTex);
    item.arrayElement = bindlessSlot;
    setDesc.bindings.push_back(item);
  }

  _set = _device->createBindingSet(setDesc, _layout);
  if (!_set)
  {
    Logging::Get().Error(log::RENDER, "SceneBindings: createBindingSet failed");
  }
  return _set;
}

}  // namespace pyxis
