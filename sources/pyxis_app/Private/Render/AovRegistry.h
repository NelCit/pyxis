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
#include <string>
#include <string_view>

namespace pyxis::app {

struct AovEntry {
  // CLI / file-suffix token. Stable string used by --save-aov + the
  // editor's BuildAovOutputPath suffix. Lower-case, no spaces.
  std::string_view                        name;
  // UI-friendly label shown in the editor's combo. PascalCase reads
  // better than the lower-case CLI token in a dropdown.
  std::string_view                        displayLabel;
  RenderSettings::DebugView               debugView;
  // Pointer-to-member into AovTextures. Resolved at call sites via
  // `(aovs.*entry.texturePtr).Get()` to fetch the raw nvrhi::ITexture*.
  nvrhi::TextureHandle AovTextures::*    texturePtr;
};

// Order is the same as the DebugView enum so a debug-view int can
// double as an index into this table without a search. Adding a new
// AOV is one line here + matching DebugView entry + matching AovTextures
// member + matching raygen UAV binding.
inline constexpr std::array<AovEntry, 11> AOV_REGISTRY = {{
    {"color",       "Color",       RenderSettings::DebugView::Color,       &AovTextures::colorHdr   },
    {"normal",      "Normal",      RenderSettings::DebugView::Normal,      &AovTextures::normal     },
    {"depth",       "Depth",       RenderSettings::DebugView::Depth,       &AovTextures::depth      },
    {"instanceId",  "InstanceID",  RenderSettings::DebugView::InstanceId,  &AovTextures::instanceId },
    {"materialId",  "MaterialID",  RenderSettings::DebugView::MaterialId,  &AovTextures::materialId },
    {"baseColor",   "BaseColor",   RenderSettings::DebugView::BaseColor,   &AovTextures::baseColor  },
    {"worldPos",    "WorldPos",    RenderSettings::DebugView::WorldPos,    &AovTextures::worldPos   },
    // Tier 1 Hydra-canonical (alpha, elementId, Neye, Peye).
    {"alpha",       "Alpha",       RenderSettings::DebugView::Alpha,       &AovTextures::alpha      },
    {"elementId",   "ElementID",   RenderSettings::DebugView::ElementId,   &AovTextures::elementId  },
    {"normalEye",   "NormalEye",   RenderSettings::DebugView::NormalEye,   &AovTextures::normalEye  },
    {"worldPosEye", "WorldPosEye", RenderSettings::DebugView::WorldPosEye, &AovTextures::worldPosEye},
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

// Build the per-AOV output path used by both the editor's "Save
// current AOV..." button and headless's --save-aov dispatch.
// Convention: <prefix>_<name>.exr. Headless passes its --output path
// stripped of `.exr`; the editor passes a constant suggestion stem
// like "pyxis_aov".
[[nodiscard]] inline std::string BuildAovOutputPath(std::string_view prefix,
                                                    std::string_view aovName) noexcept {
  std::string result;
  result.reserve(prefix.size() + 1 + aovName.size() + 4);
  result.assign(prefix.data(), prefix.size());
  result.push_back('_');
  result.append(aovName.data(), aovName.size());
  result.append(".exr");
  return result;
}

}  // namespace pyxis::app
