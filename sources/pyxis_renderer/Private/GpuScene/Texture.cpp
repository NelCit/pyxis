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

}  // namespace pyxis
