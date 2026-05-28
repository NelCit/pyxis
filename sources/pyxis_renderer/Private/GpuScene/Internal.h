// Pyxis renderer — GpuScene private implementation header.
//
// Defines `struct GpuScene::Impl` (the PIMPL body) plus the small
// hashing / packing / handle-decoding helpers shared between every
// per-verb .cpp file. The split (one verb-group per file: Mesh,
// Material, Texture, Instance, Light, Commit) keeps the single
// 2200-line GpuScene.cpp from growing unboundedly while preserving
// the public PIMPL contract — the public header still sees only
// `struct Impl;` + `std::unique_ptr<Impl>`. Each verb file declares
// member functions on Impl, GpuScene.cpp's public methods forward
// one line to `_impl->Verb(...)`.
//
// This header is PRIVATE — only files under Private/GpuScene/ may
// include it. Per §30 / §18.9 it must never appear in any Public/
// header transitively.

#pragma once

#include <Pyxis/Renderer/GpuScene.h>

#include <Pyxis/Platform/Logging/Log.h>
#include <Pyxis/Platform/Logging/LogCategories.h>
#include <Pyxis/Renderer/Descs/FrameStats.h>
#include <Pyxis/Renderer/Forward.h>
#include <Pyxis/Renderer/Profiler.h>

// ShaderInterop.slang lives in resources/shaders/ — the
// pyxis_renderer target's PRIVATE include path puts that directory
// on the search path so the C++ side here gets the same
// OpenPBRMaterialGPU / LightGpu layouts the shaders read. See
// pyxis_renderer/CMakeLists.txt for the wiring rationale.
#include "ShaderInterop.slang"

#include <nvrhi/nvrhi.h>

// RFC 0009 — the Flecs SceneWorld is the scene representation. P1 migrates LIGHTS:
// they live as entities in `sceneWorld` (component below) rather than a std::vector,
// with HandleBimap owning slot/generation. flecs is a PRIVATE renderer dep, so this
// header (Private only, §18.9) may include it; no Flecs type reaches Public/.
#include "GpuScene/GpuSlotMap.h"
#include "Scene/Components/Dirty.h"
#include "Scene/HandleBimap.h"
#include "Scene/Phases.h"  // RFC 0009 — §30.11 phase tags + RegisterPhasePipeline.

#include <flecs.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace pyxis {

// RFC 0009 P1 — per-light Flecs component. Stores the encoded handle (so the editor
// accessors return it directly) + the canonical LightDesc (no mirror struct: the
// existing PackLightGpu consumes it unchanged → byte-identical light buffer). The
// sort key for deterministic packing is HandleBimap::SlotIndex(handle).
struct GpuLightComponent
{
  uint32_t  handle = 0;
  LightDesc desc{};
};

// RFC 0009 P2 — per-material Flecs component. The OpenPBRMaterialDesc + its FNV1a
// hash. `desc.sourcePrim` is a string_view (POD — no owning container, so this is a
// valid §30.11 component); its backing owned string lives in the slot-indexed
// `materialSourcePrims` table on Impl (variable-length data → a GpuScene table).
struct GpuMaterialComponent
{
  OpenPBRMaterialDesc desc{};
  std::uint64_t       descHash = 0;
};

// RFC 0009 P3 — per-texture Flecs component (POD metadata only; §30.11). The GPU
// resource (nvrhi::TextureHandle), the transient decoded pixels, and the owned
// resolvedPath string are NOT POD, so they live in the slot-indexed side tables on
// Impl (textureResources / texturePixelData / textureResolvedPaths) — the §30.11
// "variable-length data in a GpuScene table, referenced by handle" rule. The decode
// writes the side tables (slot-indexed → thread-safe under the OpenMP decode); the
// component metadata (dims/format/bindlessSlot) is written serially. keyCopy's
// resolvedPath view points into textureResolvedPaths[slot].
struct GpuTextureComponent
{
  TextureKey     keyCopy{};
  std::uint64_t  keyHash        = 0;
  std::uint32_t  bindlessSlot   = 0;
  std::uint32_t  width          = 0;
  std::uint32_t  height         = 0;
  nvrhi::Format  format         = nvrhi::Format::UNKNOWN;
};

// RFC 0009 P4 — per-mesh Flecs component (POD identity). The heavy CPU/GPU mesh data
// (positions/indices/attributes/buffers/BLAS/counts) is non-POD or hot-path, so it
// lives in the slot-indexed `meshResources` side table; this component carries only
// the content hash so meshes are queryable by identity (and DestroyMesh can drop the
// dedup-map entry). The DirtyTopology tag marks an entity needing (re)upload + BLAS.
struct GpuMeshComponent
{
  std::uint64_t descHash = 0;
};

// RFC 0009 P5 — per-instance marker component (the data lives in the slot-indexed
// `instanceResources` side table — same thin-key shape as meshes/textures).
struct GpuInstanceComponent
{
};

// RFC 0009 P6 — per-volume marker component (data in the slot-indexed
// `volumeResources` side table; V2.A.5 stub — most volumes are detect-warn-skipped).
struct GpuVolumeComponent
{
};

// RFC 0009 P5 — Flecs relationships an instance entity carries: (Instance, MeshOf,
// meshEntity) and (Instance, MaterialOf, materialEntity). They make "which instances
// reference this mesh/material" a query — the basis for refcounted BLAS sharing +
// orphan detection (§30.11 "prefer pair relationships"). The TLAS/side-table build
// still resolves by handle (byte-identical); these pairs are additive metadata.
struct MeshOf
{
};
struct MaterialOf
{
};

}  // namespace pyxis

