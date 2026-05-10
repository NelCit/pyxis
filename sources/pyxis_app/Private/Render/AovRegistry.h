// Pyxis app — AOV name / DebugView / texture registry.
//
// Single source of truth for the 7 raw AOVs the inspector exposes.
// Pre-refactor each call site (ViewerMode's Save AOV button,
// HeadlessMode's --save-aov dispatch, ImGuiHost's suggested-filename
// switch) carried its own switch on the AOV identifier; adding a new
// AOV meant updating three places. The registry is one constexpr
// table that every dispatch iterates.
//
// Each entry pairs:
//   name        : `--save-aov` token + Save AOV file-suffix
//   debugView   : RenderSettings::DebugView enum value (the value
//                 the editor combo + raygen branch on)
//   texturePtr  : pointer-to-member into AovTextures pointing at the
//                 raw-AOV TextureHandle the renderer writes
//
// The registry intentionally lives in pyxis_app/Private rather than
// the public renderer surface — AovTextures is caller-allocated
// (§18.4) so the registry knows about both halves (renderer-side
// DebugView + app-side AovTextures) and bridges them.

#pragma once

#include "Render/AovTextures.h"

#include <Pyxis/Renderer/Descs/RenderSettings.h>

#include <array>
#include <string_view>

namespace pyxis::app {

struct AovEntry {
  std::string_view                        name;
  RenderSettings::DebugView               debugView;
  // Pointer-to-member into AovTextures. Resolved at call sites via
  // `(aovs.*entry.texturePtr).Get()` to fetch the raw nvrhi::ITexture*.
  nvrhi::TextureHandle AovTextures::*    texturePtr;
};

// Order is the same as the DebugView enum so a debug-view int can
// double as an index into this table without a search. Adding a new
// AOV is one line here + matching DebugView entry + matching AovTextures
// member + matching raygen UAV binding.
inline constexpr std::array<AovEntry, 7> AOV_REGISTRY = {{
    {"color",      RenderSettings::DebugView::Color,      &AovTextures::colorHdr  },
    {"normal",     RenderSettings::DebugView::Normal,     &AovTextures::normal    },
    {"depth",      RenderSettings::DebugView::Depth,      &AovTextures::depth     },
    {"instanceId", RenderSettings::DebugView::InstanceId, &AovTextures::instanceId},
    {"materialId", RenderSettings::DebugView::MaterialId, &AovTextures::materialId},
    {"baseColor",  RenderSettings::DebugView::BaseColor,  &AovTextures::baseColor },
    {"worldPos",   RenderSettings::DebugView::WorldPos,   &AovTextures::worldPos  },
}};

// Resolve a DebugView enum to its registry entry, or nullptr if the
// debug-view value is out of range. Used by ViewerMode's Save AOV
// button to pick (a) the source texture for the readback and
// (b) the filename suffix.
[[nodiscard]] inline const AovEntry* FindAovByDebugView(
    RenderSettings::DebugView debugView) noexcept {
  for (const AovEntry& entry : AOV_REGISTRY)
  {
    if (entry.debugView == debugView)
      return &entry;
  }
  return nullptr;
}

// Resolve a `--save-aov` token to its registry entry, or nullptr if
// unknown. Headless's parser uses this to dispatch one save per
// comma-separated entry.
[[nodiscard]] inline const AovEntry* FindAovByName(std::string_view name) noexcept {
  for (const AovEntry& entry : AOV_REGISTRY)
  {
    if (entry.name == name)
      return &entry;
  }
  return nullptr;
}

}  // namespace pyxis::app
