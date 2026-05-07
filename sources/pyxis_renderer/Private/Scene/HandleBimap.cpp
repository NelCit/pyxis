// Pyxis renderer — HandleBimap implementation.

#include "Scene/HandleBimap.h"

namespace pyxis::scene {

uint32_t HandleBimap::Encode(uint32_t slotIndex, uint8_t gen) noexcept {
    return ((static_cast<uint32_t>(gen) << HANDLE_SLOT_BITS) & HANDLE_GENERATION_MASK)
         | ((slotIndex + 1u) & HANDLE_SLOT_MASK);
}

uint32_t HandleBimap::SlotIndex(uint32_t encoded) noexcept {
    const uint32_t raw = (encoded & HANDLE_SLOT_MASK);
    return raw == 0u ? 0u : raw - 1u;
}

uint8_t HandleBimap::Generation(uint32_t encoded) noexcept {
    return static_cast<uint8_t>((encoded & HANDLE_GENERATION_MASK) >> HANDLE_SLOT_BITS);
}

uint32_t HandleBimap::Allocate(flecs::entity entity) noexcept {
    // Try to reuse a retired slot first.
    for (uint32_t i = 0; i < _slots.size(); ++i) {
        Slot& s = _slots[i];
        if (!s.live && !s.quarantined) {
            s.entity     = entity;
            s.live       = true;
            const uint32_t encoded = Encode(i, s.generation);
            ++_live;
            return encoded;
        }
    }

    // Otherwise, append.
    Slot s{};
    s.entity     = entity;
    s.generation = 0;
    s.live       = true;
    s.quarantined = false;
    _slots.push_back(s);
    ++_live;
    return Encode(static_cast<uint32_t>(_slots.size() - 1), 0);
}

flecs::entity HandleBimap::Resolve(uint32_t encoded) const noexcept {
    if (encoded == 0u) return flecs::entity{};
    const uint32_t idx = SlotIndex(encoded);
    if (idx >= _slots.size()) return flecs::entity{};
    const Slot& s = _slots[idx];
    if (!s.live || s.quarantined) return flecs::entity{};
    if (s.generation != Generation(encoded)) return flecs::entity{};
    return s.entity;
}

void HandleBimap::Free(uint32_t encoded) noexcept {
    if (encoded == 0u) return;
    const uint32_t idx = SlotIndex(encoded);
    if (idx >= _slots.size()) return;
    Slot& s = _slots[idx];
    if (!s.live) return;

    s.live   = false;
    s.entity = flecs::entity{};
    if (s.generation == 0xFFu) {
        // §19.7 — quarantine the slot; never reuse.
        s.quarantined = true;
    } else {
        ++s.generation;
    }
    --_live;
}

}  // namespace pyxis::scene
