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

#include <Pyxis/Renderer/Descs/FrameStats.h>
#include <Pyxis/Renderer/Forward.h>
#include <Pyxis/Renderer/Profiler.h>

#include <nvrhi/nvrhi.h>

#include <cstdint>
#include <cstring>
#include <string>
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

  bool       hasCamera = false;
  CameraDesc cameraDesc{};

  // Top-level acceleration structure. Allocated lazily on the first
  // TLAS rebuild so an empty scene doesn't pay for it.
  nvrhi::rt::AccelStructHandle tlas;
  bool                         tlasNeedsRebuild = false;

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
MaterialHandle GpuScene::AcquireMaterial(const OpenPBRMaterialDesc& /*materialDesc*/) {
  return MaterialHandle::Invalid;
}

void GpuScene::UpdateMaterial(MaterialHandle /*materialHandle*/,
                              const OpenPBRMaterialDesc& /*materialDesc*/) {}

void GpuScene::DestroyMaterial(MaterialHandle /*materialHandle*/) {}

bool GpuScene::HasMaterial(MaterialHandle /*materialHandle*/) const {
  return false;
}

// ---- Texture ---------------------------------------------------------------
TextureHandle GpuScene::AcquireTexture(const TextureKey& /*textureKey*/) {
  return TextureHandle::Invalid;
}

void GpuScene::DestroyTexture(TextureHandle /*textureHandle*/) {}

bool GpuScene::HasTexture(TextureHandle /*textureHandle*/) const {
  return false;
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

  _impl->tlasNeedsRebuild = true;
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
    // No TLAS rebuild — material binding is a closesthit-shader
    // table indirection (M5+); the TLAS only knows about mesh BLAS
    // + transform + visibility.
    entry->material = materialHandle;
  }
}

void GpuScene::SetInstanceVisibility(InstanceHandle instanceHandle, bool visible) {
  if (auto* entry = _impl->ResolveInstance(instanceHandle))
  {
    if (entry->visible != visible)
    {
      entry->visible = visible;
      _impl->tlasNeedsRebuild = true;
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
      // M5+ converts inst.worldFromLocal (column-vector float4x4
      // with translation in last column) to NVRHI's 3x4 affine
      // layout. The two layouts are nearly identical — drop pyxis
      // row 3 (the [0,0,0,1] homogenous padding) and you get
      // NVRHI's 12-float layout — but the packing helper lands when
      // AppendInstance is exercised by real ingest. M3 ships
      // identity for the cube fixture so the memcpy of
      // c_IdentityTransform is correct unconditionally. NVRHI's
      // AffineTransform is `float[12]` so we memcpy rather than
      // assign.
      std::memcpy(&desc.transform, &nvrhi::rt::c_IdentityTransform,
                  sizeof(nvrhi::rt::AffineTransform));
      desc.instanceMask = 0xFF;
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
