// Pyxis renderer — GpuScene camera + light-verb bodies.
//
// Per-verb split off GpuScene.cpp; declarations live on
// `GpuScene::Impl` in Internal.h, public `GpuScene::Verb()`
// methods in GpuScene.cpp forward one line into here.

#include "GpuScene/Internal.h"

namespace pyxis {

using namespace gpuscene_detail;

void GpuScene::Impl::SetCamera(const CameraDesc& cameraDescIn)
{
  cameraDesc = cameraDescIn;
  hasCamera = true;
}

LightHandle GpuScene::Impl::AddLight(const LightDesc& lightDesc)
{
  // O(1) free-list pop; RemoveLight pushes back symmetrically.
  uint32_t slot = 0;
  if (!freeLightSlots.empty())
  {
    slot = freeLightSlots.back();
    freeLightSlots.pop_back();
  }
  else
  {
    if (lights.size() >= (1u << HANDLE_SLOT_BITS))
    {
      // Light handle space exhausted — Invalid is the documented
      // fallback (§18.5 lazy-acquirer contract); a one-shot spdlog
      // warn lands at the next CommitResources via
      // FrameStats::degraded once that path is wired.
      return LightHandle::Invalid;
    }
    lights.emplace_back();
    slot = static_cast<uint32_t>(lights.size() - 1);
  }

  LightEntry& entry = lights[slot];
  entry.live = true;
  entry.descCopy = lightDesc;
  lightsNeedGpuUpload = true;
  return static_cast<LightHandle>(HandleEncode(slot, entry.generation));
}

void GpuScene::Impl::UpdateLight(LightHandle lightHandle, const LightDesc& lightDesc)
{
  if (auto* entry = ResolveLight(lightHandle))
  {
    entry->descCopy = lightDesc;
    lightsNeedGpuUpload = true;
  }
}

void GpuScene::Impl::RemoveLight(LightHandle lightHandle)
{
  LightEntry* entry = ResolveLight(lightHandle);
  if (entry == nullptr)
    return;
  entry->live = false;
  entry->descCopy = LightDesc{};
  if (entry->generation == HANDLE_GENERATION_QUARANTINE)
  {
    entry->quarantined = true;
  }
  else
  {
    ++entry->generation;
    const auto slot = static_cast<std::uint32_t>(entry - lights.data());
    freeLightSlots.push_back(slot);
  }
  lightsNeedGpuUpload = true;
}

}  // namespace pyxis
