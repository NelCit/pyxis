// Pyxis renderer — GpuScene scene-mutation API.
//
// Plan §18.5. The single canonical mutation surface used by all
// ingest paths (Hydra delegate, USD-direct, viewer, headless): every
// per-frame change to the scene flows through a method on this
// class. `Public/Descs/*.h` defines the input PODs.
//
// Threading (§31):
//   - Ingest threads call mutation verbs (CreateMesh /
//     AcquireMaterial / AppendInstance / UpdateInstanceTransform /
//     etc.). The scene enqueues the mutation onto an internal
//     moodycamel::ConcurrentQueue and returns a handle synchronously;
//     the actual ECS mutation and any GPU work happens on the render
//     thread inside CommitResources.
//   - Render thread calls CommitResources(commandList) once per
//     frame to drain the queue, run the Flecs phase pipeline, and
//     submit uploads / BLAS builds / TLAS rebuilds onto the supplied
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
// PIMPL: every NVRHI handle, every entry-table vector, every
// per-frame ring slot lives behind `Impl` so the public header
// doesn't drag <nvrhi/nvrhi.h>, <vector>, or <string> into consumer
// translation units. §18.9 ABI rule.

#pragma once

#include <Pyxis/Renderer/Descs/CameraDesc.h>
#include <Pyxis/Renderer/Descs/FrameStats.h>
#include <Pyxis/Renderer/Descs/GpuSceneCreateDesc.h>
#include <Pyxis/Renderer/Descs/InstanceDesc.h>
#include <Pyxis/Renderer/Descs/LightDesc.h>
#include <Pyxis/Renderer/Descs/MeshDesc.h>
#include <Pyxis/Renderer/Descs/OpenPBRMaterialDesc.h>
#include <Pyxis/Renderer/Descs/TextureKey.h>
#include <Pyxis/Renderer/Error.h>
#include <Pyxis/Renderer/Forward.h>
#include <Pyxis/Renderer/RendererApi.h>

#include <hlsl++.h>

#include <memory>

// Forward-declarations only — the public header doesn't pull
// <nvrhi/nvrhi.h>. The renderer's private impl + render passes that
// need the full type include the NVRHI header in their .cpp instead.
namespace nvrhi {
class IDevice;
class ICommandList;
class IBuffer;
class ITexture;
class ISampler;
namespace rt {
class IAccelStruct;
}  // namespace rt
}  // namespace nvrhi

namespace pyxis {

class PYXIS_RENDERER_API GpuScene final {
public:
  GpuScene(nvrhi::IDevice* device, Profiler& profiler, const GpuSceneCreateDesc& desc);
  ~GpuScene();

  // Non-copyable + non-movable. The defaulted move ops would
  // produce a moved-from object whose `_impl = nullptr` is still
  // callable — every CommitResources / mutation verb would
  // null-deref. Both viewer and headless construct GpuScene as a
  // stack-local that never needs to move, so deletion is the safe
  // option.
  GpuScene(const GpuScene&)                = delete;
  GpuScene& operator=(const GpuScene&)     = delete;
  GpuScene(GpuScene&&) noexcept            = delete;
  GpuScene& operator=(GpuScene&&) noexcept = delete;

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
  [[nodiscard]] MaterialHandle AcquireMaterial(const OpenPBRMaterialDesc& materialDesc);
  void UpdateMaterial(MaterialHandle materialHandle, const OpenPBRMaterialDesc& materialDesc);
  void DestroyMaterial(MaterialHandle materialHandle);
  [[nodiscard]] bool HasMaterial(MaterialHandle materialHandle) const;

  // ---- Texture -------------------------------------------------------
  // AcquireTexture is also lazy + non-failing at the call site;
  // decode + upload happens asynchronously on the I/O pool. Decode
  // failures surface via FrameStats::degraded + a one-shot spdlog
  // entry and the offending texture is replaced by the
  // missing-texture colour.
  [[nodiscard]] TextureHandle AcquireTexture(const TextureKey& textureKey);
  void DestroyTexture(TextureHandle textureHandle);
  [[nodiscard]] bool HasTexture(TextureHandle textureHandle) const;

