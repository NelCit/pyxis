// Pyxis material translation — UsdShadeMaterial → OpenPBRMaterialDesc.
//
// M4 stub: UsdPreviewSurface scalar/color inputs only. Texture
// connections recognised but resolved to TextureHandle::Invalid (the
// §13 TextureCache wires real decode at M5).

#include "Pyxis/MaterialTranslation/UsdShadeToOpenPBR.h"

#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/sdf/assetPath.h>
#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/sdf/propertySpec.h>
#include <pxr/usd/usdShade/connectableAPI.h>
#include <pxr/usd/usdShade/input.h>
#include <pxr/usd/usdShade/material.h>
#include <pxr/usd/usdShade/shader.h>
#include <pxr/usd/usdShade/tokens.h>
#include <pxr/base/gf/vec2f.h>
#include <pxr/base/gf/vec3f.h>
#include <pxr/base/tf/token.h>
#include <pxr/base/vt/value.h>

#include <array>
#include <cctype>
#include <charconv>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

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
// V2.A.23 — MDL shaders authored against sourceAsset (the Omniverse
// convention) carry `info:mdl:sourceAsset` (an SdfAssetPath to the
// .mdl module) + `info:mdl:sourceAsset:subIdentifier` (a TfToken
// naming the function within the module, e.g. "OmniPBR"). They do
// NOT author the universal `info:id`, so `UsdShadeShader::GetShaderId()`
// returns false. We detect this case by checking the sourceAsset
// attribute directly and routing through `TranslateMdl` (which reads
// OmniPBR-style input names).
//
// Quality gate: dual-context Omniverse Composer exports author BOTH
// a sourceAsset-MDL surface AND a universal-context UsdPreviewSurface,
// but the actual parameter values typically live on UsdPreviewSurface;
// the MDL prim is a sourceAsset pointer with no input overrides
// (values resolve inside the .mdl module at MDL-runtime, which we
// can't read). If we let MDL win in that case, TranslateMdl reads
// zero OmniPBR inputs and the material renders with OpenPBR defaults,
// nuking the scene's look. So we only accept MDL when at least one
// OmniPBR-style input is authored on the prim — otherwise the chain
// falls through to UsdPreviewSurface where the real values are.
[[nodiscard]] bool HasMdlSourceAssetWithAuthoredInputs(
    const pxr::UsdShadeShader& shader) noexcept
{
  static const pxr::TfToken mdlSourceAssetAttr("info:mdl:sourceAsset");  // NOLINT
  const pxr::UsdAttribute attr = shader.GetPrim().GetAttribute(mdlSourceAssetAttr);
  if (!attr || !attr.HasAuthoredValue())
    return false;
  pxr::VtValue value;
  if (!attr.Get(&value) || !value.IsHolding<pxr::SdfAssetPath>())
    return false;

  // Probe for any OmniPBR / OmniGlass-style input that TranslateMdl
  // actually reads. If none are authored on the prim, the .mdl module
  // owns the values and we can't translate — defer to the next render
  // context. (Q1 added the OmniGlass entries alongside the glass →
  // transmission wiring.)
  static const std::array<pxr::TfToken, 12> MDL_INPUT_PROBE{{
      pxr::TfToken{"inputs:diffuse_color_constant"},
      pxr::TfToken{"inputs:metallic_constant"},
      pxr::TfToken{"inputs:reflection_roughness_constant"},
      pxr::TfToken{"inputs:roughness_constant"},
      pxr::TfToken{"inputs:emissive_color"},
      pxr::TfToken{"inputs:emissive_intensity"},
      pxr::TfToken{"inputs:opacity_constant"},
      pxr::TfToken{"inputs:ior_constant"},
      pxr::TfToken{"inputs:glass_color"},          // Q1 — OmniGlass
      pxr::TfToken{"inputs:glass_ior"},            // Q1 — OmniGlass
      pxr::TfToken{"inputs:frosting_roughness"},   // Q1 — OmniGlass
      pxr::TfToken{"inputs:thin_walled"},          // Q1 — OmniGlass
  }};
  for (const pxr::TfToken& inputName : MDL_INPUT_PROBE)
  {
    const pxr::UsdAttribute inputAttr =
        shader.GetPrim().GetAttribute(inputName);
    if (inputAttr && inputAttr.HasAuthoredValue())
      return true;
  }
  return false;
}

// 2026-07-06 RTX-alignment round 2 — broader probe than
// `HasMdlSourceAssetWithAuthoredInputs`'s fixed 12-name allowlist: true
// if the candidate shader authors ANY `inputs:*` override at all. Used
// as the last-resort MDL-classification tier (after the named-probe
// and the OmniPBR-preset-file parse both miss) for generic OmniPBR/
// OmniGlass instances that only override inputs outside that
// allowlist — e.g. the World Lobby's FountainGrass ground-cover, which
// authors just `albedo_brightness` + `diffuse_texture` on the shared
// `OmniPBR.mdl` module.
bool HasAnyAuthoredMdlInput(const pxr::UsdShadeShader& shader) noexcept
{
  for (const pxr::UsdShadeInput& input : shader.GetInputs())
  {
    if (input.GetAttr().HasAuthoredValue())
      return true;
  }
  return false;
}

// 2026-07-06 RTX-alignment round 2 (materials chapter) — MDL "Base"
// library preset materials. `Steel_Carbon` / `Concrete_Formed` /
// `Stucco` (and others under `Materials/Base/**`) are each a single
// per-material .mdl file that calls the shared OmniPBR base function
// with every parameter BAKED as a literal argument:
//
//   export material Steel_Carbon(*)
//    = ::OmniPBR::OmniPBR(diffuse_color_constant: color(0.2, 0.2, 0.2),
//                         metallic_constant: 0.f, ...);
//
// and author NO USD `inputs:*` overrides at all (the artist never
// touched the material in Composer), so `HasMdlSourceAssetWithAuthoredInputs`
// (which only probes a handful of commonly-overridden input names) never
// fires and the translator fell through to the UsdPreviewSurface network
// or, when that's also absent, the flat 0.18-grey `Default` fallback —
// the round-1 forensics "Steel_Carbon/Stucco fully grey,
// Concrete_Formed textureless" gap. This helper re-reads the same .mdl
// file the Shader prim points at and recovers those literals so
// TranslateMdl can use them as its per-field fallback instead of its
// own generic hardcoded defaults, while any USD-authored `inputs:*`
// value (the Oak/OakDark authoring pattern) still wins as it did
// before — see the `PresetXxx()` call sites in TranslateMdl below.
//
// Deliberately narrow: only recognizes a direct call to an OmniPBR-
// family base function (`OmniPBR`, `OmniPBRBase`, `OmniGlass`, ...).
// `Plaster_Wall_Cracked` (vMaterials' bespoke multi-layer `df::`
// BSDF graph, `= Plaster_Wall(...)`) fails this check by design —
// interpreting an arbitrary MDL DF graph is out of scope, and that
// material already has a legitimate baked-texture UsdPreviewSurface
// fallback network authored alongside it, which stays untouched.
struct MdlPreset {
  bool                                        valid = false;
  std::string                                 dir;   // .mdl file's directory (relative texture paths anchor here).
  std::unordered_map<std::string, std::string> args;  // key -> raw literal text (trimmed).
};

// Parses a float literal with an optional trailing MDL `f`/`F` suffix
// (e.g. "0.200000003f", "90", "0.5"). Returns `fallback` on any parse
// failure — never throws (this TU has exceptions enabled for USD, but
// every helper in this file stays defensive/noexcept by convention).
float ParseMdlFloatLiteral(std::string_view text, float fallback) noexcept
{
  std::size_t start = 0;
  while (start < text.size() && std::isspace(static_cast<unsigned char>(text[start])))
    ++start;
  std::size_t end = text.size();
  while (end > start && std::isspace(static_cast<unsigned char>(text[end - 1])))
    --end;
  if (start < end && (text[end - 1] == 'f' || text[end - 1] == 'F'))
    --end;
  if (start >= end)
    return fallback;
  float value = fallback;
  const auto [ptr, ec] = std::from_chars(text.data() + start, text.data() + end, value);
  (void)ptr;
  return (ec == std::errc()) ? value : fallback;
}

// Finds the ')' matching the '(' at `text[openParenPos]`, honoring
// nested parens and double-quoted string literals (asset paths) so a
// paren inside a comment-free quoted path never desyncs the count.
// Returns `npos` on an unbalanced/malformed call.
std::size_t FindMatchingParen(const std::string& text, std::size_t openParenPos) noexcept
{
  if (openParenPos >= text.size() || text[openParenPos] != '(')
    return std::string::npos;
  int  depth = 0;
  bool inString = false;
  for (std::size_t i = openParenPos; i < text.size(); ++i)
  {
    const char c = text[i];
    if (inString)
    {
      if (c == '"')
        inString = false;
      continue;
    }
    if (c == '"')
      inString = true;
    else if (c == '(')
      ++depth;
    else if (c == ')')
    {
      --depth;
      if (depth == 0)
        return i;
    }
  }
  return std::string::npos;
}

// Splits the unwrapped argument text of an MDL call into top-level
// "key: value" pairs (depth/quote-aware, so a comma inside a nested
// `color(...)`, `texture_2d(...)`, or a `/* tag N, version M */`
// comment never splits early).
void SplitMdlCallArgs(const std::string&                            argsText,
                      std::unordered_map<std::string, std::string>& outArgs) noexcept
{
  const auto trim = [](std::string s) {
    std::size_t b = 0;
    while (b < s.size() && std::isspace(static_cast<unsigned char>(s[b])))
      ++b;
    std::size_t e = s.size();
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1])))
      --e;
    return s.substr(b, e - b);
  };
  int         depth = 0;
  bool        inString = false;
  std::size_t segStart = 0;
  const auto flush = [&](std::size_t segEnd) {
    if (segEnd <= segStart)
      return;
    const std::string segment = argsText.substr(segStart, segEnd - segStart);
    const std::size_t colon = segment.find(':');
    if (colon == std::string::npos)
      return;
    std::string key = trim(segment.substr(0, colon));
    std::string value = trim(segment.substr(colon + 1));
    if (!key.empty())
      outArgs.emplace(std::move(key), std::move(value));
  };
  for (std::size_t i = 0; i < argsText.size(); ++i)
  {
    const char c = argsText[i];
    if (inString)
    {
      if (c == '"')
        inString = false;
      continue;
    }
    if (c == '"')
      inString = true;
    else if (c == '(')
      ++depth;
    else if (c == ')')
      --depth;
    else if (c == ',' && depth == 0)
    {
      flush(i);
      segStart = i + 1;
    }
  }
  flush(argsText.size());
}

