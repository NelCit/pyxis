// Pyxis renderer — public per-frame statistics snapshot.
//
// Plan §18.4. Returned by GpuScene::LastFrameStats(). Cheap
// counters callable every frame; the deeper FrameProfile (§18.7) is
// the right place for timing-heavy detail.
//
// `staleHandleDrops` is the count of Destroy*/Update* calls this
// frame that targeted a recycled or `Invalid` handle (§18.5). A
// non-zero value usually points at a stale ingest cache.
//
// `degraded` is true if any soft-fallback fired this frame
// (texture decode failed and the missing-texture colour was used,
// material translation hit `MaterialUnsupported`, etc.). Pairs with
// the one-shot spdlog line each fallback emits.

#pragma once

#include <Pyxis/Renderer/RendererApi.h>

#include <cstdint>

namespace pyxis {

struct FrameStats {
  uint64_t meshCount = 0;
  uint64_t materialCount = 0;
  uint64_t textureCount = 0;
  uint64_t instanceCount = 0;
  uint64_t lightCount = 0;
  uint64_t blasCount = 0;
  uint64_t blasBytes = 0;
  uint64_t tlasBytes = 0;
  uint64_t vertexBytes = 0;
  uint64_t indexBytes = 0;
  uint64_t textureBytes = 0;
  uint64_t pendingUploads = 0;
  uint64_t pendingBlasBuilds = 0;
  uint64_t staleHandleDrops = 0;
  bool degraded = false;
  // V2.A.5 — appended after the v1 layout. Counts UsdVolVolume /
  // OpenVDBAsset slots actually loaded + the VRAM their R32_FLOAT
  // 3D textures consume. Keeping them at the tail preserves the
  // existing struct layout for any external consumer compiled
  // against the v1.0 ABI.
  uint64_t volumeCount = 0;
  uint64_t volumeBytes = 0;
};

}  // namespace pyxis
