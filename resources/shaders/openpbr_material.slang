// Pyxis renderer — the complete OpenPBR surface closure (Q2 of
// _documentation/openpbr-complete-design.md).
//
// Spec reference: OpenPBR Surface v1.1.1 (2026-04-17),
// academysoftwarefoundation.github.io/OpenPBR. Every implementation
// site below cites the spec section (and equation name where the spec
// names one) it implements; deliberate approximations cite
// §"Flexibility of implementation" and state what is approximated.
//
// Module contract (owner-directed boundary):
//   * PURE MATH — no resource declarations, no TraceRay. shading.slang
//     keeps hit reconstruction, the UV/LOD/texture/normal pipeline, the
//     light loop, AO/shadow rays and the deterministic mirror-reflection
//     machinery, and the front-to-back composite contract with
//     raytraced_lighting_raygen.slang. It gathers OpenPBRSurfaceParams
//     ONCE post-texturing and calls the five entry points below:
//       EvaluateOpenPBRDirect    — per-light radiance (× NdotL × light color)
//       EvaluateOpenPBRAmbient   — dome-diffuse response (× INV_PI × AO)
//       OpenPBRReflectionWeight  — deterministic mirror gate weight
//       OpenPBRTransmittance     — per-surface T_pbr for the raygen loop
//       OpenPBREmission          — coat-tinted L_e
//   * Renderer direct-light convention (pre-existing — see the history
//     notes in shading.slang): direct DIFFUSE-like lobes omit the 1/π
//     Lambert normalization (contribution = ρ·NdotL, i.e. the proper
//     BRDF × π) while microfacet SPECULAR lobes are true BRDF values
//     (D·G·F). The dome-ambient path applies INV_PI explicitly. The
//     EON / SSS / fuzz terms below are scaled to that same convention
//     so the all-defaults image reproduces the historical look up to
//     the documented energy-accounting changes; RELATIVE energy ratios
//     between lobes still match the spec.
//
// Feature gates: ONE Vulkan specialization constant (bitmask), folded
// by the driver at pipeline-creation time so disabled blocks compile
// out. CRITICAL INVARIANT: a gate that is OFF produces EXACTLY the
// same float result as that feature's weight parameter == 0 (COAT off
// == coat_weight 0, FUZZ off == fuzz_weight 0, …). Every gated block
// below therefore ALSO branches on `weight > 0` at runtime, with
// energy factors exactly 1.0 and contributions exactly 0.0 in the
// off / zero-weight case — runtime toggles stay semantically clean.

#ifndef PYXIS_OPENPBR_MATERIAL_SLANG
#define PYXIS_OPENPBR_MATERIAL_SLANG

#include "ShaderInterop.slang"

// Q2 — per-pipeline OpenPBR feature bitmask. SPEC_ID_OPENPBR_FEATURES
// = 4 (ids 0-3 are taken in the RT modules; ids are scoped per SPIR-V
// module — see ShaderInterop.slang). C++-fed from RaytracedLightingPass'
// (projectionMode, featureMask) variant cache; the shader-side default
// matches the C++ OPENPBR_FEATURES_ALL per the D2 spec-constant rule.
[[vk::constant_id(SPEC_ID_OPENPBR_FEATURES)]] const uint OPENPBR_FEATURE_MASK = OPENPBR_FEATURES_ALL;

// Compile-time fold: OPENPBR_FEATURE_MASK is a specialization constant,
// so this collapses to true/false at pipeline creation.
bool OpenPBRFeature(uint bit) {
    return (OPENPBR_FEATURE_MASK & bit) != 0u;
}

static const float OPENPBR_PI     = 3.14159265358979323846;
static const float OPENPBR_INV_PI = 0.31830988618379067154;

// The gathered material parameter set — shading.slang fills this ONCE
// after texture decode (baseColor/roughness/metalness post-map) from
// OpenPBRMaterialGPU rows 0-14. Spec parameter names in trailing
// comments (parameter-reference v1.1.1).
struct OpenPBRSurfaceParams {
    float3 baseColor;                   // base_color (post-texture)
    float  baseWeight;                  // base_weight
    float  baseMetalness;               // base_metalness (post-texture)
    float  baseDiffuseRoughness;        // base_diffuse_roughness (EON σ)
    float  specularWeight;              // specular_weight ξ_s ∈ [0,∞)
    float3 specularColor;               // specular_color (top-side tint only)
    float  specularRoughness;           // specular_roughness (post-texture, [0.04,1])
    float  specularRoughnessAnisotropy; // specular_roughness_anisotropy
    float  specularIor;                 // specular_ior
    float  transmissionWeight;          // transmission_weight
    float3 transmissionColor;           // transmission_color (depth-0 constant tint)
    float  subsurfaceWeight;            // subsurface_weight
    float3 subsurfaceColor;             // subsurface_color (diffuse-fallback ρ)
    float  coatWeight;                  // coat_weight
    float3 coatColor;                   // coat_color (= T²_coat at normal incidence)
    float  coatRoughness;               // coat_roughness
    float  coatIor;                     // coat_ior (default 1.6)
    float  coatDarkening;               // coat_darkening (default 1!)
    float  fuzzWeight;                  // fuzz_weight
    float3 fuzzColor;                   // fuzz_color
    float  fuzzRoughness;               // fuzz_roughness (Zeltner α, used directly)
    float3 emission;                    // emission_color × emission_luminance × map
    float  geometryOpacity;             // geometry_opacity α
    bool   thinWalled;                  // geometry_thin_walled
    float3 tangentWorld;                // MikkTSpace tangent ⟂ shading normal; 0 = unauthored
    float3 bitangentWorld;              // handedness-corrected bitangent; 0 = unauthored
};

// ---------------------------------------------------------------------------
// Fresnel
// ---------------------------------------------------------------------------

