// Pyxis renderer — GpuScene internal helpers (gpuscene_detail).
//
// Split out of Internal.h (RFC 0009 follow-up). Handle packing (§19.7), FNV1a
// dedup hashing, the GPU-struct packers (PackMaterialGpu / PackLightGpu),
// EnsureStructuredBuffer, and the incremental UploadMeshSideTable template. All
// stateless — they operate on the device + borrowed handles, not on Impl.
// PRIVATE header — only files under Private/GpuScene/ may include it (§18.9).

#pragma once

#include "GpuScene/GpuSlotMap.h"
#include "ShaderInterop.slang"

#include <Pyxis/Renderer/GpuScene.h>  // descs + Expected / PYXIS_ERROR + HANDLE_* + hlslpp

#include <nvrhi/nvrhi.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

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
// The closesthit reads elemBuffer[gMeshInfo[meshSlot].<offset field> + …]. P2
// packing: the per-slot start offsets no longer live in a parallel uint buffer —
// UploadMeshInfo packs them into the MeshInfoGpu table from the SAME running-sum
// walk, so this template manages only the element buffer.
//
// Fast path (the audit's quadratic-load fix): when every dirty mesh is a NEW tail
// slot (minDirtySlot >= packedSlots) and the existing buffer still has capacity,
// writes ONLY the new tail region — O(new geometry) rather than O(all geometry).
// Otherwise it full re-packs, growing the element buffer GEOMETRICALLY (doubling)
// so a run of one-at-a-time appends amortises to O(total), not
// O(total · meshCount). A first allocation is sized exactly, so the common
// single-bulk-commit load wastes no VRAM headroom.
//
// Byte-identical to a single full pack: the concatenation order, per-slot offsets,
// and element bytes are a deterministic function of the live meshes, so a tail-append
// reproduces the exact buffer contents a full rebuild would write. `countOf(slot)`
// returns the element count slot contributes (drives UploadMeshInfo's offset sums);
// `appendOf(slot, out)` appends slot's elements. The two MUST agree on per-slot count.
template <typename Elem, typename CountFn, typename AppendFn>
[[nodiscard]] inline Expected<void> UploadMeshSideTable(
    nvrhi::IDevice*      device,
    nvrhi::ICommandList* commandList,
    const GpuSlotMap&    slots,
    uint32_t             minDirtySlot,
    uint32_t&            packedSlots,
    nvrhi::BufferHandle& elemBuffer,
    const Elem&          emptyFallback,
    std::string_view     elemDebugName,
    std::string_view     elemErrorLabel,
    CountFn              countOf,
    AppendFn             appendOf) noexcept
{
  const uint32_t slotCount = slots.SlotCount();

  // Count-only pass (no element data copied): total element count + the tail start
  // (the element offset of the first slot >= packedSlots). Slot 0 (the §19.7
  // sentinel) and dead slots contribute 0, so the offsets stay monotone exactly as
  // the legacy per-buffer pack produced — UploadMeshInfo reproduces the same sums.
  std::uint32_t running        = 0;
  std::uint32_t tailStartElems = 0;
  for (uint32_t slot = 0; slot < slotCount; ++slot)
  {
    if (slot == packedSlots)
      tailStartElems = running;
    if (slots.IsLive(slot))
      running += countOf(slot);
  }
  const std::size_t stride     = sizeof(Elem);
  const std::size_t totalBytes = static_cast<std::size_t>(running) * stride;

  // Fast path: dirty meshes are all new tail slots + the existing buffer fits.
  // packedSlots < slotCount is implied (the caller gates on a dirty slot existing,
  // and minDirtySlot >= packedSlots), so tailStartElems was assigned above.
  const bool canAppend = elemBuffer && packedSlots <= slotCount
                         && minDirtySlot >= packedSlots
                         && totalBytes <= elemBuffer->getDesc().byteSize;

  if (canAppend)
  {
    std::vector<Elem> tail;
    for (uint32_t slot = packedSlots; slot < slotCount; ++slot)
      if (slots.IsLive(slot))
        appendOf(slot, tail);
    if (!tail.empty())
    {
      const std::size_t tailStartBytes =
          static_cast<std::size_t>(tailStartElems) * stride;
      commandList->writeBuffer(elemBuffer.Get(), tail.data(), tail.size() * stride,
                               tailStartBytes);
    }
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
  commandList->writeBuffer(elemBuffer.Get(), packed.data(), packedBytes);
  packedSlots = slotCount;
  return {};
}

}  // namespace gpuscene_detail

}  // namespace pyxis
