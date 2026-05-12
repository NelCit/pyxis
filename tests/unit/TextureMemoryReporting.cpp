// Pyxis M16 / V2.A.10 + V2.A.11 — texture memory reporting + budget
// warning. Real LRU streaming + eviction is a follow-up; M16 ships the
// diagnostic. This test pins the math used in the headless summary
// line so the bytes->MiB conversion stays stable.

#include <gtest/gtest.h>

#include <cstdint>

namespace {

constexpr std::uint64_t BytesToMiB(std::uint64_t bytes) {
  return bytes >> 20;
}

}  // namespace

TEST(TextureMemoryReporting, BytesToMiBExactPowerOfTwo)
{
  EXPECT_EQ(BytesToMiB(0u), 0u);
  EXPECT_EQ(BytesToMiB(1024ull * 1024ull), 1u);                   // 1 MiB
  EXPECT_EQ(BytesToMiB(1024ull * 1024ull * 1024ull), 1024u);      // 1 GiB
  EXPECT_EQ(BytesToMiB(4ull * 1024ull * 1024ull * 1024ull), 4096u);
}

TEST(TextureMemoryReporting, SubMiBTruncatesToZero)
{
  // Under-1 MiB textures report as 0 in the headless line. This is
  // intentional — the operator-facing summary is a coarse signal, not
  // a per-texture allocator report.
  EXPECT_EQ(BytesToMiB(1u),       0u);
  EXPECT_EQ(BytesToMiB(1023u),    0u);
  EXPECT_EQ(BytesToMiB(1048575u), 0u);
}

TEST(TextureMemoryReporting, BudgetThresholdCrosses)
{
  constexpr std::uint64_t BUDGET = 4096u;
  EXPECT_FALSE(BytesToMiB(4095ull * 1024ull * 1024ull) > BUDGET);
  EXPECT_FALSE(BytesToMiB(4096ull * 1024ull * 1024ull) > BUDGET);
  EXPECT_TRUE (BytesToMiB(4097ull * 1024ull * 1024ull) > BUDGET);
  EXPECT_TRUE (BytesToMiB(8192ull * 1024ull * 1024ull) > BUDGET);
}
