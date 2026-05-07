// Pyxis renderer — SceneWorld (Private/Scene/World.h).
//
// Plan §8 / §30.11. The Flecs-backed scene representation. Owned by
// SceneWorldFacade (Public/) and never exposed directly outside this
// module — no Flecs type appears in any Public/ header.

#pragma once

#include "Scene/HandleBimap.h"
#include "Scene/Queries/QueryCache.h"

#include <flecs.h>

#include <atomic>
#include <cstdint>
#include <memory>

namespace pyxis::scene {

class SceneWorld final {
public:
    SceneWorld();
    ~SceneWorld();

    SceneWorld(const SceneWorld&)            = delete;
    SceneWorld& operator=(const SceneWorld&) = delete;

    // ------------------------------------------------------------------
    // Lifecycle. Init() returns false on Flecs failure (rare; we surface
    // the boolean back to the SceneWorldFacade::Init() status enum).
    // ------------------------------------------------------------------
    [[nodiscard]] bool Init() noexcept;
    void               Shutdown() noexcept;
    void               Tick() noexcept;

    [[nodiscard]] bool      IsAlive()   const noexcept { return _alive; }
    [[nodiscard]] uint64_t  TickCount() const noexcept { return _tickCount; }

    // ------------------------------------------------------------------
    // Internal accessors — for systems and observers, never Public/.
    // ------------------------------------------------------------------
    [[nodiscard]] flecs::world&       World()          noexcept { return *_world; }
    [[nodiscard]] HandleBimap&        Meshes()         noexcept { return _meshHandles; }
    [[nodiscard]] HandleBimap&        Materials()      noexcept { return _materialHandles; }
    [[nodiscard]] HandleBimap&        Textures()       noexcept { return _textureHandles; }
    [[nodiscard]] HandleBimap&        Instances()      noexcept { return _instanceHandles; }
    [[nodiscard]] HandleBimap&        Lights()         noexcept { return _lightHandles; }

private:
    void RegisterComponents() noexcept;
    void RegisterPhasesAndSystems() noexcept;

    std::unique_ptr<flecs::world> _world;
    std::unique_ptr<QueryCache>   _queries;

    HandleBimap _meshHandles;
    HandleBimap _materialHandles;
    HandleBimap _textureHandles;
    HandleBimap _instanceHandles;
    HandleBimap _lightHandles;

    bool      _alive     = false;
    uint64_t  _tickCount = 0;
};

}  // namespace pyxis::scene
