// Pyxis USD ingest — indexed-primvar elementSize coherence test.
//
// USD's `elementSize` is the number of typed values that make up one LOGICAL
// primvar element; an index i addresses the block values[i*E : i*E + E]. For
// the vector channels StageWalker consumes (normals / st / displayColor /
// tangents) a logical element is a single vec, so E is 1. But two non-1 cases
// must be told apart (see ComputeFlattenedChecked in StageWalker.cpp):
//
//  - a COHERENT block primvar (E describes a real array-per-element layout) —
//    values laid out as logicalCount*E with every index addressing an in-range
//    E-block. We can't consume a block as a single vec, so the caller treats it
//    as unauthored.
//  - a BOGUS elementSize — DCC/FBX exporters mis-author it as values.size() on
//    an otherwise-standard indexed primvar (the OpenPBR Shader Playground; the
//    same data also crashes Blender 4.5/5.0 + Cinema4D 2026.1, see
//    OpenPBRShaderPlayground issue #15 / PR #18). Here the block interpretation
//    is out of range; the caller flattens as elementSize 1 and recovers the
//    authored values.
//
// This header carries the pure (USD-free) decision so it is unit-testable in
// isolation.

#pragma once

#include <cstddef>

namespace pyxis::usd_ingest {

// True iff `elementSize` is a coherent block layout over `valueCount` values
// indexed by `indices[0..indexCount)`: i.e. elementSize > 1, valueCount is a
// whole number of E-blocks, and every index addresses an in-range block. A
// false return means elementSize <= 1 OR the layout is incoherent (a bogus
// elementSize), and the caller should flatten the indexed primvar as
// elementSize 1.
[[nodiscard]] inline bool IsCoherentBlockLayout(int elementSize,
                                                std::size_t valueCount,
                                                const int* indices,
                                                std::size_t indexCount) noexcept
{
  if (elementSize <= 1)
    return false;
  const std::size_t blockSize = static_cast<std::size_t>(elementSize);
  if (valueCount == 0 || (valueCount % blockSize) != 0)
    return false;
  const std::size_t logicalCount = valueCount / blockSize;
  for (std::size_t i = 0; i < indexCount; ++i)
  {
    if (indices[i] < 0 || static_cast<std::size_t>(indices[i]) >= logicalCount)
      return false;
  }
  return true;
}

}  // namespace pyxis::usd_ingest
