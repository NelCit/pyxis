// Pyxis renderer — Dirty<T> tag components.
//
// Zero-size tags. Added when something changes, removed in the
// `System_ClearDirtyFlags` phase at the end of each CommitResources tick.
// Plan §8.1 / §30.11.
//
// Each tag is its own type so Flecs queries can match a specific dirty
// kind without a runtime mask. The `Dirty<T>` template gives us tag
// names tied to component types where useful (e.g. Dirty<Topology>).

#pragma once

namespace pyxis::scene {

// Specialised, named dirty axes. Flecs can query for them by name; in
// systems we use the explicit type so reviewers see what work each system
// does. Plan §8.1.
struct DirtyTopology     {};   // Mesh structure changed; rebuild BLAS.
struct DirtyTransform    {};   // Instance transform changed; refit TLAS.
struct DirtyMaterial     {};   // Material params changed; re-upload GPU layout.
struct DirtyVisibility   {};   // Instance hidden/shown; mark TLAS dirty.
struct DirtyTexture      {};   // Texture upload pending.
struct DirtyLight        {};   // Light params changed.
struct DirtyTextureGpu   {};   // Bindless slot pending update (post-decode).

}  // namespace pyxis::scene
