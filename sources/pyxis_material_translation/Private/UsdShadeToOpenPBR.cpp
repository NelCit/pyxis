// Pyxis material translation — UsdShadeMaterial → OpenPBRMaterialDesc.
//
// M4 stub: UsdPreviewSurface scalar/color inputs only. Texture
// connections recognised but resolved to TextureHandle::Invalid (the
// §13 TextureCache wires real decode at M5).

#include "Pyxis/MaterialTranslation/UsdShadeToOpenPBR.h"

#include <pxr/usd/usd/prim.h>
#include <pxr/usd/sdf/assetPath.h>
#include <pxr/usd/usdShade/connectableAPI.h>
#include <pxr/usd/usdShade/input.h>
#include <pxr/usd/usdShade/material.h>
#include <pxr/usd/usdShade/shader.h>
#include <pxr/usd/usdShade/tokens.h>
#include <pxr/base/gf/vec3f.h>
#include <pxr/base/tf/token.h>
#include <pxr/base/vt/value.h>

#include <string>

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

// Walk a UsdUVTexture's `inputs:st` input back to a connected
// UsdPrimvarReader_float2 node, then read its `inputs:varname`. This
// is which UV set the texture wants ("st" by default; production
// scenes occasionally author "st_1" / "UVMap" / "map1" for detail
// maps + lightmaps). Returns the empty token when the connection
// chain is missing or doesn't land on a primvar reader — caller
// treats that as "default to st".
//
// Most UsdPreviewSurface authoring flows (Substance Painter export,
// usdview's standard converter) leave this connection IMPLICIT — the
// UsdUVTexture has no `inputs:st.connect` at all, and the shading
// engine assumes `st` by default. We honour that convention by
// returning empty (= "st") on any chain break.
pxr::TfToken ResolveUVTextureVarname(const pxr::UsdShadeShader& textureShader) noexcept
{
  static const pxr::TfToken stToken("st");                                // NOLINT(readability-identifier-naming)
  static const pxr::TfToken varnameToken("varname");                      // NOLINT(readability-identifier-naming)
  static const pxr::TfToken primvarReaderFloat2Token("UsdPrimvarReader_float2"); // NOLINT(readability-identifier-naming)

  const pxr::UsdShadeInput stInput = textureShader.GetInput(stToken);
  if (!stInput)
    return {};  // implicit st — caller defaults
  pxr::UsdShadeConnectableAPI source;
  pxr::TfToken                sourceOutputName;
  pxr::UsdShadeAttributeType  sourceType;
  if (!stInput.GetConnectedSource(&source, &sourceOutputName, &sourceType))
    return {};
  const pxr::UsdShadeShader readerShader{source.GetPrim()};
  if (!readerShader.GetPrim().IsValid())
    return {};
  pxr::TfToken shaderId;
  if (!readerShader.GetShaderId(&shaderId) || shaderId != primvarReaderFloat2Token)
    return {};
  // PrimvarReader's `inputs:varname` is authored as either a string
  // (most common) or a token. Try both.
  const pxr::UsdShadeInput varnameInput = readerShader.GetInput(varnameToken);
  if (!varnameInput)
    return {};
  pxr::VtValue value;
  if (!varnameInput.Get(&value))
    return {};
  if (value.IsHolding<pxr::TfToken>())
    return value.UncheckedGet<pxr::TfToken>();
  if (value.IsHolding<std::string>())
    return pxr::TfToken(value.UncheckedGet<std::string>());
  return {};
}

// Walk a UsdPreviewSurface input back to its connected
// UsdUVTexture (the standard pattern: `inputs:diffuseColor.connect =
// </path/to/UsdUVTexture.outputs:rgb>`). Resolves `outFile` to the
// texture's asset path AND `outVarname` to which UV set the texture
// reads from (empty token = default "st", which our renderer's only
// supported UV set today). Returns false for any chain shape that
// isn't a direct UsdPreviewSurface → UsdUVTexture connection (scalar-
// only authored, connected-but-not-UsdUVTexture, missing file attr,
// etc.) — caller falls back to the scalar value.
//
// M8a UV-set indirection: pre-fix we read `inputs:file` and stopped,
// silently treating every texture as if it wanted `st`. This change
// follows the `inputs:st → PrimvarReader → varname` chain so the
// caller can detect (and currently log + skip) textures that ask for
// a UV set we don't ship per `MeshDesc::uv0` — the lobby's materials
// all author `varname = "st"` so this is a no-op there, but it
// prevents silently-wrong texturing on a future scene that authors
// `varname = "st_1"` or `"UVMap"`.
bool ResolveUVTextureBinding(const pxr::UsdShadeShader& shader,
                             const pxr::TfToken& inputName,
                             std::string& outFile,
                             pxr::TfToken& outVarname) noexcept
{
  static const pxr::TfToken usdUVTextureToken("UsdUVTexture");  // NOLINT(readability-identifier-naming)
  static const pxr::TfToken fileToken("file");                  // NOLINT(readability-identifier-naming)

  outFile.clear();
  outVarname = pxr::TfToken{};

  const pxr::UsdShadeInput input = shader.GetInput(inputName);
  if (!input)
    return false;
  pxr::UsdShadeConnectableAPI source;
  pxr::TfToken                sourceOutputName;
  pxr::UsdShadeAttributeType  sourceType;
  if (!input.GetConnectedSource(&source, &sourceOutputName, &sourceType))
    return false;
  // Treat the connectable as a shader and check info:id == UsdUVTexture.
  // If it's a UsdShadeNodeGraph wrapper or a non-shader connectable
  // we just give up — M5 only handles the direct UsdUVTexture pattern.
  const pxr::UsdShadeShader textureShader{source.GetPrim()};
  if (!textureShader.GetPrim().IsValid())
    return false;
  pxr::TfToken shaderId;
  if (!textureShader.GetShaderId(&shaderId) || shaderId != usdUVTextureToken)
    return false;
  // Read the file asset path. SdfAssetPath::GetResolvedPath runs
  // USD's ArResolver against the layer the attr was authored in
  // (handles relative `@../../tex.png@` style refs); fall back to
  // the unresolved string if resolution failed (rare for sibling
  // assets — the AcquireTexture call will surface the missing file
  // via the magenta fallback at decode time).
  const pxr::UsdShadeInput fileInput = textureShader.GetInput(fileToken);
  if (!fileInput)
    return false;
  pxr::VtValue value;
  if (!fileInput.Get(&value) || !value.IsHolding<pxr::SdfAssetPath>())
    return false;
  const pxr::SdfAssetPath asset = value.UncheckedGet<pxr::SdfAssetPath>();
  outFile = asset.GetResolvedPath();
  if (outFile.empty())
    outFile = asset.GetAssetPath();
  if (outFile.empty())
    return false;
  outVarname = ResolveUVTextureVarname(textureShader);
  return true;
}

}  // namespace

