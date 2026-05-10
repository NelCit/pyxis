// Pyxis renderer — GpuScene mesh-verb bodies.
//
// Per-verb split off GpuScene.cpp; declarations live on
// `GpuScene::Impl` in Internal.h, public `GpuScene::Verb()`
// methods in GpuScene.cpp forward one line into here.

#include "GpuScene/Internal.h"

namespace pyxis {

using namespace gpuscene_detail;

Expected<MeshHandle> GpuScene::Impl::CreateMesh(const MeshDesc& meshDesc)
{
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
  if (auto found = meshDescHashToHandle.find(descHash);
      found != meshDescHashToHandle.end())
  {
    // Validate the cached handle still resolves to a live entry — a
    // hash collision (FNV1a-64 birthday risk is negligible at v1
    // mesh counts but not zero) or a stale entry from a Destroy that
    // somehow missed the map cleanup falls through to a fresh
    // allocation.
    if (LookupMesh(found->second) != nullptr)
      return found->second;
    // Stale map entry — drop it and fall through to allocate fresh.
    meshDescHashToHandle.erase(found);
  }

  // ---- Allocate slot -----------------------------------------------------
  // O(1) free-list pop. DestroyMesh pushes the slot back here
  // symmetrically; an append-only load never enters the pop branch.
  uint32_t slot = 0;
  if (!freeMeshSlots.empty())
  {
    slot = freeMeshSlots.back();
    freeMeshSlots.pop_back();
  }
  else
  {
    if (meshes.size() >= (1u << HANDLE_SLOT_BITS))
    {
      return std::unexpected{PYXIS_ERROR(
          ErrorKind::InvalidState, "CreateMesh: mesh-handle slot space exhausted (limit = %u)",
          (1u << HANDLE_SLOT_BITS))};
    }
    meshes.emplace_back();
    slot = static_cast<uint32_t>(meshes.size() - 1);
  }

  // ---- Populate entry ----------------------------------------------------
  MeshEntry& entry = meshes[slot];
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
  meshFaceNormalsNeedUpload = true;

  // M8a: any mesh registration also dirties the per-mesh UV +
  // index flat buffers (re-uploaded next CommitResources). UV array
  // can be empty when the source authored no `primvars:st`; the
  // closesthit's HasBaseColorMap flag falls through to the scalar
  // baseColor in that case.
  meshUvsNeedUpload     = true;
  meshIndicesNeedUpload = true;

  // Record the descHash on the entry + register the handle in the
  // dedup map. DestroyMesh erases the map entry symmetrically.
  entry.descHash = descHash;
  const auto handle = static_cast<MeshHandle>(HandleEncode(slot, entry.generation));
  meshDescHashToHandle.emplace(descHash, handle);
  return handle;
}

Expected<void> GpuScene::Impl::UpdateMesh(MeshHandle /*meshHandle*/, const MeshDesc& /*meshDesc*/)
{
  return std::unexpected{PYXIS_ERROR(ErrorKind::NotImplemented, "GpuScene::UpdateMesh — M3 stub")};
}

void GpuScene::Impl::DestroyMesh(MeshHandle meshHandle)
{
  MeshEntry* entry = ResolveMesh(meshHandle);
  if (entry == nullptr)
    return;
  // §15 content-dedup map cleanup. Erase by stored hash so a future
  // CreateMesh of the same content allocates fresh instead of
  // looking up a dead slot.
  meshDescHashToHandle.erase(entry->descHash);
  entry->descHash = 0;
  entry->live = false;
  entry->needsGpuUpload = false;
  entry->needsBlasBuild = false;
  entry->positions.clear();
  entry->indices.clear();
  entry->normals.clear();
  entry->tangents.clear();
  entry->uv0.clear();
  entry->debugName.clear();
  // Drop the GPU resources. NVRHI's deferred-destruction queue
  // keeps them alive until any in-flight command list that
  // references them retires.
  entry->vertexBuffer = nullptr;
  entry->indexBuffer = nullptr;
  entry->blas = nullptr;
  entry->vertexCount = 0;
  entry->indexCount = 0;
  if (entry->generation == HANDLE_GENERATION_QUARANTINE)
  {
    entry->quarantined = true;
    // Quarantined slots are never reused — don't push to free list.
  }
  else
  {
    ++entry->generation;
    // Recycle the slot for the next CreateMesh. Generation bump
    // protects stale handles from accidentally resolving here.
    const auto slot = static_cast<std::uint32_t>(entry - meshes.data());
    freeMeshSlots.push_back(slot);
  }
}

bool GpuScene::Impl::HasMesh(MeshHandle meshHandle) const
{
  return LookupMesh(meshHandle) != nullptr;
}

}  // namespace pyxis