// Exact unpolarized dielectric Fresnel F(μ, η) — spec §"Base Substrate"
// (dielectric base): "the reflection lobe of the dielectric is
// modulated by the EXACT dielectric Fresnel factor" (not Schlick).
// η = n_transmitted / n_incident; η == 1 → index-matched, F = 0;
// g² < 0 → total internal reflection, F = 1.
float FresnelDielectricExact(float mu, float eta) {
    const float c  = saturate(mu);
    const float g2 = eta * eta - 1.0 + c * c;
    if (g2 <= 0.0)
        return 1.0;  // TIR
    const float g = sqrt(g2);
    const float a = (g - c) / (g + c);
    const float b = (c * (g + c) - 1.0) / (c * (g - c) + 1.0);
    return 0.5 * a * a * (1.0 + b * b);
}

// (1-μ)⁵ with explicit multiplies — same float-op shape the pre-Q2
// SchlickFresnel used, so the metal-at-defaults path reproduces it.
float OpenPBRPow5(float x) {
    return x * x * x * x * x;
}

// Modulated-IOR chain — spec §"Coat" eq. `specular_ior_ratio` (+ the
// TIR ratio-inversion fix of eq. `specular_ior_ratio_with_tir_fix`,
// Kutz 2021) and §"Base Substrate" eq. `modulated_ior`:
//   η_bc = n_b/n_c, INVERTED to n_c/n_b when n_c > n_b (TIR fix)
//   η_s  = lerp(n_b/n_a, η_bc, C)            (n_a = 1, ambient medium)
//   F_s  = ((1 − η_s)/(1 + η_s))²            (eq. `unmodulated_fresnel`)
//   ξ_s clamped to [0, 1/F_s]; ε = sgn(η_s − 1)·sqrt(ξ_s F_s)
//   η'_s = (1 + ε)/(1 − ε)
// Returns η'_s; `xiFsOut` is the clamped ξ_s·F_s product (= the
// modulated normal-incidence reflectance, reused by the coat-darkening
// r_d term). ξ_s = 0 → η' = 1 → F(μ, 1) = 0 exactly: the specular lobe
// dies, which is the spec's index-matched semantics AND the zero-weight
// short-circuit invariant.
float OpenPBRModulatedIor(float specularIor, float specularWeight,
                          float coatIor, float coatWeight,
                          out float xiFsOut) {
    const float nB = max(specularIor, 1e-2);
    const float nC = max(coatIor, 1e-2);
    // TIR fix: invert the coat-base ratio when the coat is denser.
    const float etaBc = (nC > nB) ? (nC / nB) : (nB / nC);
    const float etaS  = lerp(nB, etaBc, coatWeight);  // n_a = 1
    const float fsBase = (1.0 - etaS) / (1.0 + etaS);
    const float Fs = fsBase * fsBase;
    // ξ_s ∈ [0, 1/F_s] (weights > 1 may raise reflectivity); the
    // product is additionally kept just below 1 so η' stays finite.
    const float xiFs = (Fs > 1e-8) ? min(specularWeight * Fs, 0.99999) : 0.0;
    xiFsOut = xiFs;
    const float epsSign = (etaS >= 1.0) ? 1.0 : -1.0;
    const float eps = epsSign * sqrt(xiFs);
    return (1.0 + eps) / (1.0 - eps);
}

// F82-tint conductor Fresnel — spec §"Base Substrate / Metal" (Kutz
// 2021 extending Hoffman 2019), quoted exactly:
//   F_Schlick(μ) = F₀ + (1 − F₀)(1 − μ)⁵          with F₀ = base_weight·base_color
//   F_82(μ) = F_Schlick(μ) − [μ(1−μ)⁶ / (μ̄(1−μ̄)⁶)] (F_Schlick(μ̄) − F(μ̄))
//   where μ̄ = 1/7 and F(μ̄) = specular_color · F_Schlick(μ̄)
//   F_metal(μ) = specular_weight · F_82(μ)
// Reduces to plain Schlick at specular_color = white, specular_weight
// = 1 (the spec-stated property) — byte-matching the pre-Q2 metal path.
float3 OpenPBRConductorFresnel(float mu, float3 baseColor, float baseWeight,
                               float3 specularColor, float specularWeight) {
    const float3 F0 = saturate(baseColor * baseWeight);
    const float  m  = max(0.0, mu);
    const float  oneMinus  = 1.0 - m;
    const float3 schlick   = F0 + (float3(1.0, 1.0, 1.0) - F0) * OpenPBRPow5(oneMinus);
    const float  muBar     = 1.0 / 7.0;
    const float3 schlickBar = F0 + (float3(1.0, 1.0, 1.0) - F0) * OpenPBRPow5(1.0 - muBar);
    const float  oneMinus6 = OpenPBRPow5(oneMinus) * oneMinus;
    const float  denomBar  = muBar * OpenPBRPow5(1.0 - muBar) * (1.0 - muBar);
    const float  lobe      = m * oneMinus6 / denomBar;
    // F_Schlick(μ̄) − specular_color·F_Schlick(μ̄) = F_Schlick(μ̄)(1 − specular_color)
    const float3 f82 = schlick - lobe * schlickBar * (float3(1.0, 1.0, 1.0) - saturate(specularColor));
    return max(0.0, specularWeight) * max(f82, float3(0.0, 0.0, 0.0));
}

// ---------------------------------------------------------------------------
// GGX microfacet — NDF + height-correlated Smith (Heitz 2014).
// ---------------------------------------------------------------------------

// Isotropic D·G2 — float-op-identical to the pre-Q2 GgxSpecular body
// (minus the Fresnel factor, applied by the caller) so the
// all-defaults specular drift is Fresnel-only. α = r² (Disney remap,
// spec §"Microfacet model": "α = r², following Burley").
float GgxSpecularDV(float NdotL, float NdotV, float NdotH, float roughness) {
    const float alpha  = max(0.001, roughness * roughness);
    const float alpha2 = alpha * alpha;
    const float denomD = NdotH * NdotH * (alpha2 - 1.0) + 1.0;
    const float D      = alpha2 / (OPENPBR_PI * denomD * denomD);
    const float lambdaV = NdotL * sqrt(NdotV * NdotV * (1.0 - alpha2) + alpha2);
    const float lambdaL = NdotV * sqrt(NdotL * NdotL * (1.0 - alpha2) + alpha2);
    const float G2      = 0.5 / (lambdaV + lambdaL);
    return D * G2;  // (NdotL × NdotV) cancels inside G2's denominator
}

