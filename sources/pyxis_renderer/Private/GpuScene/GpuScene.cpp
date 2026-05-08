// Pyxis renderer — GpuScene PIMPL.
//
// Plan §18.5. M3-incremental: each commit fills in one slice of the
// public surface. This commit lands the mesh handle table — CreateMesh
// validates input, allocates a slot+generation handle (§19.7 packing:
// 24-bit slot, 8-bit generation), and stores the input geometry CPU-
// side as a copy. HasMesh / DestroyMesh round-trip the handle and
// honour the §18.5 stale-handle policy (recycled / Invalid handles
// silently no-op and increment FrameStats::staleHandleDrops).
//
// GPU upload is *not* part of this commit. The MeshEntry below
// holds the input data verbatim; the next M3 commit wires
// System_UploadDirtyMeshes which translates these CPU-side buffers
// into NVRHI vertex / index buffers and BLAS inputs.
//
// Other verbs (Material / Texture / Instance / Camera / Light /
// CommitResources) remain stubs — they land in subsequent M3
// commits in the dependency order documented at the top of the
// file's M3 punch list (mesh upload → BLAS → instances + TLAS →
// materials → textures → camera + lights → CommitResources phase
// pipeline).

#include <Pyxis/Renderer/GpuScene.h>

#include <Pyxis/Renderer/Descs/FrameStats.h>
#include <Pyxis/Renderer/Forward.h>
#include <Pyxis/Renderer/Profiler.h>

#include <nvrhi/nvrhi.h>

#include <cstdint>
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

}  // namespace

struct GpuScene::Impl {
    // Mesh table. Index 0 is reserved as the Invalid sentinel; valid
    // entries start at index 1. Vector grows monotonically; retired
    // slots are reused via a bumped generation until they hit 255 and
    // get quarantined.
    struct MeshEntry {
        bool                            live           = false;
        bool                            quarantined    = false;
        bool                            needsGpuUpload = false;   // true between CreateMesh and the next CommitResources upload step.
        bool                            needsBlasBuild = false;   // true between mesh upload and the next CommitResources BLAS step.
        uint8_t                         generation     = 0;
        // CPU-side copies of the input MeshDesc spans. The renderer
        // owns these from CreateMesh until DestroyMesh.
        std::vector<hlslpp::float3>     positions;
        std::vector<uint32_t>           indices;
        std::vector<hlslpp::float3>     normals;
        std::vector<hlslpp::float4>     tangents;
        std::vector<hlslpp::float2>     uv0;
        std::string                     debugName;
        // GPU-side state populated by CommitResources via NVRHI's
        // writeBuffer staging path. Both buffers are tagged
        // `isAccelStructBuildInput` so the BLAS build step can read
        // them directly. Vertex stride is sizeof(hlslpp::float3) = 16
        // bytes (12 floats + 4 padding); VK_FORMAT_R32G32B32_SFLOAT
        // with a 16-byte stride is valid ray-tracing geometry input
        // under VK_KHR_ray_tracing_pipeline.
        nvrhi::BufferHandle             vertexBuffer;
        nvrhi::BufferHandle             indexBuffer;
        uint32_t                        vertexCount = 0;
        uint32_t                        indexCount  = 0;
        // Bottom-level acceleration structure built from the buffers
        // above. v1 keys BLAS by MeshHandle (§16) — strict prototype
        // sharing means many instances of the same mesh reference one
        // BLAS, which is what the TLAS-build step (next M3 commit)
        // consumes.
        nvrhi::rt::AccelStructHandle    blas;
    };

    nvrhi::IDevice*    device   = nullptr;   // borrowed; outlives this scene.
    Profiler*          profiler = nullptr;   // borrowed.
    GpuSceneCreateDesc desc{};
    FrameStats         lastFrameStats{};

    std::vector<MeshEntry> meshes;
};

GpuScene::GpuScene(nvrhi::IDevice* device, Profiler& profiler, const GpuSceneCreateDesc& desc)
    : _impl(std::make_unique<Impl>()) {
    _impl->device   = device;
    _impl->profiler = &profiler;
    _impl->desc     = desc;
    // Slot 0 is the Invalid sentinel — keep it permanently empty.
    _impl->meshes.emplace_back();
    _impl->meshes[0].quarantined = true;
}