namespace pyxis {

namespace gpuscene_detail {

// Handle packing helpers — §19.7 layout. Slot 0 is reserved for the
// `Invalid` sentinel; valid slots start at 1.
constexpr uint32_t HandleEncode(uint32_t slot, uint8_t generation) noexcept
{
  return (slot & HANDLE_SLOT_MASK) | (static_cast<uint32_t>(generation) << HANDLE_SLOT_BITS);
}

constexpr uint32_t HandleSlot(uint32_t handleValue) noexcept
{
  return handleValue & HANDLE_SLOT_MASK;
}

constexpr uint8_t HandleGeneration(uint32_t handleValue) noexcept
{
  return static_cast<uint8_t>(handleValue >> HANDLE_SLOT_BITS);
}

// §19.7: generation 255 quarantines the slot — never reused.
constexpr uint8_t HANDLE_GENERATION_QUARANTINE = 255;

// TLAS capacity. 64K covers v1 production-class scenes; cost is
// ~8 MB TLAS scratch (each instance is ~128 B of metadata + a BLAS
// pointer). §16.5 sharding kicks in past 16M. See _rfcs/RFC-001-
// tlas-cap.md for the sizing rationale + future scaling plan.
constexpr std::size_t TLAS_MAX_INSTANCES = 65536u;

// §16 split rule threshold: BLAS for meshes ≥ 64k tris adds
// AllowCompaction to the build flags.
constexpr uint32_t BLAS_COMPACTION_TRIANGLE_THRESHOLD = 64u * 1024u;

// FNV1a-64 hash. Plan §11 calls for XXH3_64bits long-term; this M5
// stub uses FNV1a so the dedup table works without pulling xxhash
// into the renderer for one milestone. The table doesn't outlive a
// process so a swap to XXH3 at M8 (when World Lobby-scale dedup quality
// matters) is mechanical — only the hash function changes.
constexpr std::uint64_t FNV1A_64_OFFSET = 0xcbf29ce484222325ULL;
constexpr std::uint64_t FNV1A_64_PRIME  = 0x100000001b3ULL;

inline std::uint64_t HashBytes(const void* data, std::size_t size) noexcept
{
  const auto* bytes = static_cast<const std::uint8_t*>(data);
  std::uint64_t hash = FNV1A_64_OFFSET;
  for (std::size_t i = 0; i < size; ++i)
  {
    hash ^= bytes[i];
    hash *= FNV1A_64_PRIME;
  }
  return hash;
}

// Hash an OpenPBRMaterialDesc for dedup. We exclude the
// `sourcePrim` view (diagnostics-only, not part of the material
// identity per §18.5) so two materials authored from different USD
// prims with identical fields collapse to the same handle. The
// `_reserved[16]` trailing slot is included since two
// minor-version-different layouts MUST hash differently — once the
// reserved slots get populated by §22.3 future fields, the hash
// reflects that automatically.
inline std::uint64_t HashMaterialDesc(const OpenPBRMaterialDesc& desc) noexcept
{
  // Hash the bytes of the struct *excluding* the sourcePrim view
  // (which is a pointer + length pair into caller-owned storage,
  // unstable across calls). We hash the prefix up to sourcePrim
  // and the suffix after it, which on the M5 layout are the body
  // and the _reserved[16] tail respectively.
  const auto* base = reinterpret_cast<const std::uint8_t*>(&desc);
  const std::size_t prefixSize = offsetof(OpenPBRMaterialDesc, sourcePrim);
  const std::size_t reservedOff = offsetof(OpenPBRMaterialDesc, _reserved);
  const std::size_t reservedSize = sizeof(desc._reserved);

  std::uint64_t hash = HashBytes(base, prefixSize);
  // Mix in source enum (1 byte at the same offset prefixSize would
  // already be covered if it preceded sourcePrim; in the current
  // layout `Source source` sits BEFORE sourcePrim so prefixSize
  // already covers it — see OpenPBRMaterialDesc.h).
  hash = (hash ^ HashBytes(base + reservedOff, reservedSize)) * FNV1A_64_PRIME;
  return hash;
}

// Hash a MeshDesc by content (positions + indices + optional vertex
// attributes). Used by CreateMesh's content-dedup path so two
// `MeshDesc` calls with byte-identical geometry collapse to the same
// MeshHandle — and therefore one BLAS, satisfying §15's "BLAS keyed
// on `MeshHandle`. If the same SdfPath mesh is consumed by N
// instancers, all share one BLAS." Without this, three separate
// `def Mesh "FooSphere"` prims with identical points/indices yield
// three handles + three BLAS even though they're geometrically
// indistinguishable.
//
// `debugName` is intentionally NOT hashed — it's a diagnostic-only
// field per the §18.4 MeshDesc contract, and dedup by name would
// defeat the geometric-identity dedup we want here. The first
// CreateMesh's debugName "wins" for the shared entry.
inline std::uint64_t HashMeshDesc(const MeshDesc& desc) noexcept
{
  std::uint64_t hash = FNV1A_64_OFFSET;
  // Hash each span's contents byte-by-byte. Empty spans (optional
  // attributes) hash to the FNV1a identity for that mix step.
  auto mixSpan = [&](const void* data, std::size_t bytes) {
    if (bytes == 0)
      return;
    hash = (hash ^ HashBytes(data, bytes)) * FNV1A_64_PRIME;
  };
  mixSpan(desc.positions.data(), desc.positions.size_bytes());
  mixSpan(desc.indices.data(),   desc.indices.size_bytes());
  mixSpan(desc.normals.data(),   desc.normals.size_bytes());
  mixSpan(desc.tangents.data(),  desc.tangents.size_bytes());
  mixSpan(desc.uv0.data(),       desc.uv0.size_bytes());
  return hash;
}

// Hash a TextureKey. The key body is small + has no pointer-bearing
// fields besides the path string_view, which we hash as bytes
// independently.
inline std::uint64_t HashTextureKey(const TextureKey& key) noexcept
{
  std::uint64_t hash = FNV1A_64_OFFSET;
  // Role + colorspace are small enums; hash their bytes directly.
  const auto roleByte = static_cast<std::uint8_t>(key.role);
  const auto cspByte = static_cast<std::uint8_t>(key.colorspace);
  hash ^= roleByte; hash *= FNV1A_64_PRIME;
  hash ^= cspByte;  hash *= FNV1A_64_PRIME;
  // Resolved path string contents — caller-owned span, hash bytes.
  hash = (hash ^ HashBytes(key.resolvedPath.data(), key.resolvedPath.size()))
         * FNV1A_64_PRIME;
  return hash;
}

// Pack an OpenPBRMaterialDesc + computed flag bits into the §11
// 80-byte GPU layout the closesthit reads. `baseColorTex` etc. are
// the BINDLESS slot indices the caller resolved (or
// INVALID_BINDLESS_TEXTURE for "no texture for this lobe").
inline shaderinterop::OpenPBRMaterialGPU PackMaterialGpu(
    const OpenPBRMaterialDesc& desc, std::uint32_t flags,
    std::uint32_t baseColorSlot, std::uint32_t normalSlot, std::uint32_t metallicSlot,
    std::uint32_t roughnessSlot, std::uint32_t emissionSlot, std::uint32_t opacitySlot,
    std::uint32_t transmissionSlot, std::uint32_t coatRoughnessSlot) noexcept
{
  shaderinterop::OpenPBRMaterialGPU gpu{};
  gpu.baseColorR = static_cast<float>(desc.baseColor.x);
  gpu.baseColorG = static_cast<float>(desc.baseColor.y);
  gpu.baseColorB = static_cast<float>(desc.baseColor.z);
  gpu.flags = flags;
  gpu.baseColorTex = baseColorSlot;
  gpu.normalTex = normalSlot;
  gpu.metallicTex = metallicSlot;
  gpu.roughnessTex = roughnessSlot;
  gpu.roughness = desc.roughness;
  gpu.metalness = desc.metalness;
  gpu.opacity = desc.opacity;
  gpu.specularIor = desc.specularIor;
  gpu.coatWeight = desc.coatWeight;
  gpu.coatRoughness = desc.coatRoughness;
  gpu.emissionLuminance = desc.emissionLuminance;
  gpu.emissionTex = emissionSlot;
  gpu.opacityTex = opacitySlot;
  gpu.transmissionTex = transmissionSlot;
  gpu.coatRoughnessTex = coatRoughnessSlot;
  gpu.normalStrength = desc.normalStrength;  // V2.A.23 (was _reserved0)
  // M9 emission RGB. UsdPreviewSurface authors emissive as a color3f
  // (`emissiveColor`); the closesthit emits emissionColor ×
  // emissionLuminance × (sampled emissionTex) when the
  // MATERIAL_FLAG_EMISSIVE bit is set.
  gpu.emissionR = static_cast<float>(desc.emissionColor.x);
  gpu.emissionG = static_cast<float>(desc.emissionColor.y);
  gpu.emissionB = static_cast<float>(desc.emissionColor.z);
  gpu._reserved1 = 0;
  // V2.A.24 — UV transform (scale / rotate-degrees→radians /
  // translate). The closesthit applies it before texture sampling
  // when MATERIAL_FLAG_HAS_UV_TRANSFORM is set (computed in Commit.cpp
  // from these same desc fields).
  constexpr float DEG_TO_RAD = 3.14159265358979323846f / 180.0f;
  gpu.uvScaleX        = desc.baseColorUvScaleX;
  gpu.uvScaleY        = desc.baseColorUvScaleY;
  gpu.uvRotateRadians = desc.baseColorUvRotationDeg * DEG_TO_RAD;
  gpu.uvTranslateX    = desc.baseColorUvTranslationX;
  gpu.uvTranslateY    = desc.baseColorUvTranslationY;
  gpu._reserved2 = 0.0f;
  gpu._reserved3 = 0.0f;
  gpu._reserved4 = 0.0f;
  return gpu;
}

// M7: pack a LightDesc into the 80-byte GPU layout the closesthit
// reads at binding 5. Mirrors LightDesc::Kind into the `kind` enum
// the shader branches on. Direction is normalised here so the
// shader doesn't need to. envMapTex is the bindless slot of the
// dome's lat-long EXR (or INVALID_BINDLESS_TEXTURE for a
// procedural dome — M7-simple ignores it; M7-full's IBL importance-
// sampling lands when the user fills in the closesthit body).
//
// M8a UsdLux coverage: LightDesc gained Cylinder / Geometry / Portal
// kinds that the M7-simple closesthit doesn't render yet. We force
// their `intensity` to 0 here so the shader's existing
// `intensity <= 0 → skip` sentinel keeps them inert; the descCopy on
// LightEntry still carries the original authored intensity so the
// M9 polish pass (which adds proper rendering) can read it back.
inline shaderinterop::LightGpu PackLightGpu(const LightDesc& desc,
                                            std::uint32_t envMapSlot) noexcept
{
  shaderinterop::LightGpu gpu{};
  gpu.colorR = static_cast<float>(desc.color.x);
  gpu.colorG = static_cast<float>(desc.color.y);
  gpu.colorB = static_cast<float>(desc.color.z);
  // PR5 — position-based kinds now all route through the same
  // closesthit code path: point at `L.position` with 1/r² falloff
  // (proper per-shape area sampling lands with M7-full NEE).
  // Stays in lockstep with the LIGHT_KIND_* mirror in
  // ShaderInterop.slang. Only the Sphere / Disk kinds remain
  // unsupported on the GPU side (they still pack their Kind so
  // a future closesthit upgrade can drop the gate).
  const bool kindIsRenderable = (desc.kind == LightDesc::Kind::Distant
                                 || desc.kind == LightDesc::Kind::Dome
                                 || desc.kind == LightDesc::Kind::Rect
                                 || desc.kind == LightDesc::Kind::Cylinder
                                 || desc.kind == LightDesc::Kind::Geometry
                                 || desc.kind == LightDesc::Kind::Portal);
  gpu.intensity = kindIsRenderable ? desc.intensity : 0.0f;
  // Normalise direction defensively — USD's UsdLuxDistantLight
  // authoring conventions sometimes ship un-normalised vectors;
  // the shader assumes unit length so the simple Lambert pass the
  // user fills in at M7-full doesn't need to renormalise.
  const auto dirX = static_cast<float>(desc.direction.x);
  const auto dirY = static_cast<float>(desc.direction.y);
  const auto dirZ = static_cast<float>(desc.direction.z);
  const float dirLen = std::sqrt(dirX * dirX + dirY * dirY + dirZ * dirZ);
  const float dirInvLen = (dirLen > 1e-6f) ? (1.0f / dirLen) : 0.0f;
  gpu.dirX = dirX * dirInvLen;
  gpu.dirY = dirY * dirInvLen;
  gpu.dirZ = dirZ * dirInvLen;
  gpu.kind = static_cast<std::uint32_t>(desc.kind);
  gpu.posX = static_cast<float>(desc.position.x);
  gpu.posY = static_cast<float>(desc.position.y);
  gpu.posZ = static_cast<float>(desc.position.z);
  gpu.envMapTex = envMapSlot;
  gpu.axisUx = static_cast<float>(desc.axisU.x);
  gpu.axisUy = static_cast<float>(desc.axisU.y);
  gpu.axisUz = static_cast<float>(desc.axisU.z);
  gpu.doubleSided = desc.doubleSided ? 1u : 0u;
  gpu.axisVx = static_cast<float>(desc.axisV.x);
  gpu.axisVy = static_cast<float>(desc.axisV.y);
  gpu.axisVz = static_cast<float>(desc.axisV.z);
  // M9-fidelity dome Y-rotation. Only meaningful for Kind::Dome but
  // packed unconditionally — the miss shader gates on kind itself.
  gpu.domeRotationYRadians = static_cast<float>(desc.domeRotationY);
  // M9-fidelity UsdLuxShapingAPI cone. Stored as cos(half-angle) for
  // cheap dot-product comparison in closesthit. shapingConeAngle is
  // in DEGREES on LightDesc per the UsdLuxShapingAPI convention; 90°
  // (the default) means "no cone" — clamp cosOuter to 0.0 in that
  // case so the closesthit path skips the falloff. Softness is a
  // 0..1 fraction of the half-angle defining the smooth-step
  // interior edge.
  const float coneHalfAngleDeg = static_cast<float>(desc.shapingConeAngle);
  if (coneHalfAngleDeg < 90.0f - 1e-4f)
  {
    constexpr float DEG_TO_RAD = 3.14159265358979323846f / 180.0f;
    const float coneHalfAngleRad = coneHalfAngleDeg * DEG_TO_RAD;
    const float softness = std::clamp(static_cast<float>(desc.shapingConeSoftness),
                                      0.0f, 1.0f);
    gpu.shapingConeCosOuter = std::cos(coneHalfAngleRad);
    gpu.shapingConeCosInner = std::cos(coneHalfAngleRad * (1.0f - softness));
  }
  else
  {
    gpu.shapingConeCosOuter = 0.0f;  // sentinel: no cone
    gpu.shapingConeCosInner = 0.0f;
  }
  gpu._reserved1 = 0.0f;
  gpu._reserved2 = 0.0f;
  return gpu;
}

// Lazily (re)create a structured buffer if `handle` is null or
// smaller than `byteSize`. The 6 CommitResources upload phases all
// hit the same shape: structured buffer, ShaderResource state,
// keepInitialState=true, no raw / typed views — only the stride,
// debug name, and error label vary. Returns Expected<void>; the
// caller PYXIS_TRYs to short-circuit on createBuffer failure.
//
// Lives in `gpuscene_detail::` rather than on Impl because it
// doesn't touch any Impl state — just the device + the borrowed
// handle reference.
[[nodiscard]] inline Expected<void> EnsureStructuredBuffer(
    nvrhi::IDevice* device,
    nvrhi::BufferHandle& handle,
    std::size_t byteSize,
    std::size_t structStride,
    std::string_view debugName,
    std::string_view errorLabel) noexcept
{
  if (handle && handle->getDesc().byteSize >= byteSize)
    return {};
  nvrhi::BufferDesc bufDesc;
  bufDesc.byteSize = byteSize;
  bufDesc.structStride = structStride;
  bufDesc.canHaveRawViews = false;
  bufDesc.canHaveTypedViews = false;
  bufDesc.format = nvrhi::Format::UNKNOWN;
  bufDesc.debugName = std::string{debugName};
  bufDesc.initialState = nvrhi::ResourceStates::ShaderResource;
  bufDesc.keepInitialState = true;
  handle = device->createBuffer(bufDesc);
  if (!handle)
  {
    return std::unexpected{PYXIS_ERROR(
        ErrorKind::OutOfMemoryGpu,
        "CommitResources: createBuffer(%.*s, %zu bytes) failed",
        static_cast<int>(errorLabel.size()), errorLabel.data(), byteSize)};
  }
  return {};
}

// RFC 0009 follow-up — incremental upload of a concatenated per-mesh side-table.
//
// Layout (ascending live-slot order):
//   elemBuffer = [slot1 elems][slot2 elems]…   (StructuredBuffer<Elem>)
//   offBuffer  = element start-offset per slot  (StructuredBuffer<uint>)
// The closesthit reads elemBuffer[offBuffer[meshSlot] + …].
//
// Fast path (the audit's quadratic-load fix): when every dirty mesh is a NEW tail
// slot (minDirtySlot >= packedSlots) and the existing buffers still have capacity,
// writes ONLY the new tail region + the (tiny) offset table — O(new geometry) rather
// than O(all geometry). Otherwise it full re-packs, growing the element/offset
// buffers GEOMETRICALLY (doubling) so a run of one-at-a-time appends amortises to
// O(total), not O(total · meshCount). A first allocation is sized exactly, so the
// common single-bulk-commit load wastes no VRAM headroom.
//
// Byte-identical to a single full pack: the concatenation order, per-slot offsets,
// and element bytes are a deterministic function of the live meshes, so a tail-append
// reproduces the exact buffer contents a full rebuild would write. `countOf(slot)`
// returns the element count slot contributes (drives the offset table); `appendOf(
// slot, out)` appends slot's elements. The two MUST agree on per-slot count.
template <typename Elem, typename CountFn, typename AppendFn>
[[nodiscard]] inline Expected<void> UploadMeshSideTable(
    nvrhi::IDevice*      device,
    nvrhi::ICommandList* commandList,
    const GpuSlotMap&    slots,
    uint32_t             minDirtySlot,
    uint32_t&            packedSlots,
    nvrhi::BufferHandle& elemBuffer,
    nvrhi::BufferHandle& offBuffer,
    const Elem&          emptyFallback,
    std::string_view     elemDebugName,
    std::string_view     elemErrorLabel,
    std::string_view     offDebugName,
    std::string_view     offErrorLabel,
    CountFn              countOf,
    AppendFn             appendOf) noexcept
{
  const uint32_t slotCount = slots.SlotCount();

  // Offset table — a cheap count-only pass (no element data copied). Slot 0 (the
  // §19.7 sentinel) and dead slots contribute 0, so their offset equals the next
  // live slot's start, exactly as the legacy per-buffer pack produced.
  std::vector<std::uint32_t> offsets(slotCount, 0u);
  std::uint32_t running = 0;
  for (uint32_t slot = 0; slot < slotCount; ++slot)
  {
    offsets[slot] = running;
    if (slots.IsLive(slot))
      running += countOf(slot);
  }
  const std::size_t stride       = sizeof(Elem);
  const std::size_t totalBytes   = static_cast<std::size_t>(running) * stride;
  const std::size_t offsetsBytes = offsets.size() * sizeof(std::uint32_t);

  // Fast path: dirty meshes are all new tail slots + the existing buffers fit.
  const bool canAppend = elemBuffer && offBuffer && packedSlots <= slotCount
                         && minDirtySlot >= packedSlots
                         && totalBytes <= elemBuffer->getDesc().byteSize
                         && offsetsBytes <= offBuffer->getDesc().byteSize;

  if (canAppend)
  {
    std::vector<Elem> tail;
    for (uint32_t slot = packedSlots; slot < slotCount; ++slot)
      if (slots.IsLive(slot))
        appendOf(slot, tail);
    if (!tail.empty())
    {
      const std::size_t tailStartBytes =
          static_cast<std::size_t>(offsets[packedSlots]) * stride;
      commandList->writeBuffer(elemBuffer.Get(), tail.data(), tail.size() * stride,
                               tailStartBytes);
    }
    commandList->writeBuffer(offBuffer.Get(), offsets.data(), offsetsBytes);
    packedSlots = slotCount;
    return {};
  }

  // Full re-pack.
  std::vector<Elem> packed;
  packed.reserve(running != 0u ? running : 1u);
  for (uint32_t slot = 0; slot < slotCount; ++slot)
    if (slots.IsLive(slot))
      appendOf(slot, packed);
  if (packed.empty())
    packed.push_back(emptyFallback);  // keep a valid 1-element bindless buffer.
  const std::size_t packedBytes = packed.size() * stride;

  // Geometric growth: a grow of an existing buffer doubles (amortised-O(1) append
  // over a run of incremental commits); a first/sufficient allocation is left exact
  // (EnsureStructuredBuffer early-outs when the current size already fits).
  const auto grownSize = [](const nvrhi::BufferHandle& handle, std::size_t need) -> std::size_t {
    if (handle && handle->getDesc().byteSize < need)
      return std::max(need, static_cast<std::size_t>(handle->getDesc().byteSize) * 2u);
    return need;
  };
  PYXIS_TRY(EnsureStructuredBuffer(device, elemBuffer, grownSize(elemBuffer, packedBytes),
                                   stride, elemDebugName, elemErrorLabel));
  PYXIS_TRY(EnsureStructuredBuffer(device, offBuffer, grownSize(offBuffer, offsetsBytes),
                                   sizeof(std::uint32_t), offDebugName, offErrorLabel));
  commandList->writeBuffer(elemBuffer.Get(), packed.data(), packedBytes);
  commandList->writeBuffer(offBuffer.Get(), offsets.data(), offsetsBytes);
  packedSlots = slotCount;
  return {};
}

}  // namespace gpuscene_detail

// PIMPL body. Defined here so per-verb .cpp files can declare member
// functions on it; the public header (GpuScene.h) only forward-
// declares this. All NVRHI handles + STL containers live behind this
// boundary per §18.9.
//
// Field order here groups related data for cache locality +
// readability (per-resource sections, dirty-flag bools clustered
// at the end); optimal-packing reorder would scatter related
// fields apart and hurt the cognitive map without changing
// runtime hot-path costs (Impl is allocated once per renderer
// instance, not in any loop).
// NOLINTNEXTLINE(clang-analyzer-optin.performance.Padding)
struct GpuScene::Impl
{
  // RFC 0009 P4 — per-mesh resource record (the heavy CPU/GPU data). Slot-indexed in
  // `meshResources`, paired with a GpuMeshComponent entity (meshSlots owns
  // slot/generation/liveness; DirtyTopology marks (re)upload+BLAS). The old
  // live/quarantined/generation/needsGpuUpload/needsBlasBuild fields moved to
  // meshSlots + the DirtyTopology tag.
  struct MeshResource
  {
    std::vector<hlslpp::float3>  positions;
    std::vector<std::uint32_t>   indices;
    std::vector<hlslpp::float3>  normals;
    std::vector<hlslpp::float4>  tangents;
    std::vector<hlslpp::float2>  uv0;
    std::string                  debugName;

