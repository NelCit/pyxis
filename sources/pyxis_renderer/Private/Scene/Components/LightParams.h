// Pyxis renderer — LightParams component.
//
// Plan §8 / §25.G. v1 supports Distant, Dome, Rect (§42 caps the list).
// POD layout sized so Flecs archetypes stay tight.

#pragma once

#include <cstdint>

namespace pyxis::scene {

enum class LightKind : uint8_t {
  Distant = 0,
  Dome = 1,
  Rect = 2,
};

struct LightParams {
  LightKind kind = LightKind::Distant;
  uint8_t doubleSided = 0;  // rect lights only.
  uint16_t pad0 = 0;        // explicit padding for layout / alignment.
  float color[3] = {1.f, 1.f, 1.f};
  float intensity = 1.f;
  float direction[3] = {0.f, -1.f, 0.f};  // distant lights.
  float position[3] = {0.f, 0.f, 0.f};    // rect lights.
  float axisU[3] = {1.f, 0.f, 0.f};
  float axisV[3] = {0.f, 1.f, 0.f};
  uint32_t envMapBindless = 0;  // dome lights — bindless slot.
  uint32_t pad1 = 0;            // explicit padding.
};

}  // namespace pyxis::scene
