// Pyxis renderer — GpuScene camera + light-verb bodies.
//
// Per-verb split off GpuScene.cpp; declarations live on
// `GpuScene::Impl` in Internal.h, public `GpuScene::Verb()`
// methods in GpuScene.cpp forward one line into here.
//
// RFC 0009 P1 — lights are Flecs entities (GpuLightComponent in `sceneWorld`),
// not a std::vector. `lightHandles` (HandleBimap, §8.2) owns slot/generation and
// the handle<->entity map; the GPU light buffer is packed from a slot-sorted query
// at commit time (CollectLiveLightsSorted). No mirror data: the LightDesc lives
// only on the entity.

#include "GpuScene/Internal.h"

#include <algorithm>
#include <utility>

namespace pyxis {

using namespace gpuscene_detail;

void GpuScene::Impl::SetCamera(const CameraDesc& cameraDescIn)
{
  cameraDesc = cameraDescIn;
  hasCamera = true;
}

LightHandle GpuScene::Impl::AddLight(const LightDesc& lightDesc)
{
  // Handle-space guard (§18.5): one slot per live light; cap at the 24-bit slot
  // space. Invalid is the documented lazy-acquirer fallback when exhausted.
  if (lightHandles.LiveCount() >= (1u << HANDLE_SLOT_BITS))
    return LightHandle::Invalid;

  const flecs::entity entity = sceneWorld.entity();
  const uint32_t handle = lightHandles.Allocate(entity);
  entity.set<GpuLightComponent>({handle, lightDesc});
  lightsNeedGpuUpload = true;
  return static_cast<LightHandle>(handle);
}

void GpuScene::Impl::UpdateLight(LightHandle lightHandle, const LightDesc& lightDesc)
{
  const flecs::entity entity = lightHandles.Resolve(static_cast<uint32_t>(lightHandle));
  if (entity.id() == 0 || !entity.is_alive())
  {
    // §18.5 — a non-Invalid handle that no longer resolves (recycled / stale
    // generation) is a counted silent drop; Invalid (0) is not.
    if (static_cast<uint32_t>(lightHandle) != 0)
      ++lastFrameStats.staleHandleDrops;
    return;
  }
  entity.set<GpuLightComponent>({static_cast<uint32_t>(lightHandle), lightDesc});
  lightsNeedGpuUpload = true;
}

void GpuScene::Impl::RemoveLight(LightHandle lightHandle)
{
  const flecs::entity entity = lightHandles.Resolve(static_cast<uint32_t>(lightHandle));
  if (entity.id() == 0 || !entity.is_alive())
  {
    if (static_cast<uint32_t>(lightHandle) != 0)
      ++lastFrameStats.staleHandleDrops;  // §18.5 — stale handle counted.
    return;
  }
  entity.destruct();
  // Bimap bumps the slot generation (or quarantines at 255), matching the old
  // ResolveLight + freeLightSlots behaviour.
  lightHandles.Free(static_cast<uint32_t>(lightHandle));
  lightsNeedGpuUpload = true;
}

void GpuScene::Impl::CollectLiveLightsSorted(std::vector<std::pair<uint32_t, LightDesc>>& out)
{
  out.clear();
  lightQuery.each([&](GpuLightComponent& light) { out.emplace_back(light.handle, light.desc); });
  // Slot order = insertion order for the static scenes the golden/parity suite
  // renders, so the packed buffer + editor accessors stay byte-identical to the
  // old vector-order walk.
  std::sort(out.begin(), out.end(), [](const auto& lhs, const auto& rhs) {
    return scene::HandleBimap::SlotIndex(lhs.first) < scene::HandleBimap::SlotIndex(rhs.first);
  });
}

}  // namespace pyxis
