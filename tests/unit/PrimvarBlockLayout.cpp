// Pyxis — IsCoherentBlockLayout unit tests.
//
// Pins the elementSize coherence decision that StageWalker's
// ComputeFlattenedChecked uses to tell a genuine array-per-element (block)
// primvar apart from a DCC-mis-authored ("bogus") elementSize. The header is
// USD-free, so these tests are pure arithmetic.

#include "PrimvarBlockLayout.h"

#include <gtest/gtest.h>

using pyxis::usd_ingest::IsCoherentBlockLayout;

// elementSize 1 (and 0) is the standard single-value-per-element case — never a
// block; the caller flattens directly.
TEST(PrimvarBlockLayout, ElementSizeOneIsNeverABlock) {
  const int indices[] = {0, 1, 2, 5, 99};
  EXPECT_FALSE(IsCoherentBlockLayout(1, 100, indices, 5));
  EXPECT_FALSE(IsCoherentBlockLayout(0, 100, indices, 5));
}

// The OpenPBR Playground pathology: elementSize == values.size() on a
// standard indexed primvar (one normal per face-vertex). values.size()/E == 1,
// so any index > 0 is out of block range → NOT coherent → recovered as E=1.
TEST(PrimvarBlockLayout, BogusElementSizeEqualToValueCountIsNotCoherent) {
  const int indices[] = {0, 1, 757};  // book_006_mesh: 758 values, idx up to 757
  EXPECT_FALSE(IsCoherentBlockLayout(758, 758, indices, 3));
}

// A real block primvar: 300 values = 100 logical elements of 3, indices in
// [0, 99] → coherent → the caller treats it as unconsumable for a vec channel.
TEST(PrimvarBlockLayout, GenuineBlockLayoutIsCoherent) {
  const int indices[] = {0, 50, 99};
  EXPECT_TRUE(IsCoherentBlockLayout(3, 300, indices, 3));
}

// elementSize > 1 but values.size() not divisible by it → cannot be a block
// layout → not coherent (recover as E=1).
TEST(PrimvarBlockLayout, NonDivisibleValueCountIsNotCoherent) {
  const int indices[] = {0, 1, 2};
  EXPECT_FALSE(IsCoherentBlockLayout(3, 301, indices, 3));
}

// Divisible, but an index addresses past the last block (here logicalCount=100,
// index 100 is out of range) → not coherent.
TEST(PrimvarBlockLayout, OutOfRangeBlockIndexIsNotCoherent) {
  const int indices[] = {0, 100};
  EXPECT_FALSE(IsCoherentBlockLayout(4, 400, indices, 2));
}

// Empty values can't form a block; negative indices are invalid.
TEST(PrimvarBlockLayout, EmptyValuesAndNegativeIndices) {
  const int ok[] = {0};
  EXPECT_FALSE(IsCoherentBlockLayout(3, 0, ok, 1));
  const int neg[] = {-1};
  EXPECT_FALSE(IsCoherentBlockLayout(3, 300, neg, 1));
}
