// Pyxis M21 / V2.A.15 — --load-mode + --population-mask CLI parsing.
//
// Stub plumbing: parser accepts the values + stores into CliArgs;
// the rest of the pipeline still loads-all. Tests pin the parser so
// future plumbing has a stable target.

#include "CliArgs.h"

#include <gtest/gtest.h>

using pyxis::app::CliArgs;
using pyxis::app::Parse;

TEST(LoadModeAndMaskParsing, LoadModeAllParses)
{
  const char* argv[] = {"pyxis.exe", "--load-mode", "all"};
  const CliArgs cli = Parse(3, const_cast<char**>(argv));
  EXPECT_FALSE(cli.invalid);
  EXPECT_EQ(cli.loadMode, "all");
}

TEST(LoadModeAndMaskParsing, LoadModeNoneParses)
{
  const char* argv[] = {"pyxis.exe", "--load-mode", "none"};
  const CliArgs cli = Parse(3, const_cast<char**>(argv));
  EXPECT_FALSE(cli.invalid);
  EXPECT_EQ(cli.loadMode, "none");
}

TEST(LoadModeAndMaskParsing, PopulationMaskParses)
{
  const char* argv[] = {"pyxis.exe", "--population-mask", "/World/Lobby,/World/Lights"};
  const CliArgs cli = Parse(3, const_cast<char**>(argv));
  EXPECT_FALSE(cli.invalid);
  EXPECT_EQ(cli.populationMask, "/World/Lobby,/World/Lights");
}

TEST(LoadModeAndMaskParsing, MissingValueIsInvalid)
{
  const char* argv1[] = {"pyxis.exe", "--load-mode"};
  EXPECT_TRUE(Parse(2, const_cast<char**>(argv1)).invalid);

  const char* argv2[] = {"pyxis.exe", "--population-mask"};
  EXPECT_TRUE(Parse(2, const_cast<char**>(argv2)).invalid);
}

TEST(LoadModeAndMaskParsing, DefaultsAreEmpty)
{
  const char* argv[] = {"pyxis.exe"};
  const CliArgs cli = Parse(1, const_cast<char**>(argv));
  EXPECT_TRUE(cli.loadMode.empty());
  EXPECT_TRUE(cli.populationMask.empty());
}
