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
#include <pxr/base/gf/vec2f.h>
#include <pxr/base/gf/vec3f.h>
#include <pxr/base/tf/token.h>
#include <pxr/base/vt/value.h>

#include <cctype>
#include <filesystem>
#include <string>
#include <string_view>

namespace pyxis::material_translation {

namespace {

// V2.A.18 — multi-render-context surface resolution.
//
// USD allows a single UsdShadeMaterial to author multiple surface
// outputs side-by-side, one per render context: `outputs:surface`
// (universal — UsdPreviewSurface convention), `outputs:mtlx:surface`
// (MaterialX), `outputs:mdl:surface` (MDL / Omniverse). v1 just
// queried the universal context + sniffed the connected shader's
// `info:id` — which silently picked UsdPreviewSurface even on
// materials that also authored a MaterialX or MDL surface (common in
// production Omniverse + MaterialX-DCC scenes).
//
// v2 tries contexts in priority order (mdl → mtlx → universal) per
// plan §V2.A.18 + the priority block at the bottom of that section
// ("falling back to whichever exists"). Each `ComputeSurfaceSource`
// call returns ONLY the surface authored under the named context, so
// materials with one surface still resolve correctly (the other two
// contexts return nothing and we fall through).
struct SurfaceMatch {
  enum class Kind : uint8_t {
    None,
    Mdl,
    MaterialXOpenPBR,
    MaterialXStandardSurface,
    UsdPreviewSurface,
  };
  pxr::UsdShadeShader shader;
  Kind                kind = Kind::None;
};

// Sniff a connected shader's info:id and bucket it into the right
// SurfaceMatch::Kind. Unknown ids fall to Kind::None — the caller
// keeps walking the context chain.
[[nodiscard]] SurfaceMatch::Kind ClassifyShaderId(const pxr::TfToken& shaderId) noexcept
{
  static const pxr::TfToken usdPreviewSurfaceToken("UsdPreviewSurface");  // NOLINT(readability-identifier-naming)
  const std::string& idStr = shaderId.GetString();
  if (idStr.starts_with("mdl::"))
    return SurfaceMatch::Kind::Mdl;
  if (idStr.starts_with("ND_open_pbr_surface"))
    return SurfaceMatch::Kind::MaterialXOpenPBR;
  if (idStr.starts_with("ND_standard_surface"))
    return SurfaceMatch::Kind::MaterialXStandardSurface;
  if (shaderId == usdPreviewSurfaceToken)
    return SurfaceMatch::Kind::UsdPreviewSurface;
  return SurfaceMatch::Kind::None;
}

// Try a single render context. Returns a fully-populated match (with
// the connected shader + classified kind) if the named
// `outputs:[<context>:]surface` output is authored AND connects to a
// recognised shader; otherwise Kind::None.
//
// Important: this resolves AGAINST THE NAMED OUTPUT, not through
// `ComputeSurfaceSource(contextVector)`. USD's resolver falls back to
// the universal context whenever the requested context isn't
// authored, which silently picks UsdPreviewSurface even when we
// explicitly asked for `mtlx`/`mdl`. Going through the named output
// keeps each context strictly independent so the priority chain in
// `ResolveSurface` behaves as documented.
[[nodiscard]] SurfaceMatch TryRenderContext(const pxr::UsdShadeMaterial& material,
                                             const pxr::TfToken&          contextToken) noexcept
{
  SurfaceMatch match;
  const pxr::UsdShadeOutput surfaceOutput =
      contextToken.IsEmpty() ? material.GetSurfaceOutput()
                             : material.GetSurfaceOutput(contextToken);
  if (!surfaceOutput || !surfaceOutput.HasConnectedSource())
    return match;
  pxr::UsdShadeConnectableAPI source;
  pxr::TfToken                sourceOutputName;
  pxr::UsdShadeAttributeType  sourceType;
  if (!surfaceOutput.GetConnectedSource(&source, &sourceOutputName, &sourceType))
    return match;
  const pxr::UsdShadeShader candidate{source.GetPrim()};
  if (!candidate.GetPrim().IsValid())
    return match;
  pxr::TfToken shaderId;
  if (!candidate.GetShaderId(&shaderId))
    return match;
  match.kind = ClassifyShaderId(shaderId);
  if (match.kind != SurfaceMatch::Kind::None)
    match.shader = candidate;
  return match;
}

// Walk the (mdl → mtlx → universal) render-context priority chain
// and return the first match. Per the plan §V2.A.18, mdl wins over
// MaterialX wins over UsdPreviewSurface — production Omniverse
// pipelines author all three side-by-side; we want the highest-
// fidelity authored form. The empty TfToken at the tail is the
// universal context (where UsdPreviewSurface lives).
[[nodiscard]] SurfaceMatch ResolveSurface(const pxr::UsdShadeMaterial& material) noexcept
{
  // TfToken ctors are noexcept after first use (interned) but can
  // throw on the very first call (heap alloc for the string). Wrap
  // in function-local statics so they're constructed once on first
  // entry into this TU and never throw thereafter (cert-err58-cpp).
  static const pxr::TfToken mdlToken("mdl");      // NOLINT(readability-identifier-naming)
  static const pxr::TfToken mtlxToken("mtlx");    // NOLINT(readability-identifier-naming)
  static const pxr::TfToken universalToken{};     // NOLINT(readability-identifier-naming)

  if (SurfaceMatch match = TryRenderContext(material, mdlToken);
      match.kind != SurfaceMatch::Kind::None)
    return match;
  if (SurfaceMatch match = TryRenderContext(material, mtlxToken);
      match.kind != SurfaceMatch::Kind::None)
    return match;
  return TryRenderContext(material, universalToken);
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

// V2.A.18 — UsdTransform2d UV transform captured from the
// UsdUVTexture.st → UsdTransform2d → UsdPrimvarReader chain. USD
// composes UV coords through scale → rotation (degrees, CCW) →
// translation (matching MaterialX's `texcoord` xform convention).
// All-zero defaults represent "no Transform2d in the chain";
// `hasTransform2d` distinguishes "absent" from "authored identity".
struct UVTransform2d {
  float translationX = 0.0f;
  float translationY = 0.0f;
  float rotationDeg  = 0.0f;
  float scaleX       = 1.0f;
  float scaleY       = 1.0f;
  bool  hasTransform2d = false;
};

// Read an authored UsdShadeInput as float / float2 and stamp into
// `outX` / `outY`. Returns true iff the input was authored AND held a
// matching type. Silent fallback on type mismatch — typos in the
// authoring layer would otherwise crash the translator on an
// `UncheckedGet`.
bool ReadShaderInputFloat(const pxr::UsdShadeShader& shader,
                           const pxr::TfToken&        name,
                           float&                     outValue) noexcept
{
  const pxr::UsdShadeInput input = shader.GetInput(name);
  if (!input)
    return false;
  pxr::VtValue value;
  if (!input.Get(&value) || !value.IsHolding<float>())
    return false;
  outValue = value.UncheckedGet<float>();
  return true;
}

bool ReadShaderInputFloat2(const pxr::UsdShadeShader& shader,
                            const pxr::TfToken&        name,
                            float&                     outX,
                            float&                     outY) noexcept
{
  const pxr::UsdShadeInput input = shader.GetInput(name);
  if (!input)
    return false;
  pxr::VtValue value;
  if (!input.Get(&value))
    return false;
  if (value.IsHolding<pxr::GfVec2f>())
  {
    const pxr::GfVec2f vec = value.UncheckedGet<pxr::GfVec2f>();
    outX = vec[0];
    outY = vec[1];
    return true;
  }
  return false;
}

// V2.A.18 — capture UsdTransform2d params + return the upstream
// `in` connection so the caller can continue walking toward the
// PrimvarReader. `outConnected` stays null on a chain break.
void ReadTransform2dParams(const pxr::UsdShadeShader& xformShader,
                            UVTransform2d&             outXform,
                            pxr::UsdShadeShader&       outConnected) noexcept
{
  static const pxr::TfToken translationToken("translation");  // NOLINT(readability-identifier-naming)
  static const pxr::TfToken rotationToken("rotation");        // NOLINT(readability-identifier-naming)
  static const pxr::TfToken scaleToken("scale");              // NOLINT(readability-identifier-naming)
  static const pxr::TfToken inToken("in");                    // NOLINT(readability-identifier-naming)

  outXform.hasTransform2d = true;
  ReadShaderInputFloat2(xformShader, translationToken,
                         outXform.translationX, outXform.translationY);
  ReadShaderInputFloat(xformShader, rotationToken, outXform.rotationDeg);
  ReadShaderInputFloat2(xformShader, scaleToken, outXform.scaleX, outXform.scaleY);

  // Continue walking: Transform2d.inputs:in connects to the next
  // node in the chain (usually a PrimvarReader, but USD allows
  // arbitrary chains; we just follow one hop).
  const pxr::UsdShadeInput inInput = xformShader.GetInput(inToken);
  if (!inInput)
    return;
  pxr::UsdShadeConnectableAPI inSource;
  pxr::TfToken                inOutputName;
  pxr::UsdShadeAttributeType  inSourceType;
  if (!inInput.GetConnectedSource(&inSource, &inOutputName, &inSourceType))
    return;
  outConnected = pxr::UsdShadeShader{inSource.GetPrim()};
}

// Walk a UsdUVTexture's `inputs:st` input back to a connected
// UsdPrimvarReader_float2 node, then read its `inputs:varname`. This
// is which UV set the texture wants ("st" by default; production
// scenes occasionally author "st_1" / "UVMap" / "map1" for detail
// maps + lightmaps). Returns the empty token when the connection
// chain is missing or doesn't land on a primvar reader — caller
// treats that as "default to st".
//
// V2.A.18 — the chain may be direct (texture.st → PrimvarReader) or
// go through one or more intermediates (texture.st → Transform2d →
// PrimvarReader). When a UsdTransform2d is in the path, we capture
// its `translation` / `rotation` / `scale` into `outXform` and keep
// walking toward the reader.
//
// Most UsdPreviewSurface authoring flows (Substance Painter export,
// usdview's standard converter) leave this connection IMPLICIT — the
// UsdUVTexture has no `inputs:st.connect` at all, and the shading
// engine assumes `st` by default. We honour that convention by
// returning empty (= "st") on any chain break.
pxr::TfToken ResolveUVTextureVarnameAndXform(const pxr::UsdShadeShader& textureShader,
                                              UVTransform2d&             outXform) noexcept
{
  static const pxr::TfToken stToken("st");                                       // NOLINT(readability-identifier-naming)
  static const pxr::TfToken varnameToken("varname");                             // NOLINT(readability-identifier-naming)
  static const pxr::TfToken primvarReaderFloat2Token("UsdPrimvarReader_float2"); // NOLINT(readability-identifier-naming)
  static const pxr::TfToken usdTransform2dToken("UsdTransform2d");               // NOLINT(readability-identifier-naming)

  const pxr::UsdShadeInput stInput = textureShader.GetInput(stToken);
  if (!stInput)
    return {};  // implicit st — caller defaults
  pxr::UsdShadeConnectableAPI source;
  pxr::TfToken                sourceOutputName;
  pxr::UsdShadeAttributeType  sourceType;
  if (!stInput.GetConnectedSource(&source, &sourceOutputName, &sourceType))
    return {};
  pxr::UsdShadeShader currentShader{source.GetPrim()};

  // Bounded chain walk (max 8 hops) so a degenerate cycle in the
  // authoring layer can't put us into an infinite loop. Real chains
  // are at most 2 hops (Transform2d → PrimvarReader); the 8-hop
  // budget accommodates future extensions (PlaceUV, additional
  // transform nodes) without changing the algorithm.
  for (int hop = 0; hop < 8; ++hop)
  {
    if (!currentShader.GetPrim().IsValid())
      return {};
    pxr::TfToken shaderId;
    if (!currentShader.GetShaderId(&shaderId))
      return {};

    if (shaderId == primvarReaderFloat2Token)
    {
      // Terminal node — read varname and we're done.
      const pxr::UsdShadeInput varnameInput = currentShader.GetInput(varnameToken);
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

    if (shaderId == usdTransform2dToken)
    {
      // Intermediate node — capture params + continue walking.
      pxr::UsdShadeShader next;
      ReadTransform2dParams(currentShader, outXform, next);
      currentShader = next;
      continue;
    }

    // Unknown intermediate (e.g. a custom UV-flipper). Give up; the
    // caller treats this as "implicit st".
    return {};
  }
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
struct UVTextureBinding {
  std::string   file;       // V2.A.7: when hasUdim, this is the 1001-substituted path.
  std::string   udimPattern;// V2.A.7: original path with `<UDIM>` token preserved.
  pxr::TfToken  varname;
  pxr::TfToken  wrapS;       // V2.A.24 — `useMetadata` / `repeat` / `mirror` / `clamp` / `black`.
  pxr::TfToken  wrapT;
  pxr::TfToken  sourceColorSpace;  // V2.A.29 — `auto` / `raw` / `sRGB`.
  UVTransform2d uvXform;     // V2.A.18 — UsdTransform2d in the chain (identity when absent).
  bool          hasUdim = false;   // V2.A.7 — `<UDIM>` token was present in the asset path.
};

bool ResolveUVTextureBinding(const pxr::UsdShadeShader& shader,
                             const pxr::TfToken& inputName,
                             UVTextureBinding& outBinding) noexcept
{
  static const pxr::TfToken usdUVTextureToken("UsdUVTexture");  // NOLINT(readability-identifier-naming)
  static const pxr::TfToken fileToken("file");                  // NOLINT(readability-identifier-naming)
  static const pxr::TfToken wrapSToken("wrapS");                // NOLINT(readability-identifier-naming)
  static const pxr::TfToken wrapTToken("wrapT");                // NOLINT(readability-identifier-naming)
  static const pxr::TfToken sourceColorSpaceToken("sourceColorSpace");  // NOLINT(readability-identifier-naming)

  std::string&   outFile    = outBinding.file;
  pxr::TfToken&  outVarname = outBinding.varname;
  outBinding.wrapS = pxr::TfToken{};
  outBinding.wrapT = pxr::TfToken{};
  outBinding.sourceColorSpace = pxr::TfToken{};

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

  // V2.A.7 — UDIM. Detect `<UDIM>` in the asset path, preserve the
  // pattern for multi-tile enumeration at the call site, and
  // substitute `1001` (the bottom-left tile per USD spec) into the
  // primary `file` field so the existing single-handle slot points
  // at tile 1001. The caller's multi-tile loop then walks 1002..1099
  // and acquires each existing tile into bindless. The closesthit
  // shader samples only the 1001 handle today (per v2 "loaded but
  // not yet shader-visible" directive).
  if (const auto udimPos = outFile.find("<UDIM>"); udimPos != std::string::npos)
  {
    outBinding.udimPattern = outFile;
    outBinding.hasUdim = true;
    constexpr std::size_t UDIM_TOKEN_LEN = 6u;  // length of literal "<UDIM>"
    outFile.replace(udimPos, UDIM_TOKEN_LEN, "1001");
  }

  // M21 / V2.A.14 — compressed-texture format detection. Pyxis ships
  // with stb_image (PNG/JPG/etc) + tinyexr (EXR) at v2.0; DDS / KTX2 /
  // TIFF decoders + BCn-encoded uploads land in a follow-up milestone.
  // For now we detect the extension on the resolved path, log a one-
  // shot warning, and let the loader fail through to the magenta-
  // missing-texture fallback so the operator sees the gap explicitly.
  {
    auto endsWithCaseInsensitive = [](const std::string& path,
                                       std::string_view suffix) -> bool
    {
      if (path.size() < suffix.size())
        return false;
      const std::size_t offset = path.size() - suffix.size();
      for (std::size_t idx = 0; idx < suffix.size(); ++idx)
      {
        const char lhs = static_cast<char>(std::tolower(
            static_cast<unsigned char>(path[offset + idx])));
        const char rhs = static_cast<char>(std::tolower(
            static_cast<unsigned char>(suffix[idx])));
        if (lhs != rhs)
          return false;
      }
      return true;
    };
    if (endsWithCaseInsensitive(outFile, ".dds")
        || endsWithCaseInsensitive(outFile, ".ktx2")
        || endsWithCaseInsensitive(outFile, ".tif")
        || endsWithCaseInsensitive(outFile, ".tiff"))
    {
      // Decoders for these formats are a follow-up; the renderer's
      // magenta fallback will fire on AcquireTexture. Loud-log here
      // so the operator knows why the texture is magenta.
      // (No spdlog handle in this static lib; the warning lands in
      // the renderer when AcquireTexture surfaces the decode failure.)
    }
  }

  // V2.A.24 — record authored wrap modes (`useMetadata` / `repeat` /
  // `mirror` / `clamp` / `black`). Pyxis's global sampler is `repeat`;
  // the artist-authored value is preserved on the desc so future
  // per-material sampler dispatch has a CPU-side source.
  if (const pxr::UsdShadeInput wrapSInput = textureShader.GetInput(wrapSToken); wrapSInput)
  {
    pxr::VtValue value;
    if (wrapSInput.Get(&value) && value.IsHolding<pxr::TfToken>())
      outBinding.wrapS = value.UncheckedGet<pxr::TfToken>();
  }
  if (const pxr::UsdShadeInput wrapTInput = textureShader.GetInput(wrapTToken); wrapTInput)
  {
    pxr::VtValue value;
    if (wrapTInput.Get(&value) && value.IsHolding<pxr::TfToken>())
      outBinding.wrapT = value.UncheckedGet<pxr::TfToken>();
  }
  // V2.A.29 — UsdUVTexture sourceColorSpace authored by the artist.
  if (const pxr::UsdShadeInput csInput = textureShader.GetInput(sourceColorSpaceToken); csInput)
  {
    pxr::VtValue value;
    if (csInput.Get(&value) && value.IsHolding<pxr::TfToken>())
      outBinding.sourceColorSpace = value.UncheckedGet<pxr::TfToken>();
  }

  outVarname = ResolveUVTextureVarnameAndXform(textureShader, outBinding.uvXform);
  return true;
}

// V2.A.18 — MaterialX OpenPBR (`open_pbr_surface`) translator. Pyxis's
// canonical target; the OpenPBRMaterialDesc IS the MTLX OpenPBR model
// post-translation, so field-for-field mapping.
void TranslateMaterialXOpenPBR(const pxr::UsdShadeShader& shader,
                                OpenPBRMaterialDesc&       desc) noexcept
{
  desc.baseColor      = ReadColor(shader, pxr::TfToken("base_color"),
                                   hlslpp::float3{0.18f, 0.18f, 0.18f});
  desc.baseWeight     = ReadFloat(shader, pxr::TfToken("base_weight"), 1.0f);
  desc.metalness      = ReadFloat(shader, pxr::TfToken("base_metalness"), 0.0f);
  desc.roughness      = ReadFloat(shader, pxr::TfToken("specular_roughness"), 0.5f);
  desc.specularWeight = ReadFloat(shader, pxr::TfToken("specular_weight"), 1.0f);
  desc.specularIor    = ReadFloat(shader, pxr::TfToken("specular_ior"), 1.5f);
  desc.opacity        = ReadFloat(shader, pxr::TfToken("geometry_opacity"), 1.0f);
  desc.coatWeight     = ReadFloat(shader, pxr::TfToken("coat_weight"), 0.0f);
  desc.coatRoughness  = ReadFloat(shader, pxr::TfToken("coat_roughness"), 0.0f);
  desc.transmissionWeight = ReadFloat(shader, pxr::TfToken("transmission_weight"), 0.0f);
  desc.emissionColor  = ReadColor(shader, pxr::TfToken("emission_color"),
                                   hlslpp::float3{0.0f, 0.0f, 0.0f});
  desc.emissionLuminance = ReadFloat(shader, pxr::TfToken("emission_luminance"), 0.0f);
  desc.source = OpenPBRMaterialDesc::Source::MaterialX;
}

// V2.A.18 — Autodesk standard_surface translator (MaterialX). Uses
// `base` as the layer weight (not `base_weight` like OpenPBR) and
// collapses the (float emission weight, color3 emission color) pair
// into Pyxis's `emissionLuminance + emissionColor` fields.
void TranslateMaterialXStandardSurface(const pxr::UsdShadeShader& shader,
                                        OpenPBRMaterialDesc&       desc) noexcept
{
  desc.baseColor      = ReadColor(shader, pxr::TfToken("base_color"),
                                   hlslpp::float3{0.18f, 0.18f, 0.18f});
  desc.baseWeight     = ReadFloat(shader, pxr::TfToken("base"), 1.0f);
  desc.metalness      = ReadFloat(shader, pxr::TfToken("metalness"), 0.0f);
  desc.roughness      = ReadFloat(shader, pxr::TfToken("specular_roughness"), 0.5f);
  desc.specularWeight = ReadFloat(shader, pxr::TfToken("specular"), 1.0f);
  desc.specularIor    = ReadFloat(shader, pxr::TfToken("specular_IOR"), 1.5f);
  desc.coatWeight     = ReadFloat(shader, pxr::TfToken("coat"), 0.0f);
  desc.coatRoughness  = ReadFloat(shader, pxr::TfToken("coat_roughness"), 0.0f);
  desc.transmissionWeight = ReadFloat(shader, pxr::TfToken("transmission"), 0.0f);
  desc.emissionColor  = ReadColor(shader, pxr::TfToken("emission_color"),
                                   hlslpp::float3{0.0f, 0.0f, 0.0f});
  desc.emissionLuminance = ReadFloat(shader, pxr::TfToken("emission"), 0.0f);
  desc.source = OpenPBRMaterialDesc::Source::MaterialX;
}

// V2.A.23 — MDL OmniPBR / OmniGlass translator. Inputs match
// Omniverse's OmniPBR.mdl shader. Inputs missing on a given variant
// fall through to the OpenPBR scalar defaults.
void TranslateMdl(const pxr::UsdShadeShader& shader,
                   OpenPBRMaterialDesc&       desc) noexcept
{
  desc.baseColor = ReadColor(shader, pxr::TfToken("diffuse_color_constant"),
                              hlslpp::float3{0.18f, 0.18f, 0.18f});
  desc.metalness = ReadFloat(shader, pxr::TfToken("metallic_constant"), 0.0f);
  desc.roughness = ReadFloat(shader, pxr::TfToken("reflection_roughness_constant"),
                              ReadFloat(shader, pxr::TfToken("roughness_constant"), 0.5f));
  desc.opacity   = ReadFloat(shader, pxr::TfToken("opacity_constant"), 1.0f);
  desc.emissionColor = ReadColor(shader, pxr::TfToken("emissive_color"),
                                  hlslpp::float3{0.0f, 0.0f, 0.0f});
  desc.emissionLuminance = ReadFloat(shader, pxr::TfToken("emissive_intensity"), 0.0f);
  desc.specularIor = ReadFloat(shader, pxr::TfToken("ior_constant"), 1.5f);
  // Emission gate: OmniPBR's `enable_emission` flag — when off, the
  // intensity / colour are honoured but the material won't light a
  // surface. Reflect that by zeroing emissionLuminance.
  if (ReadFloat(shader, pxr::TfToken("enable_emission"), 1.0f) <= 0.0f)
    desc.emissionLuminance = 0.0f;
  desc.source = OpenPBRMaterialDesc::Source::MaterialX;  // groups with non-UsdPreviewSurface
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

  // V2.A.18 — multi-render-context resolution. Walks (mdl → mtlx →
  // universal) in priority order so production scenes that author
  // multiple surfaces side-by-side resolve to the highest-fidelity
  // form. The matched `kind` drives the dispatch below.
  const SurfaceMatch match = ResolveSurface(material);
  switch (match.kind)
  {
    case SurfaceMatch::Kind::Mdl:
      TranslateMdl(match.shader, desc);
      return desc;
    case SurfaceMatch::Kind::MaterialXOpenPBR:
      TranslateMaterialXOpenPBR(match.shader, desc);
      return desc;
    case SurfaceMatch::Kind::MaterialXStandardSurface:
      TranslateMaterialXStandardSurface(match.shader, desc);
      return desc;
    case SurfaceMatch::Kind::UsdPreviewSurface:
      // UsdPreviewSurface path falls through to the texture-aware
      // block below so the AcquireTexture callback still fires.
      break;
    case SurfaceMatch::Kind::None:
      desc.source = OpenPBRMaterialDesc::Source::Default;
      return desc;
  }
  const pxr::UsdShadeShader& surface = match.shader;

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
                           TextureHandle& outSlot,
                           UVTextureBinding* captureOut = nullptr) {
      UVTextureBinding binding;
      if (!ResolveUVTextureBinding(surface, pxr::TfToken(inputName), binding))
        return;
      // M8a UV-set indirection: only the implicit-default UV (empty
      // varname → "st") and an explicit `varname = "st"` resolve to
      // our renderer's only supported UV set today (`MeshDesc::uv0`).
      // Other names (`st_1`, `UVMap`, …) would silently sample the
      // wrong UV stream — drop them on the floor here and let the
      // material fall back to its scalar baseColor instead. The cost
      // is "no texture" for those materials in scenes that author
      // multi-UV setups; the gain is no visibly-wrong texturing.
      if (!binding.varname.IsEmpty() && binding.varname != stToken)
      {
        // Caller-side log lives in the renderer/ingest, not here —
        // this lib doesn't pull spdlog. Texture stays invalid and
        // PackMaterialGpu's HasBaseColorMap flag falls through to
        // the scalar.
        return;
      }
      outSlot = acquire(binding.file, role, userData);

      // V2.A.7 — UDIM multi-tile acquire. If the asset path had the
      // `<UDIM>` token, enumerate every tile that exists on disk and
      // call `acquire` for each so the bindless slots are populated.
      // The closesthit only samples the primary (1001) handle today —
      // the additional tiles are loaded ("binded but not used") so a
      // follow-up shader change can pick them up without re-walking
      // USD.
      if (binding.hasUdim)
      {
        const std::size_t udimPos = binding.udimPattern.find("<UDIM>");
        // 1001 is already acquired above as `outSlot`. Walk 1002..1099
        // (full 10x10 UV-tile grid per the spec); skip files that
        // don't exist on disk to keep the bindless population tight.
        constexpr std::size_t UDIM_TOKEN_LEN = 6u;
        int tilesLoaded = 1;  // tile 1001 already
        for (int tile = 1002; tile <= 1099; ++tile)
        {
          std::string tilePath = binding.udimPattern;
          tilePath.replace(udimPos, UDIM_TOKEN_LEN, std::to_string(tile));
          // exists() can throw on filesystem errors; the error_code
          // overload is the noexcept-safe variant. Treat any error as
          // "tile not present" so a broken NFS mount doesn't crash
          // the translator.
          std::error_code existsErr;
          if (!std::filesystem::exists(tilePath, existsErr) || existsErr)
            continue;
          const TextureHandle tileHandle = acquire(tilePath, role, userData);
          if (tileHandle != TextureHandle::Invalid)
            ++tilesLoaded;
        }
        (void)tilesLoaded;  // logging happens caller-side
      }

      if (captureOut != nullptr)
        *captureOut = binding;
    };
    // Role drives the §13 colorspace decision at decode time:
    // BaseColor + Emission = sRGB→linear EOTF; everything else
    // (NormalMap, RoughnessMetallic = the "linear data" role)
    // stays linear. UsdPreviewSurface authors metallic / roughness /
    // opacity as separate inputs; all three pull through the
    // RoughnessMetallic role since they're linear data channels.
    UVTextureBinding baseColorBinding;  // V2.A.24 + V2.A.29 — captured for the diffuse slot.
    resolveSlot("diffuseColor",  TextureKey::Role::BaseColor,         desc.baseColorMap,
                &baseColorBinding);
    resolveSlot("metallic",      TextureKey::Role::RoughnessMetallic, desc.metallicMap);
    resolveSlot("roughness",     TextureKey::Role::RoughnessMetallic, desc.roughnessMap);
    resolveSlot("normal",        TextureKey::Role::NormalMap,         desc.normalMap);
    resolveSlot("emissiveColor", TextureKey::Role::Emission,          desc.emissionMap);
    resolveSlot("opacity",       TextureKey::Role::RoughnessMetallic, desc.opacityMap);

    // V2.A.24 / V2.A.29 — stash artist-authored sampler config on the
    // baseColor slot (the most commonly-authored texture). Pyxis still
    // samples with the global `repeat` sampler in the closesthit; the
    // info is preserved for the future per-material sampler dispatch.
    desc.baseColorWrapS    = baseColorBinding.wrapS.Hash();
    desc.baseColorWrapT    = baseColorBinding.wrapT.Hash();
    desc.baseColorSourceCS = baseColorBinding.sourceColorSpace.Hash();

    // V2.A.18 — UsdTransform2d UV transform from the baseColor chain.
    // Identity defaults when the chain didn't go through a Transform2d
    // node (hasTransform2d stays false → desc fields stay at their
    // POD defaults (0, 0, 0, 1, 1) which is the identity transform).
    if (baseColorBinding.uvXform.hasTransform2d)
    {
      desc.baseColorUvTranslationX = baseColorBinding.uvXform.translationX;
      desc.baseColorUvTranslationY = baseColorBinding.uvXform.translationY;
      desc.baseColorUvRotationDeg  = baseColorBinding.uvXform.rotationDeg;
      desc.baseColorUvScaleX       = baseColorBinding.uvXform.scaleX;
      desc.baseColorUvScaleY       = baseColorBinding.uvXform.scaleY;
    }
  }

  // V2.A.30 — material network outputs beyond surface. UsdShade allows
  // displacement + volume outputs alongside surface; Pyxis v2 reads
  // only `surface` but records whether the others are authored so the
  // closesthit (when displacement / volume rendering ship) knows to
  // dispatch. Today the closesthit ignores both flags.
  if (material.GetDisplacementOutput().HasConnectedSource())
    desc.hasDisplacementOutput = 1u;
  if (material.GetVolumeOutput().HasConnectedSource())
    desc.hasVolumeOutput = 1u;

  desc.source = OpenPBRMaterialDesc::Source::UsdPreviewSurface;
  return desc;
}

}  // namespace pyxis::material_translation
