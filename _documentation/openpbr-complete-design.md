# Design: complete OpenPBR surface model (`openpbr_material.slang`) with spec-constant feature gates

Owner-directed engineering design note (2026-06-13). Follows the pass-split
(passes-split-design.md, merged #85). Spec reference: OpenPBR v1.1.1
(2026-04-17) — exact formulas extracted and quoted in the implementation.

## Goal

Replace the Lambert+GGX subset in `shading.slang` with the complete OpenPBR
closure stack, in a NEW module `resources/shaders/openpbr_material.slang`
(owner-directed file boundary). `shading.slang` keeps orchestration (hit
reconstruction, texture/normal pipeline, light loop, AO/shadow/reflection
rays) and calls `EvaluateOpenPBR(...)` per light + dome. Feature blocks are
compiled in/out via ONE `[[vk::constant_id]]` uint bitmask (owner-directed:
spec constants, not cbuffer branches), toggled from the ImGui editor and from
Kit's Render Settings (Pyxis section).

## Closure stack (spec v1.1.1, boxed lobe-mixture reduction)

f_PBR = lerp(f_transparent, f_surface, geometry_opacity)
f_surface = F*f_fuzz + lerp(1, 1-E[fuzz_white], F) * f_coated-base
f_coated-base = C*f_coat + lerp(1, T_coat*(1-E[f_coat]), C) * f_base-substrate
L_e (emission) = lerp(1, T_coat, C) * emission_color * emission_luminance
f_base-substrate = lerp(f_dielectric-base, f_conductor(F82), base_metalness)
f_dielectric-base = f^R_specular + (1-E[f^R_specular]) * lerp(lerp(f_diffuse, f_SSS-as-diffuse, S), f^T, T)

Implementation choices (each is a spec-sanctioned approximation, documented
at the code site against the spec section it approximates):
- Diffuse = EON (energy-preserving Oren-Nayar, Fujii) with
  base_diffuse_roughness; analytic A/B + albedo terms. Gate: EON (off =
  plain Lambert).
- Metal = F82-tint Fresnel (Kutz, mu_bar = 1/7) * specular_weight; GGX/Smith
  shared with dielectric. Always on (core correctness, cheap).
- Dielectric specular = EXACT Fresnel F(mu, eta'), not Schlick, with the
  modulated-IOR chain: eta_s = lerp(n_b/n_a, n_b/n_c, C) -> F_s ->
  specular_weight xi clamp [0, 1/F_s] -> eta'. TIR ratio inversion when
  coat_ior > specular_ior. specular_color tints top-side reflection only.
- Coat = GGX layer with coat_color = T_coat^2 semantics, view-dependent
  absorption T^(1/mu_i + 1/mu_o), coat_darkening (default 1!) via
  Delta = (1-K)/(1-E_b K) with the d'Eon E_F(eta) log fit, and the
  always-on roughening r' = lerp(r, min(1, r^4 + 2 r_c^4)^(1/4), C).
  Gate: COAT.
- Fuzz = Zeltner/Burley/Chiang LTC sheen via an analytic polynomial fit of
  the E_fuzz(mu_o, alpha) reflectance table embedded as shader constants
  (deterministic, no LUT texture binding). Gray transmittance to base.
  Gate: FUZZ.
- Transmission: transmission_weight selects the translucent base;
  depth = 0 constant-tint semantics map EXACTLY onto the existing
  front-to-back compositing — per-surface transmittance uses the spec's own
  T_pbr = 1 - alpha(1 - (1-M) T_dielectric) with transmission_color folded
  into T_dielectric. Replaces the current opacity-only formula. Gate:
  TRANSMISSION.
- Subsurface -> diffuse fallback with rho = subsurface_color (sanctioned).
  Gate: SUBSURFACE.
- Anisotropy: normative alpha_t = r^2 sqrt(2/(1+(1-a)^2)), alpha_b =
  (1-a) alpha_t, driven by the MikkTSpace tangent frame. Gate: ANISOTROPY
  (off = alpha = r^2, explicitly endorsed).
- Directional albedos E[f_coat] / E[f^R_specular] / E_ON: analytic GGX
  albedo polynomial fits (Karis/Lazarov-style) — no LUT textures,
  deterministic.
- Thin-film: NOT implemented this phase (weight defaults 0; documented).
- Thin-walled: flag bit honored by the transmission path (ignore-underside
  approximation, sanctioned).
- White-furnace configurations from the spec become new golden fixtures.

## Feature mask

ShaderInterop.slang (dual-language):
  OPENPBR_FEATURE_COAT=1<<0, FUZZ=1<<1, TRANSMISSION=1<<2, SUBSURFACE=1<<3,
  ANISOTROPY=1<<4, EON_DIFFUSE=1<<5; OPENPBR_FEATURES_ALL = all bits.
- `[[vk::constant_id(SPEC_ID_OPENPBR_FEATURES = 4)]] const uint
  OPENPBR_FEATURE_MASK = OPENPBR_FEATURES_ALL;` declared in
  openpbr_material.slang (per-module id space; ids 0-3 taken in the RT
  modules).
- RaytracedLightingPass: variant cache keyed (projectionMode, featureMask) —
  small map + failure-latch set replacing the fixed 2-array; perspective+ALL
  built eagerly; others lazily in `EnsureFeaturePipeline(mask)` on the CPU
  frame path (PyxisRenderer RenderFrame, before the graph). IsOperational
  gains the mask. ReloadShaders drops the cache and rebuilds the active key.
  GBuffer pass is mask-independent (visibility + alpha-test only).

## Parameter plumbing

- `OpenPBRMaterialDesc` (public, MAJOR layout extension — version bump noted
  in PR): wire the parked baseWeight/specularWeight; ADD specularColor[3],
  baseDiffuseRoughness, specularRoughnessAnisotropy, coatColor[3], coatIor,
  coatDarkening, fuzzWeight, fuzzColor[3], fuzzRoughness,
  transmissionColor[3], subsurfaceWeight, subsurfaceColor[3], thinWalled —
  then a FRESH `uint32_t _reserved[16]` tail. ADD the missing
  sizeof/offsetof static_asserts (none exist today). Defaults = OpenPBR
  spec defaults (load-bearing: specular_roughness 0.3 does NOT apply — our
  desc default roughness stays as-is for ingest compat; spec defaults apply
  to NEW fields: coat_ior 1.6, coat_darkening 1, fuzz_roughness 0.5, etc.).
- `HashMaterialDesc` REWRITTEN in the same change: hash every semantic field
  explicitly (fixes the existing un-hashed gap sourcePrim.._reserved AND the
  offsetof landmine; stale comments corrected). Dedup re-bucketing is
  value-identical so images hold.
- `OpenPBRMaterialGPU` 128 B -> 240 B (private interop; new rows 8-14 for
  specularColor+weight, coatColor+coatIor, coatDarkening+fuzz+baseDiffuseRoughness,
  fuzzColor+aniso, transmissionColor+subsurfaceWeight, subsurfaceColor+baseWeight,
  transmissionWeight + reserved tail): static_asserts updated, PackMaterialGpu
  extended.
- MaterialFlag: new bits 18+ (ThinWalled; map-presence bits later), mirror
  kept in lockstep.
- Translators (both adapters share FromUsdShade): MaterialX open_pbr_surface
  wires the full new set incl. coat_color/coat_ior/coat_darkening/fuzz_*/
  transmission_color/subsurface_*/anisotropy; standard_surface maps sheen->
  fuzz and coat_affect_color/roughness -> coat_darkening approximation (spec
  has no coat_affect_*); UsdPreviewSurface keeps clearcoat mapping; MDL
  OmniPBR wires clearcoat_tint/ior/roughness/weight, OmniGlass -> transmission.
  Unmappable inputs warn-skip in the existing StageWalker style.

## Control surface

- RenderSettings (public POD, appended at tail per repo precedent #79):
  `uint32_t openPbrFeatureMask = OPENPBR_FEATURES_ALL;` + NEW
  `uint32_t _reserved[4]` tail (fixing the zero-reserved problem). Layout
  test updated. Headless/Kit defaults = ALL (image-stable for parity).
- ImGui: "OpenPBR Features" CollapsingHeader after the SSAA section
  (EditorPanel.cpp:433), one checkbox per bit; state in ImGuiHost
  (_editorOpenPbr*), accessor GetOpenPbrFeatureMask(), pushed in
  ViewerMode's settings block.
- Omniverse: render_settings.py adds one BOOL row per bit (carb paths under
  the established roots) composing the mask; delegate adds the descriptor +
  per-_Execute poll (type-coerced like ReadPersistEngineSetting); NEW
  `PyxisEngine::SetOpenPbrFeatureMask(uint32_t)` applied where RenderFrame
  builds RenderSettings (PyxisEngine.cpp:251).

## Image policy (deliberate look change)

The complete model changes shading for ALL materials (EON + exact Fresnel +
energy accounting) — this phase is a deliberate visible-change PR. The
golden suite is rebaked ONCE, in its own commit, with a quantified
per-fixture drift report (count, mean/max delta) attached to the PR.
White-furnace fixtures are added as the new physical-correctness net.
Ingest parity (§25.O.3) holds by construction (shared translator).

## Phasing (each a green checkpoint)

- Q1 CPU plumbing: desc extension + hash rewrite + GPU struct + flags +
  translators. Gate: build, 300+ unit tests (new desc layout/hash tests),
  goldens byte-equal (new fields packed, unread).
- Q2 the shader: openpbr_material.slang + shading.slang refactor + spec
  constant + variant cache + renderer wiring. Gate: build/tests, furnace
  sanity renders, quantified golden drift report (NO rebake yet).
- Q3 control surface: RenderSettings field + ImGui section + Omniverse
  wiring. Gate: build/tests, viewer toggle screenshots proving each gate
  visibly changes/restores the image, headless output identical to Q2.
- Q4 verify + rebake: white-furnace fixtures, deliberate golden rebake
  commit, bench (mask=ALL vs P5 baseline), adversarial review, PR.
