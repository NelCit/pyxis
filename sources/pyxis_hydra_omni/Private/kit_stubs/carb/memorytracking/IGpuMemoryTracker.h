// RFC 0004 — empty stand-in for NVIDIA's UNSHIPPED
// carb/memorytracking/IGpuMemoryTracker.h.
//
// omni/kit/renderer/IRenderer.h #includes this header but uses NOTHING from the
// carb::memorytracking namespace anywhere in the transitive closure omni.ui pulls
// (verified by scanning all 204 reachable headers). So an empty stub satisfies
// the include with zero ABI risk. Drop it if a future Kit ships the real header.

#pragma once
