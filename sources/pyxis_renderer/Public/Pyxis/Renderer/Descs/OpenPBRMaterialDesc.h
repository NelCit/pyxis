// Pyxis renderer — public OpenPBR material descriptor.
//
// Plan §11 + §18.4. The single canonical material POD. All ingest
// adapters (UsdPreviewSurface, MaterialX `open_pbr_surface`,
// RenderMan fallback) translate to this shape; one generic
// closesthit shader consumes it (branchless on `MaterialFlag` bits
// once §11 lands at M5+).
//
// Note that the M3 path-trace box renders the cube against a
// hardcoded grey material with no texture maps; the texture-handle
// fields are reserved here as part of the byte-stable public surface
// (§22.3) so M5+ can populate them without an ABI break.
//
// `sourcePrim` is diagnostics-only: the renderer copies the first 63
// bytes into an internal POD before queueing the mutation across
// threads (§18.5 / §31), so the view never outlives the caller's
// stack. Never hashed — only used in error messages and the AOV
// inspector.

#pragma once

#include <Pyxis/Renderer/Forward.h>
#include <Pyxis/Renderer/RendererApi.h>

#include <cstdint>
#include <hlsl++.h>
#include <string_view>

namespace pyxis {

struct OpenPBRMaterialDesc {
  enum class Source : uint8_t {
    UsdPreviewSurface,
    MaterialX,
    RenderManFallback,
    Default,
    // V2.A.23 — MDL shaders (Omniverse OmniPBR / OmniGlass / OmniSurface).
    // Detected either by `info:id` starting with `mdl::` OR by
    // `info:mdl:sourceAsset` authored on the connected shader.
    Mdl,
  };

  // Base layer — see §11 OpenPBR §3.1.
  hlslpp::float3 baseColor = {0.8f, 0.8f, 0.8f};
  float baseWeight = 1.0f;
  float metalness = 0.0f;
  float roughness = 0.5f;

  // Reserved per §11 (specular / transmission / coat / emission /
  // geometry blocks). M5+ populates these alongside the OpenPBR
  // shader; v1 closesthit only consumes the base layer above.
  float specularWeight = 1.0f;
  float specularIor = 1.5f;
  float transmissionWeight = 0.0f;
  float coatWeight = 0.0f;
  float coatRoughness = 0.0f;
  hlslpp::float3 emissionColor = {0.0f, 0.0f, 0.0f};
  float emissionLuminance = 0.0f;
  float opacity = 1.0f;

  // Texture bindings — opaque handles obtained via
  // GpuScene::AcquireTexture. Invalid means "no texture; use the
  // scalar value above for this lobe."
  TextureHandle baseColorMap = TextureHandle::Invalid;
  TextureHandle metallicMap = TextureHandle::Invalid;
  TextureHandle roughnessMap = TextureHandle::Invalid;
  TextureHandle normalMap = TextureHandle::Invalid;
  TextureHandle emissionMap = TextureHandle::Invalid;
  TextureHandle opacityMap = TextureHandle::Invalid;
  TextureHandle transmissionMap = TextureHandle::Invalid;
  TextureHandle coatRoughnessMap = TextureHandle::Invalid;

  Source source = Source::Default;
  std::string_view sourcePrim;  // diagnostics only; not hashed.

  // V2.A.30 — material network outputs beyond `surface`. UsdShade
  // allows authoring a `displacement` and / or `volume` output on
  // the same material; Pyxis v2 reads `surface` only, but records
  // whether the others are authored so the closesthit (when we
  // grow displacement + volume rendering) can decide whether to
  // dispatch. Today the closesthit ignores both — bind but don't
  // use.
  // V2.A.24 — UsdUVTexture wrap modes (per-material, per-texture).
  // Pyxis v2 samples with the global `repeat` sampler. The wrap
  // mode authored by the artist is preserved here so the v2.1 or
  // v3 sampler-per-material upgrade has a CPU-side source.
  // V2.A.29 — UsdUVTexture sourceColorSpace token. Today we pick
  // sRGB / Linear by TextureKey::Role at decode time; storing the
  // authored token lets future OCIO-aware decode override.
  // Each consumes one uint32_t from the `_reserved[16]` slot per
  // §22.3, keeping the layout byte-stable.
  uint32_t hasDisplacementOutput = 0;  // V2.A.30 (was _reserved[0])
  uint32_t hasVolumeOutput       = 0;  // V2.A.30 (was _reserved[1])
  uint32_t baseColorWrapS        = 0;  // V2.A.24 (was _reserved[2]) — UsdGeomTokens hash
  uint32_t baseColorWrapT        = 0;  // V2.A.24 (was _reserved[3])
  uint32_t baseColorSourceCS     = 0;  // V2.A.29 (was _reserved[4]) — TfToken hash

