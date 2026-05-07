// Pyxis renderer — Geom component.
//
// POD description of a mesh's vertex/index residency in the GpuScene
// vertex/index pools (§14.5). Variable-length data lives outside Flecs
// in the GpuScene tables; this component just records *where* in those
// pools the geometry sits.
//
// Plan §8 (component shape), §30.11 (POD-only rule).

#pragma once

#include <cstdint>

namespace pyxis::scene {

struct Geom {
    uint32_t vertexPageIndex   = 0;   // Index into GpuScene's vertex pool pages (§14.5).
    uint32_t vertexByteOffset  = 0;   // Byte offset into that page.
    uint32_t indexPageIndex    = 0;
    uint32_t indexByteOffset   = 0;
    uint32_t triangleCount     = 0;
    uint32_t vertexCount       = 0;
    uint32_t flags             = 0;   // bit 0 hasNormals, bit 1 hasTangents, bit 2 hasUV
};

inline constexpr uint32_t GEOM_FLAG_HAS_NORMALS  = 1u << 0;
inline constexpr uint32_t GEOM_FLAG_HAS_TANGENTS = 1u << 1;
inline constexpr uint32_t GEOM_FLAG_HAS_UV       = 1u << 2;

}  // namespace pyxis::scene
