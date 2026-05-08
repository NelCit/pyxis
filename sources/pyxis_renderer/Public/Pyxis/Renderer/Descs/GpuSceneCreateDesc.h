// Pyxis renderer — GpuScene constructor descriptor.
//
// Plan §18.4 + §33.1. Sized so the defaults are reasonable for a
// viewer; the headless path raises `framesInFlight` to 3 (§33.7
// byte-equal contract).
//
// `bindlessCapacity` 80 000 covers Moana hero textures + UDIM tiles
// per §5; lower for unit tests if you want. `stagingMib` is the
// upload-queue ring budget (§33.4). `compactBlas` defaults true
// because the §16 split rules require it for any mesh ≥ 64k tris;
// set false only for diagnostic profiling builds where you want to
// see uncompacted BLAS sizes.

#pragma once

#include <Pyxis/Renderer/RendererApi.h>

#include <cstdint>

namespace pyxis {

struct GpuSceneCreateDesc {
    uint32_t bindlessCapacity = 80'000;
    uint32_t stagingMib       = 256;
    uint32_t framesInFlight   = 2;       // ≤ MAX_FRAMES_IN_FLIGHT (§33.1).
    bool     compactBlas      = true;
};

}  // namespace pyxis