// Anisotropic D·G2 (Heitz 2014; Filament formulation). Spec
// §"Microfacet model" eq. `openpbr-anisotropy-formula` is NORMATIVE:
//   α_t = r² sqrt(2/(1 + (1 − a)²)),  α_b = (1 − a) α_t
// ("This formulation satisfies α_t² + α_b² = 2α² ... to preserve the
// average roughness regardless of the anisotropy.") — with the
// isotropic α = r², α_t² + α_b² = α_t²(1 + (1 − a)²) = 2r⁴ = 2α², so
// the identity is in α², not α⁴. The isotropic
// fallback α = r² is explicitly endorsed by the spec — the ANISOTROPY
// gate (and a == 0 / missing tangent frame) takes the isotropic path.
float GgxSpecularDVAniso(float3 N, float3 V, float3 L, float3 H,
                         float3 T, float3 B,
                         float roughness, float anisotropy) {
    const float r2 = max(0.001, roughness * roughness);
    const float oneMinusA = 1.0 - saturate(anisotropy);
    float alphaT = r2 * sqrt(2.0 / (1.0 + oneMinusA * oneMinusA));
    float alphaB = oneMinusA * alphaT;
    alphaT = clamp(alphaT, 0.001, 1.0);  // spec: α restricted to [0,1]
    alphaB = clamp(alphaB, 0.001, 1.0);
    const float NdotL = max(1e-4, dot(N, L));
    const float NdotV = max(1e-4, dot(N, V));
    const float NdotH = max(1e-4, dot(N, H));
    const float TdotH = dot(T, H);
    const float BdotH = dot(B, H);
    const float TdotV = dot(T, V);
    const float BdotV = dot(B, V);
    const float TdotL = dot(T, L);
    const float BdotL = dot(B, L);
    // Anisotropic GGX NDF.
    const float a2 = alphaT * alphaB;
    const float3 d = float3(alphaB * TdotH, alphaT * BdotH, a2 * NdotH);
    const float d2 = max(1e-12, dot(d, d));
    const float b2 = a2 / d2;
    const float D  = a2 * b2 * b2 * OPENPBR_INV_PI;
    // Height-correlated Smith visibility (includes 1/(4 NdotL NdotV)).
    const float lambdaV = NdotL * length(float3(alphaT * TdotV, alphaB * BdotV, NdotV));
    const float lambdaL = NdotV * length(float3(alphaT * TdotL, alphaB * BdotL, NdotL));
    const float G2 = 0.5 / max(1e-8, lambdaV + lambdaL);
    return D * G2;
}

// Analytic GGX directional albedo E[f_spec](μ_o) = F₀·A + B — the
// Karis/Lazarov env-BRDF polynomial fit ("Getting More Physical in
// Call of Duty: Black Ops II", Lazarov 2013; popularized as the UE4
// mobile EnvBRDFApprox in Karis, "Real Shading in Unreal Engine 4").
// Replaces the directional-albedo LUT the spec's albedo-scaling
// layering (§"Slabs" eq. `directional-reflectance-definition`,
// §"Formalism / Layering" eq. `non-reciprocal-albedo-scaling`)
// requires — deterministic, no texture binding. Fit error vs the
// tabulated split-sum integral is within a few percent over
// (μ, r) ∈ [0,1]²; input roughness is the PERCEPTUAL r (α = r², the
// same convention the fit was built for).
float2 GgxDirectionalAlbedoAB(float NdotV, float roughness) {
    const float4 c0 = float4(-1.0, -0.0275, -0.572, 0.022);
    const float4 c1 = float4(1.0, 0.0425, 1.04, -0.04);
    const float4 r  = roughness * c0 + c1;
    const float a004 = min(r.x * r.x, exp2(-9.28 * NdotV)) * r.x + r.y;
    return float2(-1.04, 1.04) * a004 + float2(r.z, r.w);
}

// ---------------------------------------------------------------------------
// EON diffuse — spec §"Base Substrate / Glossy-diffuse", eq. `EON_brdf`
// / `FON_brdf` (energy-preserving Oren-Nayar, Fujii form):
//   f_ON(ω_i, ω_o) = (w_d ρ/π)(A(σ) + B(σ)·s/t)
//   A = 1/(1 + (1/2 − 2/(3π))σ),  B = σA
//   s = ω_i·ω_o − (n·ω_i)(n·ω_o);  1/t = 1 if s ≤ 0 else 1/max(μ_i, μ_o)
// plus the energy-compensation lobe eq. `EON_comp`:
//   f^comp = (w_d ρ_ms/π)(1 − Ê(μ_i))(1 − Ê(μ_o))
//   ρ_ms = ρ²·[⟨Ê⟩/(1 − ⟨Ê⟩)] / [1 − ρ(1 − ⟨Ê⟩)]
// Ê is the (white-albedo) directional albedo of f_ON — CLOSED FORM for
// the Fujii kernel (EON paper, Portsmouth et al. 2024):
//   Ê(μ) = A + (B/π)·G(μ)
//   G(μ) = Si(acos μ − Si μ) + (2/3)((Si/μ)(1 − Si³) − Si),  Si = √(1−μ²)
// ⟨Ê⟩ = A + B·EON_AVG_G with EON_AVG_G = (2/π)∫₀¹ G(μ)μ dμ — evaluated
// once offline (composite Simpson, n = 2·10⁶; value 0.0724882124569,
// integral 2∫Gμdμ = 0.2277284357) and embedded as a constant.
// White-furnace check (built into the algebra): at ρ = 1,
// ρ_ms = 1/(1 − ⟨Ê⟩) and E_total(μ_o) = Ê(μ_o) + (1 − Ê(μ_o)) = 1.
// σ = 0 → A = 1, B = 0, Ê ≡ 1, comp ≡ 0 → exact Lambert (the
// EON_DIFFUSE-off path), satisfying the gate invariant.
// ---------------------------------------------------------------------------

static const float EON_AVG_G = 0.0724882124569;

float EonA(float sigma) {
    return 1.0 / (1.0 + (0.5 - 2.0 / (3.0 * OPENPBR_PI)) * sigma);
}