// OmniPBR-family base-function names this parser understands. Strips
// `::module::` qualification so both `OmniPBR(` (via `using ::OmniPBR
// import OmniPBR;`) and `::OmniPBR::OmniPBR(` name the same function.
bool IsRecognizedOmniPbrFamilyCall(std::string_view baseName) noexcept
{
  const std::size_t   lastSep = baseName.rfind("::");
  const std::string_view leaf = (lastSep == std::string_view::npos)
                                     ? baseName
                                     : baseName.substr(lastSep + 2);
  return leaf == "OmniPBR" || leaf == "OmniPBRBase" || leaf == "OmniGlass"
         || leaf == "OmniPBR_ClearCoat" || leaf == "OmniSurface";
}

// Loads + parses the referenced .mdl file's `export material
// <subIdentifier>(...) = <BaseCall>(...)` declaration, returning the
// baked argument literals when `<BaseCall>` is OmniPBR-family.
// `preset.valid` stays false (empty args) for: unresolvable/missing
// files (e.g. a bare `@OmniPBR.mdl@` Kit-library reference with no
// local path to resolve — TranslateMdl's own hardcoded defaults are
// already OmniPBR-shaped, so this is a safe no-op fallback), a
// declaration that isn't found, or a base call that isn't recognized
// (e.g. Plaster_Wall_Cracked's bespoke `df::` network).
MdlPreset LoadMdlPresetIfOmniPbr(const pxr::UsdShadeShader& shader) noexcept
{
  MdlPreset preset;
  static const pxr::TfToken sourceAssetAttrName("info:mdl:sourceAsset");             // NOLINT
  static const pxr::TfToken subIdentifierAttrName("info:mdl:sourceAsset:subIdentifier"); // NOLINT
  const pxr::UsdAttribute assetAttr = shader.GetPrim().GetAttribute(sourceAssetAttrName);
  const pxr::UsdAttribute subIdAttr = shader.GetPrim().GetAttribute(subIdentifierAttrName);
  if (!assetAttr || !assetAttr.HasAuthoredValue() || !subIdAttr || !subIdAttr.HasAuthoredValue())
    return preset;
  pxr::VtValue assetValue;
  pxr::VtValue subIdValue;
  if (!assetAttr.Get(&assetValue) || !assetValue.IsHolding<pxr::SdfAssetPath>())
    return preset;
  if (!subIdAttr.Get(&subIdValue) || !subIdValue.IsHolding<pxr::TfToken>())
    return preset;
  const pxr::SdfAssetPath assetPath = assetValue.UncheckedGet<pxr::SdfAssetPath>();
  const std::string subIdentifier = subIdValue.UncheckedGet<pxr::TfToken>().GetString();

  std::string mdlPath = assetPath.GetResolvedPath();
  if (mdlPath.empty())
    mdlPath = assetPath.GetAssetPath();
  if (mdlPath.empty())
    return preset;
  std::error_code existsErr;
  if (!std::filesystem::exists(mdlPath, existsErr) || existsErr)
    return preset;  // e.g. a bare Kit-library "OmniPBR.mdl" we can't resolve locally.

  std::ifstream file(mdlPath, std::ios::binary);
  if (!file.is_open())
    return preset;
  const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

  // Find "material <subIdentifier>" at a token boundary — not a
  // prefix match against a longer sibling declaration in the same
  // file (e.g. "Plaster_Wall" must not match inside
  // "Plaster_Wall_Cracked" or vice versa).
  const std::string needle = "material " + subIdentifier;
  std::size_t       searchPos = 0;
  std::size_t       declEnd = std::string::npos;  // one past the name, at/before '('.
  while (searchPos < text.size())
  {
    const std::size_t found = text.find(needle, searchPos);
    if (found == std::string::npos)
      break;
    const std::size_t afterName = found + needle.size();
    if (afterName < text.size()
        && (std::isalnum(static_cast<unsigned char>(text[afterName])) || text[afterName] == '_'))
    {
      searchPos = afterName;  // e.g. matched "Plaster_Wall" prefix of "Plaster_Wall_Cracked".
      continue;
    }
    declEnd = afterName;
    break;
  }
  if (declEnd == std::string::npos)
    return preset;

  const std::size_t nameParenStart = text.find('(', declEnd);
  if (nameParenStart == std::string::npos)
    return preset;
  const std::size_t nameParenEnd = FindMatchingParen(text, nameParenStart);
  if (nameParenEnd == std::string::npos)
    return preset;
  std::size_t cursor = nameParenEnd + 1;
  const auto skipSpace = [&]() {
    while (cursor < text.size() && std::isspace(static_cast<unsigned char>(text[cursor])))
      ++cursor;
  };
  skipSpace();
  if (cursor + 1 < text.size() && text[cursor] == '[' && text[cursor + 1] == '[')
  {
    const std::size_t annoEnd = text.find("]]", cursor + 2);
    if (annoEnd == std::string::npos)
      return preset;
    cursor = annoEnd + 2;
  }
  skipSpace();
  if (cursor >= text.size() || text[cursor] != '=')
    return preset;
  ++cursor;
  skipSpace();
  const std::size_t baseNameStart = cursor;
  while (cursor < text.size()
         && (std::isalnum(static_cast<unsigned char>(text[cursor])) || text[cursor] == '_'
             || text[cursor] == ':'))
    ++cursor;
  const std::string baseName = text.substr(baseNameStart, cursor - baseNameStart);
  if (!IsRecognizedOmniPbrFamilyCall(baseName))
    return preset;
  skipSpace();
  if (cursor >= text.size() || text[cursor] != '(')
    return preset;
  const std::size_t callParenEnd = FindMatchingParen(text, cursor);
  if (callParenEnd == std::string::npos)
    return preset;
  const std::string argsText = text.substr(cursor + 1, callParenEnd - cursor - 1);
  SplitMdlCallArgs(argsText, preset.args);
  preset.valid = !preset.args.empty();
  preset.dir = std::filesystem::path(mdlPath).parent_path().string();
  return preset;
}

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
  if (candidate.GetShaderId(&shaderId))
  {
    match.kind = ClassifyShaderId(shaderId);
  }
  // V2.A.23 — fallback for MDL shaders authored by sourceAsset rather
  // than `info:id`. Only kicks in when we asked for the `mdl` context
  // (so we don't misclassify e.g. a universal-context PxrSurface that
  // happens to have a co-authored sourceAsset) AND the shader has at
  // least one OmniPBR input authored — see
  // `HasMdlSourceAssetWithAuthoredInputs` for the rationale.
  static const pxr::TfToken mdlContext("mdl");  // NOLINT
  if (match.kind == SurfaceMatch::Kind::None && contextToken == mdlContext)
  {
    if (HasMdlSourceAssetWithAuthoredInputs(candidate))
    {
      match.kind = SurfaceMatch::Kind::Mdl;
    }
    else if (LoadMdlPresetIfOmniPbr(candidate).valid)
    {
      // 2026-07-06 RTX-alignment round 2 — an OmniPBR-family "Base"
      // library preset (Steel_Carbon / Concrete_Formed / Stucco / ...)
      // that bakes every parameter as a literal MDL argument and
      // authors NO USD `inputs:*` overrides at all, so the probe above
      // never fires. TranslateMdl re-parses the same .mdl file (see
      // LoadMdlPresetIfOmniPbr) to recover those literals.
      match.kind = SurfaceMatch::Kind::Mdl;
    }
    else if (HasAnyAuthoredMdlInput(candidate))
    {
      // A generic OmniPBR/OmniGlass instance that only overrides
      // inputs outside the narrow probe above (e.g. the World Lobby's
      // FountainGrass ground-cover, which authors just
      // `albedo_brightness` + `diffuse_texture` directly on the
      // shared `OmniPBR.mdl` module — subIdentifier is literally
      // "OmniPBR", not a custom material name). TranslateMdl's own
      // OmniPBR-shaped defaults are the right fallback for whatever it
      // doesn't author.
      //
      // Gated on the subIdentifier itself naming a recognized OmniPBR-
      // family function (not just "any authored input") — a custom-
      // named MDL material (e.g. vMaterials' "Iron_Brushed_Medium_
      // Brushing", a bespoke `df::` network under a name that doesn't
      // match `IsRecognizedOmniPbrFamilyCall`) can authored-override
      // one of ITS OWN parameters without being OmniPBR-shaped at all;
      // reading it as OmniPBR (diffuse_color_constant / metallic_
      // constant / ...) produces flat-grey garbage, which regressed
      // this exact material from its working UsdPreviewSurface-
      // fallback texture set to untextured grey when this tier
      // ignored the subIdentifier. `LoadMdlPresetIfOmniPbr` above
      // already covers the "custom name but IS an OmniPBR preset
      // call" case (Steel_Carbon et al.), so by the time we get here
      // the subIdentifier itself must be the tell.
      static const pxr::TfToken subIdentifierAttr(  // NOLINT
          "info:mdl:sourceAsset:subIdentifier");
      pxr::VtValue subIdValue;
      if (const pxr::UsdAttribute subIdAttr =
              candidate.GetPrim().GetAttribute(subIdentifierAttr);
          subIdAttr && subIdAttr.Get(&subIdValue) && subIdValue.IsHolding<pxr::TfToken>()
          && IsRecognizedOmniPbrFamilyCall(subIdValue.UncheckedGet<pxr::TfToken>().GetString()))
      {
        match.kind = SurfaceMatch::Kind::Mdl;
      }
    }
  }
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
// don't follow at M4. V2.A.13 — sampled at `timeCode` so animated
// scalar inputs track the requested frame; default-time is the
// no-animation baseline.
float ReadFloat(const pxr::UsdShadeShader& shader, const pxr::TfToken& name,
                float fallback,
                pxr::UsdTimeCode timeCode = pxr::UsdTimeCode::Default()) noexcept {
  const pxr::UsdShadeInput input = shader.GetInput(name);
  if (!input)
    return fallback;
  pxr::VtValue value;
  if (!input.Get(&value, timeCode))
    return fallback;
  if (!value.IsHolding<float>())
    return fallback;
  return value.UncheckedGet<float>();
}

// Read a color3f input as RGB. Returns `fallback` if absent / wrong
// type. Texture connections (the common case for baseColor) read as
// "no value authored" which falls back to the scalar default — the
// closesthit shader uses the flag-bit + texture handle to know to
// sample instead. V2.A.13 — sampled at `timeCode`.
hlslpp::float3 ReadColor(const pxr::UsdShadeShader& shader, const pxr::TfToken& name,
                         const hlslpp::float3& fallback,
                         pxr::UsdTimeCode timeCode = pxr::UsdTimeCode::Default()) noexcept {
  const pxr::UsdShadeInput input = shader.GetInput(name);
  if (!input)
    return fallback;
  pxr::VtValue value;
  if (!input.Get(&value, timeCode))
    return fallback;
  if (!value.IsHolding<pxr::GfVec3f>())
    return fallback;
  const pxr::GfVec3f rgb = value.UncheckedGet<pxr::GfVec3f>();
  return hlslpp::float3{rgb[0], rgb[1], rgb[2]};
}

