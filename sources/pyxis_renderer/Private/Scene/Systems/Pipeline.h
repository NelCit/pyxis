// Pyxis renderer — system registration entry point.
//
// Plan §8.2 / §30.11. Called once at SceneWorld::Init time to register
// every per-phase system into the custom pipeline.

#pragma once

#include <flecs.h>

namespace pyxis::scene {

// Registers all per-phase systems. M0 ships the no-op stubs; later
// milestones replace each `Run*` body with the real implementation.
void RegisterSystems(flecs::world& world);

}  // namespace pyxis::scene