float EonG(float mu) {
    const float m  = clamp(mu, 1e-4, 1.0);
    const float si = sqrt(saturate(1.0 - m * m));
    return si * (acos(m) - si * m)
         + (2.0 / 3.0) * ((si / m) * (1.0 - si * si * si) - si);
}

// White directional albedo Ê_ON(μ) of the Fujii lobe.
float EonDirectionalAlbedo(float mu, float sigma) {
    const float A = EonA(sigma);
    const float B = sigma * A;
    return A + (B * OPENPBR_INV_PI) * EonG(mu);
}

// Multi-scatter albedo ρ_ms/ρ-free form (per channel).
float3 EonRhoMs(float3 rho, float sigma) {
    const float A = EonA(sigma);
    const float B = sigma * A;
    const float avgE = A + B * EON_AVG_G;
    const float oneMinusAvg = saturate(1.0 - avgE);
    return rho * rho * (avgE / max(1e-4, 1.0 - avgE))
         / max(float3(1e-4, 1e-4, 1e-4),
               float3(1.0, 1.0, 1.0) - rho * oneMinusAvg);
}

// Direct-light diffuse term in the renderer's π-boosted convention
// (× π vs the spec BRDF; see the module header). Returns the
// per-channel factor that multiplies NdotL × lightColor.
float3 EonDiffuseDirect(float3 rho, float sigma, float3 N, float3 V, float3 L) {
    if (!OpenPBRFeature(OPENPBR_FEATURE_EON_DIFFUSE) || sigma <= 0.0)
        return rho;  // exact Lambert — gate invariant (σ = 0 ≡ gate off)
    const float NdotL = max(1e-4, dot(N, L));
    const float NdotV = max(1e-4, dot(N, V));
    const float A = EonA(sigma);
    const float B = sigma * A;
    const float s = dot(V, L) - NdotL * NdotV;             // footnote [^Oren_Nayar_formula]
    const float stinv = (s <= 0.0) ? s : s / max(NdotL, NdotV);
    const float3 fOn = rho * (A + B * stinv);
    const float3 comp = EonRhoMs(rho, sigma)
                      * saturate(1.0 - EonDirectionalAlbedo(NdotL, sigma))
                      * saturate(1.0 - EonDirectionalAlbedo(NdotV, sigma));
    return max(float3(0.0, 0.0, 0.0), fOn + comp);
}

// Hemispherical (ambient) effective albedo replacing ρ in the dome
// term: ρ·Ê(μ_o) + ρ_ms·(1 − Ê(μ_o))(1 − ⟨Ê⟩).
float3 EonDiffuseAmbientAlbedo(float3 rho, float sigma, float NdotV) {
    if (!OpenPBRFeature(OPENPBR_FEATURE_EON_DIFFUSE) || sigma <= 0.0)
        return rho;  // exact Lambert — gate invariant
    const float A = EonA(sigma);
    const float B = sigma * A;
    const float avgE = A + B * EON_AVG_G;
    const float eMuO = EonDirectionalAlbedo(max(1e-4, NdotV), sigma);
    return max(float3(0.0, 0.0, 0.0),
               rho * eMuO + EonRhoMs(rho, sigma) * saturate(1.0 - eMuO)
                                                 * saturate(1.0 - avgE));
}

// ---------------------------------------------------------------------------
// Fuzz — spec §"Fuzz": Zeltner/Burley/Chiang 2022 LTC sheen,
//   μ_i f_fuzz(ω_i, ω_o) = F·E_fuzz(μ_o, α)·D(μ_i | μ_o, α)
// with F = fuzz_color, α = fuzz_roughness USED DIRECTLY (no remap),
// D a normalized lobe — "so the fuzz albedo is exactly F·E_fuzz(μ_o, α)".
//
// E_fuzz(μ_o, α): analytic Gaussian fit to the Zeltner directional-
// albedo (R) table, taken VERBATIM from MaterialX
// libraries/pbrlib/genglsl/lib/mx_microfacet_sheen.glsl
// (`mx_zeltner_sheen_dir_albedo`, "adapted from
// github.com/tizian/ltc-sheen" — the paper's own reference fit data);
// MaterialX calls it with (NdotV, roughness clamped to [0.01, 1]),
// reproduced here. Deterministic — no LUT texture binding.
//
// Lobe-shape LOD (documented approximation, sanctioned by
// §"Flexibility of implementation"): the skewed LTC lobe D is replaced
// by a COSINE lobe carrying the identical directional albedo, i.e.
// f_fuzz = F·E_fuzz(μ_o, α)/π. The energy semantics the spec pins down
// stay exact: per-view albedo F·E_fuzz, WHITE-albedo layering factor
// (eq. `fuzz-layering-approx`: F_w·f_fuzz + (1 − F_w·E_fuzz)·f_base)
// and GRAY transmittance to the base (base never tinted by fuzz color).
// What is lost is only the LTC lobe's angular skew (back-scattering
// asymmetry), not its energy.
// ---------------------------------------------------------------------------

float FuzzDirectionalAlbedo(float mu, float fuzzRoughness) {
    const float x = saturate(mu);
    const float y = clamp(fuzzRoughness, 0.01, 1.0);
    const float s = y * (0.0206607 + 1.58491 * y) / (0.0379424 + y * (1.32227 + y));
    const float m = y * (-0.193854 + y * (-1.14885 + y * (1.7932 - 0.95943 * y * y)))
                  / (0.046391 + y);
    const float o = y * (0.000654023 + (-0.0207818 + 0.119681 * y) * y)
                  / (1.26264 + y * (-1.92021 + y));
    const float xm = (x - m) / s;
    return saturate(exp(-0.5 * xm * xm) / (s * sqrt(2.0 * OPENPBR_PI)) + o);
}

// ---------------------------------------------------------------------------
// Coat helpers
// ---------------------------------------------------------------------------