GpuScene::~GpuScene()                              = default;
GpuScene::GpuScene(GpuScene&&) noexcept            = default;
GpuScene& GpuScene::operator=(GpuScene&&) noexcept = default;

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
    // Linear scan for a retired non-quarantined slot. M3 single-cube
    // workloads have at most a handful of meshes; the next M3 commit
    // adds a free-list when uploading actually pressures the path.
    uint32_t slot = 0;
    for (uint32_t candidateSlot = 1; candidateSlot < _impl->meshes.size(); ++candidateSlot) {
        const Impl::MeshEntry& candidate = _impl->meshes[candidateSlot];
        if (!candidate.live && !candidate.quarantined) {
            slot = candidateSlot;
            break;
        }
    }
    if (slot == 0) {
        // No retired slot to recycle — append a new one.
        if (_impl->meshes.size() >= (1u << HANDLE_SLOT_BITS)) {
            return std::unexpected{
                PYXIS_ERROR(ErrorKind::InvalidState,
                            "CreateMesh: mesh-handle slot space exhausted (limit = %u)",
                            (1u << HANDLE_SLOT_BITS))};
        }
        _impl->meshes.emplace_back();
        slot = static_cast<uint32_t>(_impl->meshes.size() - 1);
    }

    // ---- Populate entry ----------------------------------------------------
    Impl::MeshEntry& entry = _impl->meshes[slot];
    entry.live           = true;
    entry.needsGpuUpload = true;     // drained on the next CommitResources upload step.
    entry.needsBlasBuild = true;     // drained on the next CommitResources BLAS step.
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
    if (value == 0) return;   // Invalid silently ignored, no stat bump.
    const uint32_t slot = HandleSlot(value);
    if (slot == 0 || slot >= _impl->meshes.size()) {
        ++_impl->lastFrameStats.staleHandleDrops;
        return;
    }
    Impl::MeshEntry& entry = _impl->meshes[slot];
    if (!entry.live || entry.quarantined
        || entry.generation != HandleGeneration(value)) {
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
    // references them retires (NVRHI tracks RefCountPtr ownership
    // across executeCommandList submissions), so this is safe even
    // mid-frame.
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
    if (slot == 0 || slot >= _impl->meshes.size()) return false;
    const Impl::MeshEntry& entry = _impl->meshes[slot];
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
Expected<InstanceHandle> GpuScene::AppendInstance(const InstanceDesc& /*instanceDesc*/) {
    return std::unexpected{
        PYXIS_ERROR(ErrorKind::NotImplemented, "GpuScene::AppendInstance — M3 stub")};
}

void GpuScene::UpdateInstanceTransform(InstanceHandle /*instanceHandle*/,
                                       const hlslpp::float4x4& /*worldFromLocal*/) {}

void GpuScene::UpdateInstanceMaterial(InstanceHandle /*instanceHandle*/,
                                      MaterialHandle /*materialHandle*/) {}

void GpuScene::SetInstanceVisibility(InstanceHandle /*instanceHandle*/, bool /*visible*/) {}

void GpuScene::DestroyInstance(InstanceHandle /*instanceHandle*/) {}

bool GpuScene::HasInstance(InstanceHandle /*instanceHandle*/) const {
    return false;
}

// ---- Camera & lights -------------------------------------------------------
void GpuScene::SetCamera(const CameraDesc& /*cameraDesc*/) {}

LightHandle GpuScene::AddLight(const LightDesc& /*lightDesc*/) {
    return LightHandle::Invalid;
}

void GpuScene::UpdateLight(LightHandle /*lightHandle*/, const LightDesc& /*lightDesc*/) {}

void GpuScene::RemoveLight(LightHandle /*lightHandle*/) {}

// ---- Frame boundary --------------------------------------------------------
Expected<void> GpuScene::CommitResources(nvrhi::ICommandList* commandList) {
    if (commandList == nullptr) {
        return std::unexpected{
            PYXIS_ERROR(ErrorKind::InvalidArgument,
                        "GpuScene::CommitResources: commandList is null")};
    }
    if (_impl->device == nullptr) {
        return std::unexpected{
            PYXIS_ERROR(ErrorKind::InvalidState,
                        "GpuScene::CommitResources: scene has no device "
                        "(constructed in CPU-only test mode)")};
    }

    const Profiler::CpuScope commitScope(*_impl->profiler, "render.commitResources");

    // ---- Upload pending meshes ----------------------------------------
    // Iterate the mesh table once and upload every live entry whose
    // CPU-side buffers haven't reached the GPU yet. NVRHI's
    // `writeBuffer` manages staging internally — it allocates from an
    // upload heap, memcpys the source bytes, and inserts the GPU-side
    // copy on the supplied (open) command list. Both buffers carry
    // `isAccelStructBuildInput=true` so the next M3 commit can hand
    // them to a BLAS build without re-uploading.
    //
    // Errors abort the commit; the partially-built entry's
    // `needsGpuUpload` stays true so the next commit retries.
    for (Impl::MeshEntry& entry : _impl->meshes) {
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
        entry.vertexBuffer = _impl->device->createBuffer(vertexDesc);
        if (!entry.vertexBuffer) {
            return std::unexpected{
                PYXIS_ERROR(ErrorKind::OutOfMemoryGpu,
                            "CommitResources: createBuffer(vertex, %zu bytes) failed for '%s'",
                            vertexBytes, entry.debugName.c_str())};
        }

        // Index buffer — uint32 indices are the M3 default; M5+ might
        // bump to uint16 on tiny meshes, but `MeshDesc::indices` is
        // span<uint32_t> so v1 stays uint32 throughout.
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
        entry.indexBuffer = _impl->device->createBuffer(indexDesc);
        if (!entry.indexBuffer) {
            entry.vertexBuffer = nullptr;   // drop the partially-built upload.
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
    // §16 split rules. M3's path-trace box has < 64k tris (the hardcoded
    // cube ships at 12), so PreferFastTrace alone is the right choice
    // — `AllowCompaction` is opt-in for ≥ 64k tris and pays a memory
    // savings that doesn't matter at this scale. `AllowUpdate` is
    // never set in v1: animation is post-v1 (§42), so BLAS is built
    // once and refit is irrelevant.
    static constexpr uint32_t BLAS_COMPACTION_TRIANGLE_THRESHOLD = 64u * 1024u;
    for (Impl::MeshEntry& entry : _impl->meshes) {
        if (!entry.live || entry.needsGpuUpload || !entry.needsBlasBuild) continue;
        if (!entry.vertexBuffer || !entry.indexBuffer) {
            return std::unexpected{
                PYXIS_ERROR(ErrorKind::InvalidState,
                            "CommitResources: BLAS build for '%s' missing vertex/index buffers",
                            entry.debugName.c_str())};
        }

        const uint32_t triangleCount = entry.indexCount / 3u;

        // Geometry: one triangles-only entry pointing at the mesh's
        // vertex+index buffers. Vertex format is R32G32B32_FLOAT with a
        // sizeof(hlslpp::float3) stride (16 bytes) — the SSE alignment
        // padding is benign because the format declares only 12 bytes
        // are read per vertex. Index format is R32_UINT, matching the
        // public MeshDesc::indices spelling.
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

        // Build flags: PreferFastTrace always; add AllowCompaction
        // for the ≥ 64k-tri path the §16 contract calls out. M3 cube
        // hits the small-mesh branch.
        auto buildFlags = nvrhi::rt::AccelStructBuildFlags::PreferFastTrace;
        if (triangleCount >= BLAS_COMPACTION_TRIANGLE_THRESHOLD) {
            buildFlags = buildFlags | nvrhi::rt::AccelStructBuildFlags::AllowCompaction;
        }

        // AccelStructDesc carries the geometry list (NVRHI uses the
        // same desc for both create + build to size internal scratch).
        nvrhi::rt::AccelStructDesc blasDesc;
        blasDesc.isTopLevel = false;
        blasDesc.bottomLevelGeometries.push_back(geometry);
        blasDesc.buildFlags = buildFlags;
        blasDesc.debugName  = entry.debugName.empty()
                               ? std::string{"mesh.blas"}
                               : entry.debugName + ".blas";
        entry.blas = _impl->device->createAccelStruct(blasDesc);
        if (!entry.blas) {
            return std::unexpected{
                PYXIS_ERROR(ErrorKind::AccelStructBuildFailed,
                            "CommitResources: createAccelStruct(BLAS) failed for '%s' (triCount=%u)",
                            entry.debugName.c_str(), triangleCount)};
        }

        // Build the BLAS on the supplied command list. The geometry
        // pointer points at the local `geometry` value above; NVRHI
        // copies what it needs internally before the call returns, so
        // letting the local go out of scope after the call is safe.
        commandList->buildBottomLevelAccelStruct(entry.blas.Get(),
                                                 &geometry, /*numGeometries*/ 1,
                                                 buildFlags);
        entry.needsBlasBuild = false;
    }

    // TLAS rebuild + camera + light upload land in subsequent M3
    // commits. Until then CommitResources services upload + BLAS.
    return {};
}

// ---- Introspection ---------------------------------------------------------
FrameStats GpuScene::LastFrameStats() const {
    FrameStats stats = _impl->lastFrameStats;
    // Recount live meshes + live BLAS on read so stats reflect the
    // current table state even before CommitResources lands. This
    // also keeps blasCount honest after a Destroy*: the cumulative
    // count tracked in `lastFrameStats.blasCount` would over-report
    // otherwise (BLAS gets dropped in DestroyMesh once that lands).
    uint64_t liveMeshCount = 0;
    uint64_t liveBlasCount = 0;
    for (const Impl::MeshEntry& entry : _impl->meshes) {
        if (!entry.live) continue;
        ++liveMeshCount;
        if (entry.blas) ++liveBlasCount;
    }
    stats.meshCount = liveMeshCount;
    stats.blasCount = liveBlasCount;
    return stats;
}

}  // namespace pyxis
