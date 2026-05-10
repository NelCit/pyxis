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

#include <cstdint>
#include <string>
#include <unordered_map>
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
// process so a swap to XXH3 at M8 (when Bistro-scale dedup quality
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
  gpu._reserved0 = 0;
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
  // Force-disable kinds the closesthit doesn't render. Stays in
  // lockstep with the LIGHT_KIND_* mirror in ShaderInterop.slang
  // (Distant=0, Dome=1, Rect=2).
  const bool kindIsRenderable = (desc.kind == LightDesc::Kind::Distant
                                 || desc.kind == LightDesc::Kind::Dome
                                 || desc.kind == LightDesc::Kind::Rect);
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
  gpu._reserved0 = 0;
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

}  // namespace gpuscene_detail

// PIMPL body. Defined here so per-verb .cpp files can declare member
// functions on it; the public header (GpuScene.h) only forward-
// declares this. All NVRHI handles + STL containers live behind this
// boundary per §18.9.
struct GpuScene::Impl
{
  // Per-mesh entry. Holds the CPU-side input MeshDesc spans + the
  // NVRHI vertex/index buffers + BLAS handle.
  struct MeshEntry
  {
    bool         live           = false;
    bool         quarantined    = false;
    bool         needsGpuUpload = false;
    bool         needsBlasBuild = false;
    std::uint8_t generation     = 0;

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

  struct InstanceEntry
  {
    bool             live        = false;
    bool             quarantined = false;
    std::uint8_t     generation  = 0;
    MeshHandle       mesh        = MeshHandle::Invalid;
    MaterialHandle   material    = MaterialHandle::Invalid;
    hlslpp::float4x4 worldFromLocal{};
    bool             visible     = true;
    std::string      debugName;
  };

  struct LightEntry
  {
    bool         live        = false;
    bool         quarantined = false;
    std::uint8_t generation  = 0;
    LightDesc    descCopy{};
  };

  // M5: material entry. Holds the CPU-side OpenPBRMaterialDesc copy
  // + the slot index inside the material GPU buffer (the bindless
  // table the closesthit reads via instanceCustomIndex). `descHash`
  // is the FNV1a-64 hash of the descriptor body and is used by the
  // dedup map; identical descs collapse to the same MaterialHandle
  // per the §11 dedup rule.
  struct MaterialEntry
  {
    bool                 live           = false;
    bool                 quarantined    = false;
    bool                 needsGpuUpload = false;
    std::uint8_t         generation     = 0;
    OpenPBRMaterialDesc  descCopy{};
    std::uint64_t        descHash       = 0;
    std::string          sourcePrim;       // owned copy of descCopy.sourcePrim
  };

  // M5: texture entry. The TextureKey copy + the NVRHI texture +
  // the bindless slot index used by the closesthit's
  // `baseColorMaps[material.baseColorTex]` lookup. Decode is
  // synchronous at M5 (the §31 async decode pool wires at M8 when
  // texture-load latency starts to dominate); CPU-side decoded
  // pixels are dropped after the GPU upload retires.
  struct TextureEntry
  {
    bool                 live           = false;
    bool                 quarantined    = false;
    bool                 needsGpuUpload = false;
    std::uint8_t         generation     = 0;
    TextureKey           keyCopy{};
    std::uint64_t        keyHash        = 0;
    std::string          resolvedPath;     // owned copy of keyCopy.resolvedPath
    nvrhi::TextureHandle texture;
    std::uint32_t        bindlessSlot   = 0;
    std::uint32_t        width          = 0;
    std::uint32_t        height         = 0;
    nvrhi::Format        format         = nvrhi::Format::UNKNOWN;
    std::vector<std::uint8_t> pixelData;   // dropped after upload commits
  };

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

  std::vector<MeshEntry>     meshes;
  std::vector<InstanceEntry> instances;
  std::vector<LightEntry>    lights;
  std::vector<MaterialEntry> materials;
  std::vector<TextureEntry>  textures;

