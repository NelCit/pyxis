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

  // RFC 0009 P5 — allocate a Flecs entity (slot == instanceCustomIndex).
  flecs::entity entity;
  const uint32_t handleRaw = instanceSlots.Allocate(entity);
  if (handleRaw == 0)
  {
    return std::unexpected{
        PYXIS_ERROR(ErrorKind::TlasInstanceLimitExceeded,
                    "AppendInstance: instance-handle slot space exhausted (limit = %u)",
                    (1u << HANDLE_SLOT_BITS))};
  }
  const uint32_t slot = GpuSlotMap::SlotOf(handleRaw);
  if (instanceResources.size() <= slot)
    instanceResources.resize(slot + 1u);

  InstanceResource& entry = instanceResources[slot];
  entry = InstanceResource{};  // reset a recycled slot.
  entry.mesh = instanceDesc.mesh;
  entry.material = instanceDesc.material;
  entry.worldFromLocal = instanceDesc.worldFromLocal;
  entry.visible = instanceDesc.visible;
  entry.doubleSided = instanceDesc.doubleSided;
  entry.debugName.assign(instanceDesc.debugName);

  entity.set<GpuInstanceComponent>({});
  // Relationship pairs (Instance, MeshOf, mesh) / (Instance, MaterialOf, material) —
  // make "instances of this mesh/material" a query (refcounted sharing / orphans).
  if (const flecs::entity meshEntity =
          meshSlots.Resolve(static_cast<uint32_t>(instanceDesc.mesh));
      meshEntity.id() != 0)
    entity.add<MeshOf>(meshEntity);
  if (const flecs::entity matEntity =
          materialSlots.Resolve(static_cast<uint32_t>(instanceDesc.material));
      matEntity.id() != 0)
    entity.add<MaterialOf>(matEntity);

  // New instance → TLAS pack changes + side-table gains an entry.
  tlasNeedsRebuild = true;
  instanceMaterialNeedsUpload = true;
  return static_cast<InstanceHandle>(handleRaw);
}

void GpuScene::Impl::UpdateInstanceTransform(InstanceHandle instanceHandle,
                                             const hlslpp::float4x4& worldFromLocal)
{
  const flecs::entity entity = instanceSlots.Resolve(static_cast<uint32_t>(instanceHandle));
  if (entity.id() == 0)
  {
    if (static_cast<uint32_t>(instanceHandle) != 0)
      ++lastFrameStats.staleHandleDrops;
    return;
  }
  instanceResources[GpuSlotMap::SlotOf(static_cast<uint32_t>(instanceHandle))].worldFromLocal =
      worldFromLocal;
  entity.add<scene::DirtyTransform>();  // enables a future TLAS refit; rebuild for now.
  tlasNeedsRebuild = true;
}

void GpuScene::Impl::UpdateInstanceMaterial(InstanceHandle instanceHandle,
                                            MaterialHandle materialHandle)
{
  const flecs::entity entity = instanceSlots.Resolve(static_cast<uint32_t>(instanceHandle));
  if (entity.id() == 0)
  {
    if (static_cast<uint32_t>(instanceHandle) != 0)
      ++lastFrameStats.staleHandleDrops;
    return;
  }
  // Only the side-table needs re-upload; the TLAS doesn't carry materials.
  instanceResources[GpuSlotMap::SlotOf(static_cast<uint32_t>(instanceHandle))].material =
      materialHandle;
  instanceMaterialNeedsUpload = true;
}

void GpuScene::Impl::SetInstanceVisibility(InstanceHandle instanceHandle, bool visible)
{
  const flecs::entity entity = instanceSlots.Resolve(static_cast<uint32_t>(instanceHandle));
  if (entity.id() == 0)
  {
    if (static_cast<uint32_t>(instanceHandle) != 0)
      ++lastFrameStats.staleHandleDrops;
    return;
  }
  InstanceResource& entry =
      instanceResources[GpuSlotMap::SlotOf(static_cast<uint32_t>(instanceHandle))];
  if (entry.visible != visible)
  {
    entry.visible = visible;
    tlasNeedsRebuild = true;          // in/out of the TLAS pack.
    instanceMaterialNeedsUpload = true;  // ID gaps must match the new instance set.
  }
}

void GpuScene::Impl::DestroyInstance(InstanceHandle instanceHandle)
{
  const flecs::entity entity = instanceSlots.Resolve(static_cast<uint32_t>(instanceHandle));
  if (entity.id() == 0)
  {
    if (static_cast<uint32_t>(instanceHandle) != 0)
      ++lastFrameStats.staleHandleDrops;
    return;
  }
  instanceResources[GpuSlotMap::SlotOf(static_cast<uint32_t>(instanceHandle))] = InstanceResource{};
  // Free destructs the entity (drops GpuInstanceComponent + MeshOf/MaterialOf pairs +
  // any DirtyTransform tag) + bumps the slot generation.
  instanceSlots.Free(static_cast<uint32_t>(instanceHandle));
  tlasNeedsRebuild = true;
  instanceMaterialNeedsUpload = true;
}

bool GpuScene::Impl::HasInstance(InstanceHandle instanceHandle) const
{
  return instanceSlots.Resolve(static_cast<uint32_t>(instanceHandle)).id() != 0;
}

}  // namespace pyxis