  // V2.A.18 — UsdTransform2d UV transform applied to the baseColor
  // slot (the most commonly authored). USD's UsdTransform2d node
  // composes its UV input through scale → rotation → translation,
  // matching MaterialX's `texcoord` xform convention. v2 loads the
  // values onto every UsdPreviewSurface material whose UsdUVTexture
  // is connected through a UsdTransform2d; the closesthit still
  // samples with unmodified UVs in v2.0, but the data is preserved
  // for the v2.1 sampler-per-material upgrade. Float storage; the
  // 4-byte alignment matches the surrounding uint32_t reserved
  // slots so the binary layout stays byte-stable (§22.3). Defaults
  // are the identity transform (translation=(0,0), rotation=0,
  // scale=(1,1)) so a material WITHOUT a UsdTransform2d still
  // round-trips through the closesthit's UV math unchanged.
  float baseColorUvTranslationX  = 0.0f;  // V2.A.18 (was _reserved[5])
  float baseColorUvTranslationY  = 0.0f;  // V2.A.18 (was _reserved[6])
  float baseColorUvRotationDeg   = 0.0f;  // V2.A.18 (was _reserved[7]) — USD authors degrees; we keep the unit.
  float baseColorUvScaleX        = 1.0f;  // V2.A.18 (was _reserved[8])
  float baseColorUvScaleY        = 1.0f;  // V2.A.18 (was _reserved[9])

  // V2.A.23 follow-up — normal-map strength (OmniPBR `bump_factor`,
  // UsdPreviewSurface has no direct equivalent so it stays 1.0 there).
  // Scales the tangent-space normal's XY deviation before the TBN
  // transform: 0 = flat (ignore the normal map), 1 = full authored
  // bump, >1 = exaggerated. Production MDL content authors values
  // from 0.6 (subtle) to 10 (strong relief); honoring it keeps
  // polished surfaces (e.g. Terrazzo at 1.0) from looking grainier
  // than intended and lets matte-but-detailed surfaces dial relief
  // up. Default 1.0 = full strength (the pre-this-field behavior).
  float normalStrength           = 1.0f;  // V2.A.23 (was _reserved[10])

  // V2.A.24 — normal-map tangent-handedness flips (OmniPBR
  // `flip_tangent_u` / `flip_tangent_v`). The DirectX-vs-OpenGL
  // green-channel convention; Omniverse content commonly authors
  // flip_tangent_v = 1. 0 = no flip (default). The
  // baseColorUv{Scale,Rotation,Translation}* fields above carry the
  // OmniPBR texture_scale / texture_rotate / texture_translate for
  // MDL materials (V2.A.18 originally populated them only for
  // UsdTransform2d; V2.A.24 also fills them from MDL + the closesthit
  // now applies them).
  uint32_t flipTangentU          = 0;  // V2.A.24 (was _reserved[11])
  uint32_t flipTangentV          = 0;  // V2.A.24 (was _reserved[12])
  // RFC 0005 — planar projection mode. Generalizes the V2.B
  // `worldProjection` (0/1) to OmniPBR's `project_uvw` × `world_or_object`:
  //   0 = none (sample mesh UVs)
  //   1 = world-space planar  (project_uvw=1, world_or_object=0)
  //   2 = object-space planar (project_uvw=1, world_or_object=1)
  // When non-zero the closesthit derives UV from the hit position (U axis
  // along up) so directional detail reads consistently regardless of the
  // asset's per-mesh unwrap; object space transforms the hit by
  // WorldToObject first so the projection follows the instance. Same POD
  // slot/offset as the former `worldProjection` (not an ABI add).
  uint32_t projectionMode        = 0;  // RFC 0005 (was worldProjection / _reserved[13])

