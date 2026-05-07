// Pyxis renderer — Transform component.
//
// 4x4 row-major affine world transform (§10 row-major rule). Stored byte-
// for-byte the same on the GPU side; the closesthit shader reads it as a
// float4x4. POD layout — no hlslpp dependency in the component header so
// we don't drag the math library through every Flecs include.

#pragma once

#include <cstdint>

namespace pyxis::scene {

struct Transform {
    // Row-major. row 0 = m[0..3], row 1 = m[4..7], row 2 = m[8..11], row 3 = m[12..15].
    float m[16] = { 1.f, 0.f, 0.f, 0.f,
                    0.f, 1.f, 0.f, 0.f,
                    0.f, 0.f, 1.f, 0.f,
                    0.f, 0.f, 0.f, 1.f };
};

}  // namespace pyxis::scene
