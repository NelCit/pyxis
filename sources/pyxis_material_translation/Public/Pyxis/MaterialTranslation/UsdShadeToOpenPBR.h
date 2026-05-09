// Pyxis material translation — UsdShadeMaterial → OpenPBRMaterialDesc.
//
// Plan §11 / §25.E. Both ingest adapters call this single function;
// determinism + dedup live downstream in the renderer's
// MaterialTable.
//
// M4 stub: UsdPreviewSurface inputs only. MaterialX
// (open_pbr_surface, standard_surface) + RenderMan fallback land at
// M5. Texture references in the shader graph are recognised but
// resolved to TextureHandle::Invalid in the M4 stub — the §13
// TextureCache wires real decode at M5.

#pragma once

#include <Pyxis/Renderer/Descs/OpenPBRMaterialDesc.h>

// USD lives in a versioned internal namespace aliased to `pxr`; a
// hand-rolled `namespace pxr { class UsdShadeMaterial; }` forward
// declaration would refer to a *different* (empty) class. The
// real-deal definition has to come from USD's header, so we
// transitively pull it in. Consumers of this lib (pyxis_hydra,
// pyxis_usd_ingest) already need USD anyway.
#include <pxr/usd/usdShade/material.h>

namespace pyxis::material_translation {

// Convert a UsdShadeMaterial into an OpenPBRMaterialDesc. Never
// fails — unsupported shader graphs (no `info:id =
// "UsdPreviewSurface"`, missing surface output, MaterialX-only
// network at M4) return a neutral grey default with
// `Source::FallbackGrey` so the renderer's degraded path lights up
// without aborting ingest. The caller sets `desc.sourcePrim` to the
// material prim's SdfPath.AsString() for log diagnostics.
[[nodiscard]] OpenPBRMaterialDesc FromUsdShade(const pxr::UsdShadeMaterial& material) noexcept;

}  // namespace pyxis::material_translation