    nvrhi::BufferHandle          vertexBuffer;
    nvrhi::BufferHandle          indexBuffer;
    std::uint32_t                vertexCount = 0;
    std::uint32_t                indexCount  = 0;
    nvrhi::rt::AccelStructHandle blas;

    // M7 NdotL: per-triangle face normals in object space, computed
    // from positions + indices at CreateMesh time. Used by the
    // closesthit's Lambert pass via the gMeshFaceNormals flat buffer
    // (offset = gMeshFaceOffsets[meshSlot] + PrimitiveIndex()).
    // float4 instead of float3 — std430-style 16-byte alignment +
    // closesthit reads .xyz only; the w slot is reserved for an
    // M9 per-face flag (alpha-test, double-sided override, etc.).
    std::vector<hlslpp::float4>  faceNormals;

    // §15 content-dedup: FNV1a-64 of (positions + indices + optional
    // attributes) at CreateMesh time. Stored on the entry so
    // DestroyMesh can erase the matching map entry without re-hashing
    // the (possibly-cleared) buffers.
    std::uint64_t                descHash       = 0;
  };

  // RFC 0009 P5 — per-instance resource record (slot-indexed in `instanceResources`,
  // paired with a GpuInstanceComponent entity). live/quarantined/generation moved to
  // instanceSlots; DirtyTransform marks a transform change (future TLAS refit).
  struct InstanceResource
  {
    MeshHandle       mesh        = MeshHandle::Invalid;
    MaterialHandle   material    = MaterialHandle::Invalid;
    hlslpp::float4x4 worldFromLocal{};
    bool             visible     = true;
    bool             doubleSided = false;  // V2.A.x — UsdGeomGprim::doubleSided.
    std::string      debugName;
  };

