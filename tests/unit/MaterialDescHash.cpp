// Pyxis renderer — HashMaterialDesc dedup-identity test (Q1,
// openpbr-complete-design.md).
//
// The §11 dedup hash was rewritten at Q1 from a two-byte-range sweep
// into an explicit field-by-field hash. This pins the two bugs the
// rewrite fixed plus the new behaviour:
//   (a) fields in the former un-hashed gap between `sourcePrim` and
//       `_reserved` (UV transform / normalStrength / flipTangent* /
//       projectionMode / wrap tokens) are now part of dedup identity;
//   (b) every Q1 OpenPBR-complete extension field is part of dedup
//       identity;
//   (c) the diagnostics-only `source` / `sourcePrim` fields are NOT
//       (source exclusion is a deliberate Q1 dedup change — the old
//       byte sweep mixed the enum in).
//
// HashMaterialDesc lives in Private/GpuScene/Detail.h — the unit-test
// harness is the documented §35 exception allowed into Private/.

#include "GpuScene/Detail.h"

#include <Pyxis/Renderer/Descs/OpenPBRMaterialDesc.h>

#include <gtest/gtest.h>

#include <string_view>

using namespace pyxis;
using gpuscene_detail::HashMaterialDesc;

TEST(MaterialDescHash, IdenticalDescsHashEqual)
{
  const OpenPBRMaterialDesc a;
  const OpenPBRMaterialDesc b;
  EXPECT_EQ(HashMaterialDesc(a), HashMaterialDesc(b));
}

TEST(MaterialDescHash, DiagnosticsFieldsDoNotAffectHash)
{
  OpenPBRMaterialDesc a;
  OpenPBRMaterialDesc b;
  // sourcePrim was always excluded (caller-owned view, diagnostics
  // only); `source` exclusion is the deliberate Q1 change — identical
  // semantic values from different dialects now collapse to one handle.
  a.sourcePrim = std::string_view{"/World/Looks/MatA"};
  b.sourcePrim = std::string_view{"/World/Looks/MatB"};
  a.source = OpenPBRMaterialDesc::Source::UsdPreviewSurface;
  b.source = OpenPBRMaterialDesc::Source::Mdl;
  EXPECT_EQ(HashMaterialDesc(a), HashMaterialDesc(b));
}

// (a) The previously-unhashed gap: pre-Q1, two descs differing only in
// these fields collapsed to ONE handle and rendered with the first
// one's values. They must now bucket separately.
TEST(MaterialDescHash, PreviouslyUnhashedGapFieldsChangeHash)
{
  const OpenPBRMaterialDesc base;

  OpenPBRMaterialDesc uvScale = base;
  uvScale.baseColorUvScaleX = 2.0f;
  EXPECT_NE(HashMaterialDesc(base), HashMaterialDesc(uvScale));

  OpenPBRMaterialDesc uvRotate = base;
  uvRotate.baseColorUvRotationDeg = 90.0f;
  EXPECT_NE(HashMaterialDesc(base), HashMaterialDesc(uvRotate));

  OpenPBRMaterialDesc bump = base;
  bump.normalStrength = 4.0f;
  EXPECT_NE(HashMaterialDesc(base), HashMaterialDesc(bump));

  OpenPBRMaterialDesc flip = base;
  flip.flipTangentV = 1u;
  EXPECT_NE(HashMaterialDesc(base), HashMaterialDesc(flip));

  OpenPBRMaterialDesc projection = base;
  projection.projectionMode = 1u;
  EXPECT_NE(HashMaterialDesc(base), HashMaterialDesc(projection));
}

// (b) Every Q1 extension field participates in dedup identity.
TEST(MaterialDescHash, NewOpenPBRCompleteFieldsChangeHash)
{
  const OpenPBRMaterialDesc base;
  const std::uint64_t baseHash = HashMaterialDesc(base);

  OpenPBRMaterialDesc specularColor = base;
  specularColor.specularColorG = 0.5f;
  EXPECT_NE(baseHash, HashMaterialDesc(specularColor));

  OpenPBRMaterialDesc diffuseRoughness = base;
  diffuseRoughness.baseDiffuseRoughness = 0.7f;
  EXPECT_NE(baseHash, HashMaterialDesc(diffuseRoughness));

  OpenPBRMaterialDesc anisotropy = base;
  anisotropy.specularRoughnessAnisotropy = 0.3f;
  EXPECT_NE(baseHash, HashMaterialDesc(anisotropy));

  OpenPBRMaterialDesc coatColor = base;
  coatColor.coatColorB = 0.65f;
  EXPECT_NE(baseHash, HashMaterialDesc(coatColor));

  OpenPBRMaterialDesc coatIor = base;
  coatIor.coatIor = 1.55f;
  EXPECT_NE(baseHash, HashMaterialDesc(coatIor));

  OpenPBRMaterialDesc coatDarkening = base;
  coatDarkening.coatDarkening = 0.0f;
  EXPECT_NE(baseHash, HashMaterialDesc(coatDarkening));

  OpenPBRMaterialDesc fuzz = base;
  fuzz.fuzzWeight = 1.0f;
  EXPECT_NE(baseHash, HashMaterialDesc(fuzz));

  OpenPBRMaterialDesc fuzzColor = base;
  fuzzColor.fuzzColorR = 0.2f;
  EXPECT_NE(baseHash, HashMaterialDesc(fuzzColor));

  OpenPBRMaterialDesc fuzzRoughness = base;
  fuzzRoughness.fuzzRoughness = 0.9f;
  EXPECT_NE(baseHash, HashMaterialDesc(fuzzRoughness));

  OpenPBRMaterialDesc transmissionColor = base;
  transmissionColor.transmissionColorG = 0.4f;
  EXPECT_NE(baseHash, HashMaterialDesc(transmissionColor));

  OpenPBRMaterialDesc subsurface = base;
  subsurface.subsurfaceWeight = 1.0f;
  EXPECT_NE(baseHash, HashMaterialDesc(subsurface));

  OpenPBRMaterialDesc subsurfaceColor = base;
  subsurfaceColor.subsurfaceColorB = 0.1f;
  EXPECT_NE(baseHash, HashMaterialDesc(subsurfaceColor));

  OpenPBRMaterialDesc thinWalled = base;
  thinWalled.thinWalled = 1u;
  EXPECT_NE(baseHash, HashMaterialDesc(thinWalled));
}

// Existing semantic fields keep participating (regression guard for
// the rewrite — the explicit list must not have dropped anything the
// old prefix sweep covered).
TEST(MaterialDescHash, LegacySemanticFieldsStillChangeHash)
{
  const OpenPBRMaterialDesc base;
  const std::uint64_t baseHash = HashMaterialDesc(base);

  OpenPBRMaterialDesc color = base;
  color.baseColor = hlslpp::float3{1.0f, 0.0f, 0.0f};
  EXPECT_NE(baseHash, HashMaterialDesc(color));

  OpenPBRMaterialDesc rough = base;
  rough.roughness = 0.15f;
  EXPECT_NE(baseHash, HashMaterialDesc(rough));

  OpenPBRMaterialDesc tex = base;
  tex.baseColorMap = static_cast<TextureHandle>(42u);
  EXPECT_NE(baseHash, HashMaterialDesc(tex));

  OpenPBRMaterialDesc opacity = base;
  opacity.opacity = 0.5f;
  EXPECT_NE(baseHash, HashMaterialDesc(opacity));
}