// V2.A.23 — read an authored bool/int/float input on the shader
// prim. Returns the typed value, or `fallback` when the input is
// missing OR holds a different scalar type. USD authoring exporters
// vary: Omniverse Composer writes `enable_emission` as a bool, some
// pipelines write it as int, and an MDL author might write it as
// float. Handle all three. (Moved above the MaterialX translators at
// Q1 — `geometry_thin_walled` / `thin_walled` are boolean inputs.)
[[nodiscard]] bool ReadBoolish(const pxr::UsdShadeShader& shader,
                                const pxr::TfToken&        name,
                                bool                       fallback,
                                pxr::UsdTimeCode           timeCode) noexcept
{
  const pxr::UsdShadeInput input = shader.GetInput(name);
  if (!input)
    return fallback;
  pxr::VtValue value;
  if (!input.Get(&value, timeCode))
    return fallback;
  if (value.IsHolding<bool>())
    return value.UncheckedGet<bool>();
  if (value.IsHolding<int>())
    return value.UncheckedGet<int>() != 0;
  if (value.IsHolding<float>())
    return value.UncheckedGet<float>() > 0.0f;
  return fallback;
}

// Q1 OpenPBR-complete — stamp an hlslpp color into the desc's scalar
// R/G/B triple (the Q1 desc extension stores colors as plain floats
// to avoid SIMD padding holes in the public POD).
inline void StoreRgb(const hlslpp::float3& color, float& outR, float& outG,
                     float& outB) noexcept
{
  outR = static_cast<float>(color.x);
  outG = static_cast<float>(color.y);
  outB = static_cast<float>(color.z);
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

// === MaterialX node-graph resolution (Q3 follow-up) ==========================
//
// Playground + production MaterialX materials author EVERY surface input
// through the node graph, never as direct values: `base_color` connects to an
// `ND_image_color3` (a texture), `transmission_weight` connects to a promoted
// nodegraph/material *interface input* (where the real constant lives),
// `geometry_normal` connects to `ND_normalmap` wrapping an `ND_image_vector3`.
// The pre-Q3 translators only read `.Get()` direct values, so they saw nothing
// and every MaterialX material fell back to OpenPBR defaults (grey, opaque —
// which is why playground glass rendered opaque: its transmission_weight is a
// connected interface input we never read). These helpers walk the graph the
// way the UsdPreviewSurface path walks UsdUVTexture chains: follow interface-
// input connections to the authored constant, and a direct `ND_image*`
// (optionally through one `ND_normalmap`) to the texture asset.

// Acquire a (possibly UDIM) texture. `file` is the 1001-substituted primary
// path; when `hasUdim`, the 1002..1099 tiles present on disk are acquired too
// (the closesthit samples only 1001 today, matching the UsdPreviewSurface
// path). Returns the primary handle.
[[nodiscard]] TextureHandle AcquireTextureMaybeUdim(
    AcquireTextureFn acquire, void* userData, const std::string& file,
    const std::string& udimPattern, bool hasUdim, TextureKey::Role role) noexcept
{
  if (acquire == nullptr || file.empty())
    return TextureHandle::Invalid;
  const TextureHandle primary = acquire(file, role, userData);
  if (hasUdim)
  {
    if (const std::size_t udimPos = udimPattern.find("<UDIM>");
        udimPos != std::string::npos)
    {
      constexpr std::size_t UDIM_TOKEN_LEN = 6u;  // length of literal "<UDIM>"
      for (int tile = 1002; tile <= 1099; ++tile)
      {
        std::string tilePath = udimPattern;
        tilePath.replace(udimPos, UDIM_TOKEN_LEN, std::to_string(tile));
        std::error_code existsErr;
        if (!std::filesystem::exists(tilePath, existsErr) || existsErr)
          continue;
        (void)acquire(tilePath, role, userData);
      }
    }
  }
  return primary;
}

// Directories to anchor a relative MaterialX texture path against, in priority
// order. USD's pre-computed resolved path anchors a `file` asset to the layer
// it was AUTHORED in — but MaterialX documents reference textures relative to
// the SCENE ROOT via a search path, not the .mtlx's own directory (the OpenPBR
// Playground authors `file = textures/foo.<UDIM>.png` in materials/<mat>.mtlx
// while the textures live in the sibling ShdrPlygrnd/textures/). So USD resolves
// those to empty and the relative path would leak to the loader. We try both
// the authoring-layer dir (textures co-located with the material doc) and the
// root-layer dir (the scene .usda) and let the caller pick whichever makes the
// file exist on disk.
void CollectMtlxAnchorDirs(const pxr::UsdShadeInput&  fileInput,
                           const pxr::UsdShadeShader& imageNode,
                           std::vector<std::string>&  outDirs) noexcept
{
  namespace fs = std::filesystem;
  for (const pxr::SdfPropertySpecHandle& spec :
       fileInput.GetAttr().GetPropertyStack(pxr::UsdTimeCode::Default()))
  {
    if (spec && spec->GetLayer())
    {
      const std::string real = spec->GetLayer()->GetRealPath();
      if (!real.empty())
        outDirs.push_back(fs::path(real).parent_path().string());
    }
  }
  if (const pxr::UsdStageWeakPtr stage = imageNode.GetPrim().GetStage())
  {
    if (const pxr::SdfLayerHandle root = stage->GetRootLayer())
    {
      const std::string real = root->GetRealPath();
      if (!real.empty())
        outDirs.push_back(fs::path(real).parent_path().string());
    }
  }
}

// Read an `ND_image*` node's `inputs:file` asset path (UDIM-aware) and resolve
// it to an absolute on-disk path. Prefers USD's pre-resolved path (correct for
// existing non-UDIM files); otherwise anchors against CollectMtlxAnchorDirs.
//
// UDIM: the primary tile is NOT always 1001. A model's UV shells can live
// entirely in a non-1001 tile (the playground's lampShade/toolsMetal/toolsPlastic
// ship only `.1002`/`.1003`), so a hard `<UDIM>`→1001 substitution misses them.
// We scan 1001..1100 and bind the FIRST tile that exists as the primary handle.
// The closesthit samples one handle with the global repeat sampler, so a
// single-tile object whose u lands in [N, N+1) fracs back into [0, 1) and reads
// the bound tile correctly regardless of which tile number it is. (True
// multi-tile UDIM — one object spanning several tiles — still needs per-tile
// shader selection; not plumbed. The playground objects are single-tile.)
// `outUdimPattern` is the matching ABSOLUTE `<UDIM>` pattern so the caller's
// tile enumeration (AcquireTextureMaybeUdim) can load the neighbours.
[[nodiscard]] bool ReadMtlxImageFile(const pxr::UsdShadeShader& imageNode,
                                     std::string& outFile,
                                     std::string& outUdimPattern,
                                     bool&        outHasUdim) noexcept
{
  namespace fs = std::filesystem;
  static const pxr::TfToken fileToken("file");  // NOLINT(readability-identifier-naming)
  outFile.clear();
  outUdimPattern.clear();
  outHasUdim = false;
  const pxr::UsdShadeInput fileInput = imageNode.GetInput(fileToken);
  if (!fileInput)
    return false;
  pxr::VtValue value;
  if (!fileInput.Get(&value) || !value.IsHolding<pxr::SdfAssetPath>())
    return false;
  const pxr::SdfAssetPath asset = value.UncheckedGet<pxr::SdfAssetPath>();
  const std::string rawPath = asset.GetAssetPath();
  if (rawPath.empty())
    return false;

  constexpr std::size_t UDIM_TOKEN_LEN = 6u;  // length of literal "<UDIM>"
  const auto udimPos = rawPath.find("<UDIM>");
  outHasUdim = (udimPos != std::string::npos);

  std::vector<std::string> anchorDirs;
  CollectMtlxAnchorDirs(fileInput, imageNode, anchorDirs);
  const auto existsAt = [](const std::string& path) noexcept {
    std::error_code existsErr;
    return !path.empty() && fs::exists(path, existsErr) && !existsErr;
  };

  if (!outHasUdim)
  {
    // Non-UDIM: USD's resolved path → absolute spec → anchored dirs.
    if (const std::string resolved = asset.GetResolvedPath(); existsAt(resolved))
    {
      outFile = resolved;
      return true;
    }
    if (fs::path(rawPath).is_absolute() && existsAt(rawPath))
    {
      outFile = rawPath;
      return true;
    }
    for (const std::string& dir : anchorDirs)
    {
      const std::string candidate = (fs::path(dir) / rawPath).string();
      if (existsAt(candidate))
      {
        outFile = candidate;
        return true;
      }
    }
    outFile = rawPath;  // last resort — loader surfaces the miss loudly.
    return true;
  }

  // UDIM: bind the first tile that exists (1001..1100), not a hardcoded 1001.
  for (int tile = 1001; tile <= 1100; ++tile)
  {
    std::string tileRel = rawPath;
    tileRel.replace(udimPos, UDIM_TOKEN_LEN, std::to_string(tile));
    if (fs::path(tileRel).is_absolute() && existsAt(tileRel))
    {
      outFile = tileRel;
      outUdimPattern = rawPath;
      return true;
    }
    for (const std::string& dir : anchorDirs)
    {
      const std::string candidate = (fs::path(dir) / tileRel).string();
      if (existsAt(candidate))
      {
        outFile = candidate;
        outUdimPattern = (fs::path(dir) / rawPath).string();
        return true;
      }
    }
  }

  // No tile found anywhere — last resort (1001, relative) so the loader warns.
  std::string tile1001 = rawPath;
  tile1001.replace(udimPos, UDIM_TOKEN_LEN, "1001");
  outFile = tile1001;
  outUdimPattern = rawPath;
  return true;
}

// The outcome of resolving a MaterialX surface input through the graph.
struct MtlxResolved {
  enum class Kind : uint8_t { None, Constant, Texture };
  Kind         kind = Kind::None;
  pxr::VtValue value;             // Kind::Constant — the authored value.
  std::string  file;              // Kind::Texture — 1001-substituted path.
  std::string  udimPattern;
  bool         hasUdim = false;
};

// Walk a MaterialX surface-shader input to the value that ultimately drives it.
// Follows promoted interface-input connections (`.connect` targeting another
// Input) up to the authored constant; a direct `ND_image*` output yields its
// texture asset; one `ND_normalmap` hop unwraps to its tangent-space image.
// Utility nodes we don't evaluate (invert / colorcorrect / combine / extract /
// …) yield Kind::None so the caller keeps its scalar default. The bounded hop
// budget guards against an authoring cycle.
[[nodiscard]] MtlxResolved ResolveMtlxInput(pxr::UsdShadeInput input,
                                            pxr::UsdTimeCode   timeCode) noexcept
{
  static const pxr::TfToken inToken("in");  // NOLINT(readability-identifier-naming)
  MtlxResolved out;
  for (int hop = 0; hop < 16; ++hop)
  {
    if (!input)
      return out;
    if (!input.HasConnectedSource())
    {
      // Leaf: read the authored constant (if any).
      pxr::VtValue value;
      if (input.GetAttr().HasAuthoredValue() && input.Get(&value, timeCode))
      {
        out.kind  = MtlxResolved::Kind::Constant;
        out.value = value;
      }
      return out;
    }
    pxr::UsdShadeConnectableAPI source;
    pxr::TfToken                sourceName;
    pxr::UsdShadeAttributeType  sourceType;
    if (!input.GetConnectedSource(&source, &sourceName, &sourceType))
      return out;
    if (sourceType == pxr::UsdShadeAttributeType::Input)
    {
      // Promoted interface input — hop upstream toward the authored value.
      input = source.GetInput(sourceName);
      continue;
    }
    // An output: a node produces this value.
    const pxr::UsdShadeShader node{source.GetPrim()};
    pxr::TfToken              nodeId;
    if (!node.GetShaderId(&nodeId))
      return out;
    const std::string& idStr = nodeId.GetString();
    if (idStr.starts_with("ND_image"))
    {
      if (ReadMtlxImageFile(node, out.file, out.udimPattern, out.hasUdim))
        out.kind = MtlxResolved::Kind::Texture;
      return out;
    }
    if (idStr.starts_with("ND_normalmap"))
    {
      input = node.GetInput(inToken);  // unwrap to the tangent-space image
      continue;
    }
    return out;  // unevaluated utility node → caller keeps its default
  }
  return out;
}

// V2.A.18 / Q3 — MaterialX OpenPBR (`open_pbr_surface`) translator. Pyxis's
// canonical target; the OpenPBRMaterialDesc IS the MTLX OpenPBR model
// post-translation, so field-for-field mapping. Every input is resolved
// through the node graph (ResolveMtlxInput): a promoted interface input yields
// its authored constant, a direct `ND_image*` connection yields a texture into
// the matching map slot (the six the closesthit samples: baseColor / metallic /
// roughness / normal / emission / opacity). V2.A.13 — constants sampled at
// `timeCode`.
void TranslateMaterialXOpenPBR(const pxr::UsdShadeShader& shader,
                                OpenPBRMaterialDesc&       desc,
                                AcquireTextureFn           acquire,
                                void*                      userData,
                                pxr::UsdTimeCode           timeCode) noexcept
{
  auto resolve = [&](const char* name) {
    return ResolveMtlxInput(shader.GetInput(pxr::TfToken(name)), timeCode);
  };
  auto scalar = [&](const char* name, float fallback) -> float {
    const MtlxResolved r = resolve(name);
    if (r.kind == MtlxResolved::Kind::Constant && r.value.IsHolding<float>())
      return r.value.UncheckedGet<float>();
    return fallback;
  };
  auto color = [&](const char* name, hlslpp::float3 fallback) -> hlslpp::float3 {
    const MtlxResolved r = resolve(name);
    if (r.kind == MtlxResolved::Kind::Constant && r.value.IsHolding<pxr::GfVec3f>())
    {
      const pxr::GfVec3f c = r.value.UncheckedGet<pxr::GfVec3f>();
      return hlslpp::float3{c[0], c[1], c[2]};
    }
    return fallback;
  };
  auto boolean = [&](const char* name, bool fallback) -> bool {
    const MtlxResolved r = resolve(name);
    if (r.kind == MtlxResolved::Kind::Constant)
    {
      if (r.value.IsHolding<bool>())  return r.value.UncheckedGet<bool>();
      if (r.value.IsHolding<int>())   return r.value.UncheckedGet<int>() != 0;
      if (r.value.IsHolding<float>()) return r.value.UncheckedGet<float>() > 0.0f;
    }
    return fallback;
  };
  // Scalar-or-texture: a direct image sets `outMap` (+ scalar 1, so texture × 1);
  // a constant returns its value; otherwise the fallback.
  auto scalarTex = [&](const char* name, float fallback, TextureKey::Role role,
                       TextureHandle& outMap) -> float {
    const MtlxResolved r = resolve(name);
    if (r.kind == MtlxResolved::Kind::Texture)
    {
      outMap = AcquireTextureMaybeUdim(acquire, userData, r.file, r.udimPattern,
                                       r.hasUdim, role);
      return (outMap != TextureHandle::Invalid) ? 1.0f : fallback;
    }
    if (r.kind == MtlxResolved::Kind::Constant && r.value.IsHolding<float>())
      return r.value.UncheckedGet<float>();
    return fallback;
  };
  auto colorTex = [&](const char* name, hlslpp::float3 fallback,
                      TextureKey::Role role, TextureHandle& outMap) -> hlslpp::float3 {
    const MtlxResolved r = resolve(name);
    if (r.kind == MtlxResolved::Kind::Texture)
    {
      outMap = AcquireTextureMaybeUdim(acquire, userData, r.file, r.udimPattern,
                                       r.hasUdim, role);
      if (outMap != TextureHandle::Invalid)
        return hlslpp::float3{1.0f, 1.0f, 1.0f};  // texture × white
      return fallback;
    }
    if (r.kind == MtlxResolved::Kind::Constant && r.value.IsHolding<pxr::GfVec3f>())
    {
      const pxr::GfVec3f c = r.value.UncheckedGet<pxr::GfVec3f>();
      return hlslpp::float3{c[0], c[1], c[2]};
    }
    return fallback;
  };

  desc.baseColor      = colorTex("base_color", hlslpp::float3{0.18f, 0.18f, 0.18f},
                                 TextureKey::Role::BaseColor, desc.baseColorMap);
  desc.baseWeight     = scalar("base_weight", 1.0f);
  desc.metalness      = scalarTex("base_metalness", 0.0f,
                                  TextureKey::Role::RoughnessMetallic, desc.metallicMap);
  desc.roughness      = scalarTex("specular_roughness", 0.5f,
                                  TextureKey::Role::RoughnessMetallic, desc.roughnessMap);
  desc.specularWeight = scalar("specular_weight", 1.0f);
  desc.specularIor    = scalar("specular_ior", 1.5f);
  desc.opacity        = scalarTex("geometry_opacity", 1.0f,
                                  TextureKey::Role::Opacity, desc.opacityMap);
  desc.coatWeight     = scalar("coat_weight", 0.0f);
  desc.coatRoughness  = scalar("coat_roughness", 0.0f);
  desc.transmissionWeight = scalar("transmission_weight", 0.0f);
  desc.emissionColor  = colorTex("emission_color", hlslpp::float3{0.0f, 0.0f, 0.0f},
                                 TextureKey::Role::Emission, desc.emissionMap);
  desc.emissionLuminance = scalar("emission_luminance", 0.0f);
  // Q1 OpenPBR-complete — the remaining open_pbr_surface inputs map
  // field-for-field onto the Q1 desc extension. Fallbacks are the
  // OpenPBR spec defaults (= the desc defaults).
  StoreRgb(color("specular_color", hlslpp::float3{1.0f, 1.0f, 1.0f}),
           desc.specularColorR, desc.specularColorG, desc.specularColorB);
  desc.baseDiffuseRoughness = scalar("base_diffuse_roughness", 0.0f);
  desc.specularRoughnessAnisotropy = scalar("specular_roughness_anisotropy", 0.0f);
  StoreRgb(color("coat_color", hlslpp::float3{1.0f, 1.0f, 1.0f}),
           desc.coatColorR, desc.coatColorG, desc.coatColorB);
  desc.coatIor       = scalar("coat_ior", 1.6f);
  desc.coatDarkening = scalar("coat_darkening", 1.0f);
  desc.fuzzWeight    = scalar("fuzz_weight", 0.0f);
  StoreRgb(color("fuzz_color", hlslpp::float3{1.0f, 1.0f, 1.0f}),
           desc.fuzzColorR, desc.fuzzColorG, desc.fuzzColorB);
  desc.fuzzRoughness = scalar("fuzz_roughness", 0.5f);
  StoreRgb(color("transmission_color", hlslpp::float3{1.0f, 1.0f, 1.0f}),
           desc.transmissionColorR, desc.transmissionColorG, desc.transmissionColorB);
  desc.subsurfaceWeight = scalar("subsurface_weight", 0.0f);
  StoreRgb(color("subsurface_color", hlslpp::float3{0.8f, 0.8f, 0.8f}),
           desc.subsurfaceColorR, desc.subsurfaceColorG, desc.subsurfaceColorB);
  desc.thinWalled = boolean("geometry_thin_walled", false) ? 1u : 0u;
  // Geometry normal — `ND_normalmap` wrapping a tangent-space image.
  if (const MtlxResolved r = resolve("geometry_normal");
      r.kind == MtlxResolved::Kind::Texture)
    desc.normalMap = AcquireTextureMaybeUdim(acquire, userData, r.file,
                                             r.udimPattern, r.hasUdim,
                                             TextureKey::Role::NormalMap);
  // Dropped open_pbr_surface inputs with no Q1 desc slot (warn-skip;
  // logging lands caller-side — this static lib has no spdlog):
  // coat_roughness_anisotropy, coat_rotation, specular_rotation,
  // transmission_depth/scatter/dispersion_*, subsurface_radius(_scale)/
  // scatter_anisotropy, thin_film_*, geometry_coat_normal/tangent.
  desc.source = OpenPBRMaterialDesc::Source::MaterialX;
}

// V2.A.18 / Q3 — Autodesk standard_surface translator (MaterialX). Uses
// `base` as the layer weight (not `base_weight` like OpenPBR) and
// collapses the (float emission weight, color3 emission color) pair
// into Pyxis's `emissionLuminance + emissionColor` fields. Like the
// OpenPBR translator, inputs are resolved through the node graph so
// connected textures + promoted interface constants are honoured.
void TranslateMaterialXStandardSurface(const pxr::UsdShadeShader& shader,
                                        OpenPBRMaterialDesc&       desc,
                                        AcquireTextureFn           acquire,
                                        void*                      userData,
                                        pxr::UsdTimeCode           timeCode) noexcept
{
  auto resolve = [&](const char* name) {
    return ResolveMtlxInput(shader.GetInput(pxr::TfToken(name)), timeCode);
  };
  auto scalar = [&](const char* name, float fallback) -> float {
    const MtlxResolved r = resolve(name);
    if (r.kind == MtlxResolved::Kind::Constant && r.value.IsHolding<float>())
      return r.value.UncheckedGet<float>();
    return fallback;
  };
  auto color = [&](const char* name, hlslpp::float3 fallback) -> hlslpp::float3 {
    const MtlxResolved r = resolve(name);
    if (r.kind == MtlxResolved::Kind::Constant && r.value.IsHolding<pxr::GfVec3f>())
    {
      const pxr::GfVec3f c = r.value.UncheckedGet<pxr::GfVec3f>();
      return hlslpp::float3{c[0], c[1], c[2]};
    }
    return fallback;
  };
  auto scalarTex = [&](const char* name, float fallback, TextureKey::Role role,
                       TextureHandle& outMap) -> float {
    const MtlxResolved r = resolve(name);
    if (r.kind == MtlxResolved::Kind::Texture)
    {
      outMap = AcquireTextureMaybeUdim(acquire, userData, r.file, r.udimPattern,
                                       r.hasUdim, role);
      return (outMap != TextureHandle::Invalid) ? 1.0f : fallback;
    }
    if (r.kind == MtlxResolved::Kind::Constant && r.value.IsHolding<float>())
      return r.value.UncheckedGet<float>();
    return fallback;
  };
  auto colorTex = [&](const char* name, hlslpp::float3 fallback,
                      TextureKey::Role role, TextureHandle& outMap) -> hlslpp::float3 {
    const MtlxResolved r = resolve(name);
    if (r.kind == MtlxResolved::Kind::Texture)
    {
      outMap = AcquireTextureMaybeUdim(acquire, userData, r.file, r.udimPattern,
                                       r.hasUdim, role);
      if (outMap != TextureHandle::Invalid)
        return hlslpp::float3{1.0f, 1.0f, 1.0f};
      return fallback;
    }
    if (r.kind == MtlxResolved::Kind::Constant && r.value.IsHolding<pxr::GfVec3f>())
    {
      const pxr::GfVec3f c = r.value.UncheckedGet<pxr::GfVec3f>();
      return hlslpp::float3{c[0], c[1], c[2]};
    }
    return fallback;
  };

  desc.baseColor      = colorTex("base_color", hlslpp::float3{0.18f, 0.18f, 0.18f},
                                 TextureKey::Role::BaseColor, desc.baseColorMap);
  desc.baseWeight     = scalar("base", 1.0f);
  desc.metalness      = scalarTex("metalness", 0.0f,
                                  TextureKey::Role::RoughnessMetallic, desc.metallicMap);
  desc.roughness      = scalarTex("specular_roughness", 0.5f,
                                  TextureKey::Role::RoughnessMetallic, desc.roughnessMap);
  desc.specularWeight = scalar("specular", 1.0f);
  desc.specularIor    = scalar("specular_IOR", 1.5f);
  desc.coatWeight     = scalar("coat", 0.0f);
  desc.coatRoughness  = scalar("coat_roughness", 0.0f);
  desc.transmissionWeight = scalar("transmission", 0.0f);
  desc.emissionColor  = colorTex("emission_color", hlslpp::float3{0.0f, 0.0f, 0.0f},
                                 TextureKey::Role::Emission, desc.emissionMap);
  desc.emissionLuminance = scalar("emission", 0.0f);
  // Q1 OpenPBR-complete — Standard Surface → OpenPBR mapping for the
  // Q1 desc extension. Fallbacks are the desc (OpenPBR spec) defaults,
  // matching this translator's existing convention.
  StoreRgb(color("specular_color", hlslpp::float3{1.0f, 1.0f, 1.0f}),
           desc.specularColorR, desc.specularColorG, desc.specularColorB);
  desc.specularRoughnessAnisotropy = scalar("specular_anisotropy", 0.0f);
  // Standard Surface sheen_* → OpenPBR fuzz_* (same lobe family; the
  // Zeltner LTC fuzz consumes these at Q2).
  desc.fuzzWeight = scalar("sheen", 0.0f);
  StoreRgb(color("sheen_color", hlslpp::float3{1.0f, 1.0f, 1.0f}),
           desc.fuzzColorR, desc.fuzzColorG, desc.fuzzColorB);
  desc.fuzzRoughness = scalar("sheen_roughness", 0.5f);
  StoreRgb(color("coat_color", hlslpp::float3{1.0f, 1.0f, 1.0f}),
           desc.coatColorR, desc.coatColorG, desc.coatColorB);
  desc.coatIor = scalar("coat_IOR", 1.6f);
  // Standard Surface's coat_affect_color / coat_affect_roughness have
  // NO OpenPBR equivalent — OpenPBR replaces them with coat_darkening
  // (default 1) and an always-on parameter-free roughening formula.
  // coat_affect_color > 0 maps onto coat_darkening = 1 (the default
  // anyway); any other authored value is warn-skipped, as is
  // coat_affect_roughness (logging lands caller-side — this static
  // lib has no spdlog).
  if (scalar("coat_affect_color", 0.0f) > 0.0f)
    desc.coatDarkening = 1.0f;
  desc.subsurfaceWeight = scalar("subsurface", 0.0f);
  StoreRgb(color("subsurface_color", hlslpp::float3{0.8f, 0.8f, 0.8f}),
           desc.subsurfaceColorR, desc.subsurfaceColorG, desc.subsurfaceColorB);
  StoreRgb(color("transmission_color", hlslpp::float3{1.0f, 1.0f, 1.0f}),
           desc.transmissionColorR, desc.transmissionColorG, desc.transmissionColorB);
  {
    const MtlxResolved r = resolve("thin_walled");
    bool thinWalled = false;
    if (r.kind == MtlxResolved::Kind::Constant)
    {
      if (r.value.IsHolding<bool>())  thinWalled = r.value.UncheckedGet<bool>();
      else if (r.value.IsHolding<int>())   thinWalled = r.value.UncheckedGet<int>() != 0;
      else if (r.value.IsHolding<float>()) thinWalled = r.value.UncheckedGet<float>() > 0.0f;
    }
    desc.thinWalled = thinWalled ? 1u : 0u;
  }
  // Geometry normal — direct tangent-space image or `ND_normalmap` wrap.
  if (const MtlxResolved r = resolve("normal");
      r.kind == MtlxResolved::Kind::Texture)
    desc.normalMap = AcquireTextureMaybeUdim(acquire, userData, r.file,
                                             r.udimPattern, r.hasUdim,
                                             TextureKey::Role::NormalMap);
  // Dropped standard_surface inputs not in the Q1 mapping set
  // (warn-skip): diffuse_roughness, specular_rotation,
  // coat_anisotropy/rotation/normal, transmission_depth/scatter/
  // extra_roughness/dispersion, subsurface_radius/scale/anisotropy,
  // thin_film_*, tangent.
  desc.source = OpenPBRMaterialDesc::Source::MaterialX;
}

// V2.A.23 follow-up — MDL OmniPBR exposes textures via direct
// SdfAssetPath inputs (e.g. `inputs:normalmap_texture = @./Oak_N.png@`)
// rather than the UsdPreviewSurface `UsdUVTexture` graph. Resolve
// the asset path and call the renderer's AcquireTexture callback.
// Returns `TextureHandle::Invalid` when the input is missing,
// unauthored, holds an empty path, or the callback is null.
[[nodiscard]] TextureHandle ReadMdlTextureInput(
    const pxr::UsdShadeShader& shader,
    const pxr::TfToken&        name,
    AcquireTextureFn           acquire,
    void*                      userData,
    TextureKey::Role           role,
    TextureKey::Color          colorspace) noexcept
{
  if (acquire == nullptr)
    return TextureHandle::Invalid;
  const pxr::UsdShadeInput input = shader.GetInput(name);
  if (!input)
    return TextureHandle::Invalid;
  pxr::VtValue value;
  if (!input.Get(&value))
    return TextureHandle::Invalid;
  if (!value.IsHolding<pxr::SdfAssetPath>())
    return TextureHandle::Invalid;
  const pxr::SdfAssetPath assetPath = value.UncheckedGet<pxr::SdfAssetPath>();
  const std::string& resolved = assetPath.GetResolvedPath().empty()
                                     ? assetPath.GetAssetPath()
                                     : assetPath.GetResolvedPath();
  if (resolved.empty())
    return TextureHandle::Invalid;
  return acquire(resolved, role, userData);
  (void)colorspace;  // role-derived sRGB/Linear is the renderer's call
}

// Q1 OpenPBR-complete — OmniGlass detection. Four signals, any of
// which marks the prim as glass: the universal `info:id` (e.g.
// "mdl::OmniGlass"), the Omniverse sourceAsset subIdentifier, an
// authored glass-only input (covers re-exports that strip both ids),
// or (2026-07-06 RTX-alignment round 2) a glass-only key recovered
// from an OmniGlass-family preset file (the World Lobby's
// "Clear_Glass": `export material Clear_Glass(*) =
// OmniGlass::OmniGlass(glass_color: ..., glass_ior: 1.52f, ...)` with
// NO USD-authored overrides at all, so the subIdentifier is
// "Clear_Glass" — not "OmniGlass" — and every USD-input probe above
// misses; without this the translator couldn't tell this material
// apart from a plain untextured OmniPBR default and it rendered as
// flat 0.18-grey OPAQUE instead of clear see-through glass).
[[nodiscard]] bool IsOmniGlass(const pxr::UsdShadeShader& shader,
                               const MdlPreset&           preset) noexcept
{
  pxr::TfToken shaderId;
  if (shader.GetShaderId(&shaderId)
      && shaderId.GetString().find("OmniGlass") != std::string::npos)
    return true;
  static const pxr::TfToken subIdentifierAttr("info:mdl:sourceAsset:subIdentifier");  // NOLINT
  if (const pxr::UsdAttribute attr = shader.GetPrim().GetAttribute(subIdentifierAttr); attr)
  {
    pxr::VtValue value;
    if (attr.Get(&value) && value.IsHolding<pxr::TfToken>()
        && value.UncheckedGet<pxr::TfToken>().GetString().find("OmniGlass")
               != std::string::npos)
      return true;
  }
  // Glass-only inputs (deliberately excludes `thin_walled`, which is
  // not unique to OmniGlass).
  static const std::array<pxr::TfToken, 3> GLASS_INPUT_PROBE{{
      pxr::TfToken{"glass_color"},
      pxr::TfToken{"glass_ior"},
      pxr::TfToken{"frosting_roughness"},
  }};
  for (const pxr::TfToken& inputName : GLASS_INPUT_PROBE)
  {
    const pxr::UsdShadeInput input = shader.GetInput(inputName);
    if (input && input.GetAttr().HasAuthoredValue())
      return true;
  }
  static constexpr std::array<const char*, 3> GLASS_PRESET_PROBE{
      "glass_color", "glass_ior", "frosting_roughness"};
  for (const char* key : GLASS_PRESET_PROBE)
  {
    if (preset.args.contains(key))
      return true;
  }
  return false;
}

float PresetFloat(const MdlPreset& preset, const char* key, float fallback) noexcept
{
  const auto it = preset.args.find(key);
  return (it == preset.args.end()) ? fallback : ParseMdlFloatLiteral(it->second, fallback);
}

hlslpp::float3 PresetColor(const MdlPreset& preset, const char* key,
                           hlslpp::float3 fallback) noexcept
{
  const auto it = preset.args.find(key);
  if (it == preset.args.end())
    return fallback;
  const std::string& value = it->second;
  const std::size_t  open = value.find('(');
  const std::size_t  close = value.rfind(')');
  if (open == std::string::npos || close == std::string::npos || close <= open)
    return fallback;
  const std::string inner = value.substr(open + 1, close - open - 1);
  std::vector<float> comps;
  std::size_t start = 0;
  while (start <= inner.size())
  {
    const std::size_t comma = inner.find(',', start);
    const std::size_t segEnd = (comma == std::string::npos) ? inner.size() : comma;
    comps.push_back(ParseMdlFloatLiteral(inner.substr(start, segEnd - start), 0.0f));
    if (comma == std::string::npos)
      break;
    start = comma + 1;
  }
  if (comps.size() == 1)
    return hlslpp::float3{comps[0], comps[0], comps[0]};
  if (comps.size() >= 3)
    return hlslpp::float3{comps[0], comps[1], comps[2]};
  return fallback;
}

bool PresetBool(const MdlPreset& preset, const char* key, bool fallback) noexcept
{
  const auto it = preset.args.find(key);
  if (it == preset.args.end())
    return fallback;
  if (it->second == "true")
    return true;
  if (it->second == "false")
    return false;
  return fallback;
}

// Parses an MDL `float2(x, y)` (or the single-value splat `float2(v)`)
// literal. Returns false (leaving outX/outY untouched) when the key
// is absent or malformed, matching `ReadShaderInputFloat2`'s contract.
bool PresetFloat2(const MdlPreset& preset, const char* key, float& outX, float& outY) noexcept
{
  const auto it = preset.args.find(key);
  if (it == preset.args.end())
    return false;
  const std::string& value = it->second;
  const std::size_t  open = value.find('(');
  const std::size_t  close = value.rfind(')');
  if (open == std::string::npos || close == std::string::npos || close <= open)
    return false;
  const std::string inner = value.substr(open + 1, close - open - 1);
  const std::size_t comma = inner.find(',');
  if (comma == std::string::npos)
  {
    const float v = ParseMdlFloatLiteral(inner, 0.0f);
    outX = v;
    outY = v;
    return true;
  }
  outX = ParseMdlFloatLiteral(inner.substr(0, comma), 0.0f);
  outY = ParseMdlFloatLiteral(inner.substr(comma + 1), 0.0f);
  return true;
}

// Extracts the first quoted path out of an MDL `texture_2d("path"
// /* tag N, version M */, ::tex::gamma_X)` literal. Returns "" for
// `texture_2d()` (no texture authored) or a missing key.
std::string PresetTexturePath(const MdlPreset& preset, const char* key) noexcept
{
  const auto it = preset.args.find(key);
  if (it == preset.args.end())
    return {};
  const std::string& value = it->second;
  const std::size_t  firstQuote = value.find('"');
  if (firstQuote == std::string::npos)
    return {};
  const std::size_t secondQuote = value.find('"', firstQuote + 1);
  if (secondQuote == std::string::npos)
    return {};
  return value.substr(firstQuote + 1, secondQuote - firstQuote - 1);
}

// USD-authored input wins (via ReadMdlTextureInput); otherwise falls
// back to the preset's parsed texture path, resolved relative to the
// .mdl file's own directory (matching how the .mdl's `texture_2d`
// paths are always written relative to the file that declares them).
TextureHandle ReadMdlTextureInputOrPreset(
    const pxr::UsdShadeShader& shader, const pxr::TfToken& name, const MdlPreset& preset,
    const char* presetKey, AcquireTextureFn acquire, void* userData, TextureKey::Role role,
    TextureKey::Color colorspace) noexcept
{
  const TextureHandle authored =
      ReadMdlTextureInput(shader, name, acquire, userData, role, colorspace);
  if (authored != TextureHandle::Invalid || acquire == nullptr)
    return authored;
  const std::string relPath = PresetTexturePath(preset, presetKey);
  if (relPath.empty() || preset.dir.empty())
    return TextureHandle::Invalid;
  const std::string absPath = (std::filesystem::path(preset.dir) / relPath).lexically_normal().string();
  return acquire(absPath, role, userData);
}

void TranslateMdl(const pxr::UsdShadeShader& shader,
                   OpenPBRMaterialDesc&      desc,
                   AcquireTextureFn          acquire,
                   void*                     userData,
                   pxr::UsdTimeCode          timeCode) noexcept
{
  // 2026-07-06 RTX-alignment round 2 — see LoadMdlPresetIfOmniPbr's
  // comment above. `preset.valid` is false (empty args) whenever the
  // shader isn't a locally-resolvable OmniPBR-family preset call, in
  // which case every `PresetXxx(preset, key, fallback)` call below is
  // a plain pass-through to `fallback` — byte-identical to the
  // pre-round-2 behaviour for every material that doesn't hit this
  // gap (Oak/OakDark/Felt/... already author their relevant inputs in
  // USD, so `ReadFloat`/`ReadColor`/etc. resolve those first and never
  // consult the preset fallback at all).
  const MdlPreset preset = LoadMdlPresetIfOmniPbr(shader);

  // Base albedo = diffuse_color_constant × diffuse_tint, matching the
  // MDL OmniPBR / Plain shading-graph convention. Two real authoring
  // patterns appear in the World Lobby:
  //   * OmniPBR (e.g. Oak, MetalFeet): authors `diffuse_color_constant`
  //     as the albedo, with `diffuse_tint` defaulting to (1,1,1).
  //   * Plain.mdl (e.g. Felt_Green): authors NO `diffuse_color_constant`
  //     — the colour lives entirely in `diffuse_tint=(0.48,0.56,0.34)`.
  //     Reading only diffuse_color_constant left felt at the 0.18 grey
  //     fallback instead of green.
  // So: if diffuse_color_constant is authored, it's the base; otherwise
  // the base is white when a tint is present (Plain.mdl), or the 0.18
  // neutral fallback when neither is authored. The tint then multiplies.
  // `diffuseTint` multiplies the albedo (texture if present, else the
  // constant). Kept in function scope so the texture branch below can
  // re-set desc.baseColor = tint (the closesthit then does texture ×
  // tint). When a diffuse_texture is bound the constant is IGNORED — the
  // texture IS the albedo — so a green-tinted white felt weave reads
  // green, while a tint-1 textured material (oak) is unchanged.
  const hlslpp::float3 diffuseTint = ReadColor(
      shader, pxr::TfToken("diffuse_tint"),
      PresetColor(preset, "diffuse_tint", hlslpp::float3{1.0f, 1.0f, 1.0f}), timeCode);
  {
    const pxr::UsdShadeInput constInput =
        shader.GetInput(pxr::TfToken("diffuse_color_constant"));
    const bool hasConst = constInput && constInput.GetAttr().HasAuthoredValue();
    const pxr::UsdShadeInput tintInput = shader.GetInput(pxr::TfToken("diffuse_tint"));
    const bool hasTint = tintInput && tintInput.GetAttr().HasAuthoredValue();
    const bool presetHasConst = preset.args.contains("diffuse_color_constant");
    const hlslpp::float3 base =
        hasConst ? ReadColor(shader, pxr::TfToken("diffuse_color_constant"),
                             hlslpp::float3{0.18f, 0.18f, 0.18f}, timeCode)
                 : (presetHasConst
                        ? PresetColor(preset, "diffuse_color_constant",
                                     hlslpp::float3{0.18f, 0.18f, 0.18f})
                        : (hasTint ? hlslpp::float3{1.0f, 1.0f, 1.0f}
                                   : hlslpp::float3{0.18f, 0.18f, 0.18f}));
    desc.baseColor = base * diffuseTint;
  }
  // 2026-07-05 RTX-alignment (rtx-realtime-alignment-design.md, per-
  // material albedo forensics) — MDL OmniPBR / "Base" library "Albedo
  // Adjustments" group. OmniPBR_ClearCoat.mdl applies these to the
  // SAMPLED diffuse_texture (in linear space, post sRGB-decode) before
  // the diffuse_tint multiply:
  //   scaled      = texture_sample * albedo_brightness + albedo_add
  //   desaturated = lerp(scaled, luminance(scaled), albedo_desaturation)
  //   base_color  = desaturated * diffuse_tint
  // The closesthit (shading.slang) applies the formula once the texture
  // is sampled; here we just carry the three authored scalars through.
  // Every World Lobby "Base/*" material (Oak, OakDark, Terrazzo,
  // Paint_Satin, the Aperture Emissives) authors these explicitly, and
  // so do some OmniPBR leaf materials (the Modern reception "White"
  // fixture: albedo_brightness = 0.07). Defaults (1, 0, 0) are the MDL
  // function defaults — an unauthored material is a no-op here.
  desc.albedoBrightness = ReadFloat(shader, pxr::TfToken("albedo_brightness"),
                                    PresetFloat(preset, "albedo_brightness", 1.0f), timeCode);
  desc.albedoDesaturation = ReadFloat(shader, pxr::TfToken("albedo_desaturation"),
                                      PresetFloat(preset, "albedo_desaturation", 0.0f), timeCode);
  desc.albedoAdd = ReadFloat(shader, pxr::TfToken("albedo_add"),
                             PresetFloat(preset, "albedo_add", 0.0f), timeCode);
  desc.metalness = ReadFloat(shader, pxr::TfToken("metallic_constant"),
                             PresetFloat(preset, "metallic_constant", 0.0f), timeCode);
  desc.roughness = ReadFloat(
      shader, pxr::TfToken("reflection_roughness_constant"),
      ReadFloat(shader, pxr::TfToken("roughness_constant"),
               PresetFloat(preset, "reflection_roughness_constant",
                          PresetFloat(preset, "roughness_constant", 0.5f)),
               timeCode),
      timeCode);
  desc.opacity = ReadFloat(shader, pxr::TfToken("opacity_constant"),
                           PresetFloat(preset, "opacity_constant", 1.0f), timeCode);
  // 2026-07-06 RTX-alignment round 2 (materials chapter) — OmniPBR's
  // constant/texture blend weights. See the OpenPBRMaterialDesc field
  // comments for the full rationale; `ao_to_diffuse` blends the ORM
  // AO channel into baseColor, the other two lerp the sampled
  // roughness/metallic against the scalar instead of the texture
  // unconditionally winning once bound (both consumed in
  // shading.slang's material-eval region). Defaults (0, 1, 1) are the
  // MDL OmniPBR function defaults.
  desc.aoToDiffuse = ReadFloat(shader, pxr::TfToken("ao_to_diffuse"),
                               PresetFloat(preset, "ao_to_diffuse", 0.0f), timeCode);
  desc.metallicTextureInfluence =
      ReadFloat(shader, pxr::TfToken("metallic_texture_influence"),
               PresetFloat(preset, "metallic_texture_influence", 1.0f), timeCode);
  desc.reflectionRoughnessTextureInfluence =
      ReadFloat(shader, pxr::TfToken("reflection_roughness_texture_influence"),
               PresetFloat(preset, "reflection_roughness_texture_influence", 1.0f), timeCode);
  // V2.A.23 follow-up — OmniPBR `bump_factor` scales the normal-map
  // strength. Default 1.0 = full authored bump. Production content
  // authors 0.6..10; honoring it keeps polished surfaces from looking
  // over-bumped + lets detailed-matte surfaces dial relief up.
  desc.normalStrength = ReadFloat(shader, pxr::TfToken("bump_factor"),
                                  PresetFloat(preset, "bump_factor", 1.0f), timeCode);
  // V2.A.24 — UV transform (OmniPBR texture_scale / texture_rotate /
  // texture_translate) + normal-map tangent flips. Reuses the
  // baseColorUv* desc slots (V2.A.18) which the closesthit now applies
  // to every texture sample. Without these the World Lobby's wood
  // walls render at the wrong tiling (texture_scale=(1,2)), wrong
  // grain direction (texture_rotate=90), and inverted bump
  // (flip_tangent_v=1).
  {
    float scaleX = 1.0f;
    float scaleY = 1.0f;
    if (ReadShaderInputFloat2(shader, pxr::TfToken("texture_scale"), scaleX, scaleY)
        || PresetFloat2(preset, "texture_scale", scaleX, scaleY))
    {
      desc.baseColorUvScaleX = scaleX;
      desc.baseColorUvScaleY = scaleY;
    }
    desc.baseColorUvRotationDeg = ReadFloat(shader, pxr::TfToken("texture_rotate"),
                                            PresetFloat(preset, "texture_rotate", 0.0f), timeCode);
    float transX = 0.0f;
    float transY = 0.0f;
    if (ReadShaderInputFloat2(shader, pxr::TfToken("texture_translate"), transX, transY)
        || PresetFloat2(preset, "texture_translate", transX, transY))
    {
      desc.baseColorUvTranslationX = transX;
      desc.baseColorUvTranslationY = transY;
    }
    desc.flipTangentU = ReadBoolish(shader, pxr::TfToken("flip_tangent_u"),
                                    PresetBool(preset, "flip_tangent_u", false), timeCode)
                            ? 1u
                            : 0u;
    desc.flipTangentV = ReadBoolish(shader, pxr::TfToken("flip_tangent_v"),
                                    PresetBool(preset, "flip_tangent_v", false), timeCode)
                            ? 1u
                            : 0u;
  }
  desc.emissionColor = ReadColor(shader, pxr::TfToken("emissive_color"),
                                 PresetColor(preset, "emissive_color", hlslpp::float3{0.0f, 0.0f, 0.0f}),
                                 timeCode);
  desc.emissionLuminance = ReadFloat(shader, pxr::TfToken("emissive_intensity"),
                                     PresetFloat(preset, "emissive_intensity", 0.0f), timeCode);
  desc.specularIor = ReadFloat(shader, pxr::TfToken("ior_constant"),
                               PresetFloat(preset, "ior_constant", 1.5f), timeCode);
  // V2.A.23 follow-up — OmniPBR's `enable_emission` flag, when off,
  // suppresses the surface-lighting contribution even if a non-zero
  // emissive_color / emissive_intensity is authored (the MDL stores
  // those as a "default off but ready to enable" stash; Oak.mdl's
  // (1.0, 0.1, 0.1) red is the canonical example). The MDL FUNCTION
  // default is FALSE; USD authoring only adds inputs when overridden,
  // so a missing `enable_emission` means "use the MDL default" =
  // disabled. Pre-fix the fallback was 1.0f (assumed enabled), which
  // leaked the sentinel red as authored emission on every wood / floor
  // / stucco material that authored emissive_color "just in case".
  if (!ReadBoolish(shader, pxr::TfToken("enable_emission"),
                   PresetBool(preset, "enable_emission", false), timeCode))
  {
    desc.emissionLuminance = 0.0f;
    desc.emissionColor     = hlslpp::float3{0.0f, 0.0f, 0.0f};
  }
  // V2.A.23 follow-up — MDL textures. OmniPBR-style assets author
  // textures as direct SdfAssetPath inputs, not as UsdUVTexture
  // sub-graphs. Pre-fix the translator skipped texture loading
  // entirely for MDL materials, so wood/floor/wall surfaces fell back
  // to their (typically grey-ish) scalar diffuse_color_constant
  // instead of showing their baseColor texture. Role + colorspace
  // map to TextureKey's renderer-side EOTF decision: BaseColor /
  // Emission = sRGB; everything else = linear.
  //
  // 2026-07-06 RTX-alignment round 2 — `ReadMdlTextureInputOrPreset`
  // falls back to the preset-parsed texture path (relative to the
  // .mdl file) when the USD Shader prim authors no override, which is
  // what recovers Steel_Carbon/Concrete_Formed/Stucco's real
  // BaseColor/ORM/Normal maps instead of the untextured grey fallback.
  desc.baseColorMap = ReadMdlTextureInputOrPreset(
      shader, pxr::TfToken("diffuse_texture"), preset, "diffuse_texture", acquire, userData,
      TextureKey::Role::BaseColor, TextureKey::Color::SRgb);
  // When a diffuse_texture is bound it provides the albedo; the constant
  // is dropped and only the tint modulates (closesthit does texture ×
  // baseColor). Felt's white weave × green tint → green felt.
  if (desc.baseColorMap != TextureHandle::Invalid)
    desc.baseColor = diffuseTint;
  desc.normalMap = ReadMdlTextureInputOrPreset(
      shader, pxr::TfToken("normalmap_texture"), preset, "normalmap_texture", acquire, userData,
      TextureKey::Role::NormalMap, TextureKey::Color::Linear);

  // V2.A.23 follow-up — OmniPBR packs Occlusion-Roughness-Metallic
  // into a single `ORM_texture` (R = AO, G = roughness, B = metallic;
  // the glTF / OpenPBR ORM convention) and gates it with the
  // `enable_ORM_texture` bool. This is the dominant packing in
  // production Omniverse content (the World Lobby authors *only* ORM,
  // never the separate reflectionroughness/metallic textures). The
  // closesthit samples roughness from `.g` of `roughnessTex` and
  // metallic from `.b` of `metallicTex`, so binding the SAME ORM
  // texture to both slots lines up channel-for-channel — no shader
  // change needed.
  //
  // Fallback chain: explicit per-lobe textures
  // (reflectionroughness_texture / metallic_texture) win when
  // authored; otherwise ORM fills both. enable_ORM_texture defaults
  // false in the MDL, but a material that authored an ORM asset path
  // without the flag clearly intends to use it, so we also accept a
  // bound ORM path as implicit-enable.
  const TextureHandle ormTex = ReadMdlTextureInputOrPreset(
      shader, pxr::TfToken("ORM_texture"), preset, "ORM_texture", acquire, userData,
      TextureKey::Role::RoughnessMetallic, TextureKey::Color::Linear);
  const bool ormEnabled = ReadBoolish(shader, pxr::TfToken("enable_ORM_texture"),
                                     PresetBool(preset, "enable_ORM_texture", false), timeCode)
                          || (ormTex != TextureHandle::Invalid);

  const TextureHandle roughnessTex = ReadMdlTextureInputOrPreset(
      shader, pxr::TfToken("reflectionroughness_texture"), preset, "reflectionroughness_texture",
      acquire, userData, TextureKey::Role::RoughnessMetallic, TextureKey::Color::Linear);
  const TextureHandle metallicTex = ReadMdlTextureInputOrPreset(
      shader, pxr::TfToken("metallic_texture"), preset, "metallic_texture", acquire, userData,
      TextureKey::Role::RoughnessMetallic, TextureKey::Color::Linear);

  desc.roughnessMap = (roughnessTex != TextureHandle::Invalid)
                          ? roughnessTex
                          : (ormEnabled ? ormTex : TextureHandle::Invalid);
  desc.metallicMap  = (metallicTex != TextureHandle::Invalid)
                          ? metallicTex
                          : (ormEnabled ? ormTex : TextureHandle::Invalid);

  desc.emissionMap = ReadMdlTextureInputOrPreset(
      shader, pxr::TfToken("emissive_mask_texture"), preset, "emissive_mask_texture", acquire,
      userData, TextureKey::Role::Emission, TextureKey::Color::SRgb);
  desc.opacityMap = ReadMdlTextureInputOrPreset(
      shader, pxr::TfToken("opacity_texture"), preset, "opacity_texture", acquire, userData,
      TextureKey::Role::Opacity, TextureKey::Color::Linear);
  // RFC 0005 — generalized planar projection, driven purely by the
  // material's authored OmniPBR inputs and applied to EVERY material (no
  // wood-name special case — the prior V2.B `sourceAsset`-path sniff is
  // removed). `project_uvw` turns projection on; `world_or_object` selects
  // the space (0 = world, 1 = object). The closesthit derives planar UV
  // from the hit position; object space transforms by WorldToObject first
  // so the projection follows the instance.
  //
  // Measured against the World Lobby (RFC 0005 Findings): 10 materials
  // author project_uvw=1 (tiles/stone/metal/paint), 3 of those object-space;
  // oak authors project_uvw=0, so it samples its mesh UVs (the oak-grain
  // question is a separate `st`/texture-transform investigation, not
  // projection — and `uv_space_index` is never authored as the secondary
  // set, so no UV-set path is needed here).
  {
    const bool projectUvw = ReadBoolish(shader, pxr::TfToken("project_uvw"),
                                        PresetBool(preset, "project_uvw", false), timeCode);
    const bool objectSpace = ReadBoolish(shader, pxr::TfToken("world_or_object"),
                                         PresetBool(preset, "world_or_object", false), timeCode);
    desc.projectionMode = projectUvw ? (objectSpace ? 2u : 1u) : 0u;
  }
  // Q1 OpenPBR-complete — OmniPBR clearcoat → OpenPBR coat. Gated on
  // `enable_clearcoat` (MDL function default FALSE; a missing input
  // means "use the MDL default" = disabled — same convention as
  // enable_emission above), so disabled-clearcoat materials keep the
  // desc coat fields at their defaults and dedup cleanly.
  if (ReadBoolish(shader, pxr::TfToken("enable_clearcoat"),
                  PresetBool(preset, "enable_clearcoat", false), timeCode))
  {
    desc.coatWeight = ReadFloat(shader, pxr::TfToken("clearcoat_weight"),
                                PresetFloat(preset, "clearcoat_weight", 1.0f), timeCode);
    StoreRgb(ReadColor(shader, pxr::TfToken("clearcoat_tint"),
                       PresetColor(preset, "clearcoat_tint", hlslpp::float3{1.0f, 1.0f, 1.0f}),
                       timeCode),
             desc.coatColorR, desc.coatColorG, desc.coatColorB);
    desc.coatIor = ReadFloat(shader, pxr::TfToken("clearcoat_ior"),
                             PresetFloat(preset, "clearcoat_ior", 1.6f), timeCode);
    desc.coatRoughness =
        ReadFloat(shader, pxr::TfToken("clearcoat_reflection_roughness"),
                 PresetFloat(preset, "clearcoat_reflection_roughness", 0.0f), timeCode);
    // Dropped OmniPBR clearcoat inputs with no OpenPBR equivalent
    // (warn-skip; logging lands caller-side — no spdlog here):
    // clearcoat_transparency, clearcoat_bump_factor, clearcoat_flatten,
    // clearcoat_normalmap_texture.
  }
  // Q1 OpenPBR-complete — OmniGlass → OpenPBR transmission. OmniGlass
  // is a dielectric transmitter: transmission is unconditionally on,
  // glass_color is the depth-0 constant transmission tint, glass_ior
  // drives the dielectric Fresnel, frosting_roughness replaces the
  // OmniPBR roughness chain (OmniGlass authors no
  // reflection_roughness_constant — smooth glass default 0).
  if (IsOmniGlass(shader, preset))
  {
    desc.transmissionWeight = 1.0f;
    StoreRgb(ReadColor(shader, pxr::TfToken("glass_color"),
                       PresetColor(preset, "glass_color", hlslpp::float3{1.0f, 1.0f, 1.0f}),
                       timeCode),
             desc.transmissionColorR, desc.transmissionColorG, desc.transmissionColorB);
    desc.specularIor = ReadFloat(shader, pxr::TfToken("glass_ior"),
                                 PresetFloat(preset, "glass_ior", desc.specularIor), timeCode);
    desc.roughness = ReadFloat(shader, pxr::TfToken("frosting_roughness"),
                               PresetFloat(preset, "frosting_roughness", 0.0f), timeCode);
    desc.thinWalled = ReadBoolish(shader, pxr::TfToken("thin_walled"),
                                  PresetBool(preset, "thin_walled", false), timeCode)
                          ? 1u
                          : 0u;
    // 2026-07-05 RTX-alignment — OmniGlass is a pure dielectric
    // transmitter with no diffuse_color_constant / diffuse_tint inputs
    // at all, so the top-of-function base-albedo block above always
    // fell through to the neutral 0.18-grey fallback (no diffuse
    // network authored → base = fallback). A glass surface has no
    // OpenPBR base (diffuse) lobe, so carrying a mid-grey "albedo"
    // through was wrong on its face and measured as a large flat-grey
    // over-bright region in the baseColor/albedo AOV (the closesthit's
    // lit shading was already correct — OpenPBRDiffuseSelector zeroes
    // the base contribution once transmissionWeight > 0 — this only
    // fixed the raw diagnostic AOV). Zero it so the albedo AOV reads
    // black for glass, matching "no diffuse component."
    desc.baseColor = hlslpp::float3{0.0f, 0.0f, 0.0f};
    // Dropped OmniGlass inputs not in the Q1 mapping set (warn-skip):
    // depth, glass_color_texture, reflection_color_texture,
    // roughness_texture_influence.
  }
  // Generic MDL glass — a custom dielectric-transmitter MDL that is NOT
  // OmniGlass (the World Lobby's "Tinted_Glass" MDL: subIdentifier "Tinted_Glass",
  // no glass_* inputs). Signalled by an authored `transmission_color`. This MDL
  // names its inputs `ior` / `roughness` (NOT the OmniPBR `ior_constant` /
  // `reflection_roughness_constant` the scalar reads above used), so re-read
  // those and turn transmission on with transmission_color as the depth-0 tint.
  // Without this the tinted-glass windows render OPAQUE grey instead of the dark
  // see-through tint RTX shows.
  else if (const pxr::UsdShadeInput transmissionColorInput =
               shader.GetInput(pxr::TfToken("transmission_color"));
           transmissionColorInput && transmissionColorInput.GetAttr().HasAuthoredValue())
  {
    desc.transmissionWeight = 1.0f;
    StoreRgb(ReadColor(shader, pxr::TfToken("transmission_color"),
                       hlslpp::float3{1.0f, 1.0f, 1.0f}, timeCode),
             desc.transmissionColorR, desc.transmissionColorG, desc.transmissionColorB);
    desc.specularIor = ReadFloat(shader, pxr::TfToken("ior"), desc.specularIor, timeCode);
    desc.roughness   = ReadFloat(shader, pxr::TfToken("roughness"), desc.roughness, timeCode);
    StoreRgb(ReadColor(shader, pxr::TfToken("reflection_color"),
                       hlslpp::float3{1.0f, 1.0f, 1.0f}, timeCode),
             desc.specularColorR, desc.specularColorG, desc.specularColorB);
    desc.thinWalled =
        ReadBoolish(shader, pxr::TfToken("thin_walled"), false, timeCode) ? 1u : 0u;
    // 2026-07-05 RTX-alignment — same reasoning as the OmniGlass branch
    // above: this generic dielectric-transmitter MDL (e.g. the World
    // Lobby's "Tinted_Glass") authors no diffuse_color_constant /
    // diffuse_tint either, so base-albedo fell through to the neutral
    // 0.18-grey fallback — measured as the single largest flat-grey
    // over-bright region in the per-material albedo forensics (396530
    // px, the lobby's glass facade). Zero it; glass has no base
    // (diffuse) lobe.
    desc.baseColor = hlslpp::float3{0.0f, 0.0f, 0.0f};
  }
  desc.source = OpenPBRMaterialDesc::Source::Mdl;
}

}  // namespace

