// Pyxis renderer — public forwards + strong handles. Plan §18.2 (M0 slice).
//
// Subsequent milestones add: GpuScene, PyxisRenderer, Profiler, the public
// POD descriptors (Mesh/Material/Texture/Instance/Camera/Light), and the
// Error type. Until the renderer module ships those public classes, this
// header is the canonical pointer to "where would they go".

#pragma once

#include <cstdint>

namespace nvrhi {
class IDevice;
class ICommandList;
}  // namespace nvrhi

namespace pyxis {

// Strong handles per plan §18.9 / §19.7. The underlying type AND the
// `Invalid = 0` symbol name are part of the byte-frozen ABI contract.
// Runtime values pack a 24-bit slot + 8-bit generation into the uint32_t
// (§19.7); the literal `0` enumerator is the canonical "no handle" sentinel.
enum class MeshHandle : uint32_t { Invalid = 0 };
enum class MaterialHandle : uint32_t { Invalid = 0 };
enum class TextureHandle : uint32_t { Invalid = 0 };
enum class InstanceHandle : uint32_t { Invalid = 0 };
enum class LightHandle : uint32_t { Invalid = 0 };
// V2.A.5 — UsdVolVolume / OpenVDBAsset slot. Same 24-bit slot +
// 8-bit generation packing as the others. Closesthit doesn't sample
// the bound 3D texture in v2; the API surface is here so a future
// volume-integrator pass can reach the GPU buffer without a
// MAJOR-version rev (§22.3 additive growth).
enum class VolumeHandle : uint32_t { Invalid = 0 };

// 24-bit slot mask + 8-bit generation mask (plan §19.7).
inline constexpr uint32_t HANDLE_SLOT_BITS = 24;
inline constexpr uint32_t HANDLE_GENERATION_BITS = 8;
inline constexpr uint32_t HANDLE_SLOT_MASK = (1u << 24) - 1u;
inline constexpr uint32_t HANDLE_GENERATION_MASK = ~HANDLE_SLOT_MASK;

// Compile-time cap for every per-frame ring (command lists, binding sets,
// staging, deletion, queries). Plan §33.1.
inline constexpr uint32_t MAX_FRAMES_IN_FLIGHT = 3;

// Future public surface — forward-declared so consumers compile against the
// shape now. Definitions appear in their respective headers when the
// milestone ships them.
class GpuScene;
class PyxisRenderer;
class Profiler;
class SceneWorldFacade;

}  // namespace pyxis
