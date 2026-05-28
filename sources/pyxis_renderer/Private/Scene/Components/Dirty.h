// Pyxis renderer — Dirty<T> tag components.
//
// Zero-size tags, added by the GpuScene verbs when something changes (§30.11). They
// are cleared two ways at commit time (RFC 0009):
//   - FINE-grained (DirtyTopology, DirtyTexture): the consuming system processes only
//     the tagged entities and removes the tag itself (BuildPendingBlas / UploadPending
//     Textures), so the tag must persist across the phases that read it.
//   - COARSE "buffer dirty?" (DirtyTransform, DirtyVisibility, DirtyMaterial, DirtyLight):
//     the buffer/TLAS re-packs wholesale when any are present, so Sys_ClearDirty removes
//     them at the end of each commit (PhaseClearDirty).
// Plan §8.1 / §30.11.
//
// Each tag is its own type so Flecs queries match a specific dirty kind without a
// runtime mask; systems use the explicit type so reviewers see what each one does.

#pragma once

namespace pyxis::scene {

struct DirtyTopology {};    // Mesh added; upload vertex/index + side tables + build BLAS.
struct DirtyTransform {};   // Instance transform changed; TLAS refit-eligible.
struct DirtyMaterial {};    // Material added/updated; re-pack the OpenPBR material buffer.
struct DirtyVisibility {};  // Instance added/shown/hidden/removed; structural TLAS rebuild.
struct DirtyTexture {};     // Texture decode/upload pending.
struct DirtyLight {};       // Light added/updated/removed; re-pack the light buffer.

}  // namespace pyxis::scene
