// Pyxis renderer — handle ↔ flecs::entity bidirectional map.
//
// Plan §8.2: handle slots are O(1) by `(handle.value - 1)` index; reverse
// lookup is rare (observer-driven invariants) and goes via flecs entity
// reflection. Generation bits live in the upper 8 bits of the handle so
// use-after-destroy is detectable at Resolve() time (§19.7).
//
// Renderer-internal — no Public/ surface.

#pragma once

#include <Pyxis/Renderer/Forward.h>

#include <flecs.h>

#include <cstdint>
#include <vector>

namespace pyxis::scene {

class HandleBimap final {
public:
    HandleBimap() = default;

    // Allocates a fresh slot for `entity`. Returns the encoded handle
    // (slot + generation). Reuses retired slots with bumped generation.
    [[nodiscard]] uint32_t Allocate(flecs::entity entity) noexcept;

    // Resolves the encoded handle to its entity. Returns the null entity
    // if the slot is free or the generation has rolled past the stored
    // value (HandleStaleGeneration in §20).
    [[nodiscard]] flecs::entity Resolve(uint32_t encoded) const noexcept;

    // Marks the slot retired. The next Allocate may reuse it after
    // bumping the generation. When generation hits 255 the slot is
    // permanently quarantined (§19.7).
    void Free(uint32_t encoded) noexcept;

    [[nodiscard]] uint32_t LiveCount() const noexcept { return _live; }

private:
    struct Slot {
        flecs::entity entity;
        uint8_t       generation = 0;   // bumped on free; quarantined at 255.
        bool          live       = false;
        bool          quarantined = false;
    };

    static uint32_t Encode(uint32_t slotIndex, uint8_t gen) noexcept;
    static uint32_t SlotIndex(uint32_t encoded) noexcept;
    static uint8_t  Generation(uint32_t encoded) noexcept;

    std::vector<Slot> _slots;
    uint32_t          _live      = 0;
};

}  // namespace pyxis::scene
