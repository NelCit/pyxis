// Pyxis V2.A.2 (M12) — --variant CLI parsing + spec-tokeniser.
//
// Two layers under test:
//   1. CliArgs::Parse stores the raw spec into `cli.variantSelections`
//      (and rejects a missing value).
//   2. VariantParser::ParseVariantSelections splits the spec into
//      <primPath, setName, value> records, dropping malformed entries.
//
// The applier inside IngestUsd that authors selections onto the
// session layer is exercised by the Golden.variantset_* fixtures —
// pinning it here too would require spinning up a UsdStage, which
// already lives in the integration / golden harness.

#include "CliArgs.h"
#include "VariantParser.h"

#include <gtest/gtest.h>

using pyxis::app::CliArgs;
using pyxis::app::Parse;
using pyxis::app::ParseVariantSelections;
using pyxis::app::VariantSelection;

TEST(VariantSelectionParsing, DefaultIsEmpty)
{
  const char* argv[] = {"pyxis.exe"};
  const CliArgs cli = Parse(1, const_cast<char**>(argv));
  EXPECT_FALSE(cli.invalid);
  EXPECT_TRUE(cli.variantSelections.empty());
}

TEST(VariantSelectionParsing, SingleEntryParses)
{
  const char* argv[] = {"pyxis.exe", "--variant", "/World/Hero:lod=high"};
  const CliArgs cli = Parse(3, const_cast<char**>(argv));
  EXPECT_FALSE(cli.invalid);
  EXPECT_EQ(cli.variantSelections, "/World/Hero:lod=high");

  const auto entries = ParseVariantSelections(cli.variantSelections);
  ASSERT_EQ(entries.size(), 1u);
  EXPECT_EQ(entries[0].primPath, "/World/Hero");
  EXPECT_EQ(entries[0].setName,  "lod");
  EXPECT_EQ(entries[0].value,    "high");
}

TEST(VariantSelectionParsing, MultiEntryParses)
{
  const char* argv[] = {
      "pyxis.exe", "--variant",
      "/World/Hero:lod=high,/World/Cup:season=winter"};
  const CliArgs cli = Parse(3, const_cast<char**>(argv));
  EXPECT_FALSE(cli.invalid);

  const auto entries = ParseVariantSelections(cli.variantSelections);
  ASSERT_EQ(entries.size(), 2u);
  EXPECT_EQ(entries[0].primPath, "/World/Hero");
  EXPECT_EQ(entries[0].setName,  "lod");
  EXPECT_EQ(entries[0].value,    "high");
  EXPECT_EQ(entries[1].primPath, "/World/Cup");
  EXPECT_EQ(entries[1].setName,  "season");
  EXPECT_EQ(entries[1].value,    "winter");
}

TEST(VariantSelectionParsing, WhitespaceTrimmed)
{
  // Whitespace around the comma + around each piece. Common in copy-
  // paste-from-shell-history flows.
  const auto entries = ParseVariantSelections(
      "  /World/A : setA = valA , /World/B : setB = valB  ");
  ASSERT_EQ(entries.size(), 2u);
  EXPECT_EQ(entries[0].primPath, "/World/A");
  EXPECT_EQ(entries[0].setName,  "setA");
  EXPECT_EQ(entries[0].value,    "valA");
  EXPECT_EQ(entries[1].primPath, "/World/B");
  EXPECT_EQ(entries[1].setName,  "setB");
  EXPECT_EQ(entries[1].value,    "valB");
}

TEST(VariantSelectionParsing, EmptySpecProducesNoRecords)
{
  EXPECT_TRUE(ParseVariantSelections("").empty());
  EXPECT_TRUE(ParseVariantSelections("   ").empty());
  EXPECT_TRUE(ParseVariantSelections(",,").empty());
}

TEST(VariantSelectionParsing, MalformedEntriesDropped)
{
  // Missing '=' (path:set without value).
  EXPECT_TRUE(ParseVariantSelections("/World/A:lod").empty());
  // Missing ':' (path=value, no setName).
  EXPECT_TRUE(ParseVariantSelections("/World/A=lod").empty());
  // Relative path (must start with '/').
  EXPECT_TRUE(ParseVariantSelections("World/A:lod=high").empty());
  // Empty value.
  EXPECT_TRUE(ParseVariantSelections("/World/A:lod=").empty());
  // Empty set name.
  EXPECT_TRUE(ParseVariantSelections("/World/A:=high").empty());
  // Empty path.
  EXPECT_TRUE(ParseVariantSelections(":lod=high").empty());
}

TEST(VariantSelectionParsing, MalformedEntriesDontPoisonGoodOnes)
{
  // Mixed list: middle entry is malformed, ends should survive.
  const auto entries = ParseVariantSelections(
      "/World/Good:setA=val,bogus,/World/Better:setB=val2");
  ASSERT_EQ(entries.size(), 2u);
  EXPECT_EQ(entries[0].primPath, "/World/Good");
  EXPECT_EQ(entries[0].setName,  "setA");
  EXPECT_EQ(entries[0].value,    "val");
  EXPECT_EQ(entries[1].primPath, "/World/Better");
  EXPECT_EQ(entries[1].setName,  "setB");
  EXPECT_EQ(entries[1].value,    "val2");
}

TEST(VariantSelectionParsing, MissingArgvValueIsInvalid)
{
  const char* argv[] = {"pyxis.exe", "--variant"};
  const CliArgs cli = Parse(2, const_cast<char**>(argv));
  EXPECT_TRUE(cli.invalid);
}
