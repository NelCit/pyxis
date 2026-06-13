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
  nvrhi::ITexture* linearColor = nullptr;
  // Renderer-internal scratch (P4 pass split): the width*height*32 B
  // RWStructuredBuffer<VisibilityGpu> RaytracedGBufferPass writes (one
  // fp32-exact record per pixel) and RaytracedLightingPass reads (binding 34)
  // to deferred-shade the first hit. RaytracedGBufferPass owns the buffer
  // (EnsureVisibilityBuffer, sized off targets->color like linearColor above);
  // PyxisRenderer::RenderFrame sets this each frame. Null when targets->color
  // is unbound — both RT passes then no-op.
  nvrhi::IBuffer* visibility = nullptr;
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
