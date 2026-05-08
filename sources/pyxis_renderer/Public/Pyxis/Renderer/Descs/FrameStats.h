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
    uint64_t meshCount        = 0;
    uint64_t materialCount    = 0;
    uint64_t textureCount     = 0;
    uint64_t instanceCount    = 0;
    uint64_t lightCount       = 0;
    uint64_t blasCount        = 0;
    uint64_t blasBytes        = 0;
    uint64_t tlasBytes        = 0;
    uint64_t vertexBytes      = 0;
    uint64_t indexBytes       = 0;
    uint64_t textureBytes     = 0;
    uint64_t pendingUploads   = 0;
    uint64_t pendingBlasBuilds = 0;
    uint64_t staleHandleDrops = 0;
    bool     degraded         = false;
};

}  // namespace pyxis
