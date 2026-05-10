// Pyxis renderer — GpuScene instance-verb bodies.
//
// Per-verb split off GpuScene.cpp; declarations live on
// `GpuScene::Impl` in Internal.h, public `GpuScene::Verb()`
// methods in GpuScene.cpp forward one line into here.

#include "GpuScene/Internal.h"

namespace pyxis {

using namespace gpuscene_detail;

Expected<InstanceHandle> GpuScene::Impl::AppendInstance(const InstanceDesc& instanceDesc)
{
  if (instanceDesc.mesh == MeshHandle::Invalid)
  {
    return std::unexpected{
        PYXIS_ERROR(ErrorKind::InvalidArgument, "AppendInstance: mesh handle is Invalid")};
  }
  if (!HasMesh(instanceDesc.mesh))
  {
    return std::unexpected{
        PYXIS_ERROR(ErrorKind::InvalidHandle,
                    "AppendInstance: mesh handle %u not live (slot+generation mismatch)",
                    static_cast<uint32_t>(instanceDesc.mesh))};
  }

  uint32_t slot = 0;
  for (uint32_t candidateSlot = 1; candidateSlot < instances.size(); ++candidateSlot)
  {
    const InstanceEntry& candidate = instances[candidateSlot];
    if (!candidate.live && !candidate.quarantined)
    {
      slot = candidateSlot;
      break;
    }
  }
  if (slot == 0)
  {
    if (instances.size() >= (1u << HANDLE_SLOT_BITS))
    {
      return std::unexpected{
          PYXIS_ERROR(ErrorKind::TlasInstanceLimitExceeded,
                      "AppendInstance: instance-handle slot space exhausted (limit = %u)",
                      (1u << HANDLE_SLOT_BITS))};
    }
    instances.emplace_back();
    slot = static_cast<uint32_t>(instances.size() - 1);
  }

  InstanceEntry& entry = instances[slot];
  entry.live = true;
  entry.mesh = instanceDesc.mesh;
  entry.material = instanceDesc.material;
  entry.worldFromLocal = instanceDesc.worldFromLocal;
  entry.visible = instanceDesc.visible;
  entry.debugName.assign(instanceDesc.debugName);

  // AppendInstance changes the TLAS (new instance to pack) AND the
  // side-table (new entry to hold this instance's material slot).
  tlasNeedsRebuild = true;
  instanceMaterialNeedsUpload = true;
  return static_cast<InstanceHandle>(HandleEncode(slot, entry.generation));
}

void GpuScene::Impl::UpdateInstanceTransform(InstanceHandle instanceHandle,
                                             const hlslpp::float4x4& worldFromLocal)
{
  if (auto* entry = ResolveInstance(instanceHandle))
  {
    entry->worldFromLocal = worldFromLocal;
    tlasNeedsRebuild = true;
  }
}

void GpuScene::Impl::UpdateInstanceMaterial(InstanceHandle instanceHandle,
                                            MaterialHandle materialHandle)
{
  if (auto* entry = ResolveInstance(instanceHandle))
  {
    // M6 audit closeout: only the side-table needs re-upload; the
    // TLAS doesn't know about materials (it only carries mesh BLAS
    // + transform + visibility + the per-§15 instance slot in
    // instanceCustomIndex). Bumping just instanceMaterialNeedsUpload
    // avoids a pointless TLAS rebuild on material edits, which the
    // M9 "Save Scene As USD" + AOV inspector edit-material flows
    // will exercise per-frame.
    entry->material = materialHandle;
    instanceMaterialNeedsUpload = true;
  }
}

void GpuScene::Impl::SetInstanceVisibility(InstanceHandle instanceHandle, bool visible)
{
  if (auto* entry = ResolveInstance(instanceHandle))
  {
    if (entry->visible != visible)
    {
      entry->visible = visible;
      // Visibility flipping a slot in/out of the TLAS pack changes
      // which entries are live → side-table must re-upload too so
      // any ID gap matches the new TLAS instance set.
      tlasNeedsRebuild = true;
      instanceMaterialNeedsUpload = true;
    }
  }
}

void GpuScene::Impl::DestroyInstance(InstanceHandle instanceHandle)
{
  InstanceEntry* entry = ResolveInstance(instanceHandle);
  if (entry == nullptr)
    return;
  entry->live = false;
  entry->mesh = MeshHandle::Invalid;
  entry->material = MaterialHandle::Invalid;
  entry->worldFromLocal = hlslpp::float4x4{};
  entry->visible = false;
  entry->debugName.clear();
  if (entry->generation == HANDLE_GENERATION_QUARANTINE)
  {
    entry->quarantined = true;
  }
  else
  {
    ++entry->generation;
  }
  tlasNeedsRebuild = true;
  instanceMaterialNeedsUpload = true;
}

bool GpuScene::Impl::HasInstance(InstanceHandle instanceHandle) const
{
  const auto value = static_cast<uint32_t>(instanceHandle);
  if (value == 0)
    return false;
  const uint32_t slot = HandleSlot(value);
  if (slot == 0 || slot >= instances.size())
    return false;
  const InstanceEntry& entry = instances[slot];
  return entry.live && !entry.quarantined && entry.generation == HandleGeneration(value);
}

}  // namespace pyxis