// Hemispherical Fresnel albedo E_F(η) — the d'Eon analytic log fit the
// spec quotes (footnote [^avg_fresnel], ±0.2% for η ∈ [1, 3]):
//   E_F(η) ≈ ln((10893η − 1438.2)/(−774.4η² + 10212η + 1))
//   E_F(η) = 1 − η²(1 − E_F(1/η))   for η < 1
float AvgFresnelDEon(float eta) {
    float e = eta;
    bool inverted = false;
    if (e < 1.0) {
        e = 1.0 / e;
        inverted = true;
    }
    e = clamp(e, 1.0, 3.0);  // fit validity range
    const float ef = log((10893.0 * e - 1438.2) / (-774.4 * e * e + 10212.0 * e + 1.0));
    if (inverted) {
        const float etaC = clamp(eta, 1.0 / 3.0, 1.0);
        return 1.0 - etaC * etaC * (1.0 - ef);
    }
    return ef;
}

// View-dependent coat absorption — spec §"Coat / View-dependent
// absorption": the base lobe is multiplied by
//   T_coat^(1/μ^t_i + 1/μ^t_o),  μ^t = sqrt(1 − (1 − μ²)/η_c²)
// "reducing to T²_coat = coat_color at normal incidence" — i.e. the
// single-pass transmittance T_coat = sqrt(coat_color), so the factor
// is coat_color^(0.5·(1/μ^t_i + 1/μ^t_o)). White coat_color stays
// exactly 1 at every angle (white-furnace safe).
float3 CoatViewTransmittance(float3 coatColor, float muI, float muO, float etaC) {
    const float inv2 = 1.0 / max(1e-2, etaC * etaC);
    const float muTi = sqrt(saturate(1.0 - (1.0 - muI * muI) * inv2));
    const float muTo = sqrt(saturate(1.0 - (1.0 - muO * muO) * inv2));
    const float expnt = 0.5 * (1.0 / max(1e-2, muTi) + 1.0 / max(1e-2, muTo));
    return pow(max(coatColor, float3(0.0, 0.0, 0.0)), float3(expnt, expnt, expnt));
}

// Modulated coat darkening — spec §"Coat / Darkening":
//   Δ = (1 − K)/(1 − E_b K),  K = lerp(K_s, K_r, r_b)
//   K_s = F(ω_o, η_c)                          (smooth base)
//   K_r = 1 − (1 − E_F(η_c))/η_c²              (rough base,
//          eq. internal_diffuse_reflection_coefficient_for_rough_base)
//   r_b = lerp(r_d, r_m, M); r_d = lerp(1, r, ξ_s F_s ∈ [0,1]); r_m = r
//   applied as the modulated factor lerp(1, Δ, C·δ)
//          (eq. `modulated_darkening_factor`, δ = coat_darkening)
// E_b = normal-incidence albedo of the whole base, "approximated by
// blending the slabs' normal-incidence albedos by their mix weights".
float3 CoatDarkeningFactor(OpenPBRSurfaceParams p, float NdotV,
                           float xiFs, float3 rhoDiffuse, float diffuseSelector) {
    const float cd = p.coatWeight * p.coatDarkening;
    if (cd <= 0.0)
        return float3(1.0, 1.0, 1.0);  // zero-weight short-circuit (exactly 1)
    const float etaC = max(1e-2, p.coatIor);
    const float EF = AvgFresnelDEon(etaC);
    const float Kr = saturate(1.0 - (1.0 - EF) / (etaC * etaC));
    const float Ks = FresnelDielectricExact(NdotV, etaC);
    const float r  = p.specularRoughness;
    const float rd = lerp(1.0, r, saturate(xiFs));
    const float rb = lerp(rd, r, p.baseMetalness);
    const float K  = saturate(lerp(Ks, Kr, rb));
    // E_b: dielectric part = modulated normal reflectance (tinted by
    // specular_color, top-side) + body albedo (diffuse mix, killed by
    // transmission); metal part = ξ·F₀ = specular_weight·base_weight·base_color.
    const float3 ebDielectric = saturate(
        xiFs * saturate(p.specularColor)
        + (1.0 - xiFs) * saturate(rhoDiffuse) * diffuseSelector);
    const float3 ebMetal = saturate(p.specularWeight * p.baseWeight * p.baseColor);
    const float3 Eb = clamp(lerp(ebDielectric, ebMetal, p.baseMetalness),
                            float3(0.0, 0.0, 0.0), float3(0.99, 0.99, 0.99));
    const float3 delta = (1.0 - K) / max(float3(1e-3, 1e-3, 1e-3),
                                         float3(1.0, 1.0, 1.0) - Eb * K);
    return lerp(float3(1.0, 1.0, 1.0), delta, cd);
}

// Coat-roughened base specular roughness — spec §"Coat / Roughening"
// (always-on, parameter-free):
//   r′_B = lerp(r_B, min(1, r⁴_B + 2 r⁴_C)^(1/4), C)
float OpenPBRCoatRoughenedRoughness(OpenPBRSurfaceParams p) {
    const float r = p.specularRoughness;
    if (!OpenPBRFeature(OPENPBR_FEATURE_COAT) || p.coatWeight <= 0.0)
        return r;  // gate invariant: COAT off ≡ coat_weight 0
    const float r4  = r * r * r * r;
    const float rc  = p.coatRoughness;
    const float rc4 = rc * rc * rc * rc;
    const float roughened = pow(min(1.0, r4 + 2.0 * rc4), 0.25);
    return lerp(r, roughened, p.coatWeight);
}

// ---------------------------------------------------------------------------
// Shared sub-evaluations
// ---------------------------------------------------------------------------

// ρ of the diffuse-ish base lobe: f^T_dielectric-base mixes
// lerp(f_diffuse, f_SSS, S) (boxed closure) — the SSS slab falls back
// to a diffuse lobe with ρ = subsurface_color (explicitly authorized by
// §"Flexibility of implementation": "the subsurface color might be
// used as the albedo of a diffuse BRDF"). base_weight w_d applies to
// f_diffuse only (eq. `EON_brdf`).
float3 OpenPBRDiffuseRho(OpenPBRSurfaceParams p) {
    const float3 rhoBase = saturate(p.baseColor * p.baseWeight);
    if (!OpenPBRFeature(OPENPBR_FEATURE_SUBSURFACE) || p.subsurfaceWeight <= 0.0)
        return rhoBase;  // gate invariant: SUBSURFACE off ≡ subsurface_weight 0
    return lerp(rhoBase, saturate(p.subsurfaceColor), p.subsurfaceWeight);
}

