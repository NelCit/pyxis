// Pyxis renderer — Flecs query cache.
//
// Plan §30.11: queries are cached at registration time. Building a query
// inside a per-frame system body is a PR-blocking violation.
//
// The cache stores one `flecs::query<...>` handle per archetype the systems
// need. M0 wires the no-op systems with stub queries; later milestones
// add the real archetype queries (DirtyTopology meshes, dirty material
// uploads, etc.) here.

#pragma once

#include <flecs.h>

namespace pyxis::scene {

class QueryCache final {
public:
    explicit QueryCache(flecs::world& world);
    ~QueryCache();

    QueryCache(const QueryCache&)            = delete;
    QueryCache& operator=(const QueryCache&) = delete;

    // Cached queries are added by later milestones; this M0 cache merely
    // proves the registration-time pattern works and the unit test can
    // read it back.
    [[nodiscard]] uint32_t QueryCount() const noexcept { return _count; }

private:
    // World pointer is captured for the M3+ build-out (real flecs::query<...>
    // construction will call back through it). M0 doesn't read it; mark it
    // [[maybe_unused]] so /Werror,-Wunused-private-field doesn't fire while
    // the field is part of the public contract that lands later.
    [[maybe_unused]] flecs::world* _world = nullptr;
    uint32_t                       _count = 0;
};

}  // namespace pyxis::scene
