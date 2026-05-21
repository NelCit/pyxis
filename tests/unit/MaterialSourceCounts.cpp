// Pyxis M18 / V2.A.8 + V2.A.18 — material translation health.
// Pin the Source enum values + the per-source breakdown logic the
// StageWalker uses. We test the enum surface (so reorders are caught
// here, not in production logs) and the counting loop logic with a
// table.

#include <Pyxis/Renderer/Descs/OpenPBRMaterialDesc.h>

#include <gtest/gtest.h>

#include <array>
#include <cstdint>

TEST(MaterialSourceCounts, EnumValuesAreStable)
{
  // Reordering values shifts the headless log table — pin them.
  // V2.A.23 added `Mdl` at index 4 (after `Default`); appending keeps
  // the existing values stable + lets the StageWalker counter logic
  // light it up without renumbering.
  EXPECT_EQ(static_cast<std::uint8_t>(
      pyxis::OpenPBRMaterialDesc::Source::UsdPreviewSurface), 0u);
  EXPECT_EQ(static_cast<std::uint8_t>(
      pyxis::OpenPBRMaterialDesc::Source::MaterialX),         1u);
  EXPECT_EQ(static_cast<std::uint8_t>(
      pyxis::OpenPBRMaterialDesc::Source::RenderManFallback), 2u);
  EXPECT_EQ(static_cast<std::uint8_t>(
      pyxis::OpenPBRMaterialDesc::Source::Default),           3u);
  EXPECT_EQ(static_cast<std::uint8_t>(
      pyxis::OpenPBRMaterialDesc::Source::Mdl),               4u);
}

TEST(MaterialSourceCounts, CounterAccumulatesPerSource)
{
  using Source = pyxis::OpenPBRMaterialDesc::Source;
  const std::array<Source, 9> table{
      Source::UsdPreviewSurface,
      Source::UsdPreviewSurface,
      Source::MaterialX,
      Source::RenderManFallback,
      Source::Default,
      Source::Default,
      Source::Default,
      Source::Mdl,
      Source::Mdl,
  };
  std::uint32_t usd = 0;
  std::uint32_t mtlx = 0;
  std::uint32_t mdl = 0;
  std::uint32_t renderman = 0;
  std::uint32_t fallback = 0;
  for (const Source src : table)
  {
    switch (src)
    {
      case Source::UsdPreviewSurface: ++usd; break;
      case Source::MaterialX:         ++mtlx; break;
      case Source::Mdl:               ++mdl; break;
      case Source::RenderManFallback: ++renderman; break;
      case Source::Default:           ++fallback; break;
    }
  }
  EXPECT_EQ(usd, 2u);
  EXPECT_EQ(mtlx, 1u);
  EXPECT_EQ(mdl, 2u);
  EXPECT_EQ(renderman, 1u);
  EXPECT_EQ(fallback, 3u);
}
