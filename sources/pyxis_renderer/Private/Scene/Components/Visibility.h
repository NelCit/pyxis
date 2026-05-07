// Pyxis renderer — Visibility component.
//
// Plan §8 / §15. `visible == false` instances are simply omitted from the
// TLAS at the next CommitResources tick. Wrapped in a struct so Flecs
// distinguishes it as a typed component, not a raw bool.

#pragma once

namespace pyxis::scene {

struct Visibility {
    bool visible = true;
};

}  // namespace pyxis::scene