  // RFC 0009 P1 — lights are no longer a std::vector<LightEntry>; they live as
  // GpuLightComponent entities in `sceneWorld` (see members below). LightEntry,
  // the `lights` vector, `freeLightSlots`, LookupLight and ResolveLight are gone.

  // V2.A.5 — UsdVolVolume / OpenVDBAsset slot. Owns the dense float
  // voxel buffer (CPU-side, dropped after upload) + the NVRHI 3D
  // texture (R32_FLOAT). The closesthit doesn't sample the texture
  // in v2 — the entry exists so the volume-integrator follow-up has
  // a stable per-volume container to bind from. Size + transform
  // metadata is kept on the entry so the future integrator pass can
  // build a per-volume cbuffer without re-reading the source .vdb.
  // RFC 0009 P6 — per-volume resource record (slot-indexed in `volumeResources`,
  // paired with a GpuVolumeComponent entity; volumeSlots owns slot/generation).
  // needsGpuUpload stays per-record (volumes are rare; no DirtyVolume tag needed).
  struct VolumeResource
  {
    bool                    needsGpuUpload = false;
    std::array<uint32_t, 3> dimensions{0, 0, 0};
    std::array<float, 3>    bboxMin{0, 0, 0};
    std::array<float, 3>    bboxMax{0, 0, 0};
    std::array<float, 16>   indexToWorld{};
    std::vector<float>      voxelData;     // dropped after upload commits
    nvrhi::TextureHandle    texture;
    std::uint64_t           bytesOnGpu     = 0;  // textureBytes contribution.
    std::string             debugName;
  };

