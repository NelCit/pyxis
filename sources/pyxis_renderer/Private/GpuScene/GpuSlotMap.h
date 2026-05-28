// Pyxis renderer — Flecs-backed handle/slot map for the GpuScene entity types.
//
// RFC 0009: the §8 Flecs SceneWorld is the scene representation. The GpuScene
// per-entity stores (materials, meshes, instances, textures, volumes) used to be
// std::vector<*Entry> with a parallel free-list + per-entry generation. This map
// replaces that bookkeeping: the variable-length desc lives on a Flecs ENTITY (as a
// component), and this object owns ONLY the handle table — slot index, generation,
// liveness, and the slot->entity lookup. One instance per entity type, all sharing
// the GpuScene's single flecs::world.
//
// CRITICAL — byte-identity: the slot index IS the GPU buffer index for the
// GPU-indexed types (material/mesh/instance buffers are packed `buffer[slot]`, and
// cross-references store HandleSlot(handle)). So this map reproduces the EXACT
// gpuscene_detail::HandleEncode layout (slot 0 reserved as the Invalid sentinel,
// 24-bit slot + 8-bit generation, §19.7) and the LIFO free-slot reuse of the old
// per-verb bodies. Migrating a type therefore leaves every GPU buffer + every
// rendered pixel unchanged.
//
// PRIVATE — only files under Private/GpuScene/ include it; no Flecs type escapes
// to Public/ (§18.9 / §30.11).

#pragma once

#include <Pyxis/Renderer/Forward.h>

#include <flecs.h>

#include <cstdint>
#include <vector>

namespace pyxis {

class GpuSlotMap final
{
 public:
  // The shared world is borrowed; the map creates/destructs entities in it.
  explicit GpuSlotMap(flecs::world& world) noexcept : _world(&world)
  {
    // Slot 0 is the §19.7 Invalid sentinel — permanently quarantined so a handle
    // whose slot decodes to 0 never resolves (matches the old `entries[0]` sentinel).
    _slots.emplace_back();
    _slots[0].quarantined = true;
  }

  // §19.7 handle layout — identical to gpuscene_detail::HandleEncode.
  [[nodiscard]] static uint32_t Encode(uint32_t slot, uint8_t generation) noexcept
  {
    return (slot & HANDLE_SLOT_MASK) | (static_cast<uint32_t>(generation) << HANDLE_SLOT_BITS);
  }
  [[nodiscard]] static uint32_t SlotOf(uint32_t handle) noexcept { return handle & HANDLE_SLOT_MASK; }
  [[nodiscard]] static uint8_t GenerationOf(uint32_t handle) noexcept
  {
    return static_cast<uint8_t>(handle >> HANDLE_SLOT_BITS);
  }

  // Allocate a slot (LIFO reuse of a freed slot, else append) + a fresh entity for
  // it. Returns the encoded handle, or 0 (Invalid) if the 24-bit slot space is
  // exhausted. `outEntity` receives the new entity (set its component next).
  [[nodiscard]] uint32_t Allocate(flecs::entity& outEntity) noexcept
  {
    uint32_t slot = 0;
    if (!_freeSlots.empty())
    {
      slot = _freeSlots.back();
      _freeSlots.pop_back();
    }
    else
    {
      if (_slots.size() >= (1u << HANDLE_SLOT_BITS))
        return 0;  // slot space exhausted (§18.5 Invalid fallback).
      _slots.emplace_back();
      slot = static_cast<uint32_t>(_slots.size() - 1);
    }
    Slot& record = _slots[slot];
    record.live = true;
    record.entity = _world->entity();
    outEntity = record.entity;
    ++_live;
    return Encode(slot, record.generation);
  }

  // Resolve the encoded handle to its entity, or the null entity for
  // Invalid / out-of-range / dead / stale-generation handles.
  [[nodiscard]] flecs::entity Resolve(uint32_t handle) const noexcept
  {
    if (handle == 0)
      return flecs::entity{};
    const uint32_t slot = SlotOf(handle);
    if (slot == 0 || slot >= _slots.size())
      return flecs::entity{};
    const Slot& record = _slots[slot];
    if (!record.live || record.quarantined || record.generation != GenerationOf(handle))
      return flecs::entity{};
    return record.entity;
  }

  // Retire the slot: destruct its entity, bump the generation (or quarantine at 255,
  // §19.7) and push the slot for LIFO reuse. No-op for unknown/dead handles.
  void Free(uint32_t handle) noexcept
  {
    const uint32_t slot = SlotOf(handle);
    if (slot == 0 || slot >= _slots.size())
      return;
    Slot& record = _slots[slot];
    if (!record.live)
      return;
    record.live = false;
    if (record.entity.is_alive())
      record.entity.destruct();
    record.entity = flecs::entity{};
    if (record.generation == 0xFFu)  // §19.7 — generation 255 quarantines the slot.
    {
      record.quarantined = true;
    }
    else
    {
      ++record.generation;
      _freeSlots.push_back(slot);
    }
    --_live;
  }

  // The encoded handle currently occupying `slot` (for editor "Nth live" accessors).
  [[nodiscard]] uint32_t HandleForSlot(uint32_t slot) const noexcept
  {
    return (slot == 0 || slot >= _slots.size()) ? 0u : Encode(slot, _slots[slot].generation);
  }

  // Slot-table size (== old `entries.size()`) — the GPU buffer is sized to this so
  // dead/sentinel slots keep their index (buffer[slot] stays stable).
  [[nodiscard]] uint32_t SlotCount() const noexcept { return static_cast<uint32_t>(_slots.size()); }
  [[nodiscard]] uint32_t LiveCount() const noexcept { return _live; }
  [[nodiscard]] bool IsLive(uint32_t slot) const noexcept
  {
    return slot != 0 && slot < _slots.size() && _slots[slot].live;
  }
  [[nodiscard]] flecs::entity EntityAtSlot(uint32_t slot) const noexcept
  {
    return (slot == 0 || slot >= _slots.size()) ? flecs::entity{} : _slots[slot].entity;
  }

  // Visit live slots in ascending slot order (the deterministic packing order).
  // `fn(uint32_t slot, flecs::entity entity)`.
  template <typename Fn>
  void ForEachLiveSlot(Fn&& visitor) const
  {
    for (uint32_t slot = 1; slot < _slots.size(); ++slot)
      if (_slots[slot].live)
        visitor(slot, _slots[slot].entity);
  }

  // Destruct every entity + reset to the fresh "sentinel only" state (Clear()).
  void Reset() noexcept
  {
    for (uint32_t slot = 1; slot < _slots.size(); ++slot)
      if (_slots[slot].live && _slots[slot].entity.is_alive())
        _slots[slot].entity.destruct();
    _slots.clear();
    _freeSlots.clear();
    _slots.emplace_back();
    _slots[0].quarantined = true;
    _live = 0;
  }

 private:
  struct Slot
  {
    flecs::entity entity{};
    uint8_t generation = 0;
    bool live = false;
    bool quarantined = false;
  };

  flecs::world* _world = nullptr;  // borrowed (GpuScene::Impl owns the world).
  std::vector<Slot> _slots;
  std::vector<uint32_t> _freeSlots;
  uint32_t _live = 0;
};

}  // namespace pyxis