// (1 − T): the translucent-base selector that removes the diffuse lobe
// when transmission takes over (boxed closure:
// f^T_dielectric-base = lerp(lerp(f_diffuse, f_SSS, S), f^T, T)).
float OpenPBRDiffuseSelector(OpenPBRSurfaceParams p) {
    if (!OpenPBRFeature(OPENPBR_FEATURE_TRANSMISSION) || p.transmissionWeight <= 0.0)
        return 1.0;  // gate invariant: TRANSMISSION off ≡ transmission_weight 0
    return 1.0 - saturate(p.transmissionWeight);
}

// Fuzz layering throughput at μ_o (eq. `fuzz-layering-approx`):
// 1 − F_w·E_fuzz(μ_o, α). White-albedo E_fuzz → gray transmittance.
float OpenPBRFuzzThroughput(OpenPBRSurfaceParams p, float NdotV) {
    if (!OpenPBRFeature(OPENPBR_FEATURE_FUZZ) || p.fuzzWeight <= 0.0)
        return 1.0;  // gate invariant: FUZZ off ≡ fuzz_weight 0
    return saturate(1.0 - p.fuzzWeight * FuzzDirectionalAlbedo(NdotV, p.fuzzRoughness));
}

// ---------------------------------------------------------------------------
// Entry points
// ---------------------------------------------------------------------------

// Per-light evaluation. Returns the full layered closure × NdotL ×
// lightColor; the caller multiplies the scalar light attenuation
// (distance falloff, cone, shadow visibility). Layering follows the
// spec's boxed lobe-mixture reduction (§"Reduction to a mixture of
// lobes / Non-thin-walled mode") EXACTLY:
//   f_surface         = F·f_fuzz + lerp(1, 1 − E[f̄_fuzz], F)·f_coated-base
//   f_coated-base     = C·f_coat + lerp(1, T_coat(1 − E[f_coat]), C)·f_base-substrate
//   f_base-substrate  = lerp(f_dielectric-base, f_conductor, M)
//   f_dielectric-base = f^R_specular + (1 − E[f^R_specular])·lerp(lerp(f_diffuse, f_SSS, S), f^T, T)
// (the f^T transmission lobe is NOT part of the local radiance — it is
// reported to the raygen's front-to-back composite via
// OpenPBRTransmittance; its Fresnel transmission factor may be omitted
// per the spec's albedo-scaling note). Coat darkening multiplies the
// base substrate (spec recommendation); coat tints base — never its
// own reflection; specular_color tints the top-side dielectric
// reflection only.
float3 EvaluateOpenPBRDirect(OpenPBRSurfaceParams p, float3 N, float3 V, float3 L,
                             float3 lightColor) {
    const float NdotL = dot(N, L);
    if (NdotL <= 0.0)
        return float3(0.0, 0.0, 0.0);
    const float NdotV = max(1e-4, dot(N, V));
    const float3 H = normalize(V + L);
    const float NdotH = max(1e-4, dot(N, H));
    const float VdotH = max(1e-4, dot(V, H));

    // Modulated-IOR chain + coat-roughened base roughness.
    float xiFs;
    const float etaPrime = OpenPBRModulatedIor(p.specularIor, p.specularWeight,
                                               p.coatIor,
                                               OpenPBRFeature(OPENPBR_FEATURE_COAT)
                                                   ? saturate(p.coatWeight) : 0.0,
                                               xiFs);
    const float rPrime = OpenPBRCoatRoughenedRoughness(p);

    // --- Base substrate: shared GGX D·G2 (dielectric + metal share the
    // NDF/G per spec §"Base Substrate / Metal") -----------------------
    float specDV;
    const bool useAniso = OpenPBRFeature(OPENPBR_FEATURE_ANISOTROPY)
                          && p.specularRoughnessAnisotropy > 0.0
                          && dot(p.tangentWorld, p.tangentWorld) > 1e-8;
    if (useAniso) {
        specDV = GgxSpecularDVAniso(N, V, L, H, p.tangentWorld, p.bitangentWorld,
                                    rPrime, p.specularRoughnessAnisotropy);
    } else {
        // Isotropic α = r′² — the spec-endorsed fallback; also the exact
        // a == 0 result (gate invariant).
        specDV = GgxSpecularDV(max(1e-4, NdotL), NdotV, NdotH, rPrime);
    }
    // Dielectric reflection: EXACT Fresnel at η′, tinted by
    // specular_color (top-side initial reflection only — spec §"Base
    // Substrate", property list).
    const float3 fSpecDielectric =
        specDV * saturate(p.specularColor) * FresnelDielectricExact(VdotH, etaPrime);
    // Conductor: F82-tint (×specular_weight inside).
    const float3 fSpecMetal =
        specDV * OpenPBRConductorFresnel(VdotH, p.baseColor, p.baseWeight,
                                         p.specularColor, p.specularWeight);

    // Diffuse / SSS under the dielectric reflection lobe, scaled by the
    // albedo-scaling factor (1 − E[f^R_specular](μ_o)) (boxed closure;
    // E via the analytic GGX albedo fit, F₀ = ξ_s F_s · specular_color).
    const float3 rhoDiffuse = OpenPBRDiffuseRho(p);
    const float  diffuseSelector = OpenPBRDiffuseSelector(p);
    const float2 ab = GgxDirectionalAlbedoAB(NdotV, rPrime);
    const float3 eSpec = saturate(xiFs * saturate(p.specularColor) * ab.x + ab.y);
    const float3 fDiffuse = EonDiffuseDirect(rhoDiffuse, saturate(p.baseDiffuseRoughness), N, V, L)
                          * diffuseSelector
                          * saturate(float3(1.0, 1.0, 1.0) - eSpec);

    // f_base-substrate = lerp(dielectric-base, conductor, M). (The
    // dielectric diffuse rides under (1 − M) — metals have no diffuse.)
    const float M = saturate(p.baseMetalness);
    float3 fBase = lerp(fSpecDielectric + fDiffuse, fSpecMetal, M);

    // --- Coat layer (gate COAT; off ≡ coat_weight 0) ------------------
    float3 result = fBase;
    if (OpenPBRFeature(OPENPBR_FEATURE_COAT) && p.coatWeight > 0.0) {
        const float C = saturate(p.coatWeight);
        const float etaC = max(1e-2, p.coatIor);
        // Coat GGX lobe — untinted (coat never tints its own
        // reflection, spec §"Coat").
        const float coatDV = GgxSpecularDV(max(1e-4, NdotL), NdotV, NdotH,
                                           max(0.04, p.coatRoughness));
        const float3 fCoat = float3(1.0, 1.0, 1.0)
                           * (coatDV * FresnelDielectricExact(VdotH, etaC));
        // E[f_coat](μ_o) for the albedo-scaling factor.
        const float f0Base = (etaC - 1.0) / (etaC + 1.0);
        const float f0Coat = f0Base * f0Base;
        const float2 abc = GgxDirectionalAlbedoAB(NdotV, max(0.04, p.coatRoughness));
        const float eCoat = saturate(f0Coat * abc.x + abc.y);
        // View-dependent absorption T_coat^(1/μ_i + 1/μ_o) + modulated
        // darkening lerp(1, Δ, C·δ) on the base substrate.
        const float3 tCoat = CoatViewTransmittance(saturate(p.coatColor),
                                                   saturate(NdotL), NdotV, etaC);
        const float3 darkening = CoatDarkeningFactor(p, NdotV, xiFs,
                                                     rhoDiffuse, diffuseSelector);
        result = C * fCoat
               + lerp(float3(1.0, 1.0, 1.0), tCoat * (1.0 - eCoat), C)
                     * (fBase * darkening);
    }

    // --- Fuzz layer (gate FUZZ; off ≡ fuzz_weight 0) ------------------
    if (OpenPBRFeature(OPENPBR_FEATURE_FUZZ) && p.fuzzWeight > 0.0) {
        const float Fw = saturate(p.fuzzWeight);
        const float eFuzz = FuzzDirectionalAlbedo(NdotV, p.fuzzRoughness);
        // Cosine-lobe LOD of the LTC sheen in the renderer's π-boosted
        // diffuse convention: f_fuzz·π = fuzz_color·E_fuzz(μ_o, α).
        const float3 fFuzz = saturate(p.fuzzColor) * eFuzz;
        result = Fw * fFuzz + (1.0 - Fw * eFuzz) * result;
    }

    return result * NdotL * lightColor;
}

