// Pyxis material translation — UsdShadeMaterial → OpenPBRMaterialDesc.
//
// M4 stub: UsdPreviewSurface scalar/color inputs only. Texture
// connections recognised but resolved to TextureHandle::Invalid (the
// §13 TextureCache wires real decode at M5).

#include "Pyxis/MaterialTranslation/UsdShadeToOpenPBR.h"

#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usdShade/material.h>
#include <pxr/usd/usdShade/shader.h>
#include <pxr/usd/usdShade/tokens.h>
#include <pxr/base/gf/vec3f.h>
#include <pxr/base/tf/token.h>
#include <pxr/base/vt/value.h>

namespace pyxis::material_translation {

namespace {

// Locate the surface shader connected to `material`'s `surface`
// output (universal context). Returns an invalid UsdShadeShader if
// nothing is connected, or if the connected node isn't a
// UsdPreviewSurface. Tokens constructed function-locally on first
// use — avoids the cert-err58-cpp diagnostic that fires on TU-scope
// TfToken globals (whose ctor can throw and isn't catchable from
// static-init).
pxr::UsdShadeShader FindUsdPreviewSurface(const pxr::UsdShadeMaterial& material) noexcept {
  static const pxr::TfToken usdPreviewSurfaceToken("UsdPreviewSurface");  // NOLINT(readability-identifier-naming)

  // ComputeSurfaceSource walks the material's surface output back to
  // the connected shader prim. Default contextVector means
  // "universal render context" — Pyxis doesn't render through a
  // named context (Storm-style "glslfx") at M4. Constructing the
  // TfTokenVector explicitly avoids the deprecated single-TfToken
  // overload picked via {} ambiguity.
  pxr::TfToken sourceName;
  pxr::UsdShadeAttributeType sourceType;
  const pxr::UsdShadeShader surface =
      material.ComputeSurfaceSource(pxr::TfTokenVector{}, &sourceName, &sourceType);
  if (!surface.GetPrim().IsValid())
    return pxr::UsdShadeShader{};

  pxr::TfToken shaderId;
  if (!surface.GetShaderId(&shaderId))
    return pxr::UsdShadeShader{};
  if (shaderId != usdPreviewSurfaceToken)
    return pxr::UsdShadeShader{};
  return surface;
}

// Read a scalar float input. Returns `fallback` if the input isn't
// authored, has the wrong type, or is connected to something we
// don't follow at M4.
float ReadFloat(const pxr::UsdShadeShader& shader, const pxr::TfToken& name,
                float fallback) noexcept {
  const pxr::UsdShadeInput input = shader.GetInput(name);
  if (!input)
    return fallback;
  pxr::VtValue value;
  if (!input.Get(&value))
    return fallback;
  if (!value.IsHolding<float>())
    return fallback;
  return value.UncheckedGet<float>();
}

// Read a color3f input as RGB. Returns `fallback` if absent / wrong
// type. Texture connections (the common case for baseColor) read as
// "no value authored" which falls back to the scalar default — the
// closesthit shader uses the flag-bit + texture handle to know to
// sample instead.
hlslpp::float3 ReadColor(const pxr::UsdShadeShader& shader, const pxr::TfToken& name,
                         const hlslpp::float3& fallback) noexcept {
  const pxr::UsdShadeInput input = shader.GetInput(name);
  if (!input)
    return fallback;
  pxr::VtValue value;
  if (!input.Get(&value))
    return fallback;
  if (!value.IsHolding<pxr::GfVec3f>())
    return fallback;
  const pxr::GfVec3f rgb = value.UncheckedGet<pxr::GfVec3f>();
  return hlslpp::float3{rgb[0], rgb[1], rgb[2]};
}

// True iff `input` has a connected source — caller treats this as a
// signal that a texture (or other shader) drives the value at
// runtime. The texture itself is resolved by the §13 TextureCache at
// M5+; M4 just records "yes there's a texture here".
bool HasConnection(const pxr::UsdShadeShader& shader, const pxr::TfToken& name) noexcept {
  const pxr::UsdShadeInput input = shader.GetInput(name);
  if (!input)
    return false;
  return input.HasConnectedSource();
}

}  // namespace

OpenPBRMaterialDesc FromUsdShade(const pxr::UsdShadeMaterial& material) noexcept {
  OpenPBRMaterialDesc desc;

  if (!material.GetPrim().IsValid())
  {
    desc.source = OpenPBRMaterialDesc::Source::Default;
    return desc;
  }

  const pxr::UsdShadeShader surface = FindUsdPreviewSurface(material);
  if (!surface.GetPrim().IsValid())
  {
    // No UsdPreviewSurface connected (or material is empty / uses a
    // MaterialX-only network). Return the grey default with the
    // Default-source tag so the renderer's degraded path can flag it.
    desc.source = OpenPBRMaterialDesc::Source::Default;
    return desc;
  }

  // ---- Scalars + colors (UsdPreviewSurface input names). ----------
  desc.baseColor =
      ReadColor(surface, pxr::TfToken("diffuseColor"), hlslpp::float3{0.18f, 0.18f, 0.18f});
  desc.metalness = ReadFloat(surface, pxr::TfToken("metallic"), 0.0f);
  desc.roughness = ReadFloat(surface, pxr::TfToken("roughness"), 0.5f);
  desc.opacity = ReadFloat(surface, pxr::TfToken("opacity"), 1.0f);
  desc.specularIor = ReadFloat(surface, pxr::TfToken("ior"), 1.5f);
  desc.coatWeight = ReadFloat(surface, pxr::TfToken("clearcoat"), 0.0f);
  desc.coatRoughness = ReadFloat(surface, pxr::TfToken("clearcoatRoughness"), 0.01f);
  desc.emissionColor =
      ReadColor(surface, pxr::TfToken("emissiveColor"), hlslpp::float3{0.0f, 0.0f, 0.0f});

  // OpenPBR's transmission == UsdPreviewSurface's "transmission via
  // 1 - opacity" approximation when the network doesn't author a
  // separate transmission channel. The closesthit shader picks the
  // path based on the resulting (metalness, transmissionWeight) so
  // the heuristic only matters for opacity < 1 networks.
  if (desc.opacity < 1.0f && desc.metalness < 0.5f)
  {
    desc.transmissionWeight = 1.0f - desc.opacity;
  }

  // ---- Texture connections. M4 stub: just record that a connection
  // exists; the actual TextureHandle is resolved by the §13
  // TextureCache at M5+. The closesthit shader reads the flag bits
  // (HasBaseColorMap etc., §11.6) to know whether to sample the
  // texture or use the scalar above; until M5 wires the cache,
  // every map handle stays Invalid and the scalar wins. -------------
  (void)HasConnection(surface, pxr::TfToken("diffuseColor"));
  (void)HasConnection(surface, pxr::TfToken("metallic"));
  (void)HasConnection(surface, pxr::TfToken("roughness"));
  (void)HasConnection(surface, pxr::TfToken("normal"));
  (void)HasConnection(surface, pxr::TfToken("emissiveColor"));
  (void)HasConnection(surface, pxr::TfToken("opacity"));

  desc.source = OpenPBRMaterialDesc::Source::UsdPreviewSurface;
  return desc;
}

}  // namespace pyxis::material_translation
