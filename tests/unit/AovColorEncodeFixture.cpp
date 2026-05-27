// Pyxis Hydra — color-AOV encode policy + Omniverse⇄engine parity invariant.
//
// The contract (commit bb94133 + the viewer present-encode fix): every Pyxis
// display path applies the sRGB OETF to the ACES-tonemapped LINEAR color EXACTLY
// ONCE, so the standalone viewer, the headless PNG, and the Omniverse Kit viewport
// produce the same pixels.
//
//   * Standalone headless PNG : WritePng applies sRGB to the linear readback.
//   * Standalone viewer        : SsaaResolvePass applies sRGB on present.
//   * Omniverse Kit viewport   : HdxColorCorrectionTask applies sRGB to the AOV,
//                                so HdPyxisRenderDelegate writes the AOV LINEAR
//                                (EncodeAovColorChannel with encodeSrgb=false).
//
// This fixture pins EncodeAovColorChannel (the delegate's single source of truth,
// also used in WritePyxisColorToAov) and proves the "exactly one sRGB" invariant:
// with the default (linear) AOV, host-sRGB(AOV) == sRGB(linear) == the standalone
// display value. It also locks the regression: the old sRGB-pre-encode path would
// DOUBLE-apply (the washed-out Kit bug).

#include "AovColorEncode.h"  // sources/pyxis_hydra/Private (on the unit-test include path)

#include <Pyxis/Platform/Color/ColorEncoding.h>

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>

using pyxis::color::LinearToSrgb;
using pyxis::hydra::EncodeAovColorChannel;

namespace {

// The linear-ACES means the World Lobby actually produced (readback mean ≈ 0.29)
// plus endpoints / mid-grays, so the golden values map to real content.
constexpr float LINEAR_SAMPLES[] = {0.0f, 0.01f, 0.05f, 0.18f, 0.29f, 0.5f, 0.75f, 1.0f};

// Final 8-bit display value after the host's sRGB OETF, given an AOV value.
int DisplayByteAfterHostSrgb(float aovValue) {
  return static_cast<int>(std::lround(LinearToSrgb(aovValue) * 255.0f));
}

}  // namespace

// -----------------------------------------------------------------------------
// EncodeAovColorChannel — channel policy.
// -----------------------------------------------------------------------------
TEST(AovColorEncode, DefaultRgbIsLinearVerbatim) {
  // encodeSrgb=false (the default): RGB passes through clamped-linear (the host
  // applies the OETF). NOT sRGB-encoded here.
  for (const float linear : LINEAR_SAMPLES)
    for (int channel = 0; channel < 3; ++channel)
      EXPECT_FLOAT_EQ(EncodeAovColorChannel(linear, channel, /*encodeSrgb=*/false), linear)
          << "linear=" << linear << " channel=" << channel;
}

TEST(AovColorEncode, AlphaAlwaysLinearEvenWhenSrgbRequested) {
  // Channel 3 (alpha) is never sRGB-encoded, regardless of the flag.
  for (const float alpha : LINEAR_SAMPLES) {
    EXPECT_FLOAT_EQ(EncodeAovColorChannel(alpha, 3, /*encodeSrgb=*/false), alpha);
    EXPECT_FLOAT_EQ(EncodeAovColorChannel(alpha, 3, /*encodeSrgb=*/true), alpha);
  }
}

TEST(AovColorEncode, SrgbFlagEncodesRgbOnly) {
  for (const float linear : LINEAR_SAMPLES)
    for (int channel = 0; channel < 3; ++channel)
      EXPECT_FLOAT_EQ(EncodeAovColorChannel(linear, channel, /*encodeSrgb=*/true),
                      LinearToSrgb(linear))
          << "linear=" << linear << " channel=" << channel;
}

TEST(AovColorEncode, ClampsOutOfRange) {
  EXPECT_FLOAT_EQ(EncodeAovColorChannel(-0.5f, 0, false), 0.0f);
  EXPECT_FLOAT_EQ(EncodeAovColorChannel(2.0f, 0, false), 1.0f);
  EXPECT_FLOAT_EQ(EncodeAovColorChannel(-0.5f, 3, true), 0.0f);
  EXPECT_FLOAT_EQ(EncodeAovColorChannel(5.0f, 3, true), 1.0f);
}

// -----------------------------------------------------------------------------
// The parity invariant: viewer == headless == Omniverse.
// -----------------------------------------------------------------------------
TEST(AovColorEncode, DefaultMatchesStandaloneAfterHostSrgb) {
  // Kit/usdview apply sRGB to the AOV. With the default (linear) AOV value, the
  // displayed result equals sRGB(linear) — exactly the standalone headless PNG /
  // viewer value. Exactly-one-sRGB everywhere.
  for (const float linear : LINEAR_SAMPLES) {
    const float aov = EncodeAovColorChannel(linear, /*channel=*/0, /*encodeSrgb=*/false);
    const float hostDisplayed = LinearToSrgb(aov);     // host OETF
    const float standalone = LinearToSrgb(linear);     // pyxis.exe PNG / viewer present
    EXPECT_NEAR(hostDisplayed, standalone, 1e-6f) << "linear=" << linear;
  }
}

TEST(AovColorEncode, OldSrgbPreEncodeWouldDoubleApply) {
  // Regression guard: the pre-bb94133 behaviour (sRGB-encode in the delegate) put
  // sRGB(linear) in the AOV, so the host's OETF produced sRGB(sRGB(linear)) — the
  // washed-out / too-bright Kit output. Lock that this is measurably brighter than
  // the correct single encode for mid-tones, so nobody re-introduces it.
  for (const float linear : {0.05f, 0.18f, 0.29f, 0.5f}) {
    const float doubled = LinearToSrgb(LinearToSrgb(linear));  // old: AOV=sRGB(L), host sRGB again
    const float correct = LinearToSrgb(linear);                // new: AOV=L, host sRGB once
    EXPECT_GT(doubled, correct + 0.02f) << "linear=" << linear
        << " (double-encode must be visibly brighter — the bug)";
  }
}

// -----------------------------------------------------------------------------
// Golden display bytes — the exact 8-bit values all three paths must hit for the
// representative linear inputs (so a curve/policy regression is caught precisely).
// -----------------------------------------------------------------------------
TEST(AovColorEncode, GoldenDisplayBytes) {
  struct Golden { float linear; int displayByte; };
  // displayByte = round(sRGB(linear) * 255). Independently recomputable.
  const Golden golden[] = {
      {0.0f, 0},    {0.01f, 25}, {0.05f, 63},  {0.18f, 118},
      {0.29f, 147}, {0.5f, 188}, {0.75f, 225}, {1.0f, 255},
  };
  for (const auto& gold : golden) {
    const float aov = EncodeAovColorChannel(gold.linear, 0, /*encodeSrgb=*/false);
    EXPECT_EQ(DisplayByteAfterHostSrgb(aov), gold.displayByte) << "linear=" << gold.linear;
  }
}
