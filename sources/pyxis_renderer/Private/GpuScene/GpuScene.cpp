// Pyxis renderer — GpuScene scene-mutation API.
//
// Plan §18.5. PIMPL: every NVRHI handle, entry-table vector, and
// per-frame ring slot lives inside `GpuScene::Impl` so the public
// header stays NVRHI- and STL-container-free per §18.9.
//
// Slot 0 is the canonical Invalid sentinel for every handle table
// — the ctor pushes a permanently-quarantined entry into each
// vector so a fabricated handle whose slot decodes to 0 never
// resolves to a live entry.
//
// CommitResources currently services:
//   1. Mesh GPU upload (vertex + index NVRHI buffers via
//      writeBuffer staging).
//   2. BLAS build per mesh (§16 split rule).
//   3. TLAS rebuild gathering live + visible instances.
//
// Camera-uniform upload + the path-trace dispatch live in
// PathTracePass (see Private/Passes/PathTracePass.cpp).

#include <Pyxis/Renderer/GpuScene.h>

#include <Pyxis/Platform/Logging/Log.h>
#include <Pyxis/Platform/Logging/LogCategories.h>
#include <Pyxis/Renderer/Descs/FrameStats.h>
#include <Pyxis/Renderer/Forward.h>
#include <Pyxis/Renderer/Profiler.h>

#include "Materials/MaterialFlag.h"

// ShaderInterop.slang lives in resources/shaders/ — the
// pyxis_renderer target's PRIVATE include path puts that directory
// on the search path so the C++ side here gets the same
// OpenPBRMaterialGPU layout the closesthit reads. See pyxis_renderer/
// CMakeLists.txt (`target_include_directories ... PRIVATE
// ${CMAKE_SOURCE_DIR}/resources/shaders`) for the wiring rationale.
#include "ShaderInterop.slang"

#include <nvrhi/nvrhi.h>

#include <stb_image.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace pyxis {