  // O(1) slot recycling. Each Destroy verb pushes the freed slot;
  // each Acquire / Append verb pops the latest one. Stack ordering
  // keeps the most-recently-freed slot first, which matches CPU
  // cache + keeps the §15 dedup-by-content hash stable for repeated
  // CreateMesh calls with identical geometry. Without these, the
  // per-verb bodies would linear-scan their entry vectors looking
  // for a free slot — quadratic over the lifetime of an
  // append-heavy load.
  std::vector<std::uint32_t> freeMeshSlots;
  std::vector<std::uint32_t> freeInstanceSlots;
  std::vector<std::uint32_t> freeMaterialSlots;
  std::vector<std::uint32_t> freeTextureSlots;
  std::vector<std::uint32_t> freeLightSlots;

  // M5 dedup maps: hash → handle. AcquireMaterial / AcquireTexture
  // hash their input desc / key, look up here, and return the
  // existing handle on a hit. The §11 OpenPBR architecture rule
  // ("hashed via XXH3_64bits, deduplicated") relies on these maps
  // collapsing identical materials in a Bistro-scale scene where
  // the same UsdShadeMaterial is bound to thousands of meshes.
  // M5 stub uses FNV1a-64 (10-line inline impl below); XXH3
  // upgrade is on the M8 perf-sweep checklist.
  std::unordered_map<std::uint64_t, MaterialHandle> materialDescHashToHandle;
  std::unordered_map<std::uint64_t, TextureHandle>  textureKeyHashToHandle;
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

  [[nodiscard]] const MeshEntry*     LookupMesh(MeshHandle handle) const noexcept
  { return LookupEntryImpl(static_cast<uint32_t>(handle), meshes); }
  [[nodiscard]] const MaterialEntry* LookupMaterial(MaterialHandle handle) const noexcept
  { return LookupEntryImpl(static_cast<uint32_t>(handle), materials); }
  [[nodiscard]] const TextureEntry*  LookupTexture(TextureHandle handle) const noexcept
  { return LookupEntryImpl(static_cast<uint32_t>(handle), textures); }
  [[nodiscard]] const InstanceEntry* LookupInstance(InstanceHandle handle) const noexcept
  { return LookupEntryImpl(static_cast<uint32_t>(handle), instances); }
  [[nodiscard]] const LightEntry*    LookupLight(LightHandle handle) const noexcept
  { return LookupEntryImpl(static_cast<uint32_t>(handle), lights); }

  [[nodiscard]] MeshEntry*     ResolveMesh(MeshHandle handle) noexcept
  { return BumpIfStaleAndReturn(handle, LookupMesh(handle)); }
  [[nodiscard]] MaterialEntry* ResolveMaterial(MaterialHandle handle) noexcept
  { return BumpIfStaleAndReturn(handle, LookupMaterial(handle)); }
  [[nodiscard]] TextureEntry*  ResolveTexture(TextureHandle handle) noexcept
  { return BumpIfStaleAndReturn(handle, LookupTexture(handle)); }
  [[nodiscard]] InstanceEntry* ResolveInstance(InstanceHandle handle) noexcept
  { return BumpIfStaleAndReturn(handle, LookupInstance(handle)); }
  [[nodiscard]] LightEntry*    ResolveLight(LightHandle handle) noexcept
  { return BumpIfStaleAndReturn(handle, LookupLight(handle)); }

  // Resolve a TextureHandle to its bindless slot index, or to
  // INVALID_BINDLESS_TEXTURE for Invalid / out-of-range / dead
  // handles. Used at upload time when packing OpenPBRMaterialGPU
  // entries. Lives on Impl (rather than gpuscene_detail) so the
  // nested TextureEntry type stays in-scope without exposing it
  // outside the private PIMPL boundary.
  [[nodiscard]] std::uint32_t ResolveTextureBindlessSlot(TextureHandle handle) const noexcept
  {
    const TextureEntry* entry = LookupTexture(handle);
    return entry ? entry->bindlessSlot : shaderinterop::INVALID_BINDLESS_TEXTURE;
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

  // Commit.cpp (Clear + CommitResources — both touch every table).
  // CommitResources is an orchestrator over the per-resource-type
  // member functions below; each one services one upload/build phase
  // and propagates GPU-creation failures up through PYXIS_TRY.
  void                 Clear() noexcept;
  [[nodiscard]] Expected<void> CommitResources(nvrhi::ICommandList* commandList);

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
};

}  // namespace pyxis
