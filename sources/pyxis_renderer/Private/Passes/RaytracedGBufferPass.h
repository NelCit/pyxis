// Pyxis renderer — extended G-buffer pass (RTX-alignment design, WP2-core
// rewrite; was the thin visibility-only P4 pass split).
//
// RT pipeline A of the visibility-buffer architecture (design D1),
// extended per rtx-realtime-alignment-design.md's WP2 implementation
// contract: the raygen builds the camera primary ray (shared
// camera_ray.slang module, bit-identical to the lighting raygen's
// reconstruction), traces ONE TraceRay with a thin 20-byte payload
// (maxRecursionDepth = 1 — primary only; no transparency loop, no
// lighting), and — NEW in WP2 — evaluates the primary hit's material
// ONCE via the shared EvaluateSurfaceMaterial (shading.slang), writing
// the packed 16 B visibility record PLUS the material G-buffer
// (gAlbedo / gNormalRoughness / gEmissive), the denoiser guides
// (gViewZ / gMotionVector, moved here from RaytracedLightingPass per
// WP1), and every id/geometry AOV + the pixel-picker latch (also moved
// here — see raytraced_gbuffer.slang's file header for the one
// documented WP2-core deviation: the pick latch's color/alpha fields
// carry this pass's UNLIT baseColor, not the final lit radiance).
//
// The anyhit is the shared alpha-test stub (raytraced_anyhit.slang
// logic compiled against the thin payload) so visibility semantics are
// bit-identical to the lighting pipeline's hit acceptance.
//
// Bindings — Set 0 is the shared SceneBindings layout (owned by
// PyxisRenderer, ctor-injected; see Private/Passes/SceneBindings.h for
// the full canonical slot map). Set 1 is this pass's OWN layout
// (matched to raytraced_gbuffer.slang's `[[vk::binding(N, 1)]]`
// declarations):
//   set=1, binding=0  : RWStructuredBuffer<VisibilityGpu> (packed 16 B; owned here)
//   set=1, binding=1  : RWTexture2D<float4> gAlbedo          RGBA16F
//   set=1, binding=2  : RWTexture2D<float4> gNormalRoughness RGBA16F
//   set=1, binding=3  : RWTexture2D<float4> gEmissive        RGBA16F
//   set=1, binding=4  : RWTexture2D<float>  gViewZ           R32F
//   set=1, binding=5  : RWTexture2D<float2> gMotionVector    RG16F
//   set=1, binding=6  : RWTexture2D<float4> gOutNormal       RGBA16F
//   set=1, binding=7  : RWTexture2D<float>  gOutDepth        R32F
//   set=1, binding=8  : RWTexture2D<uint>   gOutPrimId       R32U
//   set=1, binding=9  : RWTexture2D<uint>   gOutMaterialId   R32U
//   set=1, binding=10 : RWTexture2D<float4> gOutBaseColor    RGBA16F
//   set=1, binding=11 : RWTexture2D<float4> gOutWorldPos     RGBA32F
//   set=1, binding=12 : RWTexture2D<uint>   gOutElementId    R32U
//   set=1, binding=13 : RWTexture2D<float4> gOutNormalEye    RGBA16F
//   set=1, binding=14 : RWTexture2D<float4> gOutWorldPosEye  RGBA32F
//   set=1, binding=15 : RWStructuredBuffer<PickResult> gPickResult
//   set=1, binding=16 : ConstantBuffer<FrameUiUniforms> gFrameUi
//   set=1, binding=17 : RWTexture2D<float4> gSpecularAlbedo  RGBA16F (DLSS Stage 2b, RR guide)

#pragma once

#include "RenderGraph/IRenderPass.h"

#include <Pyxis/Renderer/Descs/PickResult.h>

#include <nvrhi/nvrhi.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <unordered_map>

