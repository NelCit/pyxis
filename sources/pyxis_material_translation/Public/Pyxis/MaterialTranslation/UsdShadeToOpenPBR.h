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
#include <Pyxis/Renderer/Descs/TextureKey.h>

// USD lives in a versioned internal namespace aliased to `pxr`; a
// hand-rolled `namespace pxr { class UsdShadeMaterial; }` forward
// declaration would refer to a *different* (empty) class. The
// real-deal definition has to come from USD's header, so we
// transitively pull it in. Consumers of this lib (pyxis_hydra,
// pyxis_usd_ingest) already need USD anyway.
#include <pxr/usd/usdShade/material.h>

#include <functional>
#include <string_view>

namespace pyxis::material_translation {

// Texture-acquisition callback. Called by the translator each time
// it walks a UsdPreviewSurface input that's connected to a
// UsdUVTexture node. The callback resolves the texture path through
// the renderer's texture cache (typically GpuScene::AcquireTexture)
// and returns a TextureHandle. Returning `TextureHandle::Invalid`
// makes the corresponding map slot fall back to the scalar value
// (the §M5 fallback path).
//
// std::function works here because pyxis_material_translation is a
// STATIC library — the closure runs in the calling TU's address
// space, no DLL boundary crossing.
using AcquireTextureFn =
    std::function<TextureHandle(std::string_view resolvedPath, TextureKey::Role role)>;

// Convert a UsdShadeMaterial into an OpenPBRMaterialDesc. Never
// fails — unsupported shader graphs (no `info:id =
// "UsdPreviewSurface"`, missing surface output, MaterialX-only
// network at M4) return a neutral grey default with
// `Source::FallbackGrey` so the renderer's degraded path lights up
// without aborting ingest. The caller sets `desc.sourcePrim` to the
// material prim's SdfPath.AsString() for log diagnostics.
//
// `acquire` (optional): texture-resolution callback. When non-null,
// the translator follows UsdUVTexture connections on diffuseColor /
// normal / metallic / roughness / emissiveColor / opacity inputs and
// stamps the returned handles into desc.{baseColor,normal,...}Map.
// Pass `nullptr` to skip texture resolution (legacy callers + the
// scalar-only M4 contract).
[[nodiscard]] OpenPBRMaterialDesc FromUsdShade(const pxr::UsdShadeMaterial& material,
                                               const AcquireTextureFn& acquire = nullptr) noexcept;

}  // namespace pyxis::material_translation
