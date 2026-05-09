// Pyxis renderer — SceneWorld implementation.

#include "Scene/World.h"

#include "Scene/Components/BlasRef.h"
#include "Scene/Components/Dirty.h"
#include "Scene/Components/Geom.h"
#include "Scene/Components/LightParams.h"
#include "Scene/Components/MaterialGpu.h"
#include "Scene/Components/MaterialRef.h"
#include "Scene/Components/MeshRef.h"
#include "Scene/Components/TextureGpu.h"
#include "Scene/Components/Transform.h"
#include "Scene/Components/Visibility.h"
#include "Scene/Phases.h"
#include "Scene/Systems/Pipeline.h"

#include <Pyxis/Platform/Logging/Log.h>
#include <Pyxis/Platform/Logging/LogCategories.h>

#if defined(PYXIS_DEBUG_TOOLS) && defined(FLECS_REST)
#include <flecs/addons/rest.h>
#endif

namespace pyxis::scene {

SceneWorld::SceneWorld() = default;
SceneWorld::~SceneWorld() {
  Shutdown();
}

bool SceneWorld::Init() noexcept {
  if (_alive)
    return true;

  // Flecs's flecs::world ctor allocates internally. The renderer perimeter
  // is /EHs-c- (§30.6) so a try/catch is unavailable; we rely on the
  // PYXIS_NO_EXCEPTIONS path's std::set_terminate handler (§49) to flush
  // logs + abort cleanly if the (rare) bad_alloc fires.
  _world = std::make_unique<flecs::world>();
  if (!_world)
    return false;

  RegisterComponents();

  // §30.11: build the cached query set at registration time, never per-frame.
  _queries = std::make_unique<QueryCache>(*_world);

  RegisterPhasesAndSystems();

#if defined(PYXIS_DEBUG_TOOLS) && defined(FLECS_REST)
  // Flecs Explorer at http://localhost:27750 (Debug only — §30.11 / §4).
  _world->set<flecs::Rest>({});
  Logging::Get().Info(log::RENDER, "Flecs Explorer up at http://localhost:27750");
#endif

  _alive = true;
  _tickCount = 0;
  Logging::Get().Info(log::RENDER, "SceneWorld: initialised, phase pipeline live");
  return true;
}

void SceneWorld::Shutdown() noexcept {
  if (!_alive)
    return;
  _queries.reset();
  _world.reset();
  _alive = false;
  _tickCount = 0;
}

void SceneWorld::Tick() noexcept {
  if (!_alive || !_world)
    return;
  _world->progress();
  ++_tickCount;
}

void SceneWorld::RegisterComponents() noexcept {
  auto& world = *_world;
  world.component<Geom>();
  world.component<Transform>();
  world.component<Visibility>();
  world.component<MaterialOf>();
  world.component<MeshOf>();
  world.component<BlasRef>();
  world.component<MaterialGpu>();
  world.component<TextureGpu>();
  world.component<LightParams>();

  // Dirty tags.
  world.component<DirtyTopology>();
  world.component<DirtyTransform>();
  world.component<DirtyMaterial>();
  world.component<DirtyVisibility>();
  world.component<DirtyTexture>();
  world.component<DirtyLight>();
  world.component<DirtyTextureGpu>();
}

void SceneWorld::RegisterPhasesAndSystems() noexcept {
  RegisterPhasePipeline(*_world);
  RegisterSystems(*_world);
}

}  // namespace pyxis::scene
