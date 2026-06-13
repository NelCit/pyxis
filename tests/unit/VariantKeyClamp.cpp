// Pyxis renderer — RaytracedLightingPass::VariantKey clamp/normalize
// test (Q3 review, openpbr-complete-design.md).
//
// VariantKey(projectionMode, featureMask) is the ONE canonical
// normalization point for the lighting pass's RT-pipeline variant
// cache. EnsureFeaturePipeline, IsOperational, and Execute's
// _activeVariantKey lookup all route through it, so a disagreement
// between them is impossible by construction. This test pins the two
// normalization rules the key encodes:
//   (1) projectionMode collapses to {0, 1} (mode 1 stays 1; mode 0 and
//       any other value -> 0), matching the shader's
//       `PROJECTION_MODE == 1u` branch shape;
//   (2) the OpenPBR feature mask is clamped to OPENPBR_FEATURES_ALL
//       (0x3F) — the Q3 defensive bound on the variant cache. Two masks
//       differing only in undefined bits MUST key identically (so they
//       share one pipeline) while distinct DEFINED masks key apart.
//
// VariantKey is a public `static constexpr` member of
// RaytracedLightingPass; the header is on the renderer's Private
// include path (the §35 unit-test exception). No .cpp / device needed —
// these are pure compile-time/constexpr key computations.

#include "Passes/RaytracedLightingPass.h"

#include <gtest/gtest.h>

#include <cstdint>

using pyxis::RaytracedLightingPass;
namespace si = pyxis::shaderinterop;

namespace {
constexpr std::uint32_t kAll = si::OPENPBR_FEATURES_ALL;  // 0x3F
}  // namespace

// Sanity: the clamp constant is the documented 0x3F.
TEST(VariantKeyClamp, FeaturesAllIsExpectedValue)
{
  EXPECT_EQ(kAll, 0x3Fu);
}

// (2) Distinct DEFINED masks -> distinct keys (same projection).
TEST(VariantKeyClamp, DistinctDefinedMasksGiveDistinctKeys)
{
  const std::uint64_t k00 = RaytracedLightingPass::VariantKey(0u, 0x00u);
  const std::uint64_t k01 = RaytracedLightingPass::VariantKey(0u, 0x01u);
  const std::uint64_t k3f = RaytracedLightingPass::VariantKey(0u, 0x3Fu);
  EXPECT_NE(k00, k01);
  EXPECT_NE(k01, k3f);
  EXPECT_NE(k00, k3f);
}

// (1) Projection normalization — mode 1 stays 1; mode 0 and any other
// value collapse to 0 (the same key). Verify what the code ACTUALLY
// does: the perspective bucket swallows every non-1 mode.
TEST(VariantKeyClamp, ProjectionCollapsesToZeroOrOne)
{
  const std::uint64_t persp = RaytracedLightingPass::VariantKey(0u, kAll);
  const std::uint64_t ortho = RaytracedLightingPass::VariantKey(1u, kAll);
  EXPECT_NE(persp, ortho);  // 0 and 1 are distinct projection buckets.

  // Mode 0 and out-of-range modes (2, 99, ~0u) all collapse to the
  // perspective bucket — same key.
  EXPECT_EQ(persp, RaytracedLightingPass::VariantKey(2u, kAll));
  EXPECT_EQ(persp, RaytracedLightingPass::VariantKey(99u, kAll));
  EXPECT_EQ(persp, RaytracedLightingPass::VariantKey(~0u, kAll));
  // ...but mode 1 does NOT collapse.
  EXPECT_NE(persp, RaytracedLightingPass::VariantKey(1u, kAll));
}

// (2) The Q3 clamp: a mask with undefined high bits keys IDENTICALLY to
// the same mask with those bits stripped. 0xFFFFFFFF -> 0x3F.
TEST(VariantKeyClamp, UndefinedBitsClampToSameKey)
{
  EXPECT_EQ(RaytracedLightingPass::VariantKey(0u, 0xFFFFFFFFu),
            RaytracedLightingPass::VariantKey(0u, 0x3Fu));
  // Masks differing ONLY in undefined bits (6+) share a key.
  EXPECT_EQ(RaytracedLightingPass::VariantKey(0u, 0x2Au),
            RaytracedLightingPass::VariantKey(0u, 0x2Au | 0xFFFFFFC0u));
  // Cross-projection too.
  EXPECT_EQ(RaytracedLightingPass::VariantKey(1u, 0xDEADBEC0u | 0x15u),
            RaytracedLightingPass::VariantKey(1u, 0x15u));
}

// NormalizeFeatureMask is the shared clamp primitive; pin it directly.
TEST(VariantKeyClamp, NormalizeFeatureMaskStripsUndefinedBits)
{
  EXPECT_EQ(RaytracedLightingPass::NormalizeFeatureMask(0xFFFFFFFFu), 0x3Fu);
  EXPECT_EQ(RaytracedLightingPass::NormalizeFeatureMask(0x3Fu), 0x3Fu);
  EXPECT_EQ(RaytracedLightingPass::NormalizeFeatureMask(0x00u), 0x00u);
  EXPECT_EQ(RaytracedLightingPass::NormalizeFeatureMask(0x2Au), 0x2Au);
  // The default mask is untouched by the clamp (no golden pixel moves).
  EXPECT_EQ(RaytracedLightingPass::NormalizeFeatureMask(kAll), kAll);
}

// The low 32 bits of the key ARE the normalized mask, and the high bits
// carry projection — so the key uniquely identifies (proj, mask') and
// the two consumers can't disagree.
TEST(VariantKeyClamp, KeyEncodesProjectionAndNormalizedMask)
{
  const std::uint64_t key = RaytracedLightingPass::VariantKey(1u, 0x2Au);
  EXPECT_EQ(static_cast<std::uint32_t>(key & 0xFFFFFFFFull), 0x2Au);
  EXPECT_EQ(static_cast<std::uint32_t>(key >> 32), 1u);
}
