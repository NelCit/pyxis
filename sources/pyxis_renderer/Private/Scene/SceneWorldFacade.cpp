// Pyxis renderer — SceneWorldFacade public implementation.

#include <Pyxis/Renderer/SceneWorldFacade.h>

#include "Scene/World.h"

namespace pyxis {

struct SceneWorldFacade::Impl {
    scene::SceneWorld world;
};

SceneWorldFacade::SceneWorldFacade()  : _impl(new Impl()) {}
SceneWorldFacade::~SceneWorldFacade() { Shutdown(); delete _impl; }

SceneWorldStatus SceneWorldFacade::Init() noexcept {
    if (!_impl) return SceneWorldStatus::Unknown;
    return _impl->world.Init() ? SceneWorldStatus::Ok
                                : SceneWorldStatus::FlecsInitFailed;
}

void SceneWorldFacade::Tick() noexcept {
    if (_impl) _impl->world.Tick();
}

void SceneWorldFacade::Shutdown() noexcept {
    if (_impl) _impl->world.Shutdown();
}

bool SceneWorldFacade::IsAlive() const noexcept {
    return _impl && _impl->world.IsAlive();
}

uint64_t SceneWorldFacade::TickCount() const noexcept {
    return _impl ? _impl->world.TickCount() : 0u;
}

}  // namespace pyxis