namespace {

// Handle packing helpers — §19.7 layout. Slot 0 is reserved for the
// `Invalid` sentinel; valid slots start at 1.
constexpr uint32_t HandleEncode(uint32_t slot, uint8_t generation) noexcept {
  return (slot & HANDLE_SLOT_MASK) | (static_cast<uint32_t>(generation) << HANDLE_SLOT_BITS);
}

constexpr uint32_t HandleSlot(uint32_t handleValue) noexcept {
  return handleValue & HANDLE_SLOT_MASK;
}

constexpr uint8_t HandleGeneration(uint32_t handleValue) noexcept {
  return static_cast<uint8_t>(handleValue >> HANDLE_SLOT_BITS);
}

// §19.7: generation 255 quarantines the slot — never reused.
constexpr uint8_t HANDLE_GENERATION_QUARANTINE = 255;

// TLAS capacity. M3 single-cube ships one instance; the cap of 256
// is enough for the M3.5 default-scene composition (3 spheres +
// ground = ~5 instances). M6+ scales this up alongside the
// 16M-instance shard threshold (§16.5).
constexpr std::size_t TLAS_MAX_INSTANCES = 256u;

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

std::uint64_t HashBytes(const void* data, std::size_t size) noexcept {
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
std::uint64_t HashMaterialDesc(const OpenPBRMaterialDesc& desc) noexcept {
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

// Hash a TextureKey. The key body is small + has no pointer-bearing
// fields besides the path string_view, which we hash as bytes
// independently.
std::uint64_t HashTextureKey(const TextureKey& key) noexcept {
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
shaderinterop::OpenPBRMaterialGPU PackMaterialGpu(
    const OpenPBRMaterialDesc& desc, std::uint32_t flags,
    std::uint32_t baseColorSlot, std::uint32_t normalSlot, std::uint32_t metallicSlot,
    std::uint32_t roughnessSlot, std::uint32_t emissionSlot, std::uint32_t opacitySlot,
    std::uint32_t transmissionSlot, std::uint32_t coatRoughnessSlot) noexcept {
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

// Resolve a TextureHandle to its bindless slot index, or to
// INVALID_BINDLESS_TEXTURE for Invalid / out-of-range / dead handles.
// Used at upload time when packing OpenPBRMaterialGPU entries.
template <typename TextureEntryVector>
std::uint32_t ResolveTextureBindlessSlot(TextureHandle handle,
                                         const TextureEntryVector& textures) noexcept {
  const auto value = static_cast<std::uint32_t>(handle);
  if (value == 0)
    return shaderinterop::INVALID_BINDLESS_TEXTURE;
  const std::uint32_t slot = HandleSlot(value);
  if (slot == 0 || slot >= textures.size())
    return shaderinterop::INVALID_BINDLESS_TEXTURE;
  const auto& entry = textures[slot];
  if (!entry.live || entry.quarantined || entry.generation != HandleGeneration(value))
    return shaderinterop::INVALID_BINDLESS_TEXTURE;
  return entry.bindlessSlot;
}

}  // namespace

struct GpuScene::Impl {
  // Per-mesh entry. Holds the CPU-side input MeshDesc spans + the
  // NVRHI vertex/index buffers + BLAS handle.
  struct MeshEntry {
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
  };

  struct InstanceEntry {
    bool             live        = false;
    bool             quarantined = false;
    std::uint8_t     generation  = 0;
    MeshHandle       mesh        = MeshHandle::Invalid;
    MaterialHandle   material    = MaterialHandle::Invalid;
    hlslpp::float4x4 worldFromLocal{};
    bool             visible     = true;
    std::string      debugName;
  };

  struct LightEntry {
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
  struct MaterialEntry {
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
  struct TextureEntry {
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

  // Resolver helpers — centralise the §18.5 stale-handle policy for
  // void-returning Update* / Destroy* verbs. Invalid silently no-ops
  // with no counter bump; recycled / out-of-range handles bump
  // staleHandleDrops and return nullptr.
  [[nodiscard]] InstanceEntry* ResolveInstance(InstanceHandle handle) noexcept {
    const auto value = static_cast<uint32_t>(handle);
    if (value == 0)
      return nullptr;
    const uint32_t slot = HandleSlot(value);
    if (slot == 0 || slot >= instances.size())
    {
      ++lastFrameStats.staleHandleDrops;
      return nullptr;
    }
    InstanceEntry& entry = instances[slot];
    if (!entry.live || entry.quarantined || entry.generation != HandleGeneration(value))
    {
      ++lastFrameStats.staleHandleDrops;
      return nullptr;
    }
    return &entry;
  }

  [[nodiscard]] LightEntry* ResolveLight(LightHandle handle) noexcept {
    const auto value = static_cast<uint32_t>(handle);
    if (value == 0)
      return nullptr;
    const uint32_t slot = HandleSlot(value);
    if (slot == 0 || slot >= lights.size())
    {
      ++lastFrameStats.staleHandleDrops;
      return nullptr;
    }
    LightEntry& entry = lights[slot];
    if (!entry.live || entry.quarantined || entry.generation != HandleGeneration(value))
    {
      ++lastFrameStats.staleHandleDrops;
      return nullptr;
    }
    return &entry;
  }
};

GpuScene::GpuScene(nvrhi::IDevice* device, Profiler& profiler, const GpuSceneCreateDesc& desc)
    : _impl(std::make_unique<Impl>()) {
  _impl->device = device;
  _impl->profiler = &profiler;
  _impl->desc = desc;
  // Slot 0 is the Invalid sentinel for every handle table — keep
  // each one permanently quarantined so a fabricated handle whose
  // slot decodes to 0 never resolves.
  _impl->meshes.emplace_back();
  _impl->meshes[0].quarantined = true;
  _impl->instances.emplace_back();
  _impl->instances[0].quarantined = true;
  _impl->lights.emplace_back();
  _impl->lights[0].quarantined = true;
}

// Out-of-line dtor so unique_ptr<Impl>'s deleter sees the complete
// Impl type — Impl is forward-declared in the public header.
GpuScene::~GpuScene() = default;

// ---- Mesh ------------------------------------------------------------------
Expected<MeshHandle> GpuScene::CreateMesh(const MeshDesc& meshDesc) {
  // ---- Validate input ----------------------------------------------------
  if (meshDesc.positions.empty())
  {
    return std::unexpected{
        PYXIS_ERROR(ErrorKind::InvalidArgument,
                    "CreateMesh: positions span is empty (mesh requires >= 1 vertex)")};
  }
  if (meshDesc.indices.empty())
  {
    return std::unexpected{
        PYXIS_ERROR(ErrorKind::InvalidArgument,
                    "CreateMesh: indices span is empty (mesh requires a triangle list)")};
  }
  if ((meshDesc.indices.size() % 3) != 0)
  {
    return std::unexpected{PYXIS_ERROR(
        ErrorKind::InvalidArgument,
        "CreateMesh: indices.size()=%zu is not a multiple of 3 (triangle list expected)",
        meshDesc.indices.size())};
  }
  const uint32_t vertexCount = static_cast<uint32_t>(meshDesc.positions.size());
  for (const uint32_t indexValue : meshDesc.indices)
  {
    if (indexValue >= vertexCount)
    {
      return std::unexpected{PYXIS_ERROR(ErrorKind::InvalidArgument,
                                         "CreateMesh: index %u >= vertexCount %u", indexValue,
                                         vertexCount)};
    }
  }
  if (!meshDesc.normals.empty() && meshDesc.normals.size() != meshDesc.positions.size())
  {
    return std::unexpected{
        PYXIS_ERROR(ErrorKind::InvalidArgument,
                    "CreateMesh: normals.size()=%zu must match positions.size()=%zu",
                    meshDesc.normals.size(), meshDesc.positions.size())};
  }
  if (!meshDesc.tangents.empty() && meshDesc.tangents.size() != meshDesc.positions.size())
  {
    return std::unexpected{
        PYXIS_ERROR(ErrorKind::InvalidArgument,
                    "CreateMesh: tangents.size()=%zu must match positions.size()=%zu",
                    meshDesc.tangents.size(), meshDesc.positions.size())};
  }
  if (!meshDesc.uv0.empty() && meshDesc.uv0.size() != meshDesc.positions.size())
  {
    return std::unexpected{PYXIS_ERROR(ErrorKind::InvalidArgument,
                                       "CreateMesh: uv0.size()=%zu must match positions.size()=%zu",
                                       meshDesc.uv0.size(), meshDesc.positions.size())};
  }

  // ---- Allocate slot -----------------------------------------------------
  uint32_t slot = 0;
  for (uint32_t candidateSlot = 1; candidateSlot < _impl->meshes.size(); ++candidateSlot)
  {
    const Impl::MeshEntry& candidate = _impl->meshes[candidateSlot];
    if (!candidate.live && !candidate.quarantined)
    {
      slot = candidateSlot;
      break;
    }
  }
  if (slot == 0)
  {
    if (_impl->meshes.size() >= (1u << HANDLE_SLOT_BITS))
    {
      return std::unexpected{PYXIS_ERROR(
          ErrorKind::InvalidState, "CreateMesh: mesh-handle slot space exhausted (limit = %u)",
          (1u << HANDLE_SLOT_BITS))};
    }
    _impl->meshes.emplace_back();
    slot = static_cast<uint32_t>(_impl->meshes.size() - 1);
  }

  // ---- Populate entry ----------------------------------------------------
  Impl::MeshEntry& entry = _impl->meshes[slot];
  entry.live = true;
  entry.needsGpuUpload = true;
  entry.needsBlasBuild = true;
  entry.vertexCount = static_cast<uint32_t>(meshDesc.positions.size());
  entry.indexCount = static_cast<uint32_t>(meshDesc.indices.size());
  entry.positions.assign(meshDesc.positions.begin(), meshDesc.positions.end());
  entry.indices.assign(meshDesc.indices.begin(), meshDesc.indices.end());
  entry.normals.assign(meshDesc.normals.begin(), meshDesc.normals.end());
  entry.tangents.assign(meshDesc.tangents.begin(), meshDesc.tangents.end());
  entry.uv0.assign(meshDesc.uv0.begin(), meshDesc.uv0.end());
  entry.debugName.assign(meshDesc.debugName);

  return static_cast<MeshHandle>(HandleEncode(slot, entry.generation));
}

Expected<void> GpuScene::UpdateMesh(MeshHandle /*meshHandle*/, const MeshDesc& /*meshDesc*/) {
  return std::unexpected{PYXIS_ERROR(ErrorKind::NotImplemented, "GpuScene::UpdateMesh — M3 stub")};
}

void GpuScene::DestroyMesh(MeshHandle meshHandle) {
  const auto value = static_cast<uint32_t>(meshHandle);
  if (value == 0)
    return;
  const uint32_t slot = HandleSlot(value);
  if (slot == 0 || slot >= _impl->meshes.size())
  {
    ++_impl->lastFrameStats.staleHandleDrops;
    return;
  }
  Impl::MeshEntry& entry = _impl->meshes[slot];
  if (!entry.live || entry.quarantined || entry.generation != HandleGeneration(value))
  {
    ++_impl->lastFrameStats.staleHandleDrops;
    return;
  }
  entry.live = false;
  entry.needsGpuUpload = false;
  entry.needsBlasBuild = false;
  entry.positions.clear();
  entry.indices.clear();
  entry.normals.clear();
  entry.tangents.clear();
  entry.uv0.clear();
  entry.debugName.clear();
  // Drop the GPU resources. NVRHI's deferred-destruction queue
  // keeps them alive until any in-flight command list that
  // references them retires.
  entry.vertexBuffer = nullptr;
  entry.indexBuffer = nullptr;
  entry.blas = nullptr;
  entry.vertexCount = 0;
  entry.indexCount = 0;
  if (entry.generation == HANDLE_GENERATION_QUARANTINE)
  {
    entry.quarantined = true;
  }
  else
  {
    ++entry.generation;
  }
}

bool GpuScene::HasMesh(MeshHandle meshHandle) const {
  const auto value = static_cast<uint32_t>(meshHandle);
  if (value == 0)
    return false;
  const uint32_t slot = HandleSlot(value);
  if (slot == 0 || slot >= _impl->meshes.size())
    return false;
  const Impl::MeshEntry& entry = _impl->meshes[slot];
  return entry.live && !entry.quarantined && entry.generation == HandleGeneration(value);
}

// ---- Material --------------------------------------------------------------
MaterialHandle GpuScene::AcquireMaterial(const OpenPBRMaterialDesc& materialDesc) {
  // §11 dedup: hash → existing handle if present, else allocate
  // a new slot. Lazy-init the materials vector with slot 0
  // reserved (the §19.7 invalid-handle sentinel) so `materialId =
  // 0` in the closesthit unambiguously means "no material".
  if (_impl->materials.empty())
    _impl->materials.emplace_back();  // sentinel slot 0

  const std::uint64_t hash = HashMaterialDesc(materialDesc);
  if (auto found = _impl->materialDescHashToHandle.find(hash);
      found != _impl->materialDescHashToHandle.end())
  {
    // Cache hit — return the existing handle. (Hash collisions
    // are theoretically possible at FNV1a-64; in practice
    // material counts in v1 are small enough that the
    // birthday-paradox risk is negligible. M8 XXH3 swap revisits.)
    return found->second;
  }

  uint32_t slot = 0;
  for (uint32_t candidate = 1; candidate < _impl->materials.size(); ++candidate)
  {
    auto& entry = _impl->materials[candidate];
    if (!entry.live && !entry.quarantined)
    {
      slot = candidate;
      break;
    }
  }
  if (slot == 0)
  {
    if (_impl->materials.size() >= (1u << HANDLE_SLOT_BITS))
      return MaterialHandle::Invalid;  // slot space exhausted
    _impl->materials.emplace_back();
    slot = static_cast<uint32_t>(_impl->materials.size() - 1);
  }

  Impl::MaterialEntry& entry = _impl->materials[slot];
  entry.live = true;
  entry.needsGpuUpload = true;
  entry.descCopy = materialDesc;
  entry.descHash = hash;
  entry.sourcePrim.assign(materialDesc.sourcePrim);
  entry.descCopy.sourcePrim = entry.sourcePrim;  // re-point at owned copy

  const auto handle = static_cast<MaterialHandle>(HandleEncode(slot, entry.generation));
  _impl->materialDescHashToHandle.emplace(hash, handle);
  _impl->materialsNeedGpuUpload = true;
  return handle;
}

void GpuScene::UpdateMaterial(MaterialHandle materialHandle,
                              const OpenPBRMaterialDesc& materialDesc) {
  const auto value = static_cast<uint32_t>(materialHandle);
  if (value == 0)
    return;
  const uint32_t slot = HandleSlot(value);
  if (slot == 0 || slot >= _impl->materials.size())
  {
    ++_impl->lastFrameStats.staleHandleDrops;
    return;
  }
  Impl::MaterialEntry& entry = _impl->materials[slot];
  if (!entry.live || entry.quarantined || entry.generation != HandleGeneration(value))
  {
    ++_impl->lastFrameStats.staleHandleDrops;
    return;
  }
  // Re-hash + dedup-map maintenance: drop the old hash entry, add
  // the new one. If the new hash already maps to a different live
  // material, we leave that alone (Update doesn't merge handles —
  // semantics: this material's *fields* changed in place).
  _impl->materialDescHashToHandle.erase(entry.descHash);
  entry.descCopy = materialDesc;
  entry.descHash = HashMaterialDesc(materialDesc);
  entry.sourcePrim.assign(materialDesc.sourcePrim);
  entry.descCopy.sourcePrim = entry.sourcePrim;
  entry.needsGpuUpload = true;
  _impl->materialDescHashToHandle.emplace(entry.descHash, materialHandle);
  _impl->materialsNeedGpuUpload = true;
}

void GpuScene::DestroyMaterial(MaterialHandle materialHandle) {
  const auto value = static_cast<uint32_t>(materialHandle);
  if (value == 0)
    return;
  const uint32_t slot = HandleSlot(value);
  if (slot == 0 || slot >= _impl->materials.size())
  {
    ++_impl->lastFrameStats.staleHandleDrops;
    return;
  }
  Impl::MaterialEntry& entry = _impl->materials[slot];
  if (!entry.live || entry.quarantined || entry.generation != HandleGeneration(value))
  {
    ++_impl->lastFrameStats.staleHandleDrops;
    return;
  }
  _impl->materialDescHashToHandle.erase(entry.descHash);
  entry.live = false;
  entry.descCopy = OpenPBRMaterialDesc{};
  entry.sourcePrim.clear();
  if (entry.generation == HANDLE_GENERATION_QUARANTINE)
    entry.quarantined = true;
  else
    ++entry.generation;
}

bool GpuScene::HasMaterial(MaterialHandle materialHandle) const {
  const auto value = static_cast<uint32_t>(materialHandle);
  if (value == 0)
    return false;
  const uint32_t slot = HandleSlot(value);
  if (slot == 0 || slot >= _impl->materials.size())
    return false;
  const Impl::MaterialEntry& entry = _impl->materials[slot];
  return entry.live && !entry.quarantined && entry.generation == HandleGeneration(value);
}

// ---- Texture ---------------------------------------------------------------
TextureHandle GpuScene::AcquireTexture(const TextureKey& textureKey) {
  // Same dedup + slot-allocation shape as AcquireMaterial. Decode
  // happens lazily inside CommitResources (M5 stub: synchronous
  // stb_image decode on the render thread; the §31 async I/O pool
  // wires at M8 when texture-load latency starts to dominate).
  if (_impl->textures.empty())
    _impl->textures.emplace_back();  // sentinel slot 0

  const std::uint64_t hash = HashTextureKey(textureKey);
  if (auto found = _impl->textureKeyHashToHandle.find(hash);
      found != _impl->textureKeyHashToHandle.end())
  {
    return found->second;
  }

  uint32_t slot = 0;
  for (uint32_t candidate = 1; candidate < _impl->textures.size(); ++candidate)
  {
    auto& entry = _impl->textures[candidate];
    if (!entry.live && !entry.quarantined)
    {
      slot = candidate;
      break;
    }
  }
  if (slot == 0)
  {
    if (_impl->textures.size() >= (1u << HANDLE_SLOT_BITS))
      return TextureHandle::Invalid;
    _impl->textures.emplace_back();
    slot = static_cast<uint32_t>(_impl->textures.size() - 1);
  }

  Impl::TextureEntry& entry = _impl->textures[slot];
  entry.live = true;
  entry.needsGpuUpload = true;
  entry.keyCopy = textureKey;
  entry.keyHash = hash;
  entry.resolvedPath.assign(textureKey.resolvedPath);
  entry.keyCopy.resolvedPath = entry.resolvedPath;  // re-point at owned copy
  entry.bindlessSlot = slot;  // bindless slot = handle slot for M5

  const auto handle = static_cast<TextureHandle>(HandleEncode(slot, entry.generation));
  _impl->textureKeyHashToHandle.emplace(hash, handle);
  return handle;
}

void GpuScene::DestroyTexture(TextureHandle textureHandle) {
  const auto value = static_cast<uint32_t>(textureHandle);
  if (value == 0)
    return;
  const uint32_t slot = HandleSlot(value);
  if (slot == 0 || slot >= _impl->textures.size())
  {
    ++_impl->lastFrameStats.staleHandleDrops;
    return;
  }
  Impl::TextureEntry& entry = _impl->textures[slot];
  if (!entry.live || entry.quarantined || entry.generation != HandleGeneration(value))
  {
    ++_impl->lastFrameStats.staleHandleDrops;
    return;
  }
  _impl->textureKeyHashToHandle.erase(entry.keyHash);
  entry.live = false;
  entry.texture = nullptr;
  entry.resolvedPath.clear();
  entry.pixelData.clear();
  entry.pixelData.shrink_to_fit();
  if (entry.generation == HANDLE_GENERATION_QUARANTINE)
    entry.quarantined = true;
  else
    ++entry.generation;
}

bool GpuScene::HasTexture(TextureHandle textureHandle) const {
  const auto value = static_cast<uint32_t>(textureHandle);
  if (value == 0)
    return false;
  const uint32_t slot = HandleSlot(value);
  if (slot == 0 || slot >= _impl->textures.size())
    return false;
  const Impl::TextureEntry& entry = _impl->textures[slot];
  return entry.live && !entry.quarantined && entry.generation == HandleGeneration(value);
}

// ---- Instance --------------------------------------------------------------
Expected<InstanceHandle> GpuScene::AppendInstance(const InstanceDesc& instanceDesc) {
  if (instanceDesc.mesh == MeshHandle::Invalid)
  {
    return std::unexpected{
        PYXIS_ERROR(ErrorKind::InvalidArgument, "AppendInstance: mesh handle is Invalid")};
  }
  if (!HasMesh(instanceDesc.mesh))
  {
    return std::unexpected{
        PYXIS_ERROR(ErrorKind::InvalidHandle,
                    "AppendInstance: mesh handle %u not live (slot+generation mismatch)",
                    static_cast<uint32_t>(instanceDesc.mesh))};
  }

  uint32_t slot = 0;
  for (uint32_t candidateSlot = 1; candidateSlot < _impl->instances.size(); ++candidateSlot)
  {
    const Impl::InstanceEntry& candidate = _impl->instances[candidateSlot];
    if (!candidate.live && !candidate.quarantined)
    {
      slot = candidateSlot;
      break;
    }
  }
  if (slot == 0)
  {
    if (_impl->instances.size() >= (1u << HANDLE_SLOT_BITS))
    {
      return std::unexpected{
          PYXIS_ERROR(ErrorKind::TlasInstanceLimitExceeded,
                      "AppendInstance: instance-handle slot space exhausted (limit = %u)",
                      (1u << HANDLE_SLOT_BITS))};
    }
    _impl->instances.emplace_back();
    slot = static_cast<uint32_t>(_impl->instances.size() - 1);
  }

  Impl::InstanceEntry& entry = _impl->instances[slot];
  entry.live = true;
  entry.mesh = instanceDesc.mesh;
  entry.material = instanceDesc.material;
  entry.worldFromLocal = instanceDesc.worldFromLocal;
  entry.visible = instanceDesc.visible;
  entry.debugName.assign(instanceDesc.debugName);

  // AppendInstance changes the TLAS (new instance to pack) AND the
  // side-table (new entry to hold this instance's material slot).
  _impl->tlasNeedsRebuild = true;
  _impl->instanceMaterialNeedsUpload = true;
  return static_cast<InstanceHandle>(HandleEncode(slot, entry.generation));
}

void GpuScene::UpdateInstanceTransform(InstanceHandle instanceHandle,
                                       const hlslpp::float4x4& worldFromLocal) {
  if (auto* entry = _impl->ResolveInstance(instanceHandle))
  {
    entry->worldFromLocal = worldFromLocal;
    _impl->tlasNeedsRebuild = true;
  }
}

void GpuScene::UpdateInstanceMaterial(InstanceHandle instanceHandle,
                                      MaterialHandle materialHandle) {
  if (auto* entry = _impl->ResolveInstance(instanceHandle))
  {
    // M6 audit closeout: only the side-table needs re-upload; the
    // TLAS doesn't know about materials (it only carries mesh BLAS
    // + transform + visibility + the per-§15 instance slot in
    // instanceCustomIndex). Bumping just instanceMaterialNeedsUpload
    // avoids a pointless TLAS rebuild on material edits, which the
    // M9 "Save Scene As USD" + AOV inspector edit-material flows
    // will exercise per-frame.
    entry->material = materialHandle;
    _impl->instanceMaterialNeedsUpload = true;
  }
}

void GpuScene::SetInstanceVisibility(InstanceHandle instanceHandle, bool visible) {
  if (auto* entry = _impl->ResolveInstance(instanceHandle))
  {
    if (entry->visible != visible)
    {
      entry->visible = visible;
      // Visibility flipping a slot in/out of the TLAS pack changes
      // which entries are live → side-table must re-upload too so
      // any ID gap matches the new TLAS instance set.
      _impl->tlasNeedsRebuild = true;
      _impl->instanceMaterialNeedsUpload = true;
    }
  }
}

void GpuScene::DestroyInstance(InstanceHandle instanceHandle) {
  Impl::InstanceEntry* entry = _impl->ResolveInstance(instanceHandle);
  if (entry == nullptr)
    return;
  entry->live = false;
  entry->mesh = MeshHandle::Invalid;
  entry->material = MaterialHandle::Invalid;
  entry->worldFromLocal = hlslpp::float4x4{};
  entry->visible = false;
  entry->debugName.clear();
  if (entry->generation == HANDLE_GENERATION_QUARANTINE)
  {
    entry->quarantined = true;
  }
  else
  {
    ++entry->generation;
  }
  _impl->tlasNeedsRebuild = true;
  _impl->instanceMaterialNeedsUpload = true;
}

bool GpuScene::HasInstance(InstanceHandle instanceHandle) const {
  const auto value = static_cast<uint32_t>(instanceHandle);
  if (value == 0)
    return false;
  const uint32_t slot = HandleSlot(value);
  if (slot == 0 || slot >= _impl->instances.size())
    return false;
  const Impl::InstanceEntry& entry = _impl->instances[slot];
  return entry.live && !entry.quarantined && entry.generation == HandleGeneration(value);
}

// ---- Camera & lights -------------------------------------------------------
void GpuScene::SetCamera(const CameraDesc& cameraDesc) {
  _impl->cameraDesc = cameraDesc;
  _impl->hasCamera = true;
}

LightHandle GpuScene::AddLight(const LightDesc& lightDesc) {
  uint32_t slot = 0;
  for (uint32_t candidateSlot = 1; candidateSlot < _impl->lights.size(); ++candidateSlot)
  {
    const Impl::LightEntry& candidate = _impl->lights[candidateSlot];
    if (!candidate.live && !candidate.quarantined)
    {
      slot = candidateSlot;
      break;
    }
  }
  if (slot == 0)
  {
    if (_impl->lights.size() >= (1u << HANDLE_SLOT_BITS))
    {
      // Light handle space exhausted — Invalid is the documented
      // fallback (§18.5 lazy-acquirer contract); a one-shot spdlog
      // warn lands at the next CommitResources via
      // FrameStats::degraded once that path is wired.
      return LightHandle::Invalid;
    }
    _impl->lights.emplace_back();
    slot = static_cast<uint32_t>(_impl->lights.size() - 1);
  }

  Impl::LightEntry& entry = _impl->lights[slot];
  entry.live = true;
  entry.descCopy = lightDesc;
  return static_cast<LightHandle>(HandleEncode(slot, entry.generation));
}

void GpuScene::UpdateLight(LightHandle lightHandle, const LightDesc& lightDesc) {
  if (auto* entry = _impl->ResolveLight(lightHandle))
  {
    entry->descCopy = lightDesc;
  }
}

void GpuScene::RemoveLight(LightHandle lightHandle) {
  Impl::LightEntry* entry = _impl->ResolveLight(lightHandle);
  if (entry == nullptr)
    return;
  entry->live = false;
  entry->descCopy = LightDesc{};
  if (entry->generation == HANDLE_GENERATION_QUARANTINE)
  {
    entry->quarantined = true;
  }
  else
  {
    ++entry->generation;
  }
}

// ---- Frame boundary --------------------------------------------------------
Expected<void> GpuScene::CommitResources(nvrhi::ICommandList* commandList) {
  // Zero the per-frame counters at the start of every commit so the
  // value `LastFrameStats()` reports between this commit and the
  // next is exactly what happened during the current in-progress
  // frame. That honours the §18.5 / FrameStats.h "this frame"
  // contract on `staleHandleDrops` / `pendingUploads` /
  // `pendingBlasBuilds`. Cumulative counters (`meshCount`,
  // `instanceCount`, ...) are recomputed on read from the live
  // tables so they don't need to be reset here.
  _impl->lastFrameStats.staleHandleDrops = 0;
  _impl->lastFrameStats.pendingUploads = 0;
  _impl->lastFrameStats.pendingBlasBuilds = 0;

  if (commandList == nullptr)
  {
    return std::unexpected{
        PYXIS_ERROR(ErrorKind::InvalidArgument, "GpuScene::CommitResources: commandList is null")};
  }
  if (_impl->device == nullptr)
  {
    return std::unexpected{PYXIS_ERROR(ErrorKind::InvalidState,
                                       "GpuScene::CommitResources: scene has no device "
                                       "(constructed in CPU-only test mode)")};
  }

  const Profiler::CpuScope commitScope(*_impl->profiler, "render.commitResources");

  // ---- Upload pending meshes ----------------------------------------
  for (Impl::MeshEntry& entry : _impl->meshes)
  {
    if (!entry.live || !entry.needsGpuUpload)
      continue;

    // Vertex buffer — hlslpp::float3 stride (16 bytes / vertex) on
    // x86_64 SSE. VK_FORMAT_R32G32B32_SFLOAT with that stride is
    // valid ray-tracing geometry input under
    // VK_KHR_ray_tracing_pipeline.
    const std::size_t vertexBytes = entry.positions.size() * sizeof(hlslpp::float3);
    nvrhi::BufferDesc vertexDesc;
    vertexDesc.byteSize = vertexBytes;
    vertexDesc.debugName =
        entry.debugName.empty() ? std::string{"mesh.vertex"} : entry.debugName + ".vertex";
    vertexDesc.isVertexBuffer = true;
    vertexDesc.isAccelStructBuildInput = true;
    vertexDesc.initialState = nvrhi::ResourceStates::CopyDest;
    vertexDesc.keepInitialState = true;
    entry.vertexBuffer = _impl->device->createBuffer(vertexDesc);
    if (!entry.vertexBuffer)
    {
      return std::unexpected{
          PYXIS_ERROR(ErrorKind::OutOfMemoryGpu,
                      "CommitResources: createBuffer(vertex, %zu bytes) failed for '%s'",
                      vertexBytes, entry.debugName.c_str())};
    }

    const std::size_t indexBytes = entry.indices.size() * sizeof(uint32_t);
    nvrhi::BufferDesc indexDesc;
    indexDesc.byteSize = indexBytes;
    indexDesc.debugName =
        entry.debugName.empty() ? std::string{"mesh.index"} : entry.debugName + ".index";
    indexDesc.isIndexBuffer = true;
    indexDesc.isAccelStructBuildInput = true;
    indexDesc.format = nvrhi::Format::R32_UINT;
    indexDesc.initialState = nvrhi::ResourceStates::CopyDest;
    indexDesc.keepInitialState = true;
    entry.indexBuffer = _impl->device->createBuffer(indexDesc);
    if (!entry.indexBuffer)
    {
      entry.vertexBuffer = nullptr;
      return std::unexpected{
          PYXIS_ERROR(ErrorKind::OutOfMemoryGpu,
                      "CommitResources: createBuffer(index, %zu bytes) failed for '%s'",
                      indexBytes, entry.debugName.c_str())};
    }

    commandList->writeBuffer(entry.vertexBuffer.Get(), entry.positions.data(), vertexBytes);
    commandList->writeBuffer(entry.indexBuffer.Get(), entry.indices.data(), indexBytes);
    entry.needsGpuUpload = false;
  }

  // ---- M5: lazy-init missing-texture + bindless sampler -------------
  // The magenta 4×4 fallback lives in slot 0 of the bindless texture
  // table; every material whose resolved path failed to decode
  // points at it via INVALID_BINDLESS_TEXTURE → fallback gating in
  // the closesthit. Created once on the first commit that has a
  // device, reused for the lifetime of the scene.
  if (!_impl->missingTexture)
  {
    nvrhi::TextureDesc missingDesc;
    missingDesc.width = 4;
    missingDesc.height = 4;
    missingDesc.format = nvrhi::Format::RGBA8_UNORM;
    missingDesc.dimension = nvrhi::TextureDimension::Texture2D;
    missingDesc.debugName = "scene.missingTexture";
    missingDesc.initialState = nvrhi::ResourceStates::ShaderResource;
    missingDesc.keepInitialState = true;
    _impl->missingTexture = _impl->device->createTexture(missingDesc);
    if (!_impl->missingTexture)
    {
      return std::unexpected{
          PYXIS_ERROR(ErrorKind::OutOfMemoryGpu,
                      "CommitResources: createTexture(missingTexture 4x4) failed")};
    }
    // Magenta + black checker — visibly broken to debug eyes.
    static constexpr std::uint8_t MAGENTA[4] = {255, 0, 255, 255};
    static constexpr std::uint8_t BLACK[4]   = {0, 0, 0, 255};
    std::uint8_t pixels[4 * 4 * 4];
    for (std::size_t row = 0; row < 4; ++row)
    {
      for (std::size_t col = 0; col < 4; ++col)
      {
        const auto& src = ((col ^ row) & 1u) ? MAGENTA : BLACK;
        std::memcpy(&pixels[(row * 4u + col) * 4u], src, 4);
      }
    }
    commandList->writeTexture(_impl->missingTexture.Get(), 0, 0, pixels,
                              static_cast<std::size_t>(4u * 4u));
  }
  if (!_impl->bindlessSampler)
  {
    nvrhi::SamplerDesc samplerDesc;
    samplerDesc.minFilter = true;
    samplerDesc.magFilter = true;
    samplerDesc.mipFilter = true;
    samplerDesc.addressU = nvrhi::SamplerAddressMode::Wrap;
    samplerDesc.addressV = nvrhi::SamplerAddressMode::Wrap;
    samplerDesc.addressW = nvrhi::SamplerAddressMode::Wrap;
    _impl->bindlessSampler = _impl->device->createSampler(samplerDesc);
  }

  // ---- M5: decode + upload pending textures -------------------------
  // Synchronous stb_image decode on the render thread. The §31
  // async I/O pool wires at M8 when texture-load latency starts to
  // dominate frame time; for the M5 stub textured cube one
  // texture per scene is the realistic upper bound.
  for (Impl::TextureEntry& entry : _impl->textures)
  {
    if (!entry.live || !entry.needsGpuUpload || entry.resolvedPath.empty())
      continue;

    int width = 0;
    int height = 0;
    int channelsInFile = 0;
    // stbi_load returns 4-channel RGBA8 since we request 4. The
    // pointer is malloc-allocated; we copy into entry.pixelData
    // and free immediately.
    stbi_uc* decoded = stbi_load(entry.resolvedPath.c_str(), &width, &height,
                                 &channelsInFile, 4);
    if (decoded == nullptr || width <= 0 || height <= 0)
    {
      Logging::Get().Warn(log::RENDER,
                          std::string{"TextureCache: stbi_load failed for "}
                              + entry.resolvedPath
                              + " — falling back to missing-texture (slot 0).");
      entry.needsGpuUpload = false;
      entry.bindlessSlot = 0;  // points at missingTexture
      if (decoded)
        stbi_image_free(decoded);
      continue;
    }

    const auto pixelByteCount = static_cast<std::size_t>(width) * height * 4;
    entry.pixelData.assign(decoded, decoded + pixelByteCount);
    stbi_image_free(decoded);
    entry.width = static_cast<std::uint32_t>(width);
    entry.height = static_cast<std::uint32_t>(height);
    // BaseColor + Emission go through the sRGB→linear EOTF on
    // sample; everything else is linear data (normal maps, ORM
    // packs, etc.). §13 colorspace rule.
    entry.format = (entry.keyCopy.role == TextureKey::Role::BaseColor
                    || entry.keyCopy.role == TextureKey::Role::Emission)
                       ? nvrhi::Format::SRGBA8_UNORM
                       : nvrhi::Format::RGBA8_UNORM;

    nvrhi::TextureDesc texDesc;
    texDesc.width = entry.width;
    texDesc.height = entry.height;
    texDesc.format = entry.format;
    texDesc.dimension = nvrhi::TextureDimension::Texture2D;
    texDesc.debugName = entry.resolvedPath;
    texDesc.initialState = nvrhi::ResourceStates::ShaderResource;
    texDesc.keepInitialState = true;
    entry.texture = _impl->device->createTexture(texDesc);
    if (!entry.texture)
    {
      return std::unexpected{PYXIS_ERROR(ErrorKind::OutOfMemoryGpu,
                                         "CommitResources: createTexture failed for '%s'",
                                         entry.resolvedPath.c_str())};
    }
    commandList->writeTexture(entry.texture.Get(), 0, 0, entry.pixelData.data(),
                              static_cast<std::size_t>(entry.width) * 4u);
    // Drop the CPU copy now that the GPU upload is queued — staging
    // ring + the §31 deletion guard ensure the bytes survive the
    // submit. Frees ~width*height*4 bytes per texture.
    entry.pixelData.clear();
    entry.pixelData.shrink_to_fit();
    entry.needsGpuUpload = false;
  }

  // ---- M5: pack + upload material GPU buffer ------------------------
  // Re-uploaded whenever any material was added or updated this
  // frame (or when the materials vector grew). Small enough at v1
  // (~80 bytes per material × hundreds-of-thousands materials cap
  // = a few MiB worst case) that we always re-upload the whole
  // table rather than tracking dirty ranges.
  if (_impl->materialsNeedGpuUpload && _impl->materials.size() > 1)
  {
    std::vector<shaderinterop::OpenPBRMaterialGPU> packed;
    packed.resize(_impl->materials.size());
    for (std::uint32_t slot = 0; slot < _impl->materials.size(); ++slot)
    {
      const Impl::MaterialEntry& entry = _impl->materials[slot];
      if (slot == 0 || !entry.live)
      {
        // Sentinel slot 0 + dead materials → magenta-fallback
        // material so the closesthit reads valid bytes regardless.
        packed[slot] = PackMaterialGpu(
            OpenPBRMaterialDesc{},
            static_cast<std::uint32_t>(MaterialFlag::None),
            shaderinterop::INVALID_BINDLESS_TEXTURE,
            shaderinterop::INVALID_BINDLESS_TEXTURE,
            shaderinterop::INVALID_BINDLESS_TEXTURE,
            shaderinterop::INVALID_BINDLESS_TEXTURE,
            shaderinterop::INVALID_BINDLESS_TEXTURE,
            shaderinterop::INVALID_BINDLESS_TEXTURE,
            shaderinterop::INVALID_BINDLESS_TEXTURE,
            shaderinterop::INVALID_BINDLESS_TEXTURE);
        continue;
      }
      // Compute MaterialFlag bits from the desc + the texture
      // handles. The closesthit reads `flags` to short-circuit the
      // bindless lookup so a missing texture renders the scalar
      // fallback rather than the magenta missingTexture.
      std::uint32_t flags = 0;
      if (entry.descCopy.opacity < 1.0f) flags |= MaterialFlag::AlphaTested;
      if (entry.descCopy.coatWeight > 0.0f) flags |= MaterialFlag::CoatEnabled;
      if (entry.descCopy.transmissionWeight > 0.0f)
        flags |= MaterialFlag::TransmissionEnabled;
      if (entry.descCopy.emissionLuminance > 0.0f) flags |= MaterialFlag::Emissive;

      const std::uint32_t baseColorSlot =
          ResolveTextureBindlessSlot(entry.descCopy.baseColorMap, _impl->textures);
      const std::uint32_t normalSlot =
          ResolveTextureBindlessSlot(entry.descCopy.normalMap, _impl->textures);
      const std::uint32_t metallicSlot =
          ResolveTextureBindlessSlot(entry.descCopy.metallicMap, _impl->textures);
      const std::uint32_t roughnessSlot =
          ResolveTextureBindlessSlot(entry.descCopy.roughnessMap, _impl->textures);
      const std::uint32_t emissionSlot =
          ResolveTextureBindlessSlot(entry.descCopy.emissionMap, _impl->textures);
      const std::uint32_t opacitySlot =
          ResolveTextureBindlessSlot(entry.descCopy.opacityMap, _impl->textures);
      const std::uint32_t transmissionSlot =
          ResolveTextureBindlessSlot(entry.descCopy.transmissionMap, _impl->textures);
      const std::uint32_t coatRoughnessSlot =
          ResolveTextureBindlessSlot(entry.descCopy.coatRoughnessMap, _impl->textures);

      if (baseColorSlot != shaderinterop::INVALID_BINDLESS_TEXTURE)
        flags |= MaterialFlag::HasBaseColorMap;
      if (normalSlot != shaderinterop::INVALID_BINDLESS_TEXTURE)
        flags |= MaterialFlag::HasNormalMap;
      if (metallicSlot != shaderinterop::INVALID_BINDLESS_TEXTURE)
        flags |= MaterialFlag::HasMetallicMap;
      if (roughnessSlot != shaderinterop::INVALID_BINDLESS_TEXTURE)
        flags |= MaterialFlag::HasRoughnessMap;
      if (emissionSlot != shaderinterop::INVALID_BINDLESS_TEXTURE)
        flags |= MaterialFlag::HasEmissionMap;
      if (opacitySlot != shaderinterop::INVALID_BINDLESS_TEXTURE)
        flags |= MaterialFlag::HasOpacityMap;
      if (transmissionSlot != shaderinterop::INVALID_BINDLESS_TEXTURE)
        flags |= MaterialFlag::HasTransmissionMap;
      if (coatRoughnessSlot != shaderinterop::INVALID_BINDLESS_TEXTURE)
        flags |= MaterialFlag::HasCoatRoughnessMap;

      packed[slot] = PackMaterialGpu(
          entry.descCopy, flags,
          baseColorSlot, normalSlot, metallicSlot, roughnessSlot,
          emissionSlot, opacitySlot, transmissionSlot, coatRoughnessSlot);
    }

    const std::size_t bufferBytes = packed.size() * sizeof(shaderinterop::OpenPBRMaterialGPU);
    // (Re)create the GPU buffer if it doesn't exist or grew. M5 stub
    // grows monotonically — DestroyMaterial leaves a hole rather
    // than reclaiming the slot index. M8 perf sweep adds compaction.
    if (!_impl->materialGpuBuffer
        || _impl->materialGpuBuffer->getDesc().byteSize < bufferBytes)
    {
      nvrhi::BufferDesc bufDesc;
      bufDesc.byteSize = bufferBytes;
      bufDesc.structStride = sizeof(shaderinterop::OpenPBRMaterialGPU);
      bufDesc.canHaveRawViews = false;
      bufDesc.canHaveTypedViews = false;
      bufDesc.format = nvrhi::Format::UNKNOWN;
      bufDesc.debugName = "scene.materials";
      bufDesc.initialState = nvrhi::ResourceStates::ShaderResource;
      bufDesc.keepInitialState = true;
      _impl->materialGpuBuffer = _impl->device->createBuffer(bufDesc);
      if (!_impl->materialGpuBuffer)
      {
        return std::unexpected{
            PYXIS_ERROR(ErrorKind::OutOfMemoryGpu,
                        "CommitResources: createBuffer(materials, %zu bytes) failed",
                        bufferBytes)};
      }
    }
    commandList->writeBuffer(_impl->materialGpuBuffer.Get(), packed.data(), bufferBytes);
    // Per-entry needsGpuUpload flags are advisory only since we
    // always re-upload the whole table; clearing them keeps the
    // bookkeeping consistent so a future incremental-upload path
    // (M8+) can drop into place without semantic changes.
    for (Impl::MaterialEntry& entry : _impl->materials)
      entry.needsGpuUpload = false;
    _impl->materialsNeedGpuUpload = false;
  }

  // ---- Build pending BLAS -------------------------------------------
  // §16 split rule: PreferFastTrace always, AllowCompaction for ≥
  // 64k tris. AllowUpdate is never set in v1 — animation is post-v1
  // (§42).
  //
  // BLAS memory + scratch + async compaction are RTXMU-managed
  // behind NVRHI's `createAccelStruct` / `buildBottomLevelAccelStruct`
  // (§16, NVRHI_WITH_RTXMU=ON in _cmake/Thirdparty.cmake). What that
  // means for the code below: we set the AllowCompaction flag at
  // build time, RTXMU sees the build complete on the GPU at queue-
  // submit time, then RTXMU enqueues the compaction copy + recycles
  // the original buffer when its post-build-info read retires. Pyxis
  // does not query post-build sizes, does not allocate compacted
  // copies, does not free originals — RTXMU owns the lifecycle.
  for (Impl::MeshEntry& entry : _impl->meshes)
  {
    if (!entry.live || entry.needsGpuUpload || !entry.needsBlasBuild)
      continue;
    if (!entry.vertexBuffer || !entry.indexBuffer)
    {
      return std::unexpected{
          PYXIS_ERROR(ErrorKind::InvalidState,
                      "CommitResources: BLAS build for '%s' missing vertex/index buffers",
                      entry.debugName.c_str())};
    }

    const uint32_t triangleCount = entry.indexCount / 3u;

    nvrhi::rt::GeometryTriangles triangles;
    triangles.setVertexBuffer(entry.vertexBuffer.Get())
        .setVertexFormat(nvrhi::Format::RGB32_FLOAT)
        .setVertexCount(entry.vertexCount)
        .setVertexStride(sizeof(hlslpp::float3))
        .setIndexBuffer(entry.indexBuffer.Get())
        .setIndexFormat(nvrhi::Format::R32_UINT)
        .setIndexCount(entry.indexCount);

    nvrhi::rt::GeometryDesc geometry;
    geometry.setTriangles(triangles).setFlags(nvrhi::rt::GeometryFlags::Opaque);

    auto buildFlags = nvrhi::rt::AccelStructBuildFlags::PreferFastTrace;
    if (triangleCount >= BLAS_COMPACTION_TRIANGLE_THRESHOLD)
    {
      buildFlags = buildFlags | nvrhi::rt::AccelStructBuildFlags::AllowCompaction;
    }

    nvrhi::rt::AccelStructDesc blasDesc;
    blasDesc.isTopLevel = false;
    blasDesc.bottomLevelGeometries.push_back(geometry);
    blasDesc.buildFlags = buildFlags;
    blasDesc.debugName =
        entry.debugName.empty() ? std::string{"mesh.blas"} : entry.debugName + ".blas";
    entry.blas = _impl->device->createAccelStruct(blasDesc);
    if (!entry.blas)
    {
      return std::unexpected{
          PYXIS_ERROR(ErrorKind::AccelStructBuildFailed,
                      "CommitResources: createAccelStruct(BLAS) failed for '%s' (triCount=%u)",
                      entry.debugName.c_str(), triangleCount)};
    }

    commandList->buildBottomLevelAccelStruct(entry.blas.Get(), &geometry, /*numGeometries*/ 1,
                                             buildFlags);
    entry.needsBlasBuild = false;
  }

  // ---- Rebuild TLAS if instances changed ----------------------------
  // Lazy-allocate the TLAS on first need; size it to a fixed
  // M3-friendly capacity (TLAS_MAX_INSTANCES). M6+ grows this with
  // the scene budget.
  if (_impl->tlasNeedsRebuild)
  {
    if (!_impl->tlas)
    {
      nvrhi::rt::AccelStructDesc tlasDesc;
      tlasDesc.isTopLevel = true;
      tlasDesc.topLevelMaxInstances = TLAS_MAX_INSTANCES;
      tlasDesc.buildFlags = nvrhi::rt::AccelStructBuildFlags::PreferFastTrace;
      tlasDesc.debugName = "scene.tlas";
      _impl->tlas = _impl->device->createAccelStruct(tlasDesc);
      if (!_impl->tlas)
      {
        return std::unexpected{
            PYXIS_ERROR(ErrorKind::AccelStructBuildFailed,
                        "CommitResources: createAccelStruct(TLAS, max=%zu) failed",
                        TLAS_MAX_INSTANCES)};
      }
    }

    // Gather one nvrhi::rt::InstanceDesc per live + visible instance
    // whose mesh has a live BLAS. Skipping instances whose BLAS
    // isn't ready yet is the right behaviour during partial
    // mid-frame ingest — they'll join on the next CommitResources
    // tick.
    std::vector<nvrhi::rt::InstanceDesc> instanceDescs;
    instanceDescs.reserve(_impl->instances.size());
    for (uint32_t slot = 1; slot < _impl->instances.size(); ++slot)
    {
      const Impl::InstanceEntry& inst = _impl->instances[slot];
      if (!inst.live || !inst.visible)
        continue;
      const auto meshValue = static_cast<uint32_t>(inst.mesh);
      const uint32_t meshSlot = HandleSlot(meshValue);
      if (meshSlot == 0 || meshSlot >= _impl->meshes.size())
        continue;
      const Impl::MeshEntry& mesh = _impl->meshes[meshSlot];
      if (!mesh.live || !mesh.blas)
        continue;

      nvrhi::rt::InstanceDesc desc;
      // §10 row-major + column-vector float4x4 → NVRHI's 3x4 affine
      // layout. NVRHI's AffineTransform is `float[12]` storing 3
      // rows of 4 columns in row-major order — Pyxis's
      // worldFromLocal rows 0..2 (drop row 3, the [0,0,0,1]
      // homogeneous padding) are byte-equivalent. hlslpp::store
      // writes 16 floats row-major; we keep the first 12.
      float worldRowMajor[16];
      hlslpp::store(worldRowMajor, inst.worldFromLocal);
      std::memcpy(&desc.transform, worldRowMajor, sizeof(nvrhi::rt::AffineTransform));
      desc.instanceMask = 0xFF;
      // Plan §15 — instanceCustomIndex carries the INSTANCE slot
      // (24-bit cap matches §19.7's HANDLE_SLOT_BITS). The
      // closesthit reads `instanceMaterial[InstanceID()]` to
      // resolve the material slot, then `materials[that]` to read
      // the OpenPBR fields. The indirection costs one extra buffer
      // load per closest-hit and frees the custom index for the
      // §41 M6 instanceId AOV + future picking (§19.4).
      desc.instanceID = slot;
      desc.instanceContributionToHitGroupIndex = 0;
      desc.flags = nvrhi::rt::InstanceFlags::None;
      desc.bottomLevelAS = mesh.blas.Get();
      instanceDescs.push_back(desc);
    }

    if (instanceDescs.size() > TLAS_MAX_INSTANCES)
    {
      return std::unexpected{
          PYXIS_ERROR(ErrorKind::TlasInstanceLimitExceeded,
                      "CommitResources: TLAS rebuild needs %zu instances, cap is %zu",
                      instanceDescs.size(), TLAS_MAX_INSTANCES)};
    }

    commandList->buildTopLevelAccelStruct(_impl->tlas.Get(), instanceDescs.data(),
                                          instanceDescs.size(),
                                          nvrhi::rt::AccelStructBuildFlags::PreferFastTrace);
    _impl->tlasNeedsRebuild = false;
  }

  // Plan §15 / M6 P0 — upload the instance→material side-table.
  // Indexed by instance slot (so dead/sparse slots are present but
  // unread; they're never visited because the TLAS only contains
  // live instances). Each entry holds the material slot bound to
  // that instance; slot 0 always maps to material slot 0 (the
  // GpuScene sentinel grey material). Re-uploaded whenever the
  // dedicated dirty flag fires — independent of TLAS rebuild so
  // UpdateInstanceMaterial doesn't pointlessly rebuild the TLAS.
  if (_impl->instanceMaterialNeedsUpload && !_impl->instances.empty())
  {
    const std::size_t instanceTableEntries = _impl->instances.size();
    std::vector<std::uint32_t> instanceMaterialTable(instanceTableEntries, 0u);
    for (std::size_t entrySlot = 1; entrySlot < instanceTableEntries; ++entrySlot)
    {
      const Impl::InstanceEntry& inst = _impl->instances[entrySlot];
      if (!inst.live)
        continue;
      const auto materialValue = static_cast<std::uint32_t>(inst.material);
      instanceMaterialTable[entrySlot] =
          (materialValue == 0) ? 0u : HandleSlot(materialValue);
    }
    const std::size_t instanceTableBytes =
        instanceMaterialTable.size() * sizeof(std::uint32_t);
    if (!_impl->instanceMaterialBuffer
        || _impl->instanceMaterialBuffer->getDesc().byteSize < instanceTableBytes)
    {
      nvrhi::BufferDesc bufDesc;
      bufDesc.byteSize = instanceTableBytes;
      bufDesc.structStride = sizeof(std::uint32_t);
      bufDesc.canHaveRawViews = false;
      bufDesc.canHaveTypedViews = false;
      bufDesc.format = nvrhi::Format::UNKNOWN;
      bufDesc.debugName = "GpuScene.instanceMaterialBuffer";
      bufDesc.initialState = nvrhi::ResourceStates::ShaderResource;
      bufDesc.keepInitialState = true;
      _impl->instanceMaterialBuffer = _impl->device->createBuffer(bufDesc);
      if (!_impl->instanceMaterialBuffer)
      {
        return std::unexpected{
            PYXIS_ERROR(ErrorKind::OutOfMemoryGpu,
                        "CommitResources: createBuffer(instanceMaterialBuffer) failed")};
      }
    }
    commandList->writeBuffer(_impl->instanceMaterialBuffer.Get(),
                             instanceMaterialTable.data(), instanceTableBytes);
    _impl->instanceMaterialNeedsUpload = false;
  }

  return {};
}

nvrhi::rt::IAccelStruct* GpuScene::GetTlas() const noexcept {
  return _impl->tlas.Get();
}

const CameraDesc& GpuScene::GetCamera() const noexcept {
  return _impl->cameraDesc;
}

bool GpuScene::HasCamera() const noexcept {
  return _impl->hasCamera;
}

nvrhi::IBuffer* GpuScene::GetMaterialBuffer() const noexcept {
  return _impl->materialGpuBuffer.Get();
}

nvrhi::IBuffer* GpuScene::GetInstanceMaterialBuffer() const noexcept {
  return _impl->instanceMaterialBuffer.Get();
}

// ---- Introspection ---------------------------------------------------------
FrameStats GpuScene::LastFrameStats() const {
  FrameStats stats = _impl->lastFrameStats;
  // Recount live meshes / instances / lights / BLAS on read so
  // stats reflect the current table state even before
  // CommitResources lands.
  uint64_t liveMeshCount = 0;
  uint64_t liveBlasCount = 0;
  for (const Impl::MeshEntry& entry : _impl->meshes)
  {
    if (!entry.live)
      continue;
    ++liveMeshCount;
    if (entry.blas)
      ++liveBlasCount;
  }
  uint64_t liveInstanceCount = 0;
  for (const Impl::InstanceEntry& entry : _impl->instances)
  {
    if (entry.live)
      ++liveInstanceCount;
  }
  uint64_t liveLightCount = 0;
  for (const Impl::LightEntry& entry : _impl->lights)
  {
    if (entry.live)
      ++liveLightCount;
  }
  stats.meshCount = liveMeshCount;
  stats.blasCount = liveBlasCount;
  stats.instanceCount = liveInstanceCount;
  stats.lightCount = liveLightCount;
  return stats;
}

}  // namespace pyxis
