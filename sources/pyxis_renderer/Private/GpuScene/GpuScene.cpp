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

#if defined(PYXIS_DEBUG_TOOLS) && defined(FLECS_REST)
#include <flecs/addons/rest.h>  // Flecs Explorer (Debug-tools only); ticked by progress().
#endif

namespace pyxis {

using namespace gpuscene_detail;  // bring the helpers into scope so the bodies below stay terse


GpuScene::GpuScene(nvrhi::IDevice* device, Profiler& profiler, const GpuSceneCreateDesc& desc)
    : _impl(std::make_unique<Impl>()) {
  _impl->device = device;
  _impl->profiler = &profiler;
  _impl->desc = desc;
  // M8a perf: pre-reserve entry vectors + dedup maps to skip the
  // first dozen geometric reallocations during scene load. Sized for
  // a "modest production scene" (lobby = ~1K meshes / 1K instances /
  // ~150 materials / ~200 textures); larger scenes (World Lobby ~50K) pay
  // a few extra reallocations past these caps but never the cliff
  // from 0 → 1024. Memory cost is bounded — each entry vector is at
  // most a few hundred bytes per slot, so 4096 reservations cost
  // ~MB-class up front.
  constexpr std::size_t INITIAL_MESH_RESERVE     = 4096;
  constexpr std::size_t INITIAL_MATERIAL_RESERVE = 512;
  constexpr std::size_t INITIAL_TEXTURE_RESERVE  = 1024;
  _impl->meshDescHashToHandle.reserve(INITIAL_MESH_RESERVE);
  _impl->materialDescHashToHandle.reserve(INITIAL_MATERIAL_RESERVE);
  _impl->textureKeyHashToHandle.reserve(INITIAL_TEXTURE_RESERVE);
  // RFC 0009 — meshes/materials/textures/instances all use GpuSlotMap, whose ctor
  // reserves the slot-0 Invalid sentinel; no vector-backed sentinel left to seed.
  // RFC 0009 P1 — lights live in `sceneWorld`; no sentinel slot needed (the
  // HandleBimap encodes slot+1 so handle 0 stays Invalid). Build the cached
  // light query ONCE here (§30.11: never inside a per-frame body).
  _impl->lightQuery = _impl->sceneWorld.query<GpuLightComponent>();
  // RFC 0009 follow-up — cache the Dirty<T> clear/gate queries once (§30.11).
  _impl->dirtyTransformQuery =
      _impl->sceneWorld.query_builder().with<scene::DirtyTransform>().build();
  _impl->dirtyVisibilityQuery =
      _impl->sceneWorld.query_builder().with<scene::DirtyVisibility>().build();
  _impl->dirtyMaterialQuery =
      _impl->sceneWorld.query_builder().with<scene::DirtyMaterial>().build();
  _impl->dirtyLightQuery =
      _impl->sceneWorld.query_builder().with<scene::DirtyLight>().build();
  _impl->removalSentinel = _impl->sceneWorld.entity();  // carries removal dirty signals.

#if defined(PYXIS_DEBUG_TOOLS) && defined(FLECS_REST)
  // RFC 0009 follow-up — re-enable the Flecs Explorer on the GpuScene-owned world (the
  // retired SceneWorldFacade used to host it). The REST server is ticked by
  // sceneWorld.progress() inside CommitResources. Debug-tools builds only (§30.11 / §4).
  _impl->sceneWorld.set<flecs::Rest>({});
  Logging::Get().Info(log::RENDER,
                      "Flecs Explorer up at http://localhost:27750 (GpuScene sceneWorld)");
#endif
}

// Out-of-line dtor so unique_ptr<Impl>'s deleter sees the complete
// Impl type — Impl is forward-declared in the public header.
GpuScene::~GpuScene() = default;

// ---- Mesh ------------------------------------------------------------------
// Bodies live in Mesh.cpp (`GpuScene::Impl::CreateMesh`, etc.); the
// public methods below forward.
Expected<MeshHandle> GpuScene::CreateMesh(const MeshDesc& meshDesc)
{
  return _impl->CreateMesh(meshDesc);
}

Expected<void> GpuScene::UpdateMesh(MeshHandle meshHandle, const MeshDesc& meshDesc)
{
  return _impl->UpdateMesh(meshHandle, meshDesc);
}

void GpuScene::DestroyMesh(MeshHandle meshHandle)
{
  _impl->DestroyMesh(meshHandle);
}

bool GpuScene::HasMesh(MeshHandle meshHandle) const
{
  return _impl->HasMesh(meshHandle);
}

// ---- Material --------------------------------------------------------------
// Bodies in Material.cpp.
MaterialHandle GpuScene::AcquireMaterial(const OpenPBRMaterialDesc& materialDesc)
{
  return _impl->AcquireMaterial(materialDesc);
}

void GpuScene::UpdateMaterial(MaterialHandle materialHandle,
                              const OpenPBRMaterialDesc& materialDesc)
{
  _impl->UpdateMaterial(materialHandle, materialDesc);
}

void GpuScene::DestroyMaterial(MaterialHandle materialHandle)
{
  _impl->DestroyMaterial(materialHandle);
}

bool GpuScene::HasMaterial(MaterialHandle materialHandle) const
{
  return _impl->HasMaterial(materialHandle);
}

// ---- Texture ---------------------------------------------------------------
// Bodies in Texture.cpp.
TextureHandle GpuScene::AcquireTexture(const TextureKey& textureKey)
{
  return _impl->AcquireTexture(textureKey);
}

void GpuScene::DestroyTexture(TextureHandle textureHandle)
{
  _impl->DestroyTexture(textureHandle);
}

bool GpuScene::HasTexture(TextureHandle textureHandle) const
{
  return _impl->HasTexture(textureHandle);
}

std::uint32_t GpuScene::EvictColdTextures(uint64_t targetBytes)
{
  return _impl->EvictColdTextures(targetBytes);
}

// ---- Instance --------------------------------------------------------------
// Bodies in Instance.cpp.
Expected<InstanceHandle> GpuScene::AppendInstance(const InstanceDesc& instanceDesc)
{
  return _impl->AppendInstance(instanceDesc);
}

void GpuScene::UpdateInstanceTransform(InstanceHandle instanceHandle,
                                       const hlslpp::float4x4& worldFromLocal)
{
  _impl->UpdateInstanceTransform(instanceHandle, worldFromLocal);
}

void GpuScene::UpdateInstanceMaterial(InstanceHandle instanceHandle,
                                      MaterialHandle materialHandle)
{
  _impl->UpdateInstanceMaterial(instanceHandle, materialHandle);
}

void GpuScene::SetInstanceVisibility(InstanceHandle instanceHandle, bool visible)
{
  _impl->SetInstanceVisibility(instanceHandle, visible);
}

void GpuScene::DestroyInstance(InstanceHandle instanceHandle)
{
  _impl->DestroyInstance(instanceHandle);
}

bool GpuScene::HasInstance(InstanceHandle instanceHandle) const
{
  return _impl->HasInstance(instanceHandle);
}

// ---- Camera & lights -------------------------------------------------------
// Bodies in Light.cpp.
void GpuScene::SetCamera(const CameraDesc& cameraDesc)
{
  _impl->SetCamera(cameraDesc);
}

LightHandle GpuScene::AddLight(const LightDesc& lightDesc)
{
  return _impl->AddLight(lightDesc);
}

void GpuScene::UpdateLight(LightHandle lightHandle, const LightDesc& lightDesc)
{
  _impl->UpdateLight(lightHandle, lightDesc);
}

void GpuScene::RemoveLight(LightHandle lightHandle)
{
  _impl->RemoveLight(lightHandle);
}

// ---- Volumes (V2.A.5) ------------------------------------------------------
// Bodies in Volume.cpp.
VolumeHandle GpuScene::AddVolume(const VolumeDesc& volumeDesc)
{
  return _impl->AddVolume(volumeDesc);
}

void GpuScene::RemoveVolume(VolumeHandle volumeHandle)
{
  _impl->RemoveVolume(volumeHandle);
}

bool GpuScene::HasVolume(VolumeHandle volumeHandle) const
{
  return _impl->HasVolume(volumeHandle);
}

uint32_t GpuScene::GetVolumeCount() const noexcept
{
  // RFC 0009 P6 — slot count (== old volumes.size()); slot == volume index.
  return _impl->volumeSlots.SlotCount();
}

nvrhi::ITexture* GpuScene::GetVolumeTextureAt(uint32_t volumeSlot) const noexcept
{
  if (!_impl->volumeSlots.IsLive(volumeSlot) || volumeSlot >= _impl->volumeResources.size())
    return nullptr;
  return _impl->volumeResources[volumeSlot].texture.Get();
}

// ---- Scene-wide reset + frame boundary -------------------------------------
// Bodies in Commit.cpp.
void GpuScene::Clear() noexcept
{
  _impl->Clear();
}

Expected<void> GpuScene::CommitResources(nvrhi::ICommandList* commandList)
{
  return _impl->CommitResources(commandList);
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
  // The single-dome env-map texture (the first live Dome with a resolved env-map, slot
  // order — the convention every production renderer follows; multiple domes is post-v1,
  // §43). Review fix #2 — the selection is computed once per commit in
  // RefreshDomeEnvMapCache and cached here; this accessor is on the per-frame
  // PathTracePass binding path, so it must not allocate/sort. nullptr when no dome with
  // an env-map exists → PathTracePass binds a 1×1 black fallback.
  return _impl->domeEnvMapTexture;
}

nvrhi::ISampler* GpuScene::GetBindlessSampler() const noexcept {
  return _impl->bindlessSampler.Get();
}

nvrhi::ISampler* GpuScene::GetDomeSampler() const noexcept {
  return _impl->domeSampler.Get();
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

nvrhi::IBuffer* GpuScene::GetMeshUvsBuffer() const noexcept {
  return _impl->meshUvsBuffer.Get();
}

nvrhi::IBuffer* GpuScene::GetMeshUvOffsetsBuffer() const noexcept {
  return _impl->meshUvOffsetsBuffer.Get();
}

nvrhi::IBuffer* GpuScene::GetMeshIndicesBuffer() const noexcept {
  return _impl->meshIndicesBuffer.Get();
}

nvrhi::IBuffer* GpuScene::GetMeshIndexOffsetsBuffer() const noexcept {
  return _impl->meshIndexOffsetsBuffer.Get();
}

nvrhi::IBuffer* GpuScene::GetMeshVertexNormalsBuffer() const noexcept {
  return _impl->meshVertexNormalsBuffer.Get();
}

nvrhi::IBuffer* GpuScene::GetMeshVertexNormalOffsetsBuffer() const noexcept {
  return _impl->meshVertexNormalOffsetsBuffer.Get();
}

nvrhi::IBuffer* GpuScene::GetMeshTangentsBuffer() const noexcept {
  return _impl->meshTangentsBuffer.Get();
}

nvrhi::IBuffer* GpuScene::GetMeshTangentOffsetsBuffer() const noexcept {
  return _impl->meshTangentOffsetsBuffer.Get();
}

nvrhi::ITexture* GpuScene::GetMissingTexture() const noexcept {
  return _impl->missingTexture.Get();
}

uint32_t GpuScene::GetBindlessTextureCount() const noexcept {
  // RFC 0009 P3 — slot count (== old textures.size()); the bindless table is sized to
  // this so slot == bindless index stays stable.
  return _impl->textureSlots.SlotCount();
}

nvrhi::ITexture* GpuScene::GetBindlessTextureAt(uint32_t bindlessSlot) const noexcept {
  // bindlessSlot == texture slot for M5. The GPU resource lives in the side table.
  if (!_impl->textureSlots.IsLive(bindlessSlot) || bindlessSlot >= _impl->textureResources.size())
    return nullptr;
  return _impl->textureResources[bindlessSlot].Get();
}

// ---- Editor introspection (M7 follow-up) -----------------------------------
// All four At() helpers walk the table in slot order, count live
// non-quarantined entries, and return the `liveIndex`-th one's
// handle / desc copy. Out-of-range or all-dead returns Invalid /
// default-init. v1 uses these only from the viewer's editor panel
// (call frequency = once per frame) so the linear walk is fine.
uint32_t GpuScene::GetLiveLightCount() const noexcept {
  return _impl->lightHandles.LiveCount();
}

LightHandle GpuScene::GetLightHandleAt(uint32_t liveIndex) const noexcept {
  std::vector<std::pair<uint32_t, LightDesc>> liveLights;
  _impl->CollectLiveLightsSorted(liveLights);
  if (liveIndex >= liveLights.size())
    return LightHandle::Invalid;
  return static_cast<LightHandle>(liveLights[liveIndex].first);
}

LightDesc GpuScene::GetLightDescAt(uint32_t liveIndex) const noexcept {
  std::vector<std::pair<uint32_t, LightDesc>> liveLights;
  _impl->CollectLiveLightsSorted(liveLights);
  if (liveIndex >= liveLights.size())
    return LightDesc{};
  return liveLights[liveIndex].second;
}

uint32_t GpuScene::GetLiveMaterialCount() const noexcept {
  return _impl->materialSlots.LiveCount();
}

MaterialHandle GpuScene::GetMaterialHandleAt(uint32_t liveIndex) const noexcept {
  // RFC 0009 P2 — walk live material slots in slot order (matches the old vector walk).
  uint32_t walked = 0;
  MaterialHandle result = MaterialHandle::Invalid;
  _impl->materialSlots.ForEachLiveSlot([&](uint32_t slot, flecs::entity) {
    if (result == MaterialHandle::Invalid && walked++ == liveIndex)
      result = static_cast<MaterialHandle>(_impl->materialSlots.HandleForSlot(slot));
  });
  return result;
}

OpenPBRMaterialDesc GpuScene::GetMaterialDescAt(uint32_t liveIndex) const noexcept {
  uint32_t walked = 0;
  OpenPBRMaterialDesc result{};
  bool found = false;
  _impl->materialSlots.ForEachLiveSlot([&](uint32_t slot, flecs::entity entity) {
    if (!found && walked++ == liveIndex)
    {
      result = entity.get<GpuMaterialComponent>().desc;
      // Review fix #5 — the stored desc.sourcePrim views materialSourcePrims[slot],
      // which can move when that vector reallocates (dangling for SSO-short paths).
      // Re-point at the CURRENT owned string so the returned view is always valid.
      if (slot < _impl->materialSourcePrims.size())
        result.sourcePrim = _impl->materialSourcePrims[slot];
      found = true;
    }
  });
  return result;
}

std::string_view GpuScene::GetTexturePath(TextureHandle textureHandle) const noexcept {
  // Editor-side path lookup. RFC 0009 P3 — returns the owned resolvedPath string
  // from the slot-indexed side table. Empty when the handle is Invalid / dead.
  const flecs::entity entity = _impl->textureSlots.Resolve(static_cast<uint32_t>(textureHandle));
  if (entity.id() == 0)
    return {};
  const uint32_t slot = GpuSlotMap::SlotOf(static_cast<uint32_t>(textureHandle));
  if (slot >= _impl->textureResolvedPaths.size())
    return {};
  return _impl->textureResolvedPaths[slot];
}

MaterialHandle GpuScene::LookupInstanceMaterialBySlot(uint32_t instanceSlot) const noexcept {
  // Slot 0 is the §15 sentinel; the picker writes 0 when no instance
  // was hit OR for a degenerate primitive that maps back to the
  // permanent quarantine entry. Either way → no selection.
  // RFC 0009 P5 — slot-indexed instance resource via instanceSlots liveness.
  if (instanceSlot == 0 || !_impl->instanceSlots.IsLive(instanceSlot)
      || instanceSlot >= _impl->instanceResources.size())
    return MaterialHandle::Invalid;
  return _impl->instanceResources[instanceSlot].material;
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
  // RFC 0009 P4 — meshes are Flecs entities; walk live slots + their resource record.
  _impl->meshSlots.ForEachLiveSlot([&](uint32_t slot, flecs::entity) {
    const Impl::MeshResource& entry = _impl->meshResources[slot];
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
  });
  // RFC 0009 P5 — instances are Flecs entities; the slot map tracks live count O(1).
  const uint64_t liveInstanceCount = _impl->instanceSlots.LiveCount();
  // RFC 0009 P1 — lights are Flecs entities; the bimap tracks the live count O(1).
  const uint64_t liveLightCount = _impl->lightHandles.LiveCount();
  // RFC 0009 P2 — materials are Flecs entities; the slot map tracks live count O(1)
  // (already excludes the slot-0 sentinel).
  const uint64_t liveMaterialCount = _impl->materialSlots.LiveCount();
  // RFC 0009 P3 — textures are Flecs entities; walk live slots, read the component
  // metadata for the byte tally. bytesPerBlock is "bytes per pixel" for the
  // uncompressed formats the pool holds today (compressed BCn under-counts but isn't
  // authorized yet — M8 follow-up).
  uint64_t liveTextureCount = 0;
  uint64_t textureBytes = 0;
  _impl->textureSlots.ForEachLiveSlot([&](uint32_t, flecs::entity entity) {
    const GpuTextureComponent& component = entity.get<GpuTextureComponent>();
    ++liveTextureCount;
    const uint64_t bytesPerPixel = nvrhi::getFormatInfo(component.format).bytesPerBlock;
    textureBytes += static_cast<uint64_t>(component.width) * component.height * bytesPerPixel;
  });
  // V2.A.5 — live volumes + their VRAM footprint. `bytesOnGpu` is
  // computed at AddVolume time as `dim_x * dim_y * dim_z * sizeof(float)`
  // since every volume binds a R32_FLOAT 3D texture. Slot 0 sentinel
  // is `live=false` so the sum naturally excludes it.
  // RFC 0009 P6 — volumes are Flecs entities; walk live slots for the byte tally.
  uint64_t liveVolumeCount = 0;
  uint64_t volumeBytes = 0;
  _impl->volumeSlots.ForEachLiveSlot([&](uint32_t slot, flecs::entity) {
    ++liveVolumeCount;
    volumeBytes += _impl->volumeResources[slot].bytesOnGpu;
  });
  stats.meshCount = liveMeshCount;
  stats.blasCount = liveBlasCount;
  stats.instanceCount = liveInstanceCount;
  stats.lightCount = liveLightCount;
  stats.materialCount = liveMaterialCount;
  stats.textureCount = liveTextureCount;
  stats.volumeCount = liveVolumeCount;
  stats.vertexBytes = vertexBytes;
  stats.indexBytes = indexBytes;
  stats.textureBytes = textureBytes;
  stats.volumeBytes = volumeBytes;
  stats.blasBytes = blasBytesEstimate;
  // TLAS byte estimate: VkAccelerationStructureInstanceKHR is 64 B
  // per instance + ~64 B internal book-keeping per entry. Same
  // heuristic-pending-real-RTXMU-query caveat as blasBytes.
  constexpr uint64_t TLAS_BYTES_PER_INSTANCE_ESTIMATE = 128;
  stats.tlasBytes = liveInstanceCount * TLAS_BYTES_PER_INSTANCE_ESTIMATE;
  return stats;
}

}  // namespace pyxis