namespace pyxis {

class GpuScene;
class SceneBindings;
struct SceneResources;

class RaytracedGBufferPass final : public IRenderPass {
 public:
  RaytracedGBufferPass(nvrhi::IDevice* device, GpuScene& scene, SceneBindings& sceneBindings);
  ~RaytracedGBufferPass() override;
  RaytracedGBufferPass(const RaytracedGBufferPass&) = delete;
  RaytracedGBufferPass& operator=(const RaytracedGBufferPass&) = delete;

  void Execute(nvrhi::ICommandList* commandList, const PassContext& context) override;
  [[nodiscard]] std::string_view Name() const override { return "pass.RaytracedGBuffer"; }

  // Re-load the four entry points' .spv (all compiled from ONE
  // raytraced_gbuffer.slang source, WP2-core) from disk, recreate the RT
  // pipeline + shader binding table. Layout / cbuffers / fallbacks /
  // cached binding sets survive (sets reference the layout, not the
  // pipeline). Returns true iff every step completed; on false the
  // pre-reload pipeline is preserved and rendering continues unchanged.
  // Same contract as RaytracedLightingPass::ReloadShaders.
  [[nodiscard]] bool ReloadShaders() noexcept override;

  // The width*height*16 B (packed, WP2) visibility buffer the raygen
  // writes (Set 1 binding 0) and the RaytracedLightingPass reads (its
  // own Set 1). Also (re)creates the material G-buffer / denoiser-guide
  // textures (gAlbedo / gNormalRoughness / gEmissive) at the same
  // dimensions — one Ensure call, one resize cadence, matching the
  // visibility buffer's lifetime exactly (all four are this pass's own
  // per-pixel scratch, recreated together on a size change only — NEVER
  // inside Execute). PyxisRenderer calls this each RenderFrame and
  // threads the visibility-buffer result through PassContext::visibility.
  [[nodiscard]] nvrhi::IBuffer* EnsureVisibilityBuffer(uint32_t width, uint32_t height);

  // P5 (design D2) — make sure the RT pipeline variant for the ACTIVE
  // camera projection mode exists. The raygen's PROJECTION_MODE
  // specialization constant (its module includes camera_ray.slang) is
  // the only difference between variants: [0] = perspective (eager,
  // ctor), [1] = orthographic (lazy, first frame the camera reports
  // it). Called by PyxisRenderer each RenderFrame on the CPU frame
  // path BEFORE the graph walks — never creates inside Execute
  // (§30.10). Both RT passes select by the same
  // GpuScene::GetCamera().projectionMode source each frame, so they
  // always run the same variant. Same contract as
  // RaytracedLightingPass::EnsureProjectionPipeline.
  void EnsureProjectionPipeline();

  // P6 review — pass-health probe for PyxisRenderer's frame path. True
  // iff the ctor loaded all shaders AND the requested projection-mode
  // variant's pipeline+SBT exist or can still be lazily built (i.e. the
  // lazy build hasn't latched a failure). PyxisRenderer calls this
  // AFTER EnsureProjectionPipeline each frame: when false, it nulls
  // PassContext::visibility (and linearColor) so the downstream passes
  // degrade to the pre-split untouched-output no-op instead of shading
  // from a never-written visibility buffer.
  [[nodiscard]] bool IsOperational(uint32_t projectionMode) const noexcept {
    if (!_shadersOk)
      return false;
    const std::size_t variant = (projectionMode == 1u) ? 1u : 0u;
    if (_pipelines[variant] && _shaderTables[variant])
      return true;
    return !_variantBuildFailed[variant];  // lazy build still possible.
  }

  // Moved here from RaytracedLightingPass (WP2-core — the pick latch
  // ownership moved to this pass's raygen). Returns the cached PickResult
  // the last successful staging-buffer map copied out. See
  // RaytracedLightingPass::GetLastPickResult's pre-WP2 doc comment for
  // the one-frame-stale readback contract (unchanged).
  [[nodiscard]] PickResult GetLastPickResult() const noexcept { return _lastPickResult; }