// Dome-diffuse response (the "fast diffuse IBL" path). `domeRadiance`
// is the caller's light-color × mip-biased dome sample; this applies
// the Lambert 1/π normalization the pre-Q2 path applied, the AO
// visibility, and the same energy chain as the direct path evaluated
// at μ_i = 1 (hemispherical-irradiance proxy — deterministic, one
// sample, per §"Flexibility of implementation"). Metals contribute no
// diffuse (boxed closure: the conductor slab is opaque); the coat's
// own specular-ambient lobe is deliberately NOT added — there is no
// specular IBL in this renderer yet (pre-existing status quo; the
// deterministic mirror branch covers specular reflection).
float3 EvaluateOpenPBRAmbient(OpenPBRSurfaceParams p, float3 N, float3 V,
                              float3 domeRadiance, float aoVisibility) {
    const float NdotV = max(1e-4, dot(N, V));
    const float rPrime = OpenPBRCoatRoughenedRoughness(p);
    float xiFs;
    OpenPBRModulatedIor(p.specularIor, p.specularWeight, p.coatIor,
                        OpenPBRFeature(OPENPBR_FEATURE_COAT)
                            ? saturate(p.coatWeight) : 0.0,
                        xiFs);

    const float3 rhoDiffuse = OpenPBRDiffuseRho(p);
    const float  diffuseSelector = OpenPBRDiffuseSelector(p);
    const float2 ab = GgxDirectionalAlbedoAB(NdotV, rPrime);
    const float3 eSpec = saturate(xiFs * saturate(p.specularColor) * ab.x + ab.y);
    float3 ambient = EonDiffuseAmbientAlbedo(rhoDiffuse,
                                             saturate(p.baseDiffuseRoughness), NdotV)
                   * diffuseSelector
                   * saturate(float3(1.0, 1.0, 1.0) - eSpec)
                   * (1.0 - saturate(p.baseMetalness));

    if (OpenPBRFeature(OPENPBR_FEATURE_COAT) && p.coatWeight > 0.0) {
        const float C = saturate(p.coatWeight);
        const float etaC = max(1e-2, p.coatIor);
        const float f0Base = (etaC - 1.0) / (etaC + 1.0);
        const float f0Coat = f0Base * f0Base;
        const float2 abc = GgxDirectionalAlbedoAB(NdotV, max(0.04, p.coatRoughness));
        const float eCoat = saturate(f0Coat * abc.x + abc.y);
        const float3 tCoat = CoatViewTransmittance(saturate(p.coatColor),
                                                   1.0, NdotV, etaC);
        const float3 darkening = CoatDarkeningFactor(p, NdotV, xiFs,
                                                     rhoDiffuse, diffuseSelector);
        ambient = lerp(float3(1.0, 1.0, 1.0), tCoat * (1.0 - eCoat), C)
                * (ambient * darkening);
    }

    if (OpenPBRFeature(OPENPBR_FEATURE_FUZZ) && p.fuzzWeight > 0.0) {
        const float Fw = saturate(p.fuzzWeight);
        const float eFuzz = FuzzDirectionalAlbedo(NdotV, p.fuzzRoughness);
        ambient = Fw * saturate(p.fuzzColor) * eFuzz + (1.0 - Fw * eFuzz) * ambient;
    }

    return ambient * domeRadiance * OPENPBR_INV_PI * aoVisibility;
}

