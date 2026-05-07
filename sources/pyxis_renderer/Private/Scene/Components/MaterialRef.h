// Pyxis renderer — MaterialRef pair tag.
//
// Used as the relationship tag in `(Instance, MaterialOf, materialEntity)`
// pairs (§8.1, §30.11). Empty struct on purpose — it's a Flecs tag, not a
// data carrier. Keeping the *MaterialRef.h* name lines up with §2's
// folder layout, but conceptually this file declares the `MaterialOf`
// pair tag.

#pragma once

namespace pyxis::scene {

// Pair tag: relate Instance entity to its Material entity.
struct MaterialOf {};

}  // namespace pyxis::scene