  // RTX-alignment design (rtx-realtime-alignment-design.md), WP2-signals —
  // accessors PyxisRenderer::RenderFrame uses to thread this pass's
  // material G-buffer into PassContext (gAlbedo / gNormalRoughness /
  // gEmissive) for the new signal passes. Raw opaque pointers (§18.9 —
  // same convention RenderTargets' ITexture* fields use); these three
  // textures are this pass's OWN private scratch (recreated in lockstep
  // with the visibility buffer by EnsureVisibilityBuffer), never null
  // once the pass is operational for at least one prior frame this size.
  [[nodiscard]] nvrhi::ITexture* Albedo() const noexcept { return _gAlbedo.Get(); }
  [[nodiscard]] nvrhi::ITexture* NormalRoughness() const noexcept {
    return _gNormalRoughness.Get();
  }
  [[nodiscard]] nvrhi::ITexture* Emissive() const noexcept { return _gEmissive.Get(); }
  // DLSS Stage 2b — Ray Reconstruction's kBufferTypeSpecularAlbedo guide
  // (raytraced_gbuffer.slang's EnvBRDFApprox2/OpenPBRSpecularF0 helpers).
  // Same lifetime contract as the three accessors above.
  [[nodiscard]] nvrhi::ITexture* SpecularAlbedo() const noexcept {
    return _gSpecularAlbedo.Get();
  }

 private:
  // Build the Set-1 binding set for the supplied visibility buffer +
  // targets. Cached per `nvrhi::IBuffer*` identity (the visibility
  // buffer doubles as the cache key, same as pre-WP2); the caller-owned
  // AOV target pointers + the scene-side borrowed pointers are
  // snapshot-compared each call — any flip invalidates the cache.
  // Bounded, same eviction policy as the lighting pass.
  [[nodiscard]] nvrhi::BindingSetHandle GetOrCreateBindingSet(
      nvrhi::IBuffer* visibility, struct RenderTargets const& targets);

  nvrhi::IDevice* _device = nullptr;
  GpuScene* _scene = nullptr;
  SceneBindings* _sceneBindings = nullptr;  // borrowed; owns the shared Set-0 layout + set.

  // Compiled BASE shaders (unspecialized .spv-backed handles). Built
  // once in the ctor; the per-variant specialized raygen derives from
  // _raygenShader via createShaderSpecialization and lives inside the
  // pipeline objects below.
  nvrhi::ShaderHandle _raygenShader;
  nvrhi::ShaderHandle _missShader;
  nvrhi::ShaderHandle _closestHitShader;
  nvrhi::ShaderHandle _anyHitShader;
  // This pass's OWN Set-1 layout (pass-specific I/O only — Set 0 lives
  // on `_sceneBindings`). Every pipeline variant's
  // pipelineDesc.globalBindingLayouts = {_sceneBindings->Layout(), _passLayout}.
  nvrhi::BindingLayoutHandle _passLayout;
  // P5 (design D2) — one RT pipeline + SBT per camera projection mode
  // (see EnsureProjectionPipeline). Index = 0 perspective /
  // 1 orthographic; Execute selects by GpuScene::GetCamera()'s
  // projectionMode — a pure array lookup, no creation. Same shape +
  // failure latch as the lighting pass's variants.
  static constexpr std::size_t PROJECTION_VARIANT_COUNT = 2;
  std::array<nvrhi::rt::PipelineHandle, PROJECTION_VARIANT_COUNT> _pipelines;
  std::array<nvrhi::rt::ShaderTableHandle, PROJECTION_VARIANT_COUNT> _shaderTables;
  std::array<bool, PROJECTION_VARIANT_COUNT> _variantBuildFailed{};

  // M7 follow-up — per-frame viewer-only UI cbuffer (mousePixel gate for
  // the picker). Moved here from RaytracedLightingPass (WP2-core — the
  // picker moved with the id/geometry AOVs it latches).
  nvrhi::BufferHandle _frameUiBuffer;

