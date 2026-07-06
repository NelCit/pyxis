// Pyxis renderer — DlssStatus POD.
//
// DLSS Stage 1 (rtx-realtime-alignment-design.md, "DLSS — corrected
// stance" + "DLSS scope includes upscaling", owner 2026-07-05). Returned
// by PyxisRenderer::GetDlssStatus() — a snapshot of the
// {requested, effective} denoiser resolution PyxisRenderer::RenderFrame
// computes every frame (Private/Dlss/DlssProvider.h owns the underlying
// capability probe). Lets the viewer's status line show the SAME
// resolution the renderer already logged, without re-deriving the probe
// result itself (a second source of truth would drift the moment the
// resolution logic changes).
//
// §18.9 — std::string_view is input-only and must never be returned;
// `reason` reuses the existing ABI-safe inline-owning `ErrorMessage` POD
// (Error.h) rather than inventing a second fixed-buffer string type.

#pragma once

#include <Pyxis/Renderer/Error.h>

#include <cstdint>

namespace pyxis {

struct DlssStatus {
  // Mirrors RenderSettings::RealTimeQuality::denoiser's DENOISER_* encoding
  // (RenderSettings.h). Defaults reflect "no RenderFrame call yet" — the
  // authored default (Dlss) resolved down to today's only reachable
  // outcome (Builtin), matching the pre-Stage-1 behaviour a caller would
  // see before ever querying this accessor.
  uint32_t requestedDenoiser = DENOISER_DLSS;
  uint32_t effectiveDenoiser = DENOISER_BUILTIN;

  // Human-readable reason for a requested != effective downgrade (e.g.
  // "sl.interposer.dll not found (checked exe directory: ...)" or "Stage 2
  // integration pending (device interop required for slInit)"). Empty when
  // requestedDenoiser == effectiveDenoiser (nothing to explain).
  ErrorMessage reason;
};

}  // namespace pyxis