  // M5: material entry. Holds the CPU-side OpenPBRMaterialDesc copy
  // + the slot index inside the material GPU buffer (the bindless
  // table the closesthit reads via instanceCustomIndex). `descHash`
  // is the FNV1a-64 hash of the descriptor body and is used by the
  // dedup map; identical descs collapse to the same MaterialHandle
  // per the §11 dedup rule.
  // RFC 0009 P2 — materials are GpuMaterialComponent entities (see GpuSlotMap
  // materialSlots below), not a std::vector<MaterialEntry>. The struct is gone.

  // M5: texture entry. The TextureKey copy + the NVRHI texture +
  // the bindless slot index used by the closesthit's
  // `baseColorMaps[material.baseColorTex]` lookup. Decode is
  // synchronous at M5 (the §31 async decode pool wires at M8 when
  // texture-load latency starts to dominate); CPU-side decoded
  // pixels are dropped after the GPU upload retires.
  // RFC 0009 P3 — textures are GpuTextureComponent entities (textureSlots below)
  // plus the slot-indexed GPU-resource side tables; the TextureEntry struct is gone.

  nvrhi::IDevice*    device   = nullptr;  // borrowed; outlives this scene.
  Profiler*          profiler = nullptr;  // borrowed.
  GpuSceneCreateDesc desc{};

  // Per-frame stat counters. Mutation verbs accumulate into these
  // directly; CommitResources zeros the counters documented as
  // per-frame (`staleHandleDrops` / `pendingUploads` /
  // `pendingBlasBuilds`) at the start of each frame so
  // `LastFrameStats()` between commits reports exactly the current
  // in-progress frame's activity.
  FrameStats lastFrameStats{};

  // RFC 0009 — the Flecs SceneWorld. Entities live here; the per-type handle tables
  // map handle<->entity. Declaration order matters: tables/queries reference
  // sceneWorld, so it must be declared FIRST (destruct last).
  flecs::world                      sceneWorld;
  // P1 — lights (GpuLightComponent). HandleBimap (§8.2) owns slot/generation; the
  // query is built once in the ctor (§30.11, no per-frame build).
  scene::HandleBimap                lightHandles;
  flecs::query<GpuLightComponent>   lightQuery;
  // RFC 0009 follow-up — CommitResources runs the §30.11 phase pipeline for real via
  // sceneWorld.progress(). The per-commit command list + first-error live here (the
  // registered systems are ctor lambdas that capture `this`, so no FrameContext
  // singleton component is needed). `commitPipelineRegistered` guards one-time setup.
  nvrhi::ICommandList*              currentCommandList = nullptr;
  Expected<void>                    commitError{};
  bool                              commitPipelineRegistered = false;
  // Review fix #3 — lowest DirtyTopology mesh slot, computed ONCE per commit so the
  // five mesh side-table uploaders don't each rescan the whole mesh table.
  uint32_t                          commitLowestDirtyMeshSlot = 0;
  // P2 — materials (GpuMaterialComponent). GpuSlotMap keeps slot == GPU buffer index
  // (gpuscene_detail encoding) so the packed material buffer + instance side-table
  // are byte-identical. `materialSourcePrims` is the slot-indexed owned-string table
  // backing each desc's sourcePrim view (variable-length data → a GpuScene table).
  GpuSlotMap                        materialSlots{sceneWorld};
  std::vector<std::string>          materialSourcePrims;
  // P3 — textures (GpuTextureComponent). Slot-indexed side tables hold the non-POD
  // GPU resource + transient decode state (slot == bindless slot for M5). Distinct
  // slots are written from distinct OpenMP threads during decode (thread-safe);
  // component metadata is written serially.
  GpuSlotMap                        textureSlots{sceneWorld};
  std::vector<nvrhi::TextureHandle> textureResources;     // the GPU texture per slot.
  std::vector<std::vector<std::uint8_t>> texturePixelData;  // transient; dropped post-upload.
  std::vector<std::string>          textureResolvedPaths;  // owned backing for keyCopy.resolvedPath.
  // Review fix #4 — LRU access tick per texture slot. Mutable hot metadata lives in a
  // side table (not GpuTextureComponent) so a cache hit bumps it with a plain write
  // instead of a get<>()+set<>() round-trip that also re-persisted a fragile view.
  std::vector<std::uint64_t>        textureLastAccessTick;
  // P4 — meshes (GpuMeshComponent). meshResources is the slot-indexed heavy CPU/GPU
  // data; meshSlots owns slot/generation/liveness; DirtyTopology marks (re)upload+BLAS.
  GpuSlotMap                        meshSlots{sceneWorld};
  std::vector<MeshResource>         meshResources;
  // P5 — instances (GpuInstanceComponent). instanceResources is the slot-indexed
  // data; instanceSlots owns slot/generation/liveness (slot == instanceCustomIndex);
  // DirtyTransform marks a transform change.
  GpuSlotMap                        instanceSlots{sceneWorld};
  std::vector<InstanceResource>     instanceResources;
  // P6 — volumes (GpuVolumeComponent). volumeResources is the slot-indexed data;
  // volumeSlots owns slot/generation/liveness.
  GpuSlotMap                        volumeSlots{sceneWorld};
  std::vector<VolumeResource>       volumeResources;