// Deterministic mirror-reflection weight — the Fresnel-at-N ×
// glossy-fade gate weight the reflection branch layers on top
// (shading.slang keeps the mirror TraceRay machinery). The layering
// mirrors the boxed closure applied to the single mirror sample:
//   coat mirror (untinted) over the coat-attenuated, darkened base
//   mirror (dielectric exact-Fresnel·specular_color vs metal F82,
//   blended by metalness), everything under the fuzz throughput.
// The (1 − r)² glossy fade is the pre-Q2 deterministic stand-in for
// the missing rough-reflection blur, now evaluated at the
// COAT-ROUGHENED r′ (spec §"Coat / Roughening").
float3 OpenPBRReflectionWeight(OpenPBRSurfaceParams p, float3 N, float3 V) {
    const float NdotV = max(0.0, dot(N, V));
    float xiFs;
    const float etaPrime = OpenPBRModulatedIor(p.specularIor, p.specularWeight,
                                               p.coatIor,
                                               OpenPBRFeature(OPENPBR_FEATURE_COAT)
                                                   ? saturate(p.coatWeight) : 0.0,
                                               xiFs);
    const float rPrime = OpenPBRCoatRoughenedRoughness(p);
    const float3 fDielectric = saturate(p.specularColor)
                             * FresnelDielectricExact(NdotV, etaPrime);
    const float3 fMetal = OpenPBRConductorFresnel(NdotV, p.baseColor, p.baseWeight,
                                                  p.specularColor, p.specularWeight);
    const float glossy = (1.0 - rPrime) * (1.0 - rPrime);
    float3 baseMirror = lerp(fDielectric, fMetal, saturate(p.baseMetalness)) * glossy;

    float3 weight = baseMirror;
    if (OpenPBRFeature(OPENPBR_FEATURE_COAT) && p.coatWeight > 0.0) {
        const float C = saturate(p.coatWeight);
        const float etaC = max(1e-2, p.coatIor);
        const float f0Base = (etaC - 1.0) / (etaC + 1.0);
        const float f0Coat = f0Base * f0Base;
        const float2 abc = GgxDirectionalAlbedoAB(NdotV, max(0.04, p.coatRoughness));
        const float eCoat = saturate(f0Coat * abc.x + abc.y);
        const float rc = saturate(p.coatRoughness);
        const float coatGlossy = (1.0 - rc) * (1.0 - rc);
        const float coatMirror = FresnelDielectricExact(NdotV, etaC) * coatGlossy;
        const float3 tCoat = CoatViewTransmittance(saturate(p.coatColor),
                                                   NdotV, NdotV, etaC);
        const float3 darkening = CoatDarkeningFactor(p, NdotV, xiFs,
                                                     OpenPBRDiffuseRho(p),
                                                     OpenPBRDiffuseSelector(p));
        weight = C * coatMirror
               + lerp(float3(1.0, 1.0, 1.0), tCoat * (1.0 - eCoat), C)
                     * (baseMirror * darkening);
    }

    weight *= OpenPBRFuzzThroughput(p, NdotV);
    return saturate(weight);
}

// Per-surface straight-through transmittance for the raygen's
// front-to-back composite — the spec's own transparent-shadow formula
// (§"Opacity / Transparency"):
//   T_pbr = lerp(1, w_dielectric·T_dielectric, α)
//         = 1 − α(1 − (1 − M)·T_dielectric)
// with w_dielectric = 1 − M (metal is opaque) and T_dielectric the
// dielectric transmittance: transmission_color folded in (the
// transmission_depth = 0 default makes it a constant multiplicative
// tint — spec §"Translucent base" — which maps exactly onto the
// existing front-to-back compositing), weighted by transmission_weight
// and by (1 − reflectionWeight) for the interface Fresnel the spec
// says T_dielectric accounts for (the deterministic mirror already
// carries that energy). geometry_opacity α is the pure presence mix
// with the ambient medium — the (1 − α) leg takes NO Fresnel
// (f_transparent is a delta pass-through).
//
// Thin-walled (MATERIAL_FLAG_THIN_WALLED → p.thinWalled): spec
// §"Thin-walled mode" defines transmission_color as the
// through-the-whole-sheet transmittance with an un-deflected refraction
// lobe and underside coat/fuzz ignored (the sanctioned cheap
// approximation). At transmission_depth = 0 the solid constant-tint
// semantics COINCIDE with that — both modes share this exact path; the
// flag is honored in the sense that no second-interface term is added
// for either.
//
// Opaque exactness: α = 1, T = 0 → T_pbr = 1 − 1·(1 − 0) = 0 EXACTLY —
// no epsilon gate needed for the raygen loop's break.
float3 OpenPBRTransmittance(OpenPBRSurfaceParams p, float3 reflectionWeight) {
    float3 tDielectric = float3(0.0, 0.0, 0.0);
    if (OpenPBRFeature(OPENPBR_FEATURE_TRANSMISSION) && p.transmissionWeight > 0.0) {
        tDielectric = saturate(p.transmissionWeight) * saturate(p.transmissionColor)
                    * saturate(float3(1.0, 1.0, 1.0) - reflectionWeight);
    }
    const float alpha = saturate(p.geometryOpacity);
    const float M = saturate(p.baseMetalness);
    return saturate(float3(1.0, 1.0, 1.0)
                    - alpha * (float3(1.0, 1.0, 1.0) - (1.0 - M) * tDielectric));
}

// Coat-tinted emission — the boxed closure's EDF row:
//   L_e = lerp(1, T_coat, C)·E
// (emission sits BELOW coat and fuzz; the coat absorbs it — spec
// §"Emission". T_coat = coat_color, the normal-incidence round-trip
// transmittance the boxed reduction uses; grazing-angle darkening is
// left loose by the spec and not modeled). p.emission already carries
// emission_color × emission_luminance × (emission map).
float3 OpenPBREmission(OpenPBRSurfaceParams p) {
    float3 factor = float3(1.0, 1.0, 1.0);
    if (OpenPBRFeature(OPENPBR_FEATURE_COAT) && p.coatWeight > 0.0)
        factor = lerp(float3(1.0, 1.0, 1.0), saturate(p.coatColor),
                      saturate(p.coatWeight));
    return factor * p.emission;
}

#endif  // PYXIS_OPENPBR_MATERIAL_SLANG