OpenPBRMaterialDesc FromUsdShade(const pxr::UsdShadeMaterial& material,
                                 AcquireTextureFn acquire,
                                 void* userData,
                                 pxr::UsdTimeCode timeCode) noexcept {
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
  // V2.A.13 — `timeCode` threads through every per-translator scalar
  // / color read so an animated material attribute (e.g. animated
  // `diffuseColor`) tracks the requested frame.
  const SurfaceMatch match = ResolveSurface(material);
  switch (match.kind)
  {
    case SurfaceMatch::Kind::Mdl:
      TranslateMdl(match.shader, desc, acquire, userData, timeCode);
      return desc;
    case SurfaceMatch::Kind::MaterialXOpenPBR:
      TranslateMaterialXOpenPBR(match.shader, desc, acquire, userData, timeCode);
      return desc;
    case SurfaceMatch::Kind::MaterialXStandardSurface:
      TranslateMaterialXStandardSurface(match.shader, desc, acquire, userData, timeCode);
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
      ReadColor(surface, pxr::TfToken("diffuseColor"), hlslpp::float3{0.18f, 0.18f, 0.18f}, timeCode);
  desc.metalness = ReadFloat(surface, pxr::TfToken("metallic"), 0.0f, timeCode);
  desc.roughness = ReadFloat(surface, pxr::TfToken("roughness"), 0.5f, timeCode);
  desc.opacity = ReadFloat(surface, pxr::TfToken("opacity"), 1.0f, timeCode);
  desc.specularIor = ReadFloat(surface, pxr::TfToken("ior"), 1.5f, timeCode);
  desc.coatWeight = ReadFloat(surface, pxr::TfToken("clearcoat"), 0.0f, timeCode);
  desc.coatRoughness = ReadFloat(surface, pxr::TfToken("clearcoatRoughness"), 0.01f, timeCode);
  desc.emissionColor =
      ReadColor(surface, pxr::TfToken("emissiveColor"), hlslpp::float3{0.0f, 0.0f, 0.0f}, timeCode);
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

    // 2026-07-06 RTX-alignment round 2 (materials round-2 follow-up,
    // rtx-realtime-alignment-design.md) — root cause of
    // Plaster_Wall_Cracked rendering at linear albedo ~0.05 vs ovrtx's
    // ~0.50 (10x too dark, the biggest single-material offender).
    // Unlike OmniPBR (separate diffuse_texture + diffuse_tint inputs,
    // tint defaulting to white — see TranslateMdl above), USD's
    // UsdPreviewSurface spec has no tint concept for `diffuseColor`:
    // when the input is texture-connected, the texture IS the albedo,
    // full stop. The scalar `ReadColor` picked up above is only the
    // USD-mandated value used when the input is NOT connected — every
    // DCC-baked material in this scene (Oak, Terrazzo, Slate, Plaster,
    // ...) authors that disconnected-fallback scalar as an inert
    // (0.18, 0.18, 0.18) placeholder alongside its texture connection.
    // Every OTHER textured material here resolves through TranslateMdl
    // instead (its MDL surface is OmniPBR-family), so this path was
    // dormant; Plaster_Wall_Cracked's MDL call (`= Plaster_Wall(...)`,
    // a bespoke vMaterials df:: graph) is the one material that isn't,
    // and it fell all the way through to here. shading.slang's
    // baseColor × texSample multiply then applied that 0.18 leftover
    // as an unintended ~5.5x darkening on top of the texture's own
    // sRGB decode — matching the measured 10x gap. Resetting the tint
    // to white whenever the texture resolved (mirroring TranslateMdl's
    // `if (desc.baseColorMap != Invalid) desc.baseColor = diffuseTint`
    // pattern, with an implicit tint of white here) fixes every
    // material on this fallback path, not just Plaster's.
    if (desc.baseColorMap != TextureHandle::Invalid)
      desc.baseColor = hlslpp::float3{1.0f, 1.0f, 1.0f};

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
