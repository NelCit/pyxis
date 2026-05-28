// Pyxis renderer — GpuScene texture-verb bodies.
//
// Per-verb split off GpuScene.cpp; declarations live on
// `GpuScene::Impl` in Internal.h, public `GpuScene::Verb()`
// methods in GpuScene.cpp forward one line into here. The actual
// pixel decode (stb_image / tinyexr) happens lazily inside
// CommitResources, not at AcquireTexture time — see Commit.cpp.
//
// RFC 0009 P3 — textures are GpuTextureComponent entities in `sceneWorld`, indexed by
// `textureSlots` (GpuSlotMap; slot == bindless slot == GPU index). The GPU resource +
// transient decode pixels + owned resolvedPath live in the slot-indexed side tables
// (textureResources / texturePixelData / textureResolvedPaths). A `DirtyTexture` tag
// marks an entity awaiting decode/upload (replaces the old per-entry needsGpuUpload).

#include "GpuScene/Internal.h"

namespace pyxis {

using namespace gpuscene_detail;

namespace {

// Grow the slot-indexed texture side tables so [slot] is addressable.
void EnsureTextureSideTables(std::vector<nvrhi::TextureHandle>& resources,
                             std::vector<std::vector<std::uint8_t>>& pixels,
                             std::vector<std::string>& paths, uint32_t slot)
{
  if (resources.size() <= slot)
  {
    resources.resize(slot + 1u);
    pixels.resize(slot + 1u);
    paths.resize(slot + 1u);
  }
}

}  // namespace

TextureHandle GpuScene::Impl::AcquireTexture(const TextureKey& textureKey)
{
  // Same dedup + slot-allocation shape as AcquireMaterial. Decode happens lazily
  // inside CommitResources (the DirtyTexture tag gates it).
  const std::uint64_t hash = HashTextureKey(textureKey);
  // V2.A.12 — bump access tick on every Acquire (hit or fresh) for the LRU policy.
  ++nextTextureAccessTick;
  if (auto found = textureKeyHashToHandle.find(hash); found != textureKeyHashToHandle.end())
  {
    if (const flecs::entity hit = textureSlots.Resolve(static_cast<uint32_t>(found->second));
        hit.id() != 0)
    {
      GpuTextureComponent component = hit.get<GpuTextureComponent>();
      component.lastAccessTick = nextTextureAccessTick;
      hit.set<GpuTextureComponent>(component);
    }
    ++lruHitCount;
    return found->second;
  }
  ++lruMissCount;

  flecs::entity entity;
  const uint32_t handle = textureSlots.Allocate(entity);
  if (handle == 0)
    return TextureHandle::Invalid;  // slot space exhausted (§18.5).
  const uint32_t slot = GpuSlotMap::SlotOf(handle);
  EnsureTextureSideTables(textureResources, texturePixelData, textureResolvedPaths, slot);
  textureResolvedPaths[slot].assign(textureKey.resolvedPath);

  GpuTextureComponent component;
  component.keyCopy = textureKey;
  component.keyCopy.resolvedPath = textureResolvedPaths[slot];  // re-point at owned copy.
  component.keyHash = hash;
  component.bindlessSlot = slot;  // bindless slot = handle slot for M5.
  component.lastAccessTick = nextTextureAccessTick;
  entity.set<GpuTextureComponent>(component);
  entity.add<scene::DirtyTexture>();  // pending decode/upload.

  const auto textureHandle = static_cast<TextureHandle>(handle);
  textureKeyHashToHandle.emplace(hash, textureHandle);
  return textureHandle;
}

void GpuScene::Impl::DestroyTexture(TextureHandle textureHandle)
{
  const flecs::entity entity = textureSlots.Resolve(static_cast<uint32_t>(textureHandle));
  if (entity.id() == 0)
  {
    if (static_cast<uint32_t>(textureHandle) != 0)
      ++lastFrameStats.staleHandleDrops;
    return;
  }
  textureKeyHashToHandle.erase(entity.get<GpuTextureComponent>().keyHash);
  const uint32_t slot = GpuSlotMap::SlotOf(static_cast<uint32_t>(textureHandle));
  if (slot < textureResources.size())
  {
    textureResources[slot] = nullptr;
    textureResolvedPaths[slot].clear();
    texturePixelData[slot].clear();
    texturePixelData[slot].shrink_to_fit();
  }
  textureSlots.Free(static_cast<uint32_t>(textureHandle));  // destructs entity, bumps gen.
}

bool GpuScene::Impl::HasTexture(TextureHandle textureHandle) const
{
  return textureSlots.Resolve(static_cast<uint32_t>(textureHandle)).id() != 0;
}

// V2.A.12 — texture LRU eviction. Walks every live texture entity, sorts by
// lastAccessTick ascending (coldest first), and destroys entries until the total
// decoded byte count drops below `targetBytes`. Returns the number evicted.
std::uint32_t GpuScene::Impl::EvictColdTextures(std::uint64_t targetBytes) noexcept
{
  struct EvictionCandidate
  {
    std::uint64_t accessTick;
    uint32_t      slot;
    std::uint64_t byteCount;
  };
  std::vector<EvictionCandidate> candidates;
  std::uint64_t totalBytes = 0;
  textureSlots.ForEachLiveSlot([&](uint32_t slot, flecs::entity entity) {
    const GpuTextureComponent& component = entity.get<GpuTextureComponent>();
    const std::uint64_t entryBytes =
        static_cast<std::uint64_t>(component.width) * component.height * 4u;
    totalBytes += entryBytes;
    candidates.push_back({component.lastAccessTick, slot, entryBytes});
  });

  if (totalBytes <= targetBytes)
    return 0;  // already under budget.

  std::sort(candidates.begin(), candidates.end(),
            [](const EvictionCandidate& lhs, const EvictionCandidate& rhs) {
              return lhs.accessTick < rhs.accessTick;  // coldest first.
            });

  std::uint32_t evicted = 0;
  for (const EvictionCandidate& candidate : candidates)
  {
    if (totalBytes <= targetBytes)
      break;
    DestroyTexture(static_cast<TextureHandle>(textureSlots.HandleForSlot(candidate.slot)));
    totalBytes -= candidate.byteCount;
    ++evicted;
  }
  return evicted;
}

}  // namespace pyxis
