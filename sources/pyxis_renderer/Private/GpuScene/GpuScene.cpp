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

#include "GpuScene/Internal.h"

#include "Materials/MaterialFlag.h"

#include <stb_image.h>
#include <tinyexr.h>
#include <cstdlib>

#include <cstring>
#include <utility>

namespace pyxis {

using namespace gpuscene_detail;  // bring the helpers into scope so the bodies below stay terse


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

  // ---- §15 content-dedup -------------------------------------------------
  // Hash the geometry (positions + indices + optional attrs) and
  // check the dedup map. On a hit, return the existing handle so two
  // CreateMesh calls with byte-identical content share one MeshHandle
  // → one BLAS. Default scene's three spheres exercise this — same
  // 80-tri icosphere data inlined three times collapses to one slot.
  const std::uint64_t descHash = HashMeshDesc(meshDesc);
  if (auto found = _impl->meshDescHashToHandle.find(descHash);
      found != _impl->meshDescHashToHandle.end())
  {
    // Validate the cached handle still resolves to a live entry — a
    // hash collision (FNV1a-64 birthday risk is negligible at v1
    // mesh counts but not zero) or a stale entry from a Destroy that
    // somehow missed the map cleanup falls through to a fresh
    // allocation.
    const auto cachedValue = static_cast<std::uint32_t>(found->second);
    const std::uint32_t cachedSlot = HandleSlot(cachedValue);
    if (cachedSlot > 0 && cachedSlot < _impl->meshes.size())
    {
      const Impl::MeshEntry& cached = _impl->meshes[cachedSlot];
      if (cached.live && !cached.quarantined
          && cached.generation == HandleGeneration(cachedValue))
      {
        return found->second;
      }
    }
    // Stale map entry — drop it and fall through to allocate fresh.
    _impl->meshDescHashToHandle.erase(found);
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

  // M7 NdotL: pre-compute object-space face normals (one per
  // triangle). Closesthit reads `gMeshFaceNormals[offset +
  // PrimitiveIndex()].xyz`, transforms to world via
  // `ObjectToWorld3x4()`, and Lambert-shades against distant
  // lights. Computed here (CreateMesh time) so we don't burn cycles
  // re-deriving them at every CommitResources.
  const std::size_t triangleCount = entry.indices.size() / 3;
  entry.faceNormals.clear();
  entry.faceNormals.reserve(triangleCount);
  for (std::size_t tri = 0; tri < triangleCount; ++tri)
  {
    const std::uint32_t idx0 = entry.indices[tri * 3 + 0];
    const std::uint32_t idx1 = entry.indices[tri * 3 + 1];
    const std::uint32_t idx2 = entry.indices[tri * 3 + 2];
    if (idx0 >= entry.positions.size() || idx1 >= entry.positions.size()
        || idx2 >= entry.positions.size())
    {
      // Malformed index — emit zero normal so the closesthit's
      // Lambert reads NdotL=0 and the triangle stays unlit.
      entry.faceNormals.emplace_back(0.0f, 0.0f, 0.0f, 0.0f);
      continue;
    }
    const hlslpp::float3 pos0 = entry.positions[idx0];
    const hlslpp::float3 pos1 = entry.positions[idx1];
    const hlslpp::float3 pos2 = entry.positions[idx2];
    const hlslpp::float3 normal = hlslpp::normalize(hlslpp::cross(pos1 - pos0, pos2 - pos0));
    entry.faceNormals.emplace_back(static_cast<float>(normal.x),
                                   static_cast<float>(normal.y),
                                   static_cast<float>(normal.z), 0.0f);
  }
  _impl->meshFaceNormalsNeedUpload = true;

  // Record the descHash on the entry + register the handle in the
  // dedup map. DestroyMesh erases the map entry symmetrically.
  entry.descHash = descHash;
  const auto handle = static_cast<MeshHandle>(HandleEncode(slot, entry.generation));
  _impl->meshDescHashToHandle.emplace(descHash, handle);
  return handle;
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
  // §15 content-dedup map cleanup. Erase by stored hash so a future
  // CreateMesh of the same content allocates fresh instead of
  // looking up a dead slot.
  _impl->meshDescHashToHandle.erase(entry.descHash);
  entry.descHash = 0;
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
  _impl->lightsNeedGpuUpload = true;
  return static_cast<LightHandle>(HandleEncode(slot, entry.generation));
}

void GpuScene::UpdateLight(LightHandle lightHandle, const LightDesc& lightDesc) {
  if (auto* entry = _impl->ResolveLight(lightHandle))
  {
    entry->descCopy = lightDesc;
    _impl->lightsNeedGpuUpload = true;
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
  _impl->lightsNeedGpuUpload = true;
}

// ---- Scene-wide reset ------------------------------------------------------
void GpuScene::Clear() noexcept {
  // Reset to the post-construction shape. Every NVRHI handle owned by
  // the impl is reference-counted (nvrhi::RefCountPtr); dropping the
  // handles releases the ref and NVRHI's deferred-destruction queue
  // reclaims the underlying Vulkan objects on the next garbage
  // collection tick. Caller must have waited the device idle so no
  // in-flight command buffer still references these.

  // Mesh / instance / light / material / texture tables back to "slot
  // 0 sentinel only", matching ctor.
  _impl->meshes.clear();
  _impl->meshes.emplace_back();
  _impl->meshes[0].quarantined = true;

  _impl->instances.clear();
  _impl->instances.emplace_back();
  _impl->instances[0].quarantined = true;

  _impl->lights.clear();
  _impl->lights.emplace_back();
  _impl->lights[0].quarantined = true;

  _impl->materials.clear();
  _impl->textures.clear();

  _impl->materialDescHashToHandle.clear();
  _impl->textureKeyHashToHandle.clear();
  _impl->meshDescHashToHandle.clear();

  // GPU buffers: drop refs. CommitResources will lazily re-allocate
  // on the first AcquireMaterial / AppendInstance / AddLight after
  // Clear (the same lazy path used at scene-construction time).
  _impl->materialGpuBuffer = nullptr;
  _impl->materialsNeedGpuUpload = false;
  _impl->instanceMaterialBuffer = nullptr;
  _impl->lightsGpuBuffer = nullptr;
  _impl->lightsNeedGpuUpload = false;
  _impl->meshFaceNormalsBuffer = nullptr;
  _impl->meshFaceOffsetsBuffer = nullptr;
  _impl->meshFaceNormalsNeedUpload = false;
  _impl->instanceMeshBuffer = nullptr;

  // Sampler + missingTexture are scene-lifetime singletons that the
  // first CommitResources after Clear will re-create on demand. Drop
  // the refs so memory isn't held longer than needed across a reload.
  _impl->bindlessSampler = nullptr;
  _impl->missingTexture = nullptr;

  // TLAS + camera + dirty flags.
  _impl->tlas = nullptr;
  _impl->tlasNeedsRebuild = false;
  _impl->instanceMaterialNeedsUpload = false;
  _impl->hasCamera = false;
  _impl->cameraDesc = CameraDesc{};

  // Per-frame stat counters back to zero. Cumulative counters
  // (meshCount / instanceCount / etc.) recompute on read from the
  // live tables so they don't need explicit reset here, but the
  // FrameStats POD itself is replaced wholesale to clear bytes /
  // pendingUploads / degraded / staleHandleDrops in one shot.
  _impl->lastFrameStats = FrameStats{};
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

  // ---- M5/M7: decode + upload pending textures ----------------------
  // Synchronous decode on the render thread (§31 async I/O pool
  // wires at M8). LDR (.png/.jpg) goes through stb_image as RGBA8;
  // HDR (.exr — added at M7 for the dome environment map) goes
  // through tinyexr as RGBA32F. Format selection happens per entry
  // based on extension.
  for (Impl::TextureEntry& entry : _impl->textures)
  {
    if (!entry.live || !entry.needsGpuUpload || entry.resolvedPath.empty())
      continue;

    // Sniff extension — case-insensitive ".exr" suffix routes to
    // tinyexr; everything else goes through stb_image.
    const std::string& path = entry.resolvedPath;
    const bool isExr = path.size() >= 4
        && (path.compare(path.size() - 4, 4, ".exr") == 0
            || path.compare(path.size() - 4, 4, ".EXR") == 0);

    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> decodedPixels;
    nvrhi::Format pixelFormat = nvrhi::Format::UNKNOWN;
    std::size_t rowPitchBytes = 0;

    if (isExr)
    {
      // tinyexr LoadEXR: malloc's float[w*h*4] in RGBA order. We
      // upload directly as RGBA32_FLOAT — 16 B/pixel, so a 1024×512
      // dome env-map costs ~8 MB GPU (fine for v1; M9 polish drops
      // to RGBA16_FLOAT once the half-float pack lands).
      float* exrPixels = nullptr;
      const char* exrErr = nullptr;
      const int loadResult =
          LoadEXR(&exrPixels, &width, &height, path.c_str(), &exrErr);
      if (loadResult != TINYEXR_SUCCESS || exrPixels == nullptr || width <= 0
          || height <= 0)
      {
        Logging::Get().Warn(log::RENDER,
                            std::string{"TextureCache: LoadEXR failed for "} + path
                                + (exrErr ? std::string{" — "} + exrErr : std::string{})
                                + " — falling back to missing-texture (slot 0).");
        if (exrErr)
          FreeEXRErrorMessage(exrErr);
        if (exrPixels)
          std::free(exrPixels);
        entry.needsGpuUpload = false;
        entry.bindlessSlot = 0;
        continue;
      }
      const std::size_t pixelByteCount = static_cast<std::size_t>(width) * height * 4u
                                       * sizeof(float);
      decodedPixels.assign(reinterpret_cast<std::uint8_t*>(exrPixels),
                           reinterpret_cast<std::uint8_t*>(exrPixels) + pixelByteCount);
      std::free(exrPixels);
      pixelFormat = nvrhi::Format::RGBA32_FLOAT;
      rowPitchBytes = static_cast<std::size_t>(width) * 4u * sizeof(float);
    }
    else
    {
      int channelsInFile = 0;
      stbi_uc* decoded = stbi_load(path.c_str(), &width, &height, &channelsInFile, 4);
      if (decoded == nullptr || width <= 0 || height <= 0)
      {
        Logging::Get().Warn(log::RENDER,
                            std::string{"TextureCache: stbi_load failed for "} + path
                                + " — falling back to missing-texture (slot 0).");
        entry.needsGpuUpload = false;
        entry.bindlessSlot = 0;
        if (decoded)
          stbi_image_free(decoded);
        continue;
      }
      const auto pixelByteCount = static_cast<std::size_t>(width) * height * 4u;
      decodedPixels.assign(decoded, decoded + pixelByteCount);
      stbi_image_free(decoded);
      // BaseColor + Emission go through the sRGB→linear EOTF on
      // sample; everything else is linear data (normal maps, ORM
      // packs, env-maps via the EXR path above, etc.). §13.
      pixelFormat = (entry.keyCopy.role == TextureKey::Role::BaseColor
                     || entry.keyCopy.role == TextureKey::Role::Emission)
                        ? nvrhi::Format::SRGBA8_UNORM
                        : nvrhi::Format::RGBA8_UNORM;
      rowPitchBytes = static_cast<std::size_t>(width) * 4u;
    }

    entry.pixelData = std::move(decodedPixels);
    entry.width = static_cast<std::uint32_t>(width);
    entry.height = static_cast<std::uint32_t>(height);
    entry.format = pixelFormat;

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
                              rowPitchBytes);
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
          _impl->ResolveTextureBindlessSlot(entry.descCopy.baseColorMap);
      const std::uint32_t normalSlot =
          _impl->ResolveTextureBindlessSlot(entry.descCopy.normalMap);
      const std::uint32_t metallicSlot =
          _impl->ResolveTextureBindlessSlot(entry.descCopy.metallicMap);
      const std::uint32_t roughnessSlot =
          _impl->ResolveTextureBindlessSlot(entry.descCopy.roughnessMap);
      const std::uint32_t emissionSlot =
          _impl->ResolveTextureBindlessSlot(entry.descCopy.emissionMap);
      const std::uint32_t opacitySlot =
          _impl->ResolveTextureBindlessSlot(entry.descCopy.opacityMap);
      const std::uint32_t transmissionSlot =
          _impl->ResolveTextureBindlessSlot(entry.descCopy.transmissionMap);
      const std::uint32_t coatRoughnessSlot =
          _impl->ResolveTextureBindlessSlot(entry.descCopy.coatRoughnessMap);

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

  // ---- Pack + upload light table (M7) -------------------------------
  // Packs every LIVE LightEntry into a tightly-packed LightGpu buffer
  // bound at PathTracePass binding 5. Sparse / dead slots are
  // omitted — the closesthit iterates the buffer's full length, so
  // emitting only live lights keeps the per-hit loop tight. The
  // simple shading model in closesthit.slang ignores `intensity ==
  // 0` so a fallback 1-element zero buffer (used by PathTracePass
  // when the scene has no lights) contributes nothing.
  if (_impl->lightsNeedGpuUpload)
  {
    std::vector<shaderinterop::LightGpu> packedLights;
    packedLights.reserve(_impl->lights.size());
    for (const Impl::LightEntry& entry : _impl->lights)
    {
      if (!entry.live)
        continue;
      const std::uint32_t envMapSlot =
          _impl->ResolveTextureBindlessSlot(entry.descCopy.envMap);
      packedLights.push_back(PackLightGpu(entry.descCopy, envMapSlot));
    }
    if (!packedLights.empty())
    {
      const std::size_t lightBytes =
          packedLights.size() * sizeof(shaderinterop::LightGpu);
      if (!_impl->lightsGpuBuffer
          || _impl->lightsGpuBuffer->getDesc().byteSize < lightBytes)
      {
        nvrhi::BufferDesc bufDesc;
        bufDesc.byteSize = lightBytes;
        bufDesc.structStride = sizeof(shaderinterop::LightGpu);
        bufDesc.canHaveRawViews = false;
        bufDesc.canHaveTypedViews = false;
        bufDesc.format = nvrhi::Format::UNKNOWN;
        bufDesc.debugName = "scene.lights";
        bufDesc.initialState = nvrhi::ResourceStates::ShaderResource;
        bufDesc.keepInitialState = true;
        _impl->lightsGpuBuffer = _impl->device->createBuffer(bufDesc);
        if (!_impl->lightsGpuBuffer)
        {
          return std::unexpected{
              PYXIS_ERROR(ErrorKind::OutOfMemoryGpu,
                          "CommitResources: createBuffer(lights, %zu bytes) failed",
                          lightBytes)};
        }
      }
      commandList->writeBuffer(_impl->lightsGpuBuffer.Get(), packedLights.data(),
                               lightBytes);
    }
    _impl->lightsNeedGpuUpload = false;
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

    // M7 NdotL — instance→mesh side-table. Same shape + lifecycle
    // as instanceMaterialBuffer; piggy-backs on the same dirty flag
    // because instance ↔ mesh changes only happen when the TLAS
    // shape changes (AppendInstance / DestroyInstance / visibility
    // flip — UpdateInstanceMaterial doesn't touch instance.mesh).
    std::vector<std::uint32_t> instanceMeshTable(instanceTableEntries, 0u);
    for (std::size_t entrySlot = 1; entrySlot < instanceTableEntries; ++entrySlot)
    {
      const Impl::InstanceEntry& inst = _impl->instances[entrySlot];
      if (!inst.live)
        continue;
      const auto meshValue = static_cast<std::uint32_t>(inst.mesh);
      instanceMeshTable[entrySlot] =
          (meshValue == 0) ? 0u : HandleSlot(meshValue);
    }
    if (!_impl->instanceMeshBuffer
        || _impl->instanceMeshBuffer->getDesc().byteSize < instanceTableBytes)
    {
      nvrhi::BufferDesc bufDesc;
      bufDesc.byteSize = instanceTableBytes;
      bufDesc.structStride = sizeof(std::uint32_t);
      bufDesc.canHaveRawViews = false;
      bufDesc.canHaveTypedViews = false;
      bufDesc.format = nvrhi::Format::UNKNOWN;
      bufDesc.debugName = "GpuScene.instanceMeshBuffer";
      bufDesc.initialState = nvrhi::ResourceStates::ShaderResource;
      bufDesc.keepInitialState = true;
      _impl->instanceMeshBuffer = _impl->device->createBuffer(bufDesc);
      if (!_impl->instanceMeshBuffer)
      {
        return std::unexpected{
            PYXIS_ERROR(ErrorKind::OutOfMemoryGpu,
                        "CommitResources: createBuffer(instanceMeshBuffer) failed")};
      }
    }
    commandList->writeBuffer(_impl->instanceMeshBuffer.Get(),
                             instanceMeshTable.data(), instanceTableBytes);

    _impl->instanceMaterialNeedsUpload = false;
  }

  // ---- Pack + upload mesh face normals (M7 NdotL) -------------------
  // Concatenates every live mesh's per-triangle face normals into one
  // flat float4 buffer + a per-mesh-slot start-offset table. The
  // closesthit's NdotL Lambert pass reads:
  //   offset = gMeshFaceOffsets[meshSlot]
  //   nLocal = gMeshFaceNormals[offset + PrimitiveIndex()].xyz
  // The +1 in the offsets sizing reserves slot 0 (the §19.7
  // sentinel mesh handle) so the closesthit's "no mesh assigned"
  // path resolves to offset 0 with a black/zero normal entry.
  if (_impl->meshFaceNormalsNeedUpload && !_impl->meshes.empty())
  {
    std::vector<hlslpp::float4> packedNormals;
    std::vector<std::uint32_t>  perMeshOffsets(_impl->meshes.size(), 0u);
    for (std::size_t meshSlot = 0; meshSlot < _impl->meshes.size(); ++meshSlot)
    {
      perMeshOffsets[meshSlot] = static_cast<std::uint32_t>(packedNormals.size());
      const Impl::MeshEntry& mesh = _impl->meshes[meshSlot];
      if (!mesh.live)
        continue;
      packedNormals.insert(packedNormals.end(), mesh.faceNormals.begin(),
                           mesh.faceNormals.end());
    }
    if (packedNormals.empty())
    {
      // Empty scene with no meshes registered yet — nothing to
      // upload. The PathTracePass fallback handles this.
      packedNormals.emplace_back(0.0f, 0.0f, 0.0f, 0.0f);
    }
    const std::size_t normalsBytes = packedNormals.size() * sizeof(hlslpp::float4);
    const std::size_t offsetsBytes = perMeshOffsets.size() * sizeof(std::uint32_t);

    if (!_impl->meshFaceNormalsBuffer
        || _impl->meshFaceNormalsBuffer->getDesc().byteSize < normalsBytes)
    {
      nvrhi::BufferDesc bufDesc;
      bufDesc.byteSize = normalsBytes;
      bufDesc.structStride = sizeof(hlslpp::float4);
      bufDesc.canHaveRawViews = false;
      bufDesc.canHaveTypedViews = false;
      bufDesc.format = nvrhi::Format::UNKNOWN;
      bufDesc.debugName = "GpuScene.meshFaceNormalsBuffer";
      bufDesc.initialState = nvrhi::ResourceStates::ShaderResource;
      bufDesc.keepInitialState = true;
      _impl->meshFaceNormalsBuffer = _impl->device->createBuffer(bufDesc);
      if (!_impl->meshFaceNormalsBuffer)
      {
        return std::unexpected{
            PYXIS_ERROR(ErrorKind::OutOfMemoryGpu,
                        "CommitResources: createBuffer(meshFaceNormalsBuffer) failed")};
      }
    }
    if (!_impl->meshFaceOffsetsBuffer
        || _impl->meshFaceOffsetsBuffer->getDesc().byteSize < offsetsBytes)
    {
      nvrhi::BufferDesc bufDesc;
      bufDesc.byteSize = offsetsBytes;
      bufDesc.structStride = sizeof(std::uint32_t);
      bufDesc.canHaveRawViews = false;
      bufDesc.canHaveTypedViews = false;
      bufDesc.format = nvrhi::Format::UNKNOWN;
      bufDesc.debugName = "GpuScene.meshFaceOffsetsBuffer";
      bufDesc.initialState = nvrhi::ResourceStates::ShaderResource;
      bufDesc.keepInitialState = true;
      _impl->meshFaceOffsetsBuffer = _impl->device->createBuffer(bufDesc);
      if (!_impl->meshFaceOffsetsBuffer)
      {
        return std::unexpected{
            PYXIS_ERROR(ErrorKind::OutOfMemoryGpu,
                        "CommitResources: createBuffer(meshFaceOffsetsBuffer) failed")};
      }
    }
    commandList->writeBuffer(_impl->meshFaceNormalsBuffer.Get(), packedNormals.data(),
                             normalsBytes);
    commandList->writeBuffer(_impl->meshFaceOffsetsBuffer.Get(), perMeshOffsets.data(),
                             offsetsBytes);
    _impl->meshFaceNormalsNeedUpload = false;
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

nvrhi::IBuffer* GpuScene::GetLightBuffer() const noexcept {
  return _impl->lightsGpuBuffer.Get();
}

nvrhi::ITexture* GpuScene::GetDomeEnvMapTexture() const noexcept {
  // Walk live LightEntries in slot order; return the first Dome with
  // a valid + non-quarantined envMap-resolved texture. The miss
  // shader's lat-long sample uses just one dome (the convention every
  // production renderer follows — multiple domes is post-v1, §43).
  // Returns nullptr when no dome with an env-map exists; PathTracePass
  // binds a 1×1 black fallback in that case.
  for (const Impl::LightEntry& entry : _impl->lights)
  {
    if (!entry.live || entry.descCopy.kind != LightDesc::Kind::Dome)
      continue;
    const auto envMapValue = static_cast<std::uint32_t>(entry.descCopy.envMap);
    if (envMapValue == 0)
      continue;
    const std::uint32_t texSlot = HandleSlot(envMapValue);
    if (texSlot == 0 || texSlot >= _impl->textures.size())
      continue;
    const Impl::TextureEntry& tex = _impl->textures[texSlot];
    if (!tex.live || tex.quarantined
        || tex.generation != HandleGeneration(envMapValue))
      continue;
    if (!tex.texture)
      continue;
    return tex.texture.Get();
  }
  return nullptr;
}

nvrhi::ISampler* GpuScene::GetBindlessSampler() const noexcept {
  return _impl->bindlessSampler.Get();
}

nvrhi::IBuffer* GpuScene::GetInstanceMeshBuffer() const noexcept {
  return _impl->instanceMeshBuffer.Get();
}

nvrhi::IBuffer* GpuScene::GetMeshFaceNormalsBuffer() const noexcept {
  return _impl->meshFaceNormalsBuffer.Get();
}

nvrhi::IBuffer* GpuScene::GetMeshFaceOffsetsBuffer() const noexcept {
  return _impl->meshFaceOffsetsBuffer.Get();
}

// ---- Editor introspection (M7 follow-up) -----------------------------------
// All four At() helpers walk the table in slot order, count live
// non-quarantined entries, and return the `liveIndex`-th one's
// handle / desc copy. Out-of-range or all-dead returns Invalid /
// default-init. v1 uses these only from the viewer's editor panel
// (call frequency = once per frame) so the linear walk is fine.
uint32_t GpuScene::GetLiveLightCount() const noexcept {
  uint32_t count = 0;
  for (const Impl::LightEntry& entry : _impl->lights)
  {
    if (entry.live)
      ++count;
  }
  return count;
}

LightHandle GpuScene::GetLightHandleAt(uint32_t liveIndex) const noexcept {
  uint32_t walked = 0;
  for (uint32_t slot = 0; slot < _impl->lights.size(); ++slot)
  {
    const Impl::LightEntry& entry = _impl->lights[slot];
    if (!entry.live)
      continue;
    if (walked == liveIndex)
      return static_cast<LightHandle>(HandleEncode(slot, entry.generation));
    ++walked;
  }
  return LightHandle::Invalid;
}

LightDesc GpuScene::GetLightDescAt(uint32_t liveIndex) const noexcept {
  uint32_t walked = 0;
  for (const Impl::LightEntry& entry : _impl->lights)
  {
    if (!entry.live)
      continue;
    if (walked == liveIndex)
      return entry.descCopy;
    ++walked;
  }
  return LightDesc{};
}

uint32_t GpuScene::GetLiveMaterialCount() const noexcept {
  uint32_t count = 0;
  for (const Impl::MaterialEntry& entry : _impl->materials)
  {
    if (entry.live)
      ++count;
  }
  return count;
}

MaterialHandle GpuScene::GetMaterialHandleAt(uint32_t liveIndex) const noexcept {
  uint32_t walked = 0;
  for (uint32_t slot = 0; slot < _impl->materials.size(); ++slot)
  {
    const Impl::MaterialEntry& entry = _impl->materials[slot];
    if (!entry.live)
      continue;
    if (walked == liveIndex)
      return static_cast<MaterialHandle>(HandleEncode(slot, entry.generation));
    ++walked;
  }
  return MaterialHandle::Invalid;
}

OpenPBRMaterialDesc GpuScene::GetMaterialDescAt(uint32_t liveIndex) const noexcept {
  uint32_t walked = 0;
  for (const Impl::MaterialEntry& entry : _impl->materials)
  {
    if (!entry.live)
      continue;
    if (walked == liveIndex)
      return entry.descCopy;
    ++walked;
  }
  return OpenPBRMaterialDesc{};
}

MaterialHandle GpuScene::LookupInstanceMaterialBySlot(uint32_t instanceSlot) const noexcept {
  // Slot 0 is the §15 sentinel; the picker writes 0 when no instance
  // was hit OR for a degenerate primitive that maps back to the
  // permanent quarantine entry. Either way → no selection.
  if (instanceSlot == 0 || instanceSlot >= _impl->instances.size())
    return MaterialHandle::Invalid;
  const Impl::InstanceEntry& entry = _impl->instances[instanceSlot];
  if (!entry.live || entry.quarantined)
    return MaterialHandle::Invalid;
  return entry.material;
}

// ---- Introspection ---------------------------------------------------------
FrameStats GpuScene::LastFrameStats() const {
  FrameStats stats = _impl->lastFrameStats;
  // Recount live meshes / instances / lights / BLAS / materials /
  // textures + their associated GPU memory on read so stats reflect
  // the current table state even before CommitResources lands.
  // Cumulative counters are derived here rather than maintained as
  // delta increments because the cost (one walk over each table) is
  // negligible at v1 scales (10⁴ entries max) and the derived form
  // can never diverge from reality.
  uint64_t liveMeshCount = 0;
  uint64_t liveBlasCount = 0;
  uint64_t vertexBytes = 0;
  uint64_t indexBytes = 0;
  uint64_t blasBytesEstimate = 0;
  // Heuristic: ≈70 B per triangle for the BLAS storage. Real number
  // varies by driver + whether RTXMU compacted the build (compaction
  // typically saves 30–50%). NVRHI's IAccelStruct interface only
  // exposes `getDesc()` + `isCompacted()` — no byte size — so we
  // approximate from triangle count for the v1 panel. M8 perf-sweep
  // wires the real number via NVRHI's RTXMU pool-stats hook.
  constexpr uint64_t BLAS_BYTES_PER_TRIANGLE_ESTIMATE = 70;
  for (const Impl::MeshEntry& entry : _impl->meshes)
  {
    if (!entry.live)
      continue;
    ++liveMeshCount;
    if (entry.blas)
    {
      ++liveBlasCount;
      const uint64_t triangleCount = entry.indexCount / 3u;
      blasBytesEstimate += triangleCount * BLAS_BYTES_PER_TRIANGLE_ESTIMATE;
    }
    if (entry.vertexBuffer)
      vertexBytes += entry.vertexBuffer->getDesc().byteSize;
    if (entry.indexBuffer)
      indexBytes += entry.indexBuffer->getDesc().byteSize;
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
  uint64_t liveMaterialCount = 0;
  for (const Impl::MaterialEntry& entry : _impl->materials)
  {
    // Material slot 0 is the §19.7 sentinel — exclude it from the
    // user-visible count.
    if (entry.live)
      ++liveMaterialCount;
  }
  uint64_t liveTextureCount = 0;
  uint64_t textureBytes = 0;
  for (const Impl::TextureEntry& entry : _impl->textures)
  {
    if (!entry.live)
      continue;
    ++liveTextureCount;
    // Bytes per pixel via NVRHI's format introspection — covers every
    // uncompressed format the texture pool can hold. Pre-fix the
    // switch only knew about RGBA8 / SRGBA8 / RGBA32F and silently
    // counted everything else (RGBA16F, R32F, R32_UINT, ...) as 0,
    // making the Stats panel's "Total" row visibly inconsistent
    // with the per-row sum the user could compute by hand. The
    // bytesPerBlock convention is "bytes per pixel" for uncompressed
    // formats; compressed formats (BC1..BC7) report bytes per 4×4
    // block but Pyxis doesn't authorize compressed textures yet
    // (M8 follow-up) so this path is exact today.
    const uint64_t bytesPerPixel = nvrhi::getFormatInfo(entry.format).bytesPerBlock;
    textureBytes += static_cast<uint64_t>(entry.width) * entry.height * bytesPerPixel;
  }
  stats.meshCount = liveMeshCount;
  stats.blasCount = liveBlasCount;
  stats.instanceCount = liveInstanceCount;
  stats.lightCount = liveLightCount;
  stats.materialCount = liveMaterialCount;
  stats.textureCount = liveTextureCount;
  stats.vertexBytes = vertexBytes;
  stats.indexBytes = indexBytes;
  stats.textureBytes = textureBytes;
  stats.blasBytes = blasBytesEstimate;
  // TLAS byte estimate: VkAccelerationStructureInstanceKHR is 64 B
  // per instance + ~64 B internal book-keeping per entry. Same
  // heuristic-pending-real-RTXMU-query caveat as blasBytes.
  constexpr uint64_t TLAS_BYTES_PER_INSTANCE_ESTIMATE = 128;
  stats.tlasBytes = liveInstanceCount * TLAS_BYTES_PER_INSTANCE_ESTIMATE;
  return stats;
}

}  // namespace pyxis