  // P4 — cached visibility buffer (see EnsureVisibilityBuffer).
  nvrhi::BufferHandle _visibility;
  uint32_t _visibilityW = 0;
  uint32_t _visibilityH = 0;
  // WP2-core — the material G-buffer / denoiser-guide textures this
  // pass now owns, sized + recreated in lockstep with `_visibility`
  // (EnsureVisibilityBuffer). Unconsumed by any pass yet (the Phase B
  // signal passes that read them are out of this WP's scope) — written
  // every frame as the foundation those passes build on.
  nvrhi::TextureHandle _gAlbedo;
  nvrhi::TextureHandle _gNormalRoughness;
  nvrhi::TextureHandle _gEmissive;
  // DLSS Stage 2b — Ray Reconstruction's specular-albedo guide (see
  // SpecularAlbedo() above). Same resize cadence as the three above.
  nvrhi::TextureHandle _gSpecularAlbedo;
  // P6 review — latched when EnsureVisibilityBuffer creates a FRESH
  // buffer; Execute then clears it to the miss pattern (hitT = -1.0f
  // bits) before any early-out, so a consumer can never read
  // uninitialized records even if this pass later skips its dispatch.
  bool _visibilityNeedsClear = false;
  // P6 review — latched after the first logged createBuffer failure so
  // the per-resize allocation path doesn't spam the log every frame.
  bool _visibilityCreateFailedLogged = false;

  // Binding-set cache, keyed on the visibility buffer pointer (recreated
  // on resize). Bounded; eviction-on-overflow keeps stale dangling
  // pointers from accumulating across re-init.
  std::unordered_map<nvrhi::IBuffer*, nvrhi::BindingSetHandle> _bindingSetCache;

  // Snapshot of the borrowed / caller-owned pointers bound into the
  // Set-1 set — mismatch invalidates the cache. Mirrors
  // RaytracedLightingPass's (pre-WP2) BindingsSnapshot for the same AOV
  // targets, now owned here since the id/geometry AOVs + pick moved.
  enum class BindingSlot : std::size_t {
    NormalAov = 0,
    DepthAov,
    PrimIdAov,
    MaterialIdAov,
    BaseColorAov,
    WorldPosAov,
    ElementIdAov,
    NormalEyeAov,
    WorldPosEyeAov,
    ViewZAov,
    MotionVectorAov,
    PickResult,
    Count,
  };
  static constexpr std::size_t BINDING_SLOT_COUNT =
      static_cast<std::size_t>(BindingSlot::Count);
  using BindingsSnapshot = std::array<const void*, BINDING_SLOT_COUNT>;
  BindingsSnapshot _lastBindings{};

  bool _shadersOk = false;  // true if ctor loaded all four shaders + built pipeline.

  // Tiny no-UAV fallback textures for each raw AOV format, bound when
  // the caller doesn't supply that AOV (headless mode + M2-era
  // color-only paths). Moved here from RaytracedLightingPass (WP2-core).
  nvrhi::TextureHandle _fallbackNormalAov;
  nvrhi::TextureHandle _fallbackDepthAov;
  nvrhi::TextureHandle _fallbackPrimIdAov;
  nvrhi::TextureHandle _fallbackMaterialAov;
  nvrhi::TextureHandle _fallbackBaseColorAov;
  nvrhi::TextureHandle _fallbackWorldPosAov;
  nvrhi::TextureHandle _fallbackElementIdAov;
  nvrhi::TextureHandle _fallbackNormalEyeAov;
  nvrhi::TextureHandle _fallbackWorldPosEyeAov;
  nvrhi::TextureHandle _fallbackViewZAov;
  nvrhi::TextureHandle _fallbackMotionVectorAov;
  nvrhi::BufferHandle _fallbackPickResult;

  // Cached one-frame-stale picker readback. Moved here from
  // RaytracedLightingPass (WP2-core). Updated at the top of each
  // Execute() by mapping the staging buffer (if a copy was submitted in
  // the prior frame). The first Execute returns the default-constructed
  // value (depth -1, instanceId ~0u).
  PickResult _lastPickResult{};
  bool _pickStagingHasFrame = false;
};

}  // namespace pyxis
