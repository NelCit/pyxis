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
// optional for now; M0 just allocates monotonically. Storage is uint32_t
// to match the §18.9 / §19.7 packing convention even though this is a
// Private/ type.
enum class BlasHandle : uint32_t { Invalid = 0 };

struct BlasRef {
    BlasHandle blas = BlasHandle::Invalid;
};

}  // namespace pyxis::scene
