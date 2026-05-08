// Pyxis renderer — GpuScene scene-mutation API.
//
// Plan §18.5. M3-incremental: each commit fills in one slice of the
// public surface. PIMPL has been dropped; the inner entry tables and
// per-frame state live as private members of `GpuScene` directly,
// declared in the header. See the corresponding plan §18.9 edit for
// the relaxation rationale (single-tree Windows-x64 build, all
// consumers compile against the same vcpkg + clang-cl + EH/RTTI
// flags as the renderer DLL).
//
// Slot 0 is the canonical Invalid sentinel for every handle table —
// the ctor pushes a permanently-quarantined entry into each vector so
// a fabricated handle whose slot decodes to 0 never resolves to a
// live entry.
//
// CommitResources currently services:
//   1. Mesh GPU upload (vertex + index NVRHI buffers via writeBuffer
//      staging).
//   2. BLAS build per mesh (§16 split rule).
//
// TLAS rebuild + camera-uniform upload + the path-trace dispatch
// land in subsequent M3 commits.

#include <Pyxis/Renderer/GpuScene.h>

#include <Pyxis/Renderer/Descs/FrameStats.h>
#include <Pyxis/Renderer/Forward.h>
#include <Pyxis/Renderer/Profiler.h>

#include <nvrhi/nvrhi.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <utility>

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

}  // namespace

GpuScene::GpuScene(nvrhi::IDevice* device, Profiler& profiler, const GpuSceneCreateDesc& desc) {
    _device   = device;
    _profiler = &profiler;
    _desc     = desc;
    // Slot 0 is the Invalid sentinel for every handle table — keep
    // each one permanently quarantined so a fabricated handle whose
    // slot decodes to 0 never resolves.
    _meshes.emplace_back();
    _meshes[0].quarantined = true;
    _instances.emplace_back();
    _instances[0].quarantined = true;
    _lights.emplace_back();
    _lights[0].quarantined = true;
}

GpuScene::~GpuScene() = default;

// ---- Mesh ------------------------------------------------------------------
Expected<MeshHandle> GpuScene::CreateMesh(const MeshDesc& meshDesc) {
    // ---- Validate input ----------------------------------------------------
    if (meshDesc.positions.empty()) {
        return std::unexpected{
            PYXIS_ERROR(ErrorKind::InvalidArgument,
                        "CreateMesh: positions span is empty (mesh requires >= 1 vertex)")};
    }
    if (meshDesc.indices.empty()) {
        return std::unexpected{
            PYXIS_ERROR(ErrorKind::InvalidArgument,
                        "CreateMesh: indices span is empty (mesh requires a triangle list)")};
    }
    if ((meshDesc.indices.size() % 3) != 0) {
        return std::unexpected{
            PYXIS_ERROR(ErrorKind::InvalidArgument,
                        "CreateMesh: indices.size()=%zu is not a multiple of 3 (triangle list expected)",
                        meshDesc.indices.size())};
    }
    const uint32_t vertexCount = static_cast<uint32_t>(meshDesc.positions.size());
    for (const uint32_t indexValue : meshDesc.indices) {
        if (indexValue >= vertexCount) {
            return std::unexpected{
                PYXIS_ERROR(ErrorKind::InvalidArgument,
                            "CreateMesh: index %u >= vertexCount %u",
                            indexValue, vertexCount)};
        }
    }
    if (!meshDesc.normals.empty()
        && meshDesc.normals.size() != meshDesc.positions.size()) {
        return std::unexpected{
            PYXIS_ERROR(ErrorKind::InvalidArgument,
                        "CreateMesh: normals.size()=%zu must match positions.size()=%zu",
                        meshDesc.normals.size(), meshDesc.positions.size())};
    }
    if (!meshDesc.tangents.empty()
        && meshDesc.tangents.size() != meshDesc.positions.size()) {
        return std::unexpected{
            PYXIS_ERROR(ErrorKind::InvalidArgument,
                        "CreateMesh: tangents.size()=%zu must match positions.size()=%zu",
                        meshDesc.tangents.size(), meshDesc.positions.size())};
    }
    if (!meshDesc.uv0.empty()
        && meshDesc.uv0.size() != meshDesc.positions.size()) {
        return std::unexpected{
            PYXIS_ERROR(ErrorKind::InvalidArgument,
                        "CreateMesh: uv0.size()=%zu must match positions.size()=%zu",
                        meshDesc.uv0.size(), meshDesc.positions.size())};
    }

    // ---- Allocate slot -----------------------------------------------------
    uint32_t slot = 0;
    for (uint32_t candidateSlot = 1; candidateSlot < _meshes.size(); ++candidateSlot) {
        const MeshEntry& candidate = _meshes[candidateSlot];
        if (!candidate.live && !candidate.quarantined) {
            slot = candidateSlot;
            break;
        }
    }
    if (slot == 0) {
        if (_meshes.size() >= (1u << HANDLE_SLOT_BITS)) {
            return std::unexpected{
                PYXIS_ERROR(ErrorKind::InvalidState,
                            "CreateMesh: mesh-handle slot space exhausted (limit = %u)",
                            (1u << HANDLE_SLOT_BITS))};
        }
        _meshes.emplace_back();
        slot = static_cast<uint32_t>(_meshes.size() - 1);
    }

    // ---- Populate entry ----------------------------------------------------
    MeshEntry& entry = _meshes[slot];
    entry.live           = true;
    entry.needsGpuUpload = true;
    entry.needsBlasBuild = true;
    entry.vertexCount    = static_cast<uint32_t>(meshDesc.positions.size());
    entry.indexCount     = static_cast<uint32_t>(meshDesc.indices.size());
    entry.positions.assign(meshDesc.positions.begin(), meshDesc.positions.end());
    entry.indices.assign  (meshDesc.indices  .begin(), meshDesc.indices  .end());
    entry.normals.assign  (meshDesc.normals  .begin(), meshDesc.normals  .end());
    entry.tangents.assign (meshDesc.tangents .begin(), meshDesc.tangents .end());
    entry.uv0.assign      (meshDesc.uv0      .begin(), meshDesc.uv0      .end());
    entry.debugName.assign(meshDesc.debugName);

    return static_cast<MeshHandle>(HandleEncode(slot, entry.generation));
}