  // RFC 0009 — all per-type free-slot recycling now lives inside each GpuSlotMap
  // (gpuscene_detail encoding, LIFO reuse), so the old free-slot vectors are gone.
  // V2.A.5 — set when a fresh AddVolume needs CommitResources to
  // create the matching nvrhi::TextureHandle + write the dense
  // float buffer into it via writeTexture(). Cleared after
  // UploadPendingVolumes drains the queue.
  bool                       volumesNeedGpuUpload = false;

  // M5 dedup maps: hash → handle. AcquireMaterial / AcquireTexture
  // hash their input desc / key, look up here, and return the
  // existing handle on a hit. The §11 OpenPBR architecture rule
  // ("hashed via XXH3_64bits, deduplicated") relies on these maps
  // collapsing identical materials in a World Lobby-scale scene where
  // the same UsdShadeMaterial is bound to thousands of meshes.
  // M5 stub uses FNV1a-64 (10-line inline impl below); XXH3
  // upgrade is on the M8 perf-sweep checklist.
  std::unordered_map<std::uint64_t, MaterialHandle> materialDescHashToHandle;
  std::unordered_map<std::uint64_t, TextureHandle>  textureKeyHashToHandle;

  // V2.A.12 — LRU tick counter. Bumped on every AcquireTexture (hit
  // or miss); written onto the entry's `lastAccessTick`. Wraps at
  // 64-bit (i.e. effectively never). Eviction policy reads the
  // delta against this counter when the future `EvictColdTextures`
  // pass lands.
  std::uint64_t nextTextureAccessTick = 0;
  std::uint64_t lruHitCount  = 0;  // texture acquires that returned an existing handle
  std::uint64_t lruMissCount = 0;  // acquires that allocated a fresh slot
  // §15 content-dedup map: hash → MeshHandle. CreateMesh hashes the
  // input MeshDesc's geometry, looks up here, and returns the
  // existing handle on a hit so identical mesh content authored under
  // different SdfPaths shares one MeshHandle → one BLAS. Required for
  // PointInstancer-with-shared-prototype + identical-content scenes
  // (default.usd's three spheres) to actually share BLAS the way
  // §15 promises.
  std::unordered_map<std::uint64_t, MeshHandle>     meshDescHashToHandle;

  // M5 GPU pools.
  // - materialGpuBuffer: structured buffer of OpenPBRMaterialGPU,
  //   indexed by material slot. Re-uploaded on every commit if any
  //   material has needsGpuUpload — small enough (M5 stub: <1 MiB)
  //   that we don't bother with partial updates.
  // - instanceMaterialBuffer (M6): structured buffer of uint, indexed
  //   by instance slot, value = material slot. The closesthit reads
  //   `materials[instanceMaterial[InstanceID()]]` — one indirection
  //   so the TLAS instanceCustomIndex can carry the INSTANCE slot
  //   (per plan §15) instead of the material slot (M5 expedience).
  //   Freeing instanceCustomIndex unblocks the M6 instanceId AOV +
  //   future picking (§19.4).
  // - bindlessSampler: a single shared linear-clamp sampler. Per-
  //   role samplers (sRGB filtering, anisotropic for tangent maps,
  //   etc.) are an M9 polish item.
  nvrhi::BufferHandle  materialGpuBuffer;
  bool                 materialsNeedGpuUpload = false;
  nvrhi::BufferHandle  instanceMaterialBuffer;
  nvrhi::SamplerHandle bindlessSampler;
  // M9-fidelity per-role samplers. `bindlessSampler` (above) is
  // Wrap-Wrap-Wrap for tiling material textures; `domeSampler` is
  // Wrap-Clamp-Wrap for the HDRI dome's lat-long mapping (V-axis
  // clamp prevents the elevation seam at the poles from mirroring
  // +Y onto -Y).
  nvrhi::SamplerHandle domeSampler;

  // M7: structured buffer of LightGpu entries the closesthit reads
  // via the simple per-light contribution loop at binding 5. Sized
  // to the count of LIVE LightEntry slots (sparse storage with
  // holes is fine — the closesthit iterates the buffer's full
  // length, but we only emit live lights, so dead slots never
  // appear in the upload). Re-uploaded whenever the dedicated
  // dirty flag fires (AddLight / UpdateLight / RemoveLight).
  nvrhi::BufferHandle  lightsGpuBuffer;
  bool                 lightsNeedGpuUpload = false;

  // M7 NdotL: per-mesh face normals concatenated into one flat
  // buffer + per-mesh-slot starting offsets. Closesthit reads:
  //   nLocal = gMeshFaceNormals[gMeshFaceOffsets[meshSlot]
  //                            + PrimitiveIndex()].xyz
  // Then transforms via Vulkan's `ObjectToWorld3x4()` to get
  // world-space N for the Lambert pass. Uploaded alongside BLAS
  // builds (computed in CreateMesh; flushed at CommitResources
  // when meshFaceNormalsNeedUpload is set).
  // Plus gInstanceMeshBuffer: per-instance mesh slot, indexed by
  // instance slot — the closesthit needs to know which mesh's
  // face-normal range to look in. Same lifecycle / dirty-flag shape
  // as instanceMaterialBuffer.
  nvrhi::BufferHandle  meshFaceNormalsBuffer;
  nvrhi::BufferHandle  meshFaceOffsetsBuffer;
  bool                 meshFaceNormalsNeedUpload = false;
  // RFC 0009 follow-up — # mesh slots already concatenated into the buffer above.
  // A new upload appends only slots >= this (the tail fast path) when no lower slot
  // changed; see UploadMeshSideTable. One tracker per concatenated buffer (they share
  // a lifecycle but each owns its packed extent, so there is no cross-buffer hazard).
  uint32_t             meshFaceNormalsPackedSlots = 0;
  nvrhi::BufferHandle  instanceMeshBuffer;

  // M8a UV pipeline: per-mesh UVs + per-triangle indices concatenated
  // into flat structured buffers + per-mesh-slot start-offset tables.
  // Closesthit reads:
  //   indexOffset = gMeshIndexOffsets[meshSlot]
  //   uvOffset    = gMeshUvOffsets[meshSlot]
  //   v0,v1,v2    = gMeshIndices[indexOffset + PrimitiveIndex()*3 + {0,1,2}]
  //   uv0,uv1,uv2 = gMeshUvs[uvOffset + v_i]
  //   uv          = barycentric_interp(uv0, uv1, uv2, attribs.bary)
  // Then samples bindless gBindlessTextures[mat.baseColorTex] at uv.
  // Sized + uploaded by UploadMeshUvs / UploadMeshIndices in
  // Commit.cpp; same dirty-flag shape as meshFaceNormals.
  nvrhi::BufferHandle  meshUvsBuffer;
  nvrhi::BufferHandle  meshUvOffsetsBuffer;
  nvrhi::BufferHandle  meshIndicesBuffer;
  nvrhi::BufferHandle  meshIndexOffsetsBuffer;
  bool                 meshUvsNeedUpload     = false;
  bool                 meshIndicesNeedUpload = false;
  uint32_t             meshUvsPackedSlots     = 0;  // see meshFaceNormalsPackedSlots.
  uint32_t             meshIndicesPackedSlots = 0;

