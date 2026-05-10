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
  // ~150 materials / ~200 textures); larger scenes (Bistro ~50K) pay
  // a few extra reallocations past these caps but never the cliff
  // from 0 → 1024. Memory cost is bounded — each entry vector is at
  // most a few hundred bytes per slot, so 4096 reservations cost
  // ~MB-class up front.
  constexpr std::size_t INITIAL_MESH_RESERVE     = 4096;
  constexpr std::size_t INITIAL_INSTANCE_RESERVE = 4096;
  constexpr std::size_t INITIAL_MATERIAL_RESERVE = 512;
  constexpr std::size_t INITIAL_TEXTURE_RESERVE  = 1024;
  constexpr std::size_t INITIAL_LIGHT_RESERVE    = 256;
  _impl->meshes.reserve(INITIAL_MESH_RESERVE);
  _impl->instances.reserve(INITIAL_INSTANCE_RESERVE);
  _impl->materials.reserve(INITIAL_MATERIAL_RESERVE);
  _impl->textures.reserve(INITIAL_TEXTURE_RESERVE);
  _impl->lights.reserve(INITIAL_LIGHT_RESERVE);
  _impl->meshDescHashToHandle.reserve(INITIAL_MESH_RESERVE);
  _impl->materialDescHashToHandle.reserve(INITIAL_MATERIAL_RESERVE);
  _impl->textureKeyHashToHandle.reserve(INITIAL_TEXTURE_RESERVE);
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
    const Impl::TextureEntry* tex = _impl->LookupTexture(entry.descCopy.envMap);
    if (tex != nullptr && tex->texture)
      return tex->texture.Get();
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
  return static_cast<uint32_t>(_impl->textures.size());
}

nvrhi::ITexture* GpuScene::GetBindlessTextureAt(uint32_t bindlessSlot) const noexcept {
  if (bindlessSlot >= _impl->textures.size())
    return nullptr;
  const Impl::TextureEntry& entry = _impl->textures[bindlessSlot];
  if (!entry.live || !entry.texture)
    return nullptr;
  return entry.texture.Get();
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
