// Pyxis PR6 / V2.A.2 — UsdGeomImageable purpose-LOD spec parser.
//
// Pins ParsePurposeFilterSpec (declared in StageWalker.h) so the
// CLI's `--render-purpose <list>` round-trips the same way the
// goldens harness's `regression.json` "render_purpose" field does.

#include <Pyxis/UsdIngest/StageWalker.h>

#include <gtest/gtest.h>

using pyxis::usd_ingest::ParsePurposeFilterSpec;
using pyxis::usd_ingest::PURPOSE_FILTER_DEFAULT;
using pyxis::usd_ingest::PURPOSE_FILTER_RENDER;
using pyxis::usd_ingest::PURPOSE_FILTER_PROXY;
using pyxis::usd_ingest::PURPOSE_FILTER_GUIDE;
using pyxis::usd_ingest::PURPOSE_FILTER_DEFAULT_MASK;

TEST(PurposeFilterSpecParsing, EmptyInputUsesDefaultMask)
{
  EXPECT_EQ(ParsePurposeFilterSpec(""), PURPOSE_FILTER_DEFAULT_MASK);
  EXPECT_EQ(ParsePurposeFilterSpec("   "), PURPOSE_FILTER_DEFAULT_MASK);
  EXPECT_EQ(ParsePurposeFilterSpec(",,"), PURPOSE_FILTER_DEFAULT_MASK);
}

TEST(PurposeFilterSpecParsing, SingleTokenParses)
{
  EXPECT_EQ(ParsePurposeFilterSpec("default"), PURPOSE_FILTER_DEFAULT);
  EXPECT_EQ(ParsePurposeFilterSpec("render"),  PURPOSE_FILTER_RENDER);
  EXPECT_EQ(ParsePurposeFilterSpec("proxy"),   PURPOSE_FILTER_PROXY);
  EXPECT_EQ(ParsePurposeFilterSpec("guide"),   PURPOSE_FILTER_GUIDE);
}

TEST(PurposeFilterSpecParsing, DefaultMaskExpandsToDefaultAndRender)
{
  // The production default mask is `default | render` — the same
  // result a user would type explicitly.
  EXPECT_EQ(ParsePurposeFilterSpec("default,render"),
            PURPOSE_FILTER_DEFAULT | PURPOSE_FILTER_RENDER);
  EXPECT_EQ(PURPOSE_FILTER_DEFAULT_MASK,
            PURPOSE_FILTER_DEFAULT | PURPOSE_FILTER_RENDER);
}

TEST(PurposeFilterSpecParsing, AllFourTokensCombine)
{
  EXPECT_EQ(ParsePurposeFilterSpec("default,render,proxy,guide"),
            PURPOSE_FILTER_DEFAULT | PURPOSE_FILTER_RENDER
                | PURPOSE_FILTER_PROXY | PURPOSE_FILTER_GUIDE);
}

TEST(PurposeFilterSpecParsing, WhitespaceTolerated)
{
  EXPECT_EQ(ParsePurposeFilterSpec(" default , proxy "),
            PURPOSE_FILTER_DEFAULT | PURPOSE_FILTER_PROXY);
  EXPECT_EQ(ParsePurposeFilterSpec("render\tguide"),
            PURPOSE_FILTER_RENDER | PURPOSE_FILTER_GUIDE);
}

TEST(PurposeFilterSpecParsing, UnknownTokensSilentlyDropped)
{
  // "renders" (typo) drops; "default" still parses.
  EXPECT_EQ(ParsePurposeFilterSpec("default,renders"), PURPOSE_FILTER_DEFAULT);
  // All-unknown input collapses to the default mask (matches the
  // empty-input contract — the operator-side log surfaces the spec
  // verbatim so the typo is grep-visible).
  EXPECT_EQ(ParsePurposeFilterSpec("Default,Render"),  // case-sensitive
            PURPOSE_FILTER_DEFAULT_MASK);
}

TEST(PurposeFilterSpecParsing, DuplicateTokensIdempotent)
{
  EXPECT_EQ(ParsePurposeFilterSpec("render,render,render"),
            PURPOSE_FILTER_RENDER);
}

TEST(PurposeFilterSpecParsing, ProxyOnlyForFastPreview)
{
  // The "fast preview" idiom — operator overrides to just proxy. The
  // default render-purpose stays out so the higher-cost render prims
  // are dropped at ingest.
  EXPECT_EQ(ParsePurposeFilterSpec("default,proxy"),
            PURPOSE_FILTER_DEFAULT | PURPOSE_FILTER_PROXY);
}