  // ---- Instance ------------------------------------------------------
  [[nodiscard]] Expected<InstanceHandle> AppendInstance(const InstanceDesc& instanceDesc);
  void UpdateInstanceTransform(InstanceHandle instanceHandle,
                               const hlslpp::float4x4& worldFromLocal);
  void UpdateInstanceMaterial(InstanceHandle instanceHandle, MaterialHandle materialHandle);
  void SetInstanceVisibility(InstanceHandle instanceHandle, bool visible);
  void DestroyInstance(InstanceHandle instanceHandle);
  [[nodiscard]] bool HasInstance(InstanceHandle instanceHandle) const;

  // ---- Camera & lights -----------------------------------------------
  void SetCamera(const CameraDesc& cameraDesc);
  [[nodiscard]] LightHandle AddLight(const LightDesc& lightDesc);
  void UpdateLight(LightHandle lightHandle, const LightDesc& lightDesc);
  void RemoveLight(LightHandle lightHandle);

  // ---- Frame boundary ------------------------------------------------
  // Drains pending mutations, runs the Flecs phase pipeline, builds
  // dirty BLAS, rebuilds / refits TLAS. Render thread only.
  [[nodiscard]] Expected<void> CommitResources(nvrhi::ICommandList* commandList);

  // ---- Scene-wide reset ----------------------------------------------
  // Drops every mesh / material / texture / instance / light + the
  // TLAS + the camera + the dedup maps + per-frame counters, leaving
  // the scene in the exact post-construction state. Used by the
  // viewer's "Open scene..." path: caller waits the device idle,
  // calls Clear, then re-runs the ingest engine against the new path.
  // Render thread only — same single-writer rule as every other
  // mutation verb (§31). The caller must NOT have any in-flight
  // command list referencing the scene's TLAS / buffers when this
  // runs; the public ABI rule (§18.5) lets us assume callers honour
  // the documented synchronisation contract.
  void Clear() noexcept;

  // ---- Introspection -------------------------------------------------
  [[nodiscard]] FrameStats LastFrameStats() const;

  // Editor-side enumeration (M7 follow-up). The viewer's editor panel
  // walks live lights / materials to populate dropdowns + sliders;
  // the engine never iterates these tables itself, so the surface is
  // intentionally simple — `Count()` returns the live entry count
  // and `At(i)` returns the i-th live entry's handle / desc-copy
  // (skipping dead + quarantined slots). The pair (handle + desc)
  // is enough for the panel to (a) display current values, (b) push
  // edits back via the matching Update verb. Index validity is
  // bounded by Count() at the moment of call; callers that mutate
  // the scene mid-iteration must re-query.
  [[nodiscard]] uint32_t                 GetLiveLightCount() const noexcept;
  [[nodiscard]] LightHandle              GetLightHandleAt(uint32_t liveIndex) const noexcept;
  [[nodiscard]] LightDesc                GetLightDescAt(uint32_t liveIndex) const noexcept;

  [[nodiscard]] uint32_t                 GetLiveMaterialCount() const noexcept;
  [[nodiscard]] MaterialHandle           GetMaterialHandleAt(uint32_t liveIndex) const noexcept;
  [[nodiscard]] OpenPBRMaterialDesc      GetMaterialDescAt(uint32_t liveIndex) const noexcept;

  // ---- Render-side accessors -----------------------------------------
  // Borrowed pointer / ref valid for the lifetime of the scene (the
  // TLAS is alive after the first CommitResources that observed at
  // least one instance; nullptr before that). These exist so
  // render-side code inside pyxis_renderer (PathTracePass, future
  // pyxis_hydra) can bind the scene's TLAS + camera into their
  // descriptor sets without GpuScene having to know which passes
  // exist.
  //
  // `nvrhi::rt::IAccelStruct` is forward-declared at the top of this
  // header, so callers that only need to round-trip the pointer
  // (binding it into another desc) don't need <nvrhi/nvrhi.h>.
  // Callers that need to read its members include the full NVRHI
  // header from their .cpp.
  [[nodiscard]] nvrhi::rt::IAccelStruct* GetTlas() const noexcept;
  [[nodiscard]] const CameraDesc&        GetCamera() const noexcept;
  [[nodiscard]] bool                     HasCamera() const noexcept;