  // M9 smooth shading: per-vertex normals concatenated into one flat
  // float4 buffer + per-mesh-slot start offsets. Mirror of the
  // per-triangle face-normal buffer above but per-VERTEX so the
  // closesthit can barycentric-interpolate three vertex normals at
  // each hit. Stored as float4 for std430 alignment + a future
  // tangent.w sign-bit slot. Empty for meshes with no authored
  // normals — closesthit detects a near-zero magnitude and falls
  // back to the M7 face-normal path.
  nvrhi::BufferHandle  meshVertexNormalsBuffer;
  nvrhi::BufferHandle  meshVertexNormalOffsetsBuffer;
  bool                 meshVertexNormalsNeedUpload = false;
  uint32_t             meshVertexNormalsPackedSlots = 0;  // see meshFaceNormalsPackedSlots.

  // M9 normal mapping: per-vertex tangents from MikkTSpace. float4
  // stride — xyz is the unit tangent, w is the bitangent sign
  // (+/- 1) for the closesthit's `bitangent = sign × cross(N, T)`
  // construction. Empty when the mesh has no UVs or normals (those
  // are MikkTSpace prereqs); closesthit's normal-mapping branch then
  // falls back to using the vertex-interpolated normal without TBN.
  nvrhi::BufferHandle  meshTangentsBuffer;
  nvrhi::BufferHandle  meshTangentOffsetsBuffer;
  bool                 meshTangentsNeedUpload = false;
  uint32_t             meshTangentsPackedSlots = 0;  // see meshFaceNormalsPackedSlots.

  // Magenta 4x4 fallback texture — slot 0 in the bindless table is
  // permanently the "missing texture" colour so any material whose
  // resolved path failed to decode renders visibly-broken instead
  // of black-or-undefined-memory. The texture is created on first
  // CommitResources and reused for the lifetime of the scene.
  nvrhi::TextureHandle missingTexture;

  bool       hasCamera = false;
  CameraDesc cameraDesc{};

  // Top-level acceleration structure. Allocated lazily on the first
  // TLAS rebuild so an empty scene doesn't pay for it.
  nvrhi::rt::AccelStructHandle tlas;
  bool                         tlasNeedsRebuild = false;
  // RFC 0009 — TLAS refit (§16 "rebuilt every frame if dirty; refit otherwise").
  // `tlasStructureChanged` (set by Append/Destroy/SetVisibility — the instance SET or
  // its BLAS pointers change) forces a full rebuild; a transform-only edit
  // (UpdateInstanceTransform) leaves it false → eligible for a cheap refit.
  // `tlasAllowsUpdate` tracks whether the live TLAS was built with the AllowUpdate
  // flag. It stays false for static scenes (which never call UpdateInstanceTransform),
  // so their TLAS build is byte-identical to before; the AllowUpdate variant is only
  // created once a transform edit actually needs refit capability.
  bool                         tlasStructureChanged = true;
  bool                         tlasAllowsUpdate     = false;
  // Review fix #1 — instance count of the last TLAS build. A refit (PerformUpdate)
  // is only valid against an identical instance count/topology, so a transform-only
  // tick whose gathered count differs (e.g. DestroyMesh dropped a still-referenced
  // mesh's BLAS, which does NOT set tlasStructureChanged) must full-rebuild, not refit.
  uint32_t                     tlasBuiltInstanceCount = 0;
  // Review fix #2 — cached single-dome env-map texture, refreshed once per commit
  // (RefreshDomeEnvMapCache). GetDomeEnvMapTexture is read every frame by the
  // PathTracePass binding path; recomputing there allocated + sorted the live-light
  // set per frame.
  nvrhi::ITexture*             domeEnvMapTexture = nullptr;

  // M6 audit closeout: separate dirty track for the instance→material
  // side-table buffer (binding 4). Kept distinct from tlasNeedsRebuild
  // so UpdateInstanceMaterial doesn't pointlessly trigger a TLAS
  // rebuild — the TLAS doesn't change when an instance's bound
  // material changes; only the side-table does. AppendInstance,
  // DestroyInstance, SetInstanceVisibility (which all DO change the
  // TLAS) implicitly need a side-table re-upload too, so they bump
  // both flags. UpdateInstanceMaterial only bumps this one.
  bool                         instanceMaterialNeedsUpload = false;

  // ---- Handle resolution -----------------------------------------------
  //
  // Two layers per entry type:
  //
  //   LookupX  — const, no side effects. Returns nullptr for any
  //              invalid case (Invalid sentinel, slot out of range,
  //              recycled-generation, dead). Used by Has* predicates +
  //              read-only paths where the §18.5 stale-handle counter
  //              must NOT bump.
  //
  //   ResolveX — mutating, counter-aware. Same nullptr cases as Lookup
  //              but bumps `staleHandleDrops` whenever a non-Invalid
  //              handle failed to resolve. Used by void-returning
  //              Update* / Destroy* verbs per the §18.5 contract:
  //              Invalid silently no-ops (no counter bump); recycled /
  //              out-of-range bumps the counter and returns nullptr.
  //
  // Resolve composes from Lookup so the underlying validation logic
  // stays in one place.

  template <typename Entry>
  [[nodiscard]] static const Entry* LookupEntryImpl(uint32_t handleValue,
                                                    const std::vector<Entry>& entries) noexcept
  {
    if (handleValue == 0)
      return nullptr;
    const uint32_t slot = gpuscene_detail::HandleSlot(handleValue);
    if (slot == 0 || slot >= entries.size())
      return nullptr;
    const Entry& entry = entries[slot];
    if (!entry.live || entry.quarantined
        || entry.generation != gpuscene_detail::HandleGeneration(handleValue))
      return nullptr;
    return &entry;
  }

  // Promote a non-Invalid lookup miss into a stale-handle counter
  // bump per §18.5. Returns the same pointer for transparent chaining.
  template <typename Entry, typename Handle>
  [[nodiscard]] Entry* BumpIfStaleAndReturn(Handle handle, const Entry* entry) noexcept
  {
    if (entry == nullptr && static_cast<uint32_t>(handle) != 0)
      ++lastFrameStats.staleHandleDrops;
    return const_cast<Entry*>(entry);
  }

  // RFC 0009 P6 — resolve a VolumeHandle to its slot-indexed VolumeResource.
  [[nodiscard]] VolumeResource* ResolveVolumeResource(VolumeHandle handle) noexcept
  {
    const flecs::entity entity = volumeSlots.Resolve(static_cast<uint32_t>(handle));
    if (entity.id() == 0)
      return nullptr;
    const uint32_t slot = GpuSlotMap::SlotOf(static_cast<uint32_t>(handle));
    return slot < volumeResources.size() ? &volumeResources[slot] : nullptr;
  }

  // RFC 0009 P5 — resolve an InstanceHandle to its slot-indexed InstanceResource (or
  // null for Invalid/stale). Does NOT bump staleHandleDrops; callers that need the
  // §18.5 count do it explicitly (mirrors the per-verb pattern of the other types).
  [[nodiscard]] InstanceResource* ResolveInstanceResource(InstanceHandle handle) noexcept
  {
    const flecs::entity entity = instanceSlots.Resolve(static_cast<uint32_t>(handle));
    if (entity.id() == 0)
      return nullptr;
    const uint32_t slot = GpuSlotMap::SlotOf(static_cast<uint32_t>(handle));
    return slot < instanceResources.size() ? &instanceResources[slot] : nullptr;
  }

