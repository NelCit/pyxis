// Pyxis M19 / V2.A.4 + V2.A.13 — --frame N CLI parsing.
//
// Parses argv tables and asserts the `frameNumber` field is filled in
// correctly. The actual time-code propagation is a follow-up; this
// test pins the parser surface so future plumbing has a stable target.

#include "CliArgs.h"

#include <gtest/gtest.h>

using pyxis::app::CliArgs;
using pyxis::app::Parse;

TEST(FrameFlagParsing, DefaultIsMinusOne)
{
  const char* argv[] = {"pyxis.exe"};
  const CliArgs cli = Parse(1, const_cast<char**>(argv));
  EXPECT_FALSE(cli.invalid);
  EXPECT_EQ(cli.frameNumber, -1);
}

TEST(FrameFlagParsing, FrameFortyTwoParses)
{
  const char* argv[] = {"pyxis.exe", "--frame", "42"};
  const CliArgs cli = Parse(3, const_cast<char**>(argv));
  EXPECT_FALSE(cli.invalid);
  EXPECT_EQ(cli.frameNumber, 42);
}

TEST(FrameFlagParsing, NegativeFrameParses)
{
  const char* argv[] = {"pyxis.exe", "--frame", "-5"};
  const CliArgs cli = Parse(3, const_cast<char**>(argv));
  EXPECT_FALSE(cli.invalid);
  EXPECT_EQ(cli.frameNumber, -5);
}

TEST(FrameFlagParsing, NonNumericFrameInvalid)
{
  const char* argv[] = {"pyxis.exe", "--frame", "abc"};
  const CliArgs cli = Parse(3, const_cast<char**>(argv));
  EXPECT_TRUE(cli.invalid);
}

TEST(FrameFlagParsing, MissingFrameValueInvalid)
{
  const char* argv[] = {"pyxis.exe", "--frame"};
  const CliArgs cli = Parse(2, const_cast<char**>(argv));
  EXPECT_TRUE(cli.invalid);
}