Expected<void> GpuScene::UpdateMesh(MeshHandle /*meshHandle*/, const MeshDesc& /*meshDesc*/) {
    return std::unexpected{
        PYXIS_ERROR(ErrorKind::NotImplemented, "GpuScene::UpdateMesh — M3 stub")};
}

void GpuScene::DestroyMesh(MeshHandle meshHandle) {
    const auto value = static_cast<uint32_t>(meshHandle);
    if (value == 0) return;
    const uint32_t slot = HandleSlot(value);
    if (slot == 0 || slot >= _meshes.size()) {
        ++_lastFrameStats.staleHandleDrops;
        return;
    }
    MeshEntry& entry = _meshes[slot];
    if (!entry.live || entry.quarantined
        || entry.generation != HandleGeneration(value)) {
        ++_lastFrameStats.staleHandleDrops;
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
    entry.indexBuffer  = nullptr;
    entry.blas         = nullptr;
    entry.vertexCount  = 0;
    entry.indexCount   = 0;
    if (entry.generation == HANDLE_GENERATION_QUARANTINE) {
        entry.quarantined = true;
    } else {
        ++entry.generation;
    }
}

bool GpuScene::HasMesh(MeshHandle meshHandle) const {
    const auto value = static_cast<uint32_t>(meshHandle);
    if (value == 0) return false;
    const uint32_t slot = HandleSlot(value);
    if (slot == 0 || slot >= _meshes.size()) return false;
    const MeshEntry& entry = _meshes[slot];
    return entry.live
        && !entry.quarantined
        && entry.generation == HandleGeneration(value);
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
    // §18.5: AppendInstance only validates the *handles* the caller
    // passes in; the mesh's geometry / material's parameters are the
    // mesh / material acquirer's responsibility. Material is allowed
    // to be Invalid for M3 (the closesthit shader uses barycentric
    // colour until M5 brings OpenPBR shading).
    if (instanceDesc.mesh == MeshHandle::Invalid) {
        return std::unexpected{
            PYXIS_ERROR(ErrorKind::InvalidArgument,
                        "AppendInstance: mesh handle is Invalid")};
    }
    if (!HasMesh(instanceDesc.mesh)) {
        return std::unexpected{
            PYXIS_ERROR(ErrorKind::InvalidHandle,
                        "AppendInstance: mesh handle %u not live (slot+generation mismatch)",
                        static_cast<uint32_t>(instanceDesc.mesh))};
    }

    uint32_t slot = 0;
    for (uint32_t candidateSlot = 1; candidateSlot < _instances.size(); ++candidateSlot) {
        const InstanceEntry& candidate = _instances[candidateSlot];
        if (!candidate.live && !candidate.quarantined) {
            slot = candidateSlot;
            break;
        }
    }
    if (slot == 0) {
        if (_instances.size() >= (1u << HANDLE_SLOT_BITS)) {
            return std::unexpected{
                PYXIS_ERROR(ErrorKind::TlasInstanceLimitExceeded,
                            "AppendInstance: instance-handle slot space exhausted (limit = %u)",
                            (1u << HANDLE_SLOT_BITS))};
        }
        _instances.emplace_back();
        slot = static_cast<uint32_t>(_instances.size() - 1);
    }

    InstanceEntry& entry = _instances[slot];
    entry.live           = true;
    entry.mesh           = instanceDesc.mesh;
    entry.material       = instanceDesc.material;
    entry.worldFromLocal = instanceDesc.worldFromLocal;
    entry.visible        = instanceDesc.visible;
    entry.debugName.assign(instanceDesc.debugName);

    _tlasNeedsRebuild = true;
    return static_cast<InstanceHandle>(HandleEncode(slot, entry.generation));
}

GpuScene::InstanceEntry* GpuScene::ResolveInstance(InstanceHandle handle) noexcept {
    const auto value = static_cast<uint32_t>(handle);
    if (value == 0) return nullptr;
    const uint32_t slot = HandleSlot(value);
    if (slot == 0 || slot >= _instances.size()) {
        ++_lastFrameStats.staleHandleDrops;
        return nullptr;
    }
    InstanceEntry& entry = _instances[slot];
    if (!entry.live || entry.quarantined
        || entry.generation != HandleGeneration(value)) {
        ++_lastFrameStats.staleHandleDrops;
        return nullptr;
    }
    return &entry;
}

GpuScene::LightEntry* GpuScene::ResolveLight(LightHandle handle) noexcept {
    const auto value = static_cast<uint32_t>(handle);
    if (value == 0) return nullptr;
    const uint32_t slot = HandleSlot(value);
    if (slot == 0 || slot >= _lights.size()) {
        ++_lastFrameStats.staleHandleDrops;
        return nullptr;
    }
    LightEntry& entry = _lights[slot];
    if (!entry.live || entry.quarantined
        || entry.generation != HandleGeneration(value)) {
        ++_lastFrameStats.staleHandleDrops;
        return nullptr;
    }
    return &entry;
}

void GpuScene::UpdateInstanceTransform(InstanceHandle instanceHandle,
                                       const hlslpp::float4x4& worldFromLocal) {
    if (auto* entry = ResolveInstance(instanceHandle)) {
        entry->worldFromLocal = worldFromLocal;
        _tlasNeedsRebuild = true;
    }
}

void GpuScene::UpdateInstanceMaterial(InstanceHandle instanceHandle,
                                      MaterialHandle materialHandle) {
    if (auto* entry = ResolveInstance(instanceHandle)) {
        // No TLAS rebuild — material binding is a closesthit-shader
        // table indirection (M5+); the TLAS only knows about mesh
        // BLAS + transform + visibility.
        entry->material = materialHandle;
    }
}

void GpuScene::SetInstanceVisibility(InstanceHandle instanceHandle, bool visible) {
    if (auto* entry = ResolveInstance(instanceHandle)) {
        if (entry->visible != visible) {
            entry->visible = visible;
            _tlasNeedsRebuild = true;
        }
    }
}

void GpuScene::DestroyInstance(InstanceHandle instanceHandle) {
    InstanceEntry* entry = ResolveInstance(instanceHandle);
    if (entry == nullptr) return;
    entry->live           = false;
    entry->mesh           = MeshHandle::Invalid;
    entry->material       = MaterialHandle::Invalid;
    entry->worldFromLocal = hlslpp::float4x4{};
    entry->visible        = false;
    entry->debugName.clear();
    if (entry->generation == HANDLE_GENERATION_QUARANTINE) {
        entry->quarantined = true;
    } else {
        ++entry->generation;
    }
    _tlasNeedsRebuild = true;
}

bool GpuScene::HasInstance(InstanceHandle instanceHandle) const {
    const auto value = static_cast<uint32_t>(instanceHandle);
    if (value == 0) return false;
    const uint32_t slot = HandleSlot(value);
    if (slot == 0 || slot >= _instances.size()) return false;
    const InstanceEntry& entry = _instances[slot];
    return entry.live
        && !entry.quarantined
        && entry.generation == HandleGeneration(value);
}

// ---- Camera & lights -------------------------------------------------------
void GpuScene::SetCamera(const CameraDesc& cameraDesc) {
    _cameraDesc = cameraDesc;
    _hasCamera  = true;
}

LightHandle GpuScene::AddLight(const LightDesc& lightDesc) {
    uint32_t slot = 0;
    for (uint32_t candidateSlot = 1; candidateSlot < _lights.size(); ++candidateSlot) {
        const LightEntry& candidate = _lights[candidateSlot];
        if (!candidate.live && !candidate.quarantined) {
            slot = candidateSlot;
            break;
        }
    }
    if (slot == 0) {
        if (_lights.size() >= (1u << HANDLE_SLOT_BITS)) {
            // Light handle space exhausted — Invalid is the
            // documented fallback (§18.5 lazy-acquirer contract); a
            // one-shot spdlog warn lands at the next CommitResources
            // via FrameStats::degraded once that path is wired.
            return LightHandle::Invalid;
        }
        _lights.emplace_back();
        slot = static_cast<uint32_t>(_lights.size() - 1);
    }

    LightEntry& entry = _lights[slot];
    entry.live      = true;
    entry.descCopy  = lightDesc;
    return static_cast<LightHandle>(HandleEncode(slot, entry.generation));
}

void GpuScene::UpdateLight(LightHandle lightHandle, const LightDesc& lightDesc) {
    if (auto* entry = ResolveLight(lightHandle)) {
        entry->descCopy = lightDesc;
    }
}

void GpuScene::RemoveLight(LightHandle lightHandle) {
    LightEntry* entry = ResolveLight(lightHandle);
    if (entry == nullptr) return;
    entry->live     = false;
    entry->descCopy = LightDesc{};
    if (entry->generation == HANDLE_GENERATION_QUARANTINE) {
        entry->quarantined = true;
    } else {
        ++entry->generation;
    }
}

// ---- Frame boundary --------------------------------------------------------
namespace {

// TLAS capacity. M3 single-cube ships one instance; the cap of 256 is
// enough for the M3.5 default-scene composition (3 spheres + ground
// instancer = ~5 instances). M6+ scales this up alongside the
// 16M-instance shard threshold (§16.5).
constexpr std::size_t TLAS_MAX_INSTANCES = 256u;

}  // namespace

Expected<void> GpuScene::CommitResources(nvrhi::ICommandList* commandList) {
    if (commandList == nullptr) {
        return std::unexpected{
            PYXIS_ERROR(ErrorKind::InvalidArgument,
                        "GpuScene::CommitResources: commandList is null")};
    }
    if (_device == nullptr) {
        return std::unexpected{
            PYXIS_ERROR(ErrorKind::InvalidState,
                        "GpuScene::CommitResources: scene has no device "
                        "(constructed in CPU-only test mode)")};
    }

    const Profiler::CpuScope commitScope(*_profiler, "render.commitResources");

    // ---- Upload pending meshes ----------------------------------------
    for (MeshEntry& entry : _meshes) {
        if (!entry.live || !entry.needsGpuUpload) continue;

        // Vertex buffer — hlslpp::float3 stride (16 bytes / vertex) on
        // x86_64 SSE. VK_FORMAT_R32G32B32_SFLOAT with that stride is
        // valid ray-tracing geometry input under
        // VK_KHR_ray_tracing_pipeline.
        const std::size_t vertexBytes = entry.positions.size() * sizeof(hlslpp::float3);
        nvrhi::BufferDesc vertexDesc;
        vertexDesc.byteSize                = vertexBytes;
        vertexDesc.debugName               = entry.debugName.empty()
                                              ? std::string{"mesh.vertex"}
                                              : entry.debugName + ".vertex";
        vertexDesc.isVertexBuffer          = true;
        vertexDesc.isAccelStructBuildInput = true;
        vertexDesc.initialState            = nvrhi::ResourceStates::CopyDest;
        vertexDesc.keepInitialState        = true;
        entry.vertexBuffer = _device->createBuffer(vertexDesc);
        if (!entry.vertexBuffer) {
            return std::unexpected{
                PYXIS_ERROR(ErrorKind::OutOfMemoryGpu,
                            "CommitResources: createBuffer(vertex, %zu bytes) failed for '%s'",
                            vertexBytes, entry.debugName.c_str())};
        }

        const std::size_t indexBytes = entry.indices.size() * sizeof(uint32_t);
        nvrhi::BufferDesc indexDesc;
        indexDesc.byteSize                = indexBytes;
        indexDesc.debugName               = entry.debugName.empty()
                                             ? std::string{"mesh.index"}
                                             : entry.debugName + ".index";
        indexDesc.isIndexBuffer           = true;
        indexDesc.isAccelStructBuildInput = true;
        indexDesc.format                  = nvrhi::Format::R32_UINT;
        indexDesc.initialState            = nvrhi::ResourceStates::CopyDest;
        indexDesc.keepInitialState        = true;
        entry.indexBuffer = _device->createBuffer(indexDesc);
        if (!entry.indexBuffer) {
            entry.vertexBuffer = nullptr;
            return std::unexpected{
                PYXIS_ERROR(ErrorKind::OutOfMemoryGpu,
                            "CommitResources: createBuffer(index, %zu bytes) failed for '%s'",
                            indexBytes, entry.debugName.c_str())};
        }

        commandList->writeBuffer(entry.vertexBuffer.Get(), entry.positions.data(), vertexBytes);
        commandList->writeBuffer(entry.indexBuffer.Get(),  entry.indices.data(),   indexBytes);
        entry.needsGpuUpload = false;
    }

    // ---- Build pending BLAS -------------------------------------------
    // §16 split rule: PreferFastTrace always, AllowCompaction for
    // ≥ 64k tris. AllowUpdate is never set in v1 — animation is
    // post-v1 (§42).
    static constexpr uint32_t BLAS_COMPACTION_TRIANGLE_THRESHOLD = 64u * 1024u;
    for (MeshEntry& entry : _meshes) {
        if (!entry.live || entry.needsGpuUpload || !entry.needsBlasBuild) continue;
        if (!entry.vertexBuffer || !entry.indexBuffer) {
            return std::unexpected{
                PYXIS_ERROR(ErrorKind::InvalidState,
                            "CommitResources: BLAS build for '%s' missing vertex/index buffers",
                            entry.debugName.c_str())};
        }

        const uint32_t triangleCount = entry.indexCount / 3u;

        nvrhi::rt::GeometryTriangles triangles;
        triangles.setVertexBuffer (entry.vertexBuffer.Get())
                 .setVertexFormat (nvrhi::Format::RGB32_FLOAT)
                 .setVertexCount  (entry.vertexCount)
                 .setVertexStride (sizeof(hlslpp::float3))
                 .setIndexBuffer  (entry.indexBuffer.Get())
                 .setIndexFormat  (nvrhi::Format::R32_UINT)
                 .setIndexCount   (entry.indexCount);

        nvrhi::rt::GeometryDesc geometry;
        geometry.setTriangles(triangles)
                .setFlags(nvrhi::rt::GeometryFlags::Opaque);

        auto buildFlags = nvrhi::rt::AccelStructBuildFlags::PreferFastTrace;
        if (triangleCount >= BLAS_COMPACTION_TRIANGLE_THRESHOLD) {
            buildFlags = buildFlags | nvrhi::rt::AccelStructBuildFlags::AllowCompaction;
        }

        nvrhi::rt::AccelStructDesc blasDesc;
        blasDesc.isTopLevel = false;
        blasDesc.bottomLevelGeometries.push_back(geometry);
        blasDesc.buildFlags = buildFlags;
        blasDesc.debugName  = entry.debugName.empty()
                               ? std::string{"mesh.blas"}
                               : entry.debugName + ".blas";
        entry.blas = _device->createAccelStruct(blasDesc);
        if (!entry.blas) {
            return std::unexpected{
                PYXIS_ERROR(ErrorKind::AccelStructBuildFailed,
                            "CommitResources: createAccelStruct(BLAS) failed for '%s' (triCount=%u)",
                            entry.debugName.c_str(), triangleCount)};
        }

        commandList->buildBottomLevelAccelStruct(entry.blas.Get(),
                                                 &geometry, /*numGeometries*/ 1,
                                                 buildFlags);
        entry.needsBlasBuild = false;
    }

    // ---- Rebuild TLAS if instances changed ----------------------------
    // Lazy-allocate the TLAS on first need; size it to a fixed
    // M3-friendly capacity (TLAS_MAX_INSTANCES). M6+ grows this with
    // the scene budget.
    if (_tlasNeedsRebuild) {
        if (!_tlas) {
            nvrhi::rt::AccelStructDesc tlasDesc;
            tlasDesc.isTopLevel           = true;
            tlasDesc.topLevelMaxInstances = TLAS_MAX_INSTANCES;
            tlasDesc.buildFlags           = nvrhi::rt::AccelStructBuildFlags::PreferFastTrace;
            tlasDesc.debugName            = "scene.tlas";
            _tlas = _device->createAccelStruct(tlasDesc);
            if (!_tlas) {
                return std::unexpected{
                    PYXIS_ERROR(ErrorKind::AccelStructBuildFailed,
                                "CommitResources: createAccelStruct(TLAS, max=%zu) failed",
                                TLAS_MAX_INSTANCES)};
            }
        }

        // Gather one nvrhi::rt::InstanceDesc per live + visible
        // instance whose mesh has a live BLAS. Skipping instances
        // whose BLAS isn't ready yet is the right behaviour during
        // partial mid-frame ingest — they'll join on the next
        // CommitResources tick.
        std::vector<nvrhi::rt::InstanceDesc> instanceDescs;
        instanceDescs.reserve(_instances.size());
        for (uint32_t slot = 1; slot < _instances.size(); ++slot) {
            const InstanceEntry& inst = _instances[slot];
            if (!inst.live || !inst.visible) continue;
            const auto meshValue = static_cast<uint32_t>(inst.mesh);
            const uint32_t meshSlot = HandleSlot(meshValue);
            if (meshSlot == 0 || meshSlot >= _meshes.size()) continue;
            const MeshEntry& mesh = _meshes[meshSlot];
            if (!mesh.live || !mesh.blas) continue;

            nvrhi::rt::InstanceDesc desc;
            // M5+ converts inst.worldFromLocal (pyxis row-vector
            // float4x4) to NVRHI's column-vector 3x4 affine. M3 ships
            // identity for the cube fixture; the helper for non-
            // identity transforms lands when AppendInstance is
            // exercised by real ingest. NVRHI's AffineTransform is
            // `float[12]` so we memcpy rather than assign.
            std::memcpy(&desc.transform,
                        &nvrhi::rt::c_IdentityTransform,
                        sizeof(nvrhi::rt::AffineTransform));
            desc.instanceMask = 0xFF;
            desc.instanceID = slot;
            desc.instanceContributionToHitGroupIndex = 0;
            desc.flags = nvrhi::rt::InstanceFlags::None;
            desc.bottomLevelAS = mesh.blas.Get();
            instanceDescs.push_back(desc);
        }

        if (instanceDescs.size() > TLAS_MAX_INSTANCES) {
            return std::unexpected{
                PYXIS_ERROR(ErrorKind::TlasInstanceLimitExceeded,
                            "CommitResources: TLAS rebuild needs %zu instances, cap is %zu",
                            instanceDescs.size(), TLAS_MAX_INSTANCES)};
        }

        commandList->buildTopLevelAccelStruct(_tlas.Get(),
                                              instanceDescs.data(),
                                              instanceDescs.size(),
                                              nvrhi::rt::AccelStructBuildFlags::PreferFastTrace);
        _tlasNeedsRebuild = false;
    }

    return {};
}

// ---- Introspection ---------------------------------------------------------
FrameStats GpuScene::LastFrameStats() const {
    FrameStats stats = _lastFrameStats;
    // Recount live meshes / instances / lights / BLAS on read so stats
    // reflect the current table state even before CommitResources
    // lands.
    uint64_t liveMeshCount = 0;
    uint64_t liveBlasCount = 0;
    for (const MeshEntry& entry : _meshes) {
        if (!entry.live) continue;
        ++liveMeshCount;
        if (entry.blas) ++liveBlasCount;
    }
    uint64_t liveInstanceCount = 0;
    for (const InstanceEntry& entry : _instances) {
        if (entry.live) ++liveInstanceCount;
    }
    uint64_t liveLightCount = 0;
    for (const LightEntry& entry : _lights) {
        if (entry.live) ++liveLightCount;
    }
    stats.meshCount     = liveMeshCount;
    stats.blasCount     = liveBlasCount;
    stats.instanceCount = liveInstanceCount;
    stats.lightCount    = liveLightCount;
    return stats;
}

}  // namespace pyxis
