// Pyxis renderer — GpuScene scene-mutation API.
//
// Plan §18.5. The single canonical mutation surface used by all
// ingest paths (Hydra delegate, USD-direct, viewer, headless): every
// per-frame change to the scene flows through a method on this class.
// `Public/Descs/*.h` defines the input PODs.
//
// Threading (§31):
//   - Ingest threads call mutation verbs (CreateMesh / AcquireMaterial
//     / AppendInstance / UpdateInstanceTransform / etc.). The scene
//     enqueues the mutation onto an internal moodycamel::ConcurrentQueue
//     and returns a handle synchronously; the actual ECS mutation
//     and any GPU work happens on the render thread inside
//     CommitResources.
//   - Render thread calls CommitResources(commandList) once per frame
//     to drain the queue, run the Flecs phase pipeline, and submit
//     uploads / BLAS builds / TLAS rebuilds onto the supplied
//     command list. May fail (BlasBudgetExceeded /
//     TlasInstanceLimitExceeded / OutOfMemoryGpu); soft fallbacks
//     surface via FrameStats::degraded + a one-shot spdlog entry.
//
// Stale-handle policy (§18.5):
//   - Destroy* and Update* verbs return void; a handle whose
//     generation has been recycled (or `Invalid`) is silently
//     dropped and counted in FrameStats::staleHandleDrops.
//   - Callers needing a hard guarantee probe with HasMesh /
//     HasMaterial / HasTexture / HasInstance first.
//
// M3 status:
//   The class is fully declared so PyxisRenderer can take it as a
//   ctor argument and consumers (pyxis_app, future pyxis_hydra,
//   pyxis_usd_ingest) can compile against the final shape. The
//   implementation is a stub: every Expected<T> returns
//   ErrorKind::NotImplemented, every handle-returning verb returns
//   Invalid, every bool-returning probe returns false. Subsequent M3
//   commits fill in the real bodies in dependency order (mesh upload,
//   BLAS, TLAS, materials, lights, then CommitResources).

#pragma once

#include <Pyxis/Renderer/Error.h>
#include <Pyxis/Renderer/Forward.h>
#include <Pyxis/Renderer/RendererApi.h>
#include <Pyxis/Renderer/Descs/CameraDesc.h>
#include <Pyxis/Renderer/Descs/FrameStats.h>
#include <Pyxis/Renderer/Descs/GpuSceneCreateDesc.h>
#include <Pyxis/Renderer/Descs/InstanceDesc.h>
#include <Pyxis/Renderer/Descs/LightDesc.h>
#include <Pyxis/Renderer/Descs/MeshDesc.h>
#include <Pyxis/Renderer/Descs/OpenPBRMaterialDesc.h>
#include <Pyxis/Renderer/Descs/TextureKey.h>

#include <hlsl++.h>

#include <memory>

namespace nvrhi {
class IDevice;
class ICommandList;
}  // namespace nvrhi

namespace pyxis {

class PYXIS_RENDERER_API GpuScene final {
public:
    GpuScene(nvrhi::IDevice* device, Profiler& profiler, const GpuSceneCreateDesc& desc);
    ~GpuScene();

    GpuScene(const GpuScene&)            = delete;
    GpuScene& operator=(const GpuScene&) = delete;
    GpuScene(GpuScene&&) noexcept;
    GpuScene& operator=(GpuScene&&) noexcept;

    // ---- Mesh ----------------------------------------------------------
    [[nodiscard]] Expected<MeshHandle> CreateMesh(const MeshDesc& meshDesc);
    [[nodiscard]] Expected<void>       UpdateMesh(MeshHandle meshHandle, const MeshDesc& meshDesc);
    void                               DestroyMesh(MeshHandle meshHandle);
    [[nodiscard]] bool                 HasMesh(MeshHandle meshHandle) const;

    // ---- Material ------------------------------------------------------
    // AcquireMaterial dedupes by hash and never fails at the call
    // site; per-material conversion errors surface during the next
    // CommitResources via FrameStats::degraded + a one-shot spdlog
    // line, and the offending material falls back to the default
    // grey material.
    [[nodiscard]] MaterialHandle       AcquireMaterial(const OpenPBRMaterialDesc& materialDesc);
    void                               UpdateMaterial(MaterialHandle materialHandle,
                                                      const OpenPBRMaterialDesc& materialDesc);
    void                               DestroyMaterial(MaterialHandle materialHandle);
    [[nodiscard]] bool                 HasMaterial(MaterialHandle materialHandle) const;

    // ---- Texture -------------------------------------------------------
    // AcquireTexture is also lazy + non-failing at the call site;
    // decode + upload happens asynchronously on the I/O pool. Decode
    // failures surface via FrameStats::degraded + a one-shot spdlog
    // entry and the offending texture is replaced by the
    // missing-texture colour.
    [[nodiscard]] TextureHandle        AcquireTexture(const TextureKey& textureKey);
    void                               DestroyTexture(TextureHandle textureHandle);
    [[nodiscard]] bool                 HasTexture(TextureHandle textureHandle) const;

    // ---- Instance ------------------------------------------------------
    [[nodiscard]] Expected<InstanceHandle> AppendInstance(const InstanceDesc& instanceDesc);
    void                               UpdateInstanceTransform(InstanceHandle instanceHandle,
                                                                const hlslpp::float4x4& worldFromLocal);
    void                               UpdateInstanceMaterial(InstanceHandle instanceHandle,
                                                              MaterialHandle materialHandle);
    void                               SetInstanceVisibility(InstanceHandle instanceHandle, bool visible);
    void                               DestroyInstance(InstanceHandle instanceHandle);
    [[nodiscard]] bool                 HasInstance(InstanceHandle instanceHandle) const;

    // ---- Camera & lights -----------------------------------------------
    void                               SetCamera(const CameraDesc& cameraDesc);
    [[nodiscard]] LightHandle          AddLight(const LightDesc& lightDesc);
    void                               UpdateLight(LightHandle lightHandle, const LightDesc& lightDesc);
    void                               RemoveLight(LightHandle lightHandle);

    // ---- Frame boundary ------------------------------------------------
    // Drains pending mutations, runs the Flecs phase pipeline, builds
    // dirty BLAS, rebuilds / refits TLAS. Render thread only.
    [[nodiscard]] Expected<void>       CommitResources(nvrhi::ICommandList* commandList);

    // ---- Introspection -------------------------------------------------
    [[nodiscard]] FrameStats           LastFrameStats() const;

private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
};

}  // namespace pyxis
