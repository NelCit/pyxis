// Pyxis renderer — BlasRef component.
//
// Attached to a Mesh entity once §16's BlasCache has built (or refit) the
// BLAS for that mesh. The handle indexes into BlasCache; the Cache owns
// the GPU acceleration structure. Plan §8.

#pragma once

#include <cstdint>

namespace pyxis::scene {

// Opaque BLAS handle. Renderer-internal so we don't reuse pyxis::MeshHandle
// here (the latter is the public surface — §18.2). Generation bits are
// optional for now; M0 just allocates monotonically.
enum class BlasHandle : uint32_t { kInvalid = 0 };

struct BlasRef {
    BlasHandle blas = BlasHandle::kInvalid;
};

}  // namespace pyxis::scene