  // ------------------------------------------------------------------
  // OpenPBR-complete extension (Q1, openpbr-complete-design.md) — a
  // DELIBERATE MAJOR layout extension (§22 / RFC §44.1): the original
  // `_reserved[16]` budget was down to 2 slots, which cannot hold the
  // remaining OpenPBR v1.1.1 parameter set, so the fields below start
  // where the old `_reserved[2]` sat and grow the struct. Every offset
  // BEFORE this block is unchanged (pinned in
  // tests/unit/PublicDescLayout.cpp). Field defaults are the OpenPBR
  // spec defaults (spec parameter-reference v1.1.1); spec input names
  // in the trailing comments. Scalar R/G/B floats (not hlslpp::float3)
  // keep 4-byte packing — no SIMD padding holes.
  // ------------------------------------------------------------------
  float specularColorR = 1.0f;  // OpenPBR `specular_color` (default 1,1,1)
  float specularColorG = 1.0f;
  float specularColorB = 1.0f;
  float baseDiffuseRoughness = 0.0f;        // OpenPBR `base_diffuse_roughness`
  float specularRoughnessAnisotropy = 0.0f; // OpenPBR `specular_roughness_anisotropy`
  float coatColorR = 1.0f;  // OpenPBR `coat_color` (default 1,1,1)
  float coatColorG = 1.0f;
  float coatColorB = 1.0f;
  float coatIor = 1.6f;        // OpenPBR `coat_ior` (default 1.6 — NOT specular_ior's 1.5)
  float coatDarkening = 1.0f;  // OpenPBR `coat_darkening` (default 1 = physical darkening ON)
  float fuzzWeight = 0.0f;     // OpenPBR `fuzz_weight`
  float fuzzColorR = 1.0f;     // OpenPBR `fuzz_color` (default 1,1,1)
  float fuzzColorG = 1.0f;
  float fuzzColorB = 1.0f;
  float fuzzRoughness = 0.5f;  // OpenPBR `fuzz_roughness` (default 0.5)
  float transmissionColorR = 1.0f;  // OpenPBR `transmission_color` (default 1,1,1)
  float transmissionColorG = 1.0f;
  float transmissionColorB = 1.0f;
  float subsurfaceWeight = 0.0f;     // OpenPBR `subsurface_weight`
  float subsurfaceColorR = 0.8f;     // OpenPBR `subsurface_color` (default 0.8,0.8,0.8)
  float subsurfaceColorG = 0.8f;
  float subsurfaceColorB = 0.8f;
  uint32_t thinWalled = 0;  // OpenPBR `geometry_thin_walled` (boolean; 0 = false)

  // 2026-07-05 RTX-alignment (rtx-realtime-alignment-design.md) — MDL
  // OmniPBR / "Base" library "Albedo Adjustments" group
  // (`albedo_brightness` / `albedo_desaturation` / `albedo_add`,
  // OmniPBR_ClearCoat.mdl). Applied to the SAMPLED base-color texture
  // (post sRGB-decode, in linear space) BEFORE the diffuse_tint
  // multiply:
  //   scaled      = texture_sample * albedoBrightness + albedoAdd
  //   desaturated = lerp(scaled, luminance(scaled), albedoDesaturation)
  //   base_color  = desaturated * diffuse_tint
  // Every World Lobby "Base/*" MDL material (Oak, OakDark, Terrazzo,
  // Paint_Satin, the Aperture Emissives) authors these explicitly, as
  // does the Modern reception "White" OmniPBR leaf material
  // (albedo_brightness = 0.07). The translator previously read
  // diffuse_tint but dropped these three grading knobs, which was the
  // dominant per-material albedo-fidelity gap measured against ovrtx
  // (2-14x too bright depending on the material's authored brightness).
  // Defaults (1, 0, 0) are the MDL function defaults — a material that
  // never authors them round-trips byte-identically (no-op) through
  // the closesthit formula above, so UsdPreviewSurface / MaterialX /
  // RenderMan-sourced materials are unaffected.
  float albedoBrightness   = 1.0f;  // (was _reserved[0])
  float albedoDesaturation = 0.0f;  // (was _reserved[1])
  float albedoAdd          = 0.0f;  // (was _reserved[2])

  // 2026-07-06 RTX-alignment round 2 (materials chapter) — OmniPBR's
  // constant/texture blend weights. OmniPBR lerps a per-lobe constant
  // against its sampled texture by these weights rather than letting
  // the texture unconditionally win once bound; the translator +
  // closesthit previously always used a binary "texture if bound,
  // else constant" choice, which is a silent no-op for the (common)
  // influence=1 authoring but was measured wrong for at least one
  // World Lobby material (`rh_table_short`'s "Paint_Satin": authors
  // metallic_texture_influence=0 — i.e. "ignore the ORM's metal mask
  // entirely, this is a non-metal paint" — while an ORM texture is
  // still bound for its roughness/AO channels, so the old binary
  // logic rendered the satin tabletop chrome-white). Defaults (1, 1,
  // 0) match the MDL OmniPBR function defaults exactly, so a material
  // that never authors them (i.e. every other material measured in
  // this scene) round-trips byte-identically — no-op.
  float metallicTextureInfluence           = 1.0f;  // (was _reserved[3])
  float reflectionRoughnessTextureInfluence = 1.0f;  // (was _reserved[4])
  float aoToDiffuse                         = 0.0f;  // (was _reserved[5])

  // §22.3 reserved padding — FRESH budget after the Q1 extension
  // above. Post-Q1 fields consume these slots one at a time, keeping
  // the layout byte-stable across MINOR bumps. Once exhausted,
  // further fields require a major version (§22 + RFC §44.1). The
  // `_reserved` underscore prefix is the §22.3 / §43 plan convention —
  // overrides §30.2's public-POD camelCase rule for this slot purpose
  // only. NOTE for future consumers: when a slot becomes a named
  // field, add it to HashMaterialDesc (Private/GpuScene/Detail.h) —
  // the dedup hash is an explicit field list, not a byte sweep.
  // NOLINTNEXTLINE(readability-identifier-naming)
  uint32_t _reserved[10] = {};
};

}  // namespace pyxis