OpenPBRMaterialDesc FromUsdShade(const pxr::UsdShadeMaterial& material,
                                 AcquireTextureFn acquire,
                                 void* userData) noexcept {
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
  // UsdPreviewSurface authors emissive as a color3f only — there's no
  // separate luminance multiplier. Set emissionLuminance = 1 when the
  // tint is non-zero so the closesthit's `emissionColor × luminance`
  // term + the MaterialFlag::Emissive bit fire correctly. Magnitude-
  // based threshold keeps materials with explicit `emissiveColor =
  // (0, 0, 0)` non-emissive.
  const float emissionMagnitudeSq = static_cast<float>(
      desc.emissionColor.x * desc.emissionColor.x
      + desc.emissionColor.y * desc.emissionColor.y
      + desc.emissionColor.z * desc.emissionColor.z);
  desc.emissionLuminance = (emissionMagnitudeSq > 1e-6f) ? 1.0f : 0.0f;

  // OpenPBR's transmission == UsdPreviewSurface's "transmission via
  // 1 - opacity" approximation when the network doesn't author a
  // separate transmission channel. The closesthit shader picks the
  // path based on the resulting (metalness, transmissionWeight) so
  // the heuristic only matters for opacity < 1 networks.
  if (desc.opacity < 1.0f && desc.metalness < 0.5f)
  {
    desc.transmissionWeight = 1.0f - desc.opacity;
  }

  // ---- Texture connections (§M5). When the caller supplies an
  // acquire callback we walk each UsdPreviewSurface input back to
  // its UsdUVTexture node, pull the asset path, and stamp the
  // returned TextureHandle into the matching slot. The closesthit's
  // §11.6 flag bits (HasBaseColorMap etc.) get computed downstream
  // by GpuScene's PackMaterialGpu — it sets each Has* bit iff the
  // matching slot resolves to a valid bindless slot. So we don't
  // need to set any flags here; just stamp handles.
  //
  // Without an acquire callback (legacy callers), we silently skip
  // texture resolution — every map slot stays Invalid and the scalar
  // wins, matching the original M4 stub behaviour.
  if (acquire)
  {
    static const pxr::TfToken stToken("st");  // NOLINT(readability-identifier-naming)
    auto resolveSlot = [&](const char* inputName,
                           TextureKey::Role role,
                           TextureHandle& outSlot) {
      std::string path;
      pxr::TfToken varname;
      if (!ResolveUVTextureBinding(surface, pxr::TfToken(inputName), path, varname))
        return;
      // M8a UV-set indirection: only the implicit-default UV (empty
      // varname → "st") and an explicit `varname = "st"` resolve to
      // our renderer's only supported UV set today (`MeshDesc::uv0`).
      // Other names (`st_1`, `UVMap`, …) would silently sample the
      // wrong UV stream — drop them on the floor here and let the
      // material fall back to its scalar baseColor instead. The cost
      // is "no texture" for those materials in scenes that author
      // multi-UV setups; the gain is no visibly-wrong texturing.
      if (!varname.IsEmpty() && varname != stToken)
      {
        // Caller-side log lives in the renderer/ingest, not here —
        // this lib doesn't pull spdlog. Texture stays invalid and
        // PackMaterialGpu's HasBaseColorMap flag falls through to
        // the scalar.
        return;
      }
      outSlot = acquire(path, role, userData);
    };
    // Role drives the §13 colorspace decision at decode time:
    // BaseColor + Emission = sRGB→linear EOTF; everything else
    // (NormalMap, RoughnessMetallic = the "linear data" role)
    // stays linear. UsdPreviewSurface authors metallic / roughness /
    // opacity as separate inputs; all three pull through the
    // RoughnessMetallic role since they're linear data channels.
    resolveSlot("diffuseColor",  TextureKey::Role::BaseColor,         desc.baseColorMap);
    resolveSlot("metallic",      TextureKey::Role::RoughnessMetallic, desc.metallicMap);
    resolveSlot("roughness",     TextureKey::Role::RoughnessMetallic, desc.roughnessMap);
    resolveSlot("normal",        TextureKey::Role::NormalMap,         desc.normalMap);
    resolveSlot("emissiveColor", TextureKey::Role::Emission,          desc.emissionMap);
    resolveSlot("opacity",       TextureKey::Role::RoughnessMetallic, desc.opacityMap);
  }

  desc.source = OpenPBRMaterialDesc::Source::UsdPreviewSurface;
  return desc;
}

}  // namespace pyxis::material_translation
