// Pyxis renderer — QueryCache implementation (M0 stub).

#include "Scene/Queries/QueryCache.h"

namespace pyxis::scene {

QueryCache::QueryCache(flecs::world& world) : _world(&world), _count(0) {
    // Real queries land in M3+ (e.g. dirty-topology meshes for BLAS build).
    // M0 just records that the cache was constructed; SceneWorldInit
    // asserts QueryCount() == 0 for now.
}

QueryCache::~QueryCache() = default;

}  // namespace pyxis::scene
