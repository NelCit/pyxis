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

  // RFC 0009 P6 — allocate a Flecs entity (slot == volume index; slot 0 = sentinel).
  flecs::entity entity;
  const uint32_t handleRaw = volumeSlots.Allocate(entity);
  if (handleRaw == 0)
  {
    Logging::Get().Warn(log::RENDER, "GpuScene::AddVolume: handle space exhausted.");
    return VolumeHandle::Invalid;
  }
  const uint32_t slot = GpuSlotMap::SlotOf(handleRaw);
  if (volumeResources.size() <= slot)
    volumeResources.resize(slot + 1u);

  VolumeResource& entry = volumeResources[slot];
  entry = VolumeResource{};  // reset a recycled slot.
  entry.dimensions = volumeDesc.dimensions;
  entry.bboxMin = volumeDesc.bboxMin;
  entry.bboxMax = volumeDesc.bboxMax;
  entry.indexToWorld = volumeDesc.indexToWorld;
  entry.debugName.assign(volumeDesc.debugName);
  entry.voxelData.assign(volumeDesc.voxels.begin(), volumeDesc.voxels.end());
  entry.needsGpuUpload = true;
  entry.bytesOnGpu = static_cast<std::uint64_t>(expectedVoxels) * sizeof(float);
  entity.set<GpuVolumeComponent>({});
  volumesNeedGpuUpload = true;
  return static_cast<VolumeHandle>(handleRaw);
}

void GpuScene::Impl::RemoveVolume(VolumeHandle volumeHandle)
{
  VolumeResource* entry = ResolveVolumeResource(volumeHandle);
  if (entry == nullptr)
  {
    if (static_cast<uint32_t>(volumeHandle) != 0)
      ++lastFrameStats.staleHandleDrops;
    return;
  }
  // Reset the record (drops the GPU texture into NVRHI's deferred-destruction queue
  // + frees the CPU voxel buffer); Free destructs the entity + bumps the generation.
  *entry = VolumeResource{};
  volumeSlots.Free(static_cast<uint32_t>(volumeHandle));
}

bool GpuScene::Impl::HasVolume(VolumeHandle volumeHandle) const
{
  return volumeSlots.Resolve(static_cast<uint32_t>(volumeHandle)).id() != 0;
}

Expected<void> GpuScene::Impl::UploadPendingVolumes(nvrhi::ICommandList* commandList)
{
  if (!volumesNeedGpuUpload)
    return {};

  // RFC 0009 P6 — iterate live volume slots; upload those flagged needsGpuUpload.
  for (uint32_t slot = 1; slot < volumeSlots.SlotCount(); ++slot)
  {
    if (!volumeSlots.IsLive(slot))
      continue;
    VolumeResource& entry = volumeResources[slot];
    if (!entry.needsGpuUpload)
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