  // RFC 0009 P4 — resolve a MeshHandle to its slot-indexed MeshResource (or null for
  // Invalid/stale handles). Mirrors the old ResolveMesh contract via meshSlots.
  [[nodiscard]] MeshResource* ResolveMeshResource(MeshHandle handle) noexcept
  {
    const flecs::entity entity = meshSlots.Resolve(static_cast<uint32_t>(handle));
    if (entity.id() == 0)
      return nullptr;
    const uint32_t slot = GpuSlotMap::SlotOf(static_cast<uint32_t>(handle));
    return slot < meshResources.size() ? &meshResources[slot] : nullptr;
  }

  // RFC 0009 P1 — collect live lights as (encoded handle, desc) sorted by slot
  // index, the deterministic order the GPU light buffer + editor accessors need
  // (replaces iterating the old `lights` vector in slot order). ~30 lights, so the
  // collect+sort is negligible; called per-commit (packing) + per-frame (editor).
  void CollectLiveLightsSorted(std::vector<std::pair<uint32_t, LightDesc>>& out);

  // Resolve a TextureHandle to its bindless slot index, or to
  // INVALID_BINDLESS_TEXTURE for Invalid / out-of-range / dead handles. Used when
  // packing OpenPBRMaterialGPU entries. RFC 0009 P3 — reads the texture entity's
  // component (bindlessSlot reflects decode success: 0 = decode failed → fallback).
  [[nodiscard]] std::uint32_t ResolveTextureBindlessSlot(TextureHandle handle) const noexcept
  {
    const flecs::entity entity = textureSlots.Resolve(static_cast<uint32_t>(handle));
    if (entity.id() == 0)
      return shaderinterop::INVALID_BINDLESS_TEXTURE;
    // Review fix #8 — try_get (not get) so a live entity that somehow lacks the
    // component returns the fallback instead of dereferencing null: flecs get<T>()'s
    // presence assert is compiled out under NDEBUG (release).
    const GpuTextureComponent* component = entity.try_get<GpuTextureComponent>();
    return component ? component->bindlessSlot : shaderinterop::INVALID_BINDLESS_TEXTURE;
  }

  // ---- Verb member functions (defined in per-verb .cpp files) ----------
  // Each public `GpuScene::Verb()` in GpuScene.cpp forwards one line
  // to the matching `Impl::Verb()` here. The split keeps fields +
  // helpers visible without rewriting every `_impl->X` to `impl.X`.

  // Mesh.cpp
  Expected<MeshHandle> CreateMesh(const MeshDesc& meshDesc);
  Expected<void>       UpdateMesh(MeshHandle meshHandle, const MeshDesc& meshDesc);
  void                 DestroyMesh(MeshHandle meshHandle);
  [[nodiscard]] bool   HasMesh(MeshHandle meshHandle) const;

  // Material.cpp
  MaterialHandle       AcquireMaterial(const OpenPBRMaterialDesc& materialDesc);
  void                 UpdateMaterial(MaterialHandle materialHandle,
                                      const OpenPBRMaterialDesc& materialDesc);
  void                 DestroyMaterial(MaterialHandle materialHandle);
  [[nodiscard]] bool   HasMaterial(MaterialHandle materialHandle) const;

  // Texture.cpp
  TextureHandle        AcquireTexture(const TextureKey& textureKey);
  void                 DestroyTexture(TextureHandle textureHandle);
  [[nodiscard]] bool   HasTexture(TextureHandle textureHandle) const;
  // V2.A.12 — LRU eviction. Drops textures by ascending lastAccessTick
  // until total decoded byte count is below `targetBytes`. Returns the
  // count evicted. Safe to call between frames.
  std::uint32_t        EvictColdTextures(std::uint64_t targetBytes) noexcept;

  // Instance.cpp
  Expected<InstanceHandle> AppendInstance(const InstanceDesc& instanceDesc);
  void                     UpdateInstanceTransform(InstanceHandle instanceHandle,
                                                   const hlslpp::float4x4& worldFromLocal);
  void                     UpdateInstanceMaterial(InstanceHandle instanceHandle,
                                                  MaterialHandle materialHandle);
  void                     SetInstanceVisibility(InstanceHandle instanceHandle, bool visible);
  void                     DestroyInstance(InstanceHandle instanceHandle);
  [[nodiscard]] bool       HasInstance(InstanceHandle instanceHandle) const;

  // Light.cpp (camera + lights)
  void                 SetCamera(const CameraDesc& cameraDesc);
  LightHandle          AddLight(const LightDesc& lightDesc);
  void                 UpdateLight(LightHandle lightHandle, const LightDesc& lightDesc);
  void                 RemoveLight(LightHandle lightHandle);

  // Volume.cpp (V2.A.5)
  VolumeHandle         AddVolume(const VolumeDesc& volumeDesc);
  void                 RemoveVolume(VolumeHandle volumeHandle);
  [[nodiscard]] bool   HasVolume(VolumeHandle volumeHandle) const;

  // Commit.cpp (Clear + CommitResources — both touch every table).
  // CommitResources is an orchestrator over the per-resource-type
  // member functions below; each one services one upload/build phase
  // and propagates GPU-creation failures up through PYXIS_TRY.
  void                 Clear() noexcept;
  [[nodiscard]] Expected<void> CommitResources(nvrhi::ICommandList* commandList);
  // RFC 0009 follow-up — register the §30.11 phases + the commit systems on
  // sceneWorld (once). CommitResources then drives them via sceneWorld.progress().
  void                 RegisterCommitPipeline() noexcept;

  [[nodiscard]] Expected<void> UploadPendingMeshes(nvrhi::ICommandList* commandList);
  [[nodiscard]] Expected<void> EnsureBindlessFallbacks(nvrhi::ICommandList* commandList);
  [[nodiscard]] Expected<void> UploadPendingTextures(nvrhi::ICommandList* commandList);
  [[nodiscard]] Expected<void> UploadMaterialBuffer(nvrhi::ICommandList* commandList);
  [[nodiscard]] Expected<void> UploadLightBuffer(nvrhi::ICommandList* commandList);
  [[nodiscard]] Expected<void> BuildPendingBlas(nvrhi::ICommandList* commandList);
  [[nodiscard]] Expected<void> RebuildTlasIfDirty(nvrhi::ICommandList* commandList);
  [[nodiscard]] Expected<void> UploadInstanceSideTables(nvrhi::ICommandList* commandList);
  [[nodiscard]] Expected<void> UploadMeshFaceNormals(nvrhi::ICommandList* commandList);
  [[nodiscard]] Expected<void> UploadMeshUvs(nvrhi::ICommandList* commandList);
  [[nodiscard]] Expected<void> UploadMeshIndices(nvrhi::ICommandList* commandList);
  [[nodiscard]] Expected<void> UploadMeshVertexNormals(nvrhi::ICommandList* commandList);
  [[nodiscard]] Expected<void> UploadMeshTangents(nvrhi::ICommandList* commandList);
  [[nodiscard]] Expected<void> UploadPendingVolumes(nvrhi::ICommandList* commandList);

  // RFC 0009 follow-up — lowest live mesh slot carrying scene::DirtyTopology, or
  // meshSlots.SlotCount() when none are dirty. Drives the incremental side-table
  // append fast path: when this is >= the buffer's packedSlots, every dirty mesh is
  // a NEW tail slot and only that tail needs re-uploading (see UploadMeshSideTable).
  [[nodiscard]] uint32_t LowestDirtyMeshSlot() const noexcept;

  // Review fix #2 — recompute the cached single-dome env-map texture (the first live
  // Dome light with a resolved env-map, slot order). Called once at the end of each
  // CommitResources; GetDomeEnvMapTexture() then returns the cached pointer.
  void RefreshDomeEnvMapCache() noexcept;
};

}  // namespace pyxis
