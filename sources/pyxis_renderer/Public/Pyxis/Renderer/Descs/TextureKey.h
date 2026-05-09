// Pyxis renderer — public texture-acquisition key.
//
// Plan §13 + §18.4. Input to GpuScene::AcquireTexture. Identifies a
// texture by resolved path + role + colourspace; the renderer
// deduplicates by hashing this key (path string is hashed verbatim,
// no SdfPath canonicalisation — that's the caller's responsibility).
//
// `resolvedPath` is borrowed at the call site and copied into the
// internal cache key before the call returns (§18.9). Pyxis expects
// an `ArResolver`-resolved absolute path: the ingest adapter has
// already turned a USD asset path like `@./textures/wood.<UDIM>.exr`
// into something the OS can open.

#pragma once

#include <Pyxis/Renderer/RendererApi.h>

#include <cstdint>
#include <string_view>

namespace pyxis {

struct TextureKey {
  enum class Role : uint8_t {
    BaseColor,
    NormalMap,
    RoughnessMetallic,
    Emission,
    Opacity,
    EnvLatLong,
  };

  enum class Color : uint8_t {
    SRgb,
    Linear,
  };

  std::string_view resolvedPath;
  Role role = Role::BaseColor;
  Color colorspace = Color::SRgb;
};

}  // namespace pyxis
