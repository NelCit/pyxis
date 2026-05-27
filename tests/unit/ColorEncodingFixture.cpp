// Pyxis platform — color/half encoding helper tests.
//
// pyxis::color::{LinearToSrgb, SrgbToLinear, HalfToFloat, FloatToHalf} are the
// canonical float color/half conversions shared by the Hydra delegate's host-AOV
// composite, the headless harnesses, and the standalone writers. The whole
// "Omniverse-viewer == Pyxis-engine" color-parity invariant rests on LinearToSrgb
// being the exact standard sRGB OETF and on the half round-trip being lossless for
// display-range values, so we pin them here. See AovColorEncodeFixture.cpp for the
// AOV policy that composes these.

#include <Pyxis/Platform/Color/ColorEncoding.h>

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>

using pyxis::color::FloatToHalf;
using pyxis::color::HalfToFloat;
using pyxis::color::LinearToSrgb;
using pyxis::color::SrgbToLinear;

namespace {

// Reference sRGB OETF (independent reimplementation — catches a constant typo in
// the header that a self-referential round-trip test would miss).
double RefLinearToSrgb(double linear) {
  if (linear <= 0.0) return 0.0;
  if (linear >= 1.0) return 1.0;
  return linear <= 0.0031308 ? linear * 12.92 : 1.055 * std::pow(linear, 1.0 / 2.4) - 0.055;
}

}  // namespace

// -----------------------------------------------------------------------------
// LinearToSrgb — endpoints, threshold, known midpoints, monotonicity, clamping.
// -----------------------------------------------------------------------------
TEST(ColorEncoding, LinearToSrgbEndpoints) {
  EXPECT_FLOAT_EQ(LinearToSrgb(0.0f), 0.0f);
  EXPECT_FLOAT_EQ(LinearToSrgb(1.0f), 1.0f);
}

TEST(ColorEncoding, LinearToSrgbClampsOutOfRange) {
  EXPECT_FLOAT_EQ(LinearToSrgb(-0.5f), 0.0f);
  EXPECT_FLOAT_EQ(LinearToSrgb(2.0f), 1.0f);
  EXPECT_FLOAT_EQ(LinearToSrgb(1e9f), 1.0f);
}

TEST(ColorEncoding, LinearToSrgbLinearSegmentBelowThreshold) {
  // Below 0.0031308 the OETF is the linear segment c*12.92.
  EXPECT_NEAR(LinearToSrgb(0.001f), 0.001f * 12.92f, 1e-6f);
  EXPECT_NEAR(LinearToSrgb(0.0031308f), 0.0031308f * 12.92f, 1e-5f);
}

TEST(ColorEncoding, LinearToSrgbMatchesReferenceAcrossRange) {
  for (int i = 0; i <= 256; ++i) {
    const float linear = static_cast<float>(i) / 256.0f;
    EXPECT_NEAR(LinearToSrgb(linear), static_cast<float>(RefLinearToSrgb(linear)), 1e-4f)
        << "linear=" << linear;
  }
}

TEST(ColorEncoding, LinearToSrgbKnownMidpoint) {
  // sRGB(0.5) ≈ 0.7353569 — the classic mid-gray value the double-encode bug
  // pushes too bright; pinned so a curve-constant regression is caught here.
  EXPECT_NEAR(LinearToSrgb(0.5f), 0.735357f, 1e-4f);
  // sRGB(0.18) (18% mid-gray) ≈ 0.4613561.
  EXPECT_NEAR(LinearToSrgb(0.18f), 0.461356f, 1e-4f);
}

TEST(ColorEncoding, LinearToSrgbStrictlyMonotonic) {
  float prev = LinearToSrgb(0.0f);
  for (int i = 1; i <= 1000; ++i) {
    const float cur = LinearToSrgb(static_cast<float>(i) / 1000.0f);
    EXPECT_GT(cur, prev) << "i=" << i;
    prev = cur;
  }
}

TEST(ColorEncoding, LinearToSrgbBrightensMidtones) {
  // The OETF lifts mid/low values (that is the whole point) — guards the sign of
  // the transform, which is what the Kit double-encode got wrong.
  for (const float linear : {0.05f, 0.1f, 0.25f, 0.5f, 0.75f})
    EXPECT_GT(LinearToSrgb(linear), linear) << "linear=" << linear;
}

// -----------------------------------------------------------------------------
// SrgbToLinear — inverse of LinearToSrgb.
// -----------------------------------------------------------------------------
TEST(ColorEncoding, SrgbToLinearEndpointsAndClamp) {
  EXPECT_FLOAT_EQ(SrgbToLinear(0.0f), 0.0f);
  EXPECT_FLOAT_EQ(SrgbToLinear(1.0f), 1.0f);
  EXPECT_FLOAT_EQ(SrgbToLinear(-1.0f), 0.0f);
  EXPECT_FLOAT_EQ(SrgbToLinear(3.0f), 1.0f);
}

TEST(ColorEncoding, SrgbLinearRoundTrip) {
  for (int i = 0; i <= 256; ++i) {
    const float linear = static_cast<float>(i) / 256.0f;
    EXPECT_NEAR(SrgbToLinear(LinearToSrgb(linear)), linear, 1e-4f) << "linear=" << linear;
  }
}

// -----------------------------------------------------------------------------
// Half <-> float — display-range round-trip + special values.
// -----------------------------------------------------------------------------
TEST(ColorEncoding, HalfFloatRoundTripDisplayRange) {
  // For values that are exactly representable we expect tight equality; for the
  // rest, half has ~3 decimal digits, so 8-bit-display granularity (1/255) is the
  // tolerance that matters for color output.
  for (int i = 0; i <= 255; ++i) {
    const float val = static_cast<float>(i) / 255.0f;
    const float roundTrip = HalfToFloat(FloatToHalf(val));
    EXPECT_NEAR(roundTrip, val, 1.0f / 255.0f) << "val=" << val;
  }
}

TEST(ColorEncoding, HalfFloatExactPowersAndHalves) {
  for (const float val : {0.0f, 0.5f, 0.25f, 0.125f, 1.0f, 2.0f, 0.75f})
    EXPECT_FLOAT_EQ(HalfToFloat(FloatToHalf(val)), val) << "val=" << val;
}

TEST(ColorEncoding, HalfZeroAndOneBitPatterns) {
  EXPECT_EQ(FloatToHalf(0.0f), 0x0000u);
  EXPECT_EQ(FloatToHalf(1.0f), 0x3C00u);  // IEEE half 1.0
  EXPECT_FLOAT_EQ(HalfToFloat(0x3C00u), 1.0f);
  EXPECT_FLOAT_EQ(HalfToFloat(0x0000u), 0.0f);
}

TEST(ColorEncoding, HalfNegativePreservesSign) {
  EXPECT_FLOAT_EQ(HalfToFloat(FloatToHalf(-0.5f)), -0.5f);
}

TEST(ColorEncoding, HalfOverflowToInfinity) {
  // > half max (65504) flushes to +inf per the header contract.
  EXPECT_TRUE(std::isinf(HalfToFloat(FloatToHalf(1.0e9f))));
}

TEST(ColorEncoding, HalfSubnormalDecode) {
  // Smallest positive subnormal half = 2^-24 ≈ 5.96e-8.
  EXPECT_NEAR(HalfToFloat(0x0001u), std::ldexp(1.0f, -24), 1e-12f);
}
