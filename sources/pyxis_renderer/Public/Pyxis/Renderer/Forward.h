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

// Strong handles per plan §19.7 — 24-bit slot + 8-bit generation. kInvalid = 0.
enum class MeshHandle     : uint32_t { kInvalid = 0 };
enum class MaterialHandle : uint32_t { kInvalid = 0 };
enum class TextureHandle  : uint32_t { kInvalid = 0 };
enum class InstanceHandle : uint32_t { kInvalid = 0 };
enum class LightHandle    : uint32_t { kInvalid = 0 };

// 24-bit slot mask + 8-bit generation mask (plan §19.7).
inline constexpr uint32_t kHandleSlotBits       = 24;
inline constexpr uint32_t kHandleGenerationBits = 8;
inline constexpr uint32_t kHandleSlotMask       = (1u << 24) - 1u;
inline constexpr uint32_t kHandleGenerationMask = ~kHandleSlotMask;

// Compile-time cap for every per-frame ring (command lists, binding sets,
// staging, deletion, queries). Plan §33.1.
inline constexpr uint32_t kMaxFramesInFlight = 3;

// Future public surface — forward-declared so consumers compile against the
// shape now. Definitions appear in their respective headers when the
// milestone ships them.
class GpuScene;
class PyxisRenderer;
class Profiler;
class SceneWorldFacade;

}  // namespace pyxis
