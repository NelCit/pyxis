// Pyxis renderer — GpuScene PIMPL stub.
//
// Plan §18.5. M3-incremental scaffolding: every public method exists
// and respects its signature so consumers can compile against the
// final shape, but bodies are placeholders that return
// `ErrorKind::NotImplemented` (for Expected verbs), `Handle::Invalid`
// (for handle-returning verbs), `false` (for Has* probes), or are
// no-ops. The real implementations land in subsequent M3 commits in
// dependency order:
//
//   1. mesh upload (CreateMesh / UpdateMesh / DestroyMesh / HasMesh)
//   2. BLAS build (consumed by mesh upload finalisation)
//   3. instance + TLAS rebuild (AppendInstance / UpdateInstance* /
//      DestroyInstance / HasInstance)
//   4. material acquire / dedup (AcquireMaterial / Update / Destroy /
//      HasMaterial) — full OpenPBR shading pipeline arrives at M5
//   5. texture acquire / lazy decode (AcquireTexture / Destroy /
//      HasTexture) — also M5
//   6. camera + lights (SetCamera / AddLight / etc.) — needed by the
//      M3 path-trace box (one distant light)
//   7. CommitResources — drives the Flecs phase pipeline, drains
//      uploads, kicks BLAS / TLAS builds onto the supplied command
//      list
//
// Until each is filled in, callers see a precise NotImplemented
// diagnostic via PYXIS_ERROR rather than a silent crash.

#include <Pyxis/Renderer/GpuScene.h>

#include <Pyxis/Renderer/Descs/FrameStats.h>
#include <Pyxis/Renderer/Profiler.h>

#include <utility>

namespace pyxis {

struct GpuScene::Impl {
    nvrhi::IDevice*    device   = nullptr;   // borrowed; outlives this scene.
    Profiler*          profiler = nullptr;   // borrowed.
    GpuSceneCreateDesc desc{};
    FrameStats         lastFrameStats{};
};

GpuScene::GpuScene(nvrhi::IDevice* device, Profiler& profiler, const GpuSceneCreateDesc& desc)
    : _impl(std::make_unique<Impl>()) {
    _impl->device   = device;
    _impl->profiler = &profiler;
    _impl->desc     = desc;
}

GpuScene::~GpuScene()                              = default;
GpuScene::GpuScene(GpuScene&&) noexcept            = default;
GpuScene& GpuScene::operator=(GpuScene&&) noexcept = default;

// ---- Mesh ------------------------------------------------------------------
Expected<MeshHandle> GpuScene::CreateMesh(const MeshDesc& /*meshDesc*/) {
    return std::unexpected{
        PYXIS_ERROR(ErrorKind::NotImplemented, "GpuScene::CreateMesh — M3 stub")
    };
}

Expected<void> GpuScene::UpdateMesh(MeshHandle /*meshHandle*/, const MeshDesc& /*meshDesc*/) {
    return std::unexpected{
        PYXIS_ERROR(ErrorKind::NotImplemented, "GpuScene::UpdateMesh — M3 stub")
    };
}

void GpuScene::DestroyMesh(MeshHandle /*meshHandle*/) {
    // Stale-handle drops would increment lastFrameStats.staleHandleDrops
    // here once the handle table exists; for the stub there is no
    // table so every Destroy* on Invalid is just a no-op.
}

bool GpuScene::HasMesh(MeshHandle /*meshHandle*/) const {
    return false;
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
        PYXIS_ERROR(ErrorKind::NotImplemented, "GpuScene::AppendInstance — M3 stub")
    };
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
    // Stub: no mutations to drain, no AS to build, nothing to upload.
    // Real impl arrives once the mesh/material/instance tables exist.
    return {};
}

// ---- Introspection ---------------------------------------------------------
FrameStats GpuScene::LastFrameStats() const {
    return _impl->lastFrameStats;
}

}  // namespace pyxis
