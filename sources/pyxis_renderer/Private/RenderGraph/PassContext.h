// Pyxis renderer — PassContext.
//
// Plan §9.2. The bag of per-frame state every pass needs in
// Execute(). Passes never get a *back-pointer* to the RenderGraph or
// the GpuScene through here — those are wired at construction time
// (RaytracedLightingPass takes `GpuScene&` in its ctor). The fields below
// are the minimum every pass shares: command list, profiler scope
// target, settings, AOV bindings, monotonic frame index, active
// frames-in-flight depth (for ring sizing inside passes).

#pragma once

#include <Pyxis/Renderer/Descs/RenderSettings.h>
#include <Pyxis/Renderer/Descs/RenderTargets.h>

#include <cstdint>

namespace nvrhi {
class IBindingSet;
class IBuffer;
class ICommandList;
class ITexture;
}

namespace pyxis {

class Profiler;

struct PassContext {
  nvrhi::ICommandList* commandList = nullptr;
  Profiler* profiler = nullptr;
  const RenderSettings* settings = nullptr;
  const RenderTargets* targets = nullptr;
  // Renderer-internal scratch (NOT a public RenderTargets field): the base-res
  // LINEAR color produced by SsaaResolvePass at SSAA factor > 1, consumed by
  // BlitToSrgbPass. PyxisRenderer owns the texture and sets this each frame; null
  // at factor 1 (BlitToSrgbPass then reads targets->color directly). Keeps the
  // SSAA(downsample) + Blit(sRGB) split entirely Private/ -- no §18 surface change.
  nvrhi::ITexture* colorLinearResolved = nullptr;
  // Renderer-internal scratch (NOT a public RenderTargets field): the full-
  // render-resolution fp32 LINEAR radiance (RGBA32F, isUAV + isShaderResource)
  // RaytracedLightingPass writes (binding 2, gLinearColor) and TonemapPass
  // reads to produce the BGRA8 display output in targets->color (P3 pass
  // split). fp32 so the display transform sees bit-identical values to the
  // in-kernel payload.color the old inline raygen branch consumed.
  // RaytracedLightingPass owns the texture (EnsureLinearColor, sized off
  // targets->color, recreated on size change only — never inside a pass
  // Execute); PyxisRenderer::RenderFrame sets this each frame. Null when
  // targets->color is unbound — the RT passes and TonemapPass then no-op,
  // preserving the old "no display target, no trace" behaviour.
  //
  // DLSS Stage 2a — `mutable` so DlssPass::Execute (which only ever sees
  // `const PassContext&`, like every other pass) can redirect this
  // pointer to its OWN display-resolution output once it upscales the
  // render-resolution value CompositePass wrote. This is the one field
  // in this struct a pass is allowed to WRITE, not just read: every
  // consumer downstream of DlssPass in the graph's linear pass order
  // (AutoExposurePass, TonemapPass) is unaware of which pass produced the
  // texture it's reading, by design (same "pointer, not identity" contract
  // every other PassContext field already uses). TaaPass, by contrast,
  // does NOT redirect this pointer — its history buffer is the same size
  // as its input, so it copies its blended result back INTO the existing
  // texture instead (see TaaPass::Execute's own comment); DlssPass cannot
  // do that because render resolution != display resolution.
  mutable nvrhi::ITexture* linearColor = nullptr;
  // Renderer-internal scratch (P4 pass split): the width*height*32 B
  // RWStructuredBuffer<VisibilityGpu> RaytracedGBufferPass writes (one
  // fp32-exact record per pixel) and RaytracedLightingPass reads (binding 34)
  // to deferred-shade the first hit. RaytracedGBufferPass owns the buffer
  // (EnsureVisibilityBuffer, sized off targets->color like linearColor above);
  // PyxisRenderer::RenderFrame sets this each frame. Null when targets->color
  // is unbound — both RT passes then no-op.
  nvrhi::IBuffer* visibility = nullptr;
  // RTX-alignment design (rtx-realtime-alignment-design.md), WP2-core —
  // the shared Set-0 SceneBindings binding set (camera cbuffers, TLAS,
  // scene tables, bindless textures — see Private/Passes/SceneBindings.h),
  // rebuilt ONCE per frame by PyxisRenderer::RenderFrame (via
  // SceneBindings::Update, on the CPU path BEFORE the graph walks) and
  // consumed by every RT pass (RaytracedGBufferPass, RaytracedLightingPass)
  // as `state.bindings[0]` alongside their own Set-1 set. Null exactly
  // when there's nothing to trace yet (no TLAS, or SceneBindings failed
  // at construction) — every RT pass early-outs on that, mirroring the
  // pre-WP2 `res.tlas == nullptr` gate.
  nvrhi::IBindingSet* sceneBindingSet = nullptr;
  // RTX-alignment design (rtx-realtime-alignment-design.md), WP2-signals —
  // the G-buffer products RaytracedGBufferPass computes each frame,
  // threaded to every signal pass between it and RaytracedLightingPass
  // (DirectLighting / IndirectDiffuse / AmbientOcclusion / Reflections /
  // Translucency). PyxisRenderer::RenderFrame sets these each frame
  // (gAlbedo/gNormalRoughness/gEmissive from new accessors on
  // RaytracedGBufferPass; gViewZ/gMotionVector are simply the caller's
  // own optional RenderTargets::viewZAov/motionVector pointers — those
  // are ALREADY caller-allocated cross-DLL resources threaded via
  // `targets`, so mirroring them here needs no new accessor, just a
  // copy of what RenderFrame already has in scope). All five are
  // nullable — null exactly when RaytracedGBufferPass isn't operational
  // (mirrors the `visibility` gate above) or, for the last two, when the
  // caller didn't allocate that optional AOV. No signal pass in Phase A
  // reads gViewZ/gMotionVector yet (only AmbientOcclusionPass and
  // ReflectionsPass read gNormalRoughness, for the primary surface's
  // normal + roughness-cutoff gate) — threaded regardless, per the WP2
  // contract, so later phases don't need another PassContext growth.
  nvrhi::ITexture* gAlbedo = nullptr;
  nvrhi::ITexture* gNormalRoughness = nullptr;
  nvrhi::ITexture* gEmissive = nullptr;
  nvrhi::ITexture* gViewZ = nullptr;
  nvrhi::ITexture* gMotionVector = nullptr;
  // DLSS Stage 2b (rtx-realtime-alignment-design.md) — RR's
  // kBufferTypeSpecularAlbedo guide (RaytracedGBufferPass::SpecularAlbedo()).
  // Null under the exact same conditions as gAlbedo above; consumed only by
  // DlssPass's Ray-Reconstruction path.
  nvrhi::ITexture* gSpecularAlbedo = nullptr;
  // RTX-alignment design (rtx-realtime-alignment-design.md), WP2-final —
  // the five Phase A signal passes' own outputs, threaded here now that
  // CompositePass is their first real consumer (WP2-signals' comment on
  // this struct anticipated exactly this: "Phase B recombine them in a
  // CompositePass"). PyxisRenderer::RenderFrame sets these each frame
  // from the borrowed signal-pass pointers it already holds (the same
  // accessors DebugSignalTexture forwards to); null exactly when the
  // owning pass isn't operational this frame, mirroring the gAlbedo /
  // gNormalRoughness / gEmissive gate above.
  nvrhi::ITexture* gAo = nullptr;
  nvrhi::ITexture* gDirectDiffuse = nullptr;
  nvrhi::ITexture* gDirectSpecular = nullptr;
  nvrhi::ITexture* gIndirectDiffuse = nullptr;
  nvrhi::ITexture* gReflections = nullptr;
  nvrhi::ITexture* gReflectionWeight = nullptr;
  nvrhi::ITexture* gTranslucency = nullptr;
  // NRD stage 3 (RTX-alignment 2026-07-10, PYXIS_WITH_NRD builds only):
  // NrdDenoisePass::Execute sets these to its denoised diffuse/specular
  // outputs when NRD actually ran this frame; CompositePass then prefers
  // them over the builtin à-trous chain's outputs. `mutable` for the same
  // reason as `linearColor` above — an earlier pass hands its result
  // forward through the shared per-frame context. Always null in non-NRD
  // builds and whenever the effective denoiser isn't Nrd, so the builtin
  // path is untouched byte-for-byte.
  mutable nvrhi::ITexture* nrdDenoisedDiffuse = nullptr;
  mutable nvrhi::ITexture* nrdDenoisedSpecular = nullptr;
  // Renderer-internal scratch: an 8-byte uint2 buffer (sum of fixed-point
  // log2-luminance, lit-pixel count) AutoExposurePass clears + accumulates from
  // `linearColor` and TonemapPass reads to derive the auto exposure. PyxisRenderer
  // owns it (created once — 8 bytes, never resized) and sets it each frame.
  // Null / untouched when auto-exposure is disabled (TonemapPass ignores it).
  nvrhi::IBuffer* autoExposureStats = nullptr;
  uint64_t frameIndex = 0;
  // Default 0 to flush out anyone who forgot to wire it through —
  // PyxisRenderer::RenderFrame always sets the real value. A pass
  // that depends on this should assert framesInFlight > 0 in its
  // Execute().
  uint32_t framesInFlight = 0;
};

}  // namespace pyxis
