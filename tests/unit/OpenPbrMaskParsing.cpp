// Pyxis — --openpbr-mask CLI parse + clamp test (Q3 review,
// openpbr-complete-design.md).
//
// The --openpbr-mask flag overrides RenderSettings::openPbrFeatureMask
// (the per-lobe OpenPBR closure gates). CliArgs.cpp parses hex OR
// decimal via strtoul(base 0), then masks off every bit above
// OPENPBR_FEATURES_ALL (0x3F) so a typo can't request RT-pipeline
// variants the shader has no bits for. These tests pin the parse + the
// clamp + the documented missing-value / garbage fallback.

#include "CliArgs.h"
#include "OpenPbrFeatureBits.h"  // OPENPBR_FEATURES_ALL (== 0x3F)

#include <gtest/gtest.h>

using pyxis::app::CliArgs;
using pyxis::app::Parse;

TEST(OpenPbrMaskParsing, HexValueParses)
{
  const char* argv[] = {"pyxis.exe", "--openpbr-mask", "0x2a"};
  const CliArgs cli = Parse(3, const_cast<char**>(argv));
  EXPECT_FALSE(cli.invalid);
  EXPECT_EQ(cli.openPbrMask, 0x2au);
}

TEST(OpenPbrMaskParsing, DecimalValueParses)
{
  // "63" is decimal (strtoul base 0) == 0x3F == all defined bits.
  const char* argv[] = {"pyxis.exe", "--openpbr-mask", "63"};
  const CliArgs cli = Parse(3, const_cast<char**>(argv));
  EXPECT_FALSE(cli.invalid);
  EXPECT_EQ(cli.openPbrMask, 0x3Fu);
}

TEST(OpenPbrMaskParsing, OutOfRangeBitsAreClamped)
{
  // 0xFF sets bits 6 + 7 above OPENPBR_FEATURES_ALL — they have no
  // shader effect, so the parser masks them off to 0x3F (the clamp that
  // bounds the renderer's variant cache).
  const char* argv[] = {"pyxis.exe", "--openpbr-mask", "0xFF"};
  const CliArgs cli = Parse(3, const_cast<char**>(argv));
  EXPECT_FALSE(cli.invalid);
  EXPECT_EQ(cli.openPbrMask, pyxis::app::OPENPBR_FEATURES_ALL);
  EXPECT_EQ(cli.openPbrMask, 0x3Fu);
}

TEST(OpenPbrMaskParsing, FullUint32ClampsToAll)
{
  // The adversarial case the renderer-side clamp guards against:
  // a caller sweeping the top 26 bits. The parse-time clamp already
  // collapses them to 0x3F here.
  const char* argv[] = {"pyxis.exe", "--openpbr-mask", "0xFFFFFFFF"};
  const CliArgs cli = Parse(3, const_cast<char**>(argv));
  EXPECT_FALSE(cli.invalid);
  EXPECT_EQ(cli.openPbrMask, 0x3Fu);
}

TEST(OpenPbrMaskParsing, ZeroParsesToZero)
{
  // 0 = every closure gated off (allowed; the shader degrades to the
  // base diffuse + metal/dielectric core which is always on).
  const char* argv[] = {"pyxis.exe", "--openpbr-mask", "0"};
  const CliArgs cli = Parse(3, const_cast<char**>(argv));
  EXPECT_FALSE(cli.invalid);
  EXPECT_EQ(cli.openPbrMask, 0u);
}

TEST(OpenPbrMaskParsing, MissingValueIsInvalid)
{
  const char* argv[] = {"pyxis.exe", "--openpbr-mask"};
  EXPECT_TRUE(Parse(2, const_cast<char**>(argv)).invalid);
}

TEST(OpenPbrMaskParsing, GarbageValueIsInvalid)
{
  // ParseUInt32AnyBase rejects non-numeric input (end != str / trailing
  // junk), so the parser flags the whole CliArgs invalid.
  const char* argv1[] = {"pyxis.exe", "--openpbr-mask", "notanumber"};
  EXPECT_TRUE(Parse(3, const_cast<char**>(argv1)).invalid);

  const char* argv2[] = {"pyxis.exe", "--openpbr-mask", "0x3Fgarbage"};
  EXPECT_TRUE(Parse(3, const_cast<char**>(argv2)).invalid);
}

TEST(OpenPbrMaskParsing, DefaultIsAllFeatures)
{
  const char* argv[] = {"pyxis.exe"};
  const CliArgs cli = Parse(1, const_cast<char**>(argv));
  EXPECT_FALSE(cli.invalid);
  EXPECT_EQ(cli.openPbrMask, 0x3Fu);
}
