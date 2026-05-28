// Pyxis renderer — GpuScene material-verb bodies.
//
// Per-verb split off GpuScene.cpp; declarations live on
// `GpuScene::Impl` in Internal.h, public `GpuScene::Verb()`
// methods in GpuScene.cpp forward one line into here.
//
// RFC 0009 P2 — materials are GpuMaterialComponent entities in `sceneWorld`, indexed
// by `materialSlots` (GpuSlotMap; slot == GPU buffer index, gpuscene_detail encoding,
// slot 0 sentinel). The §11 dedup hash map stays as a hash->handle index. The
// owned sourcePrim string lives in the slot-indexed `materialSourcePrims` table; the
// stored desc's sourcePrim view points into it (preserving the old re-point).

#include "GpuScene/Internal.h"

namespace pyxis {

using namespace gpuscene_detail;

namespace {

// Write the desc + hash into the entity's component, re-pointing the desc's
// sourcePrim view at the owned per-slot string (so the view outlives the caller's
// borrowed storage — same contract as the old MaterialEntry::sourcePrim copy).
void StoreMaterialComponent(std::vector<std::string>& sourcePrims, const flecs::entity& entity,
                            uint32_t slot, const OpenPBRMaterialDesc& desc, std::uint64_t hash)
{
  if (sourcePrims.size() <= slot)
    sourcePrims.resize(slot + 1u);
  sourcePrims[slot].assign(desc.sourcePrim);
  GpuMaterialComponent component;
  component.desc = desc;
  component.desc.sourcePrim = sourcePrims[slot];  // re-point at the owned copy.
  component.descHash = hash;
  entity.set<GpuMaterialComponent>(component);
}

}  // namespace

MaterialHandle GpuScene::Impl::AcquireMaterial(const OpenPBRMaterialDesc& materialDesc)
{
  // §11 dedup: hash → existing handle if present, else allocate a new slot.
  const std::uint64_t hash = HashMaterialDesc(materialDesc);
  if (auto found = materialDescHashToHandle.find(hash); found != materialDescHashToHandle.end())
    return found->second;  // cache hit (FNV1a collision risk negligible at v1 counts).

  flecs::entity entity;
  const uint32_t handle = materialSlots.Allocate(entity);
  if (handle == 0)
    return MaterialHandle::Invalid;  // slot space exhausted (§18.5).
  StoreMaterialComponent(materialSourcePrims, entity, GpuSlotMap::SlotOf(handle), materialDesc, hash);

  const auto materialHandle = static_cast<MaterialHandle>(handle);
  materialDescHashToHandle.emplace(hash, materialHandle);
  entity.add<scene::DirtyMaterial>();  // gates the material-buffer re-pack (cleared in ClearDirty).
  return materialHandle;
}

void GpuScene::Impl::UpdateMaterial(MaterialHandle materialHandle,
                                    const OpenPBRMaterialDesc& materialDesc)
{
  const flecs::entity entity = materialSlots.Resolve(static_cast<uint32_t>(materialHandle));
  if (entity.id() == 0)
  {
    if (static_cast<uint32_t>(materialHandle) != 0)
      ++lastFrameStats.staleHandleDrops;  // §18.5 stale drop.
    return;
  }
  // Re-hash + dedup-map maintenance: drop the old hash entry, add the new one.
  // The resolved entity always carries the component (set at Acquire).
  materialDescHashToHandle.erase(entity.get<GpuMaterialComponent>().descHash);
  const std::uint64_t hash = HashMaterialDesc(materialDesc);
  StoreMaterialComponent(materialSourcePrims, entity,
                         GpuSlotMap::SlotOf(static_cast<uint32_t>(materialHandle)), materialDesc,
                         hash);
  materialDescHashToHandle.emplace(hash, materialHandle);
  entity.add<scene::DirtyMaterial>();  // gates the material-buffer re-pack (cleared in ClearDirty).
}

void GpuScene::Impl::DestroyMaterial(MaterialHandle materialHandle)
{
  const flecs::entity entity = materialSlots.Resolve(static_cast<uint32_t>(materialHandle));
  if (entity.id() == 0)
  {
    if (static_cast<uint32_t>(materialHandle) != 0)
      ++lastFrameStats.staleHandleDrops;
    return;
  }
  materialDescHashToHandle.erase(entity.get<GpuMaterialComponent>().descHash);
  // Free destructs the entity + bumps the slot generation (or quarantines at 255).
  // Matches the old verb: the GPU buffer is NOT re-uploaded here (the slot keeps its
  // stale bytes until the next Acquire/Update; goldens are static so unaffected).
  materialSlots.Free(static_cast<uint32_t>(materialHandle));
}

bool GpuScene::Impl::HasMaterial(MaterialHandle materialHandle) const
{
  return materialSlots.Resolve(static_cast<uint32_t>(materialHandle)).id() != 0;
}

}  // namespace pyxis
