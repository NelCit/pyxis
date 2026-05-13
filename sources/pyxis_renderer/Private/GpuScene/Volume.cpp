// Pyxis renderer — GpuScene volume-verb bodies + GPU upload.
//
// V2.A.5. Per-verb split following the established pattern (Light /
// Material / Texture). Public verbs in GpuScene.cpp forward one
// line into here. The actual NVRHI 3D-texture upload runs in
// CommitResources via UploadPendingVolumes — symmetric with the
// per-texture upload path.
//
// The bound texture is alive on the GPU after CommitResources but
// is NOT yet visible to any shader (no bindless slot wired in v2,
// per the user's "load full, bind but don't sample" directive).
// The volume-integrator follow-up extends the bindless layout +
// the closesthit + ShaderInterop.slang to actually sample.

#include "GpuScene/Internal.h"

namespace pyxis {

using namespace gpuscene_detail;

namespace {

// NVRHI 3D-texture cap on every desktop vendor. Source volumes
// past this need to be tiled or downsampled by the loader before
// reaching AddVolume — the loader is the right place to enforce
// production budgets (§17), not the renderer's GPU upload path.
constexpr uint32_t MAX_VOLUME_AXIS = 2048u;

}  // namespace

VolumeHandle GpuScene::Impl::AddVolume(const VolumeDesc& volumeDesc)
{
  // Input validation — Invalid on bad inputs + a one-shot warn so
  // the operator sees why the volume didn't materialise. Mirrors
  // the AddLight / AcquireMaterial fail-soft pattern.
  if (volumeDesc.dimensions[0] == 0
      || volumeDesc.dimensions[1] == 0
      || volumeDesc.dimensions[2] == 0)
  {
    Logging::Get().Warn(log::RENDER,
        "GpuScene::AddVolume: zero dimension; rejecting.");
    return VolumeHandle::Invalid;
  }
  if (volumeDesc.dimensions[0] > MAX_VOLUME_AXIS
      || volumeDesc.dimensions[1] > MAX_VOLUME_AXIS
      || volumeDesc.dimensions[2] > MAX_VOLUME_AXIS)
  {
    Logging::Get().Warn(log::RENDER,
        "GpuScene::AddVolume: dimension exceeds 2048-cap; rejecting "
        + std::string{volumeDesc.debugName});
    return VolumeHandle::Invalid;
  }
  const std::size_t expectedVoxels =
      static_cast<std::size_t>(volumeDesc.dimensions[0])
      * volumeDesc.dimensions[1]
      * volumeDesc.dimensions[2];
  if (volumeDesc.voxels.size() != expectedVoxels)
  {
    Logging::Get().Warn(log::RENDER,
        "GpuScene::AddVolume: voxel buffer size mismatch (expected "
        + std::to_string(expectedVoxels) + ", got "
        + std::to_string(volumeDesc.voxels.size()) + "); rejecting.");
    return VolumeHandle::Invalid;
  }

  // O(1) free-list pop; RemoveVolume pushes back symmetrically.
  // Slot 0 is the Invalid sentinel — first real allocation lands
  // at slot 1.
  uint32_t slot = 0;
  if (!freeVolumeSlots.empty())
  {
    slot = freeVolumeSlots.back();
    freeVolumeSlots.pop_back();
  }
  else
  {
    if (volumes.empty())
      volumes.emplace_back();  // sentinel slot 0
    if (volumes.size() >= (1u << HANDLE_SLOT_BITS))
    {
      Logging::Get().Warn(log::RENDER,
          "GpuScene::AddVolume: handle space exhausted.");
      return VolumeHandle::Invalid;
    }
    volumes.emplace_back();
    slot = static_cast<uint32_t>(volumes.size() - 1);
  }

  VolumeEntry& entry = volumes[slot];
  entry.live = true;
  entry.dimensions = volumeDesc.dimensions;
  entry.bboxMin = volumeDesc.bboxMin;
  entry.bboxMax = volumeDesc.bboxMax;
  entry.indexToWorld = volumeDesc.indexToWorld;
  entry.debugName.assign(volumeDesc.debugName);
  entry.voxelData.assign(volumeDesc.voxels.begin(), volumeDesc.voxels.end());
  entry.needsGpuUpload = true;
  entry.bytesOnGpu = static_cast<std::uint64_t>(expectedVoxels) * sizeof(float);
  volumesNeedGpuUpload = true;
  return static_cast<VolumeHandle>(HandleEncode(slot, entry.generation));
}

void GpuScene::Impl::RemoveVolume(VolumeHandle volumeHandle)
{
  VolumeEntry* entry = ResolveVolume(volumeHandle);
  if (entry == nullptr)
    return;
  entry->live = false;
  entry->texture = nullptr;
  entry->voxelData.clear();
  entry->voxelData.shrink_to_fit();
  if (entry->generation == HANDLE_GENERATION_QUARANTINE)
  {
    entry->quarantined = true;
  }
  else
  {
    ++entry->generation;
    const auto slot = static_cast<std::uint32_t>(entry - volumes.data());
    freeVolumeSlots.push_back(slot);
  }
}

bool GpuScene::Impl::HasVolume(VolumeHandle volumeHandle) const
{
  return LookupVolume(volumeHandle) != nullptr;
}

Expected<void> GpuScene::Impl::UploadPendingVolumes(nvrhi::ICommandList* commandList)
{
  if (!volumesNeedGpuUpload)
    return {};

  for (VolumeEntry& entry : volumes)
  {
    if (!entry.live || !entry.needsGpuUpload)
      continue;

    nvrhi::TextureDesc texDesc;
    texDesc.width = entry.dimensions[0];
    texDesc.height = entry.dimensions[1];
    texDesc.depth = entry.dimensions[2];
    texDesc.format = nvrhi::Format::R32_FLOAT;
    texDesc.dimension = nvrhi::TextureDimension::Texture3D;
    texDesc.debugName = entry.debugName;
    texDesc.initialState = nvrhi::ResourceStates::ShaderResource;
    texDesc.keepInitialState = true;
    entry.texture = device->createTexture(texDesc);
    if (!entry.texture)
    {
      return std::unexpected{PYXIS_ERROR(ErrorKind::OutOfMemoryGpu,
                                         "CommitResources: createTexture failed for volume '%s'",
                                         entry.debugName.c_str())};
    }
    const std::size_t rowPitch = static_cast<std::size_t>(entry.dimensions[0]) * sizeof(float);
    const std::size_t depthPitch = rowPitch * entry.dimensions[1];
    commandList->writeTexture(entry.texture.Get(), /*arraySlice*/ 0, /*mipLevel*/ 0,
                              entry.voxelData.data(), rowPitch, depthPitch);
    // Drop the CPU-side buffer once the GPU copy is queued — it
    // would otherwise pin O(dim³) floats per volume in scene
    // memory for the lifetime of the entry.
    entry.voxelData.clear();
    entry.voxelData.shrink_to_fit();
    entry.needsGpuUpload = false;
  }
  volumesNeedGpuUpload = false;
  return {};
}

}  // namespace pyxis