  // M5/M6: structured buffer of OpenPBRMaterialGPU entries the
  // closesthit reads via the instance→material indirection (see
  // `GetInstanceMaterialBuffer` below). Allocated lazily on the
  // first CommitResources that observed at least one
  // AcquireMaterial — nullptr before that. The §11 packed layout
  // is documented in resources/shaders/ShaderInterop.slang's
  // `OpenPBRMaterialGPU` struct.
  [[nodiscard]] nvrhi::IBuffer*          GetMaterialBuffer() const noexcept;

  // M6 (plan §15): structured buffer of `uint` indexed by instance
  // slot. Each entry holds the material slot bound to that instance,
  // so the closesthit reads `materials[instanceMaterial[InstanceID()]]`.
  // The indirection is what frees the TLAS instanceCustomIndex to
  // carry the INSTANCE slot (per §15) — required for the M6
  // instanceId AOV + future picking. Allocated alongside the TLAS
  // on the first CommitResources that built one; nullptr before that.
  [[nodiscard]] nvrhi::IBuffer*          GetInstanceMaterialBuffer() const noexcept;

  // M7: structured buffer of `LightGpu` entries (resources/shaders/
  // ShaderInterop.slang) packed from the LightDesc copies of every
  // LIVE LightHandle. Sized to the live-light count (sparse / dead
  // slots are omitted, NOT included as holes — the closesthit
  // iterates the buffer's full length). Allocated lazily on the
  // first CommitResources that observed at least one AddLight;
  // nullptr before that — PathTracePass binds a 1-element zero
  // sentinel fallback in that case so the closesthit never reads
  // an unbound buffer.
  [[nodiscard]] nvrhi::IBuffer*          GetLightBuffer() const noexcept;

  // M7 NdotL: per-instance mesh slot side-table (parallel to the
  // §15 instance→material side-table). Indexed by instance slot,
  // value = mesh slot. The closesthit needs this to know which
  // mesh's face-normal range to look up for the Lambert pass.
  [[nodiscard]] nvrhi::IBuffer*          GetInstanceMeshBuffer() const noexcept;

  // M7 NdotL: flat float4 buffer of object-space face normals,
  // every live mesh's normals concatenated. Closesthit reads:
  //   nLocal = gMeshFaceNormals[gMeshFaceOffsets[meshSlot]
  //                            + PrimitiveIndex()].xyz
  [[nodiscard]] nvrhi::IBuffer*          GetMeshFaceNormalsBuffer() const noexcept;

  // M7 NdotL: per-mesh-slot start offsets into GetMeshFaceNormalsBuffer.
  // Sized to the mesh table length so the closesthit's lookup is
  // bounds-safe by construction.
  [[nodiscard]] nvrhi::IBuffer*          GetMeshFaceOffsetsBuffer() const noexcept;

  // M7-IBL: env-map texture of the FIRST live UsdLuxDomeLight, or
  // nullptr if no dome with a resolved envMap exists. Miss shader
  // samples this at the ray direction's lat-long uv to draw the
  // actual HDRI background. Multi-dome scenes pick the first one
  // (production-renderer convention; multi-dome is post-v1 §43).
  [[nodiscard]] nvrhi::ITexture*         GetDomeEnvMapTexture() const noexcept;

  // M5/M7: shared linear-clamp sampler used for every bindless
  // texture lookup (materials' baseColor/normal/etc. + the dome
  // env-map). Per-role samplers (anisotropic for tangent maps, etc.)
  // are an M9 polish item.
  [[nodiscard]] nvrhi::ISampler*         GetBindlessSampler() const noexcept;

private:
  // PIMPL: NVRHI handles, entry-table vectors, per-frame ring slots
  // live behind this pointer so the public header stays NVRHI- and
  // STL-container-free. §18.9 ABI rule.
  struct Impl;
  std::unique_ptr<Impl> _impl;
};

}  // namespace pyxis
