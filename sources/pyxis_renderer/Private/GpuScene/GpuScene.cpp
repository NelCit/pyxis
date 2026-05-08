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
        bool                            live       = false;
        bool                            quarantined = false;
        uint8_t                         generation = 0;
        // CPU-side copies of the input MeshDesc spans. The renderer
        // owns these from CreateMesh until DestroyMesh.
        std::vector<hlslpp::float3>     positions;
        std::vector<uint32_t>           indices;
        std::vector<hlslpp::float3>     normals;
        std::vector<hlslpp::float4>     tangents;
        std::vector<hlslpp::float2>     uv0;
        std::string                     debugName;
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
    entry.live = true;
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
    entry.positions.clear();
    entry.indices.clear();
    entry.normals.clear();
    entry.tangents.clear();
    entry.uv0.clear();
    entry.debugName.clear();
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
Expected<void> GpuScene::CommitResources(nvrhi::ICommandList* /*commandList*/) {
    // Stub: no GPU upload yet. Real impl arrives once the mesh upload
    // path lands (next M3 commit) and the Flecs phase pipeline drives
    // System_UploadDirtyMeshes / BuildDirtyBlas / RebuildTlas.
    return {};
}

// ---- Introspection ---------------------------------------------------------
FrameStats GpuScene::LastFrameStats() const {
    FrameStats stats = _impl->lastFrameStats;
    // Recount live meshes on read so stats reflect the current table
    // even before CommitResources lands.
    uint64_t liveMeshCount = 0;
    for (const Impl::MeshEntry& entry : _impl->meshes) {
        if (entry.live) ++liveMeshCount;
    }
    stats.meshCount = liveMeshCount;
    return stats;
}

}  // namespace pyxis
