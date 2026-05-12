// Pyxis V2.A.15 — UsdStagePopulationMask parser unit test.
//
// The IngestUsd helper turns the comma-separated `--population-mask`
// CLI argument into a UsdStagePopulationMask. This test mirrors the
// parsing logic + asserts the resulting mask contains the expected
// SdfPath set.

#include <pxr/usd/sdf/path.h>
#include <pxr/usd/usd/stagePopulationMask.h>

#include <gtest/gtest.h>

#include <cctype>
#include <string>
#include <string_view>

namespace {

[[nodiscard]] pxr::UsdStagePopulationMask ParseMask(std::string_view spec) noexcept
{
  pxr::UsdStagePopulationMask mask;
  std::size_t cursor = 0;
  while (cursor < spec.size())
  {
    const std::size_t comma = spec.find(',', cursor);
    const std::size_t end = (comma == std::string_view::npos) ? spec.size() : comma;
    std::size_t start = cursor;
    while (start < end && std::isspace(static_cast<unsigned char>(spec[start])))
      ++start;
    std::size_t stop = end;
    while (stop > start && std::isspace(static_cast<unsigned char>(spec[stop - 1])))
      --stop;
    if (stop > start)
    {
      const std::string token{spec.substr(start, stop - start)};
      const pxr::SdfPath path(token);
      if (path.IsAbsolutePath())
        mask.Add(path);
    }
    if (comma == std::string_view::npos)
      break;
    cursor = comma + 1u;
  }
  return mask;
}

}  // namespace

TEST(PopulationMaskParsing, SinglePath)
{
  const auto mask = ParseMask("/World/Hero");
  ASSERT_EQ(mask.GetPaths().size(), 1u);
  EXPECT_EQ(mask.GetPaths()[0].GetString(), "/World/Hero");
}

TEST(PopulationMaskParsing, MultiPath)
{
  const auto mask = ParseMask("/World/Hero,/World/Lights");
  ASSERT_EQ(mask.GetPaths().size(), 2u);
}

TEST(PopulationMaskParsing, WhitespaceTrimmed)
{
  const auto mask = ParseMask("  /World/Hero , /World/Lights ");
  ASSERT_EQ(mask.GetPaths().size(), 2u);
  EXPECT_EQ(mask.GetPaths()[0].GetString(), "/World/Hero");
  EXPECT_EQ(mask.GetPaths()[1].GetString(), "/World/Lights");
}

TEST(PopulationMaskParsing, RelativePathRejected)
{
  // Relative paths can't anchor a population mask — drop with a
  // warning (the warning fires in IngestUsd.cpp; here we just assert
  // the mask stays empty).
  const auto mask = ParseMask("World/Hero");
  EXPECT_EQ(mask.GetPaths().size(), 0u);
}

TEST(PopulationMaskParsing, EmptyInputProducesEmptyMask)
{
  EXPECT_EQ(ParseMask("").GetPaths().size(), 0u);
  EXPECT_EQ(ParseMask("   ").GetPaths().size(), 0u);
  EXPECT_EQ(ParseMask(",,").GetPaths().size(), 0u);
}
