// Pyxis renderer — GpuScene texture-verb bodies.
//
// Per-verb split off GpuScene.cpp; declarations live on
// `GpuScene::Impl` in Internal.h, public `GpuScene::Verb()`
// methods in GpuScene.cpp forward one line into here. The actual
// pixel decode (stb_image / tinyexr) happens lazily inside
// CommitResources, not at AcquireTexture time — see Commit.cpp.

#include "GpuScene/Internal.h"

namespace pyxis {

using namespace gpuscene_detail;

TextureHandle GpuScene::Impl::AcquireTexture(const TextureKey& textureKey)
{
  // Same dedup + slot-allocation shape as AcquireMaterial. Decode
  // happens lazily inside CommitResources (M5 stub: synchronous
  // stb_image decode on the render thread; the §31 async I/O pool
  // wires at M8 when texture-load latency starts to dominate).
  if (textures.empty())
    textures.emplace_back();  // sentinel slot 0

  const std::uint64_t hash = HashTextureKey(textureKey);
  // V2.A.12 — bump access tick + hit counter on cache-hit. Eviction
  // policy (when it lands) reads `lastAccessTick` to find the coldest
  // entries; bumping on hit keeps frequently-sampled textures warm.
  ++nextTextureAccessTick;
  if (auto found = textureKeyHashToHandle.find(hash);
      found != textureKeyHashToHandle.end())
  {
    if (TextureEntry* hitEntry = ResolveTexture(found->second); hitEntry != nullptr)
      hitEntry->lastAccessTick = nextTextureAccessTick;
    ++lruHitCount;
    return found->second;
  }
  ++lruMissCount;

  // O(1) free-list pop; DestroyTexture pushes back symmetrically.
  uint32_t slot = 0;
  if (!freeTextureSlots.empty())
  {
    slot = freeTextureSlots.back();
    freeTextureSlots.pop_back();
  }
  else
  {
    if (textures.size() >= (1u << HANDLE_SLOT_BITS))
      return TextureHandle::Invalid;
    textures.emplace_back();
    slot = static_cast<uint32_t>(textures.size() - 1);
  }

  TextureEntry& entry = textures[slot];
  entry.live = true;
  entry.needsGpuUpload = true;
  entry.keyCopy = textureKey;
  entry.keyHash = hash;
  entry.resolvedPath.assign(textureKey.resolvedPath);
  entry.keyCopy.resolvedPath = entry.resolvedPath;  // re-point at owned copy
  entry.bindlessSlot = slot;  // bindless slot = handle slot for M5

  const auto handle = static_cast<TextureHandle>(HandleEncode(slot, entry.generation));
  textureKeyHashToHandle.emplace(hash, handle);
  return handle;
}

void GpuScene::Impl::DestroyTexture(TextureHandle textureHandle)
{
  TextureEntry* entry = ResolveTexture(textureHandle);
  if (entry == nullptr)
    return;
  textureKeyHashToHandle.erase(entry->keyHash);
  entry->live = false;
  entry->texture = nullptr;
  entry->resolvedPath.clear();
  entry->pixelData.clear();
  entry->pixelData.shrink_to_fit();
  if (entry->generation == HANDLE_GENERATION_QUARANTINE)
  {
    entry->quarantined = true;
  }
  else
  {
    ++entry->generation;
    const auto slot = static_cast<std::uint32_t>(entry - textures.data());
    freeTextureSlots.push_back(slot);
  }
}

bool GpuScene::Impl::HasTexture(TextureHandle textureHandle) const
{
  return LookupTexture(textureHandle) != nullptr;
}

// V2.A.12 — texture LRU eviction. Walks every live texture entry,
// sorts by `lastAccessTick` ascending (coldest first), and destroys
// entries until total decoded byte count drops below `targetBytes`.
// Returns the number of textures evicted. Safe to call between
// frames (single-writer); the resulting destroyed handles flow
// through the regular DestroyTexture cleanup.
//
// The closesthit shader sees magenta-fallback (slot 0) for any sampled
// texture whose handle was evicted; the operator gets a one-shot
// warning per evicted texture so the gap is visible.
std::uint32_t GpuScene::Impl::EvictColdTextures(std::uint64_t targetBytes) noexcept
{
  // Collect (lastAccessTick, slot, byteCount) for every live entry.
  struct EvictionCandidate
  {
    std::uint64_t accessTick;
    std::uint32_t slot;
    std::uint64_t byteCount;
  };
  std::vector<EvictionCandidate> candidates;
  std::uint64_t totalBytes = 0;
  candidates.reserve(textures.size());
  for (std::size_t slotIdx = 1; slotIdx < textures.size(); ++slotIdx)
  {
    const TextureEntry& entry = textures[slotIdx];
    if (!entry.live)
      continue;
    const std::uint64_t entryBytes = static_cast<std::uint64_t>(entry.width)
                                  * entry.height * 4u;
    totalBytes += entryBytes;
    candidates.push_back({entry.lastAccessTick,
                          static_cast<std::uint32_t>(slotIdx),
                          entryBytes});
  }

  if (totalBytes <= targetBytes)
    return 0;  // already under budget — no eviction needed

  std::sort(candidates.begin(), candidates.end(),
            [](const EvictionCandidate& lhs, const EvictionCandidate& rhs) {
              return lhs.accessTick < rhs.accessTick;  // coldest first
            });

  std::uint32_t evicted = 0;
  for (const EvictionCandidate& cand : candidates)
  {
    if (totalBytes <= targetBytes)
      break;
    const TextureEntry& entry = textures[cand.slot];
    const auto handle = static_cast<TextureHandle>(
        HandleEncode(cand.slot, entry.generation));
    DestroyTexture(handle);
    totalBytes -= cand.byteCount;
    ++evicted;
  }
  return evicted;
}

}  // namespace pyxis
