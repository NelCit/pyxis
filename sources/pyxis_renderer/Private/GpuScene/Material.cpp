// Pyxis renderer — GpuScene material-verb bodies.
//
// Per-verb split off GpuScene.cpp; declarations live on
// `GpuScene::Impl` in Internal.h, public `GpuScene::Verb()`
// methods in GpuScene.cpp forward one line into here.

#include "GpuScene/Internal.h"

namespace pyxis {

using namespace gpuscene_detail;

MaterialHandle GpuScene::Impl::AcquireMaterial(const OpenPBRMaterialDesc& materialDesc)
{
  // §11 dedup: hash → existing handle if present, else allocate
  // a new slot. Lazy-init the materials vector with slot 0
  // reserved (the §19.7 invalid-handle sentinel) so `materialId =
  // 0` in the closesthit unambiguously means "no material".
  if (materials.empty())
    materials.emplace_back();  // sentinel slot 0

  const std::uint64_t hash = HashMaterialDesc(materialDesc);
  if (auto found = materialDescHashToHandle.find(hash);
      found != materialDescHashToHandle.end())
  {
    // Cache hit — return the existing handle. (Hash collisions
    // are theoretically possible at FNV1a-64; in practice
    // material counts in v1 are small enough that the
    // birthday-paradox risk is negligible. M8 XXH3 swap revisits.)
    return found->second;
  }

  uint32_t slot = 0;
  for (uint32_t candidate = 1; candidate < materials.size(); ++candidate)
  {
    auto& entry = materials[candidate];
    if (!entry.live && !entry.quarantined)
    {
      slot = candidate;
      break;
    }
  }
  if (slot == 0)
  {
    if (materials.size() >= (1u << HANDLE_SLOT_BITS))
      return MaterialHandle::Invalid;  // slot space exhausted
    materials.emplace_back();
    slot = static_cast<uint32_t>(materials.size() - 1);
  }

  MaterialEntry& entry = materials[slot];
  entry.live = true;
  entry.needsGpuUpload = true;
  entry.descCopy = materialDesc;
  entry.descHash = hash;
  entry.sourcePrim.assign(materialDesc.sourcePrim);
  entry.descCopy.sourcePrim = entry.sourcePrim;  // re-point at owned copy

  const auto handle = static_cast<MaterialHandle>(HandleEncode(slot, entry.generation));
  materialDescHashToHandle.emplace(hash, handle);
  materialsNeedGpuUpload = true;
  return handle;
}

void GpuScene::Impl::UpdateMaterial(MaterialHandle materialHandle,
                                    const OpenPBRMaterialDesc& materialDesc)
{
  MaterialEntry* entry = ResolveMaterial(materialHandle);
  if (entry == nullptr)
    return;
  // Re-hash + dedup-map maintenance: drop the old hash entry, add
  // the new one. If the new hash already maps to a different live
  // material, we leave that alone (Update doesn't merge handles —
  // semantics: this material's *fields* changed in place).
  materialDescHashToHandle.erase(entry->descHash);
  entry->descCopy = materialDesc;
  entry->descHash = HashMaterialDesc(materialDesc);
  entry->sourcePrim.assign(materialDesc.sourcePrim);
  entry->descCopy.sourcePrim = entry->sourcePrim;
  entry->needsGpuUpload = true;
  materialDescHashToHandle.emplace(entry->descHash, materialHandle);
  materialsNeedGpuUpload = true;
}

void GpuScene::Impl::DestroyMaterial(MaterialHandle materialHandle)
{
  MaterialEntry* entry = ResolveMaterial(materialHandle);
  if (entry == nullptr)
    return;
  materialDescHashToHandle.erase(entry->descHash);
  entry->live = false;
  entry->descCopy = OpenPBRMaterialDesc{};
  entry->sourcePrim.clear();
  if (entry->generation == HANDLE_GENERATION_QUARANTINE)
    entry->quarantined = true;
  else
    ++entry->generation;
}

bool GpuScene::Impl::HasMaterial(MaterialHandle materialHandle) const
{
  return LookupMaterial(materialHandle) != nullptr;
}

}  // namespace pyxis
