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
#include <Pyxis/Renderer/Descs/VolumeDesc.h>
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
class ITexture;
}  // namespace nvrhi

namespace pyxis {

// RFC 0003 — renderer-internal accessor (Private/Scene/SceneResources.h)
// friended below so render passes + the §35 unit-test harness can reach the
// scene's raw NVRHI resources without any public getter surface.
namespace detail {
struct SceneResourcesAccess;
}  // namespace detail

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
  // V2.A.12 — LRU eviction. Drops live textures by ascending
  // `lastAccessTick` until cumulative decoded byte count falls below
  // `targetBytes`. Returns the count evicted. Safe to call between
  // frames (single-writer rule per §30.11); the resulting destroyed
  // handles route through the regular DestroyTexture cleanup, so the
  // free-list / generation bump / quarantine semantics all kick in.
  // Operators / streaming scaffolding call this from a budget
  // watchdog when the texture cache exceeds the soft cap surfaced
  // by `LastFrameStats().textureBytes`.
  std::uint32_t EvictColdTextures(uint64_t targetBytes);

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

  // ---- Volumes (V2.A.5) ----------------------------------------------
  // AddVolume copies the dense voxel buffer + metadata, allocates a
  // VolumeHandle, and queues an NVRHI 3D-texture upload that drains
  // during the next CommitResources. Returns Invalid on bad inputs
  // (dim == 0, voxel-count mismatch, dimension > 2048, or handle-
  // space exhausted) and a one-shot spdlog warning fires.
  //
  // The shader doesn't sample the resulting texture in v2 (see
  // VolumeDesc.h); the API surface ships now so the future volume-
  // integrator pass has a stable contract to bind.
  [[nodiscard]] VolumeHandle AddVolume(const VolumeDesc& volumeDesc);
  void                       RemoveVolume(VolumeHandle volumeHandle);
  [[nodiscard]] bool         HasVolume(VolumeHandle volumeHandle) const;

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

  // Editor-side texture introspection. Returns the resolved file path
  // that was supplied to `AcquireTexture` for the given handle, or an
  // empty view for `TextureHandle::Invalid` / out-of-range / dead
  // handles. The returned view aliases an internal `std::string` and
  // is valid until the next mutation on the same thread (the render
  // thread per §31 single-writer rule); copy if you need to outlive
  // that. Used by the ImGui Material panel to display what's bound on
  // each `OpenPBRMaterialDesc` texture slot.
  [[nodiscard]] std::string_view         GetTexturePath(TextureHandle textureHandle) const noexcept;

  // Click-to-select helper (M7 follow-up). The picker AOV writes the
  // raw §15 instance-slot integer (24-bit `instanceCustomIndex`); the
  // viewer takes that on click and asks the scene for the bound
  // material so the Editor's Material combo can jump to it. Returns
  // MaterialHandle::Invalid when the slot is 0 (sentinel), out of
  // range, or points at a dead / quarantined entry — caller should
  // treat that as "no selection". Generation-checked InstanceHandle
  // lookup isn't possible here because the picker only carries the
  // 24-bit slot, not the 8-bit generation.
  [[nodiscard]] MaterialHandle LookupInstanceMaterialBySlot(uint32_t instanceSlot) const noexcept;

  // ---- Render-side accessors -----------------------------------------
  // RFC 0003 — the raw-NVRHI-resource getters (TLAS, packed scene
  // buffers, samplers, bindless table) moved to the renderer-internal
  // `SceneResources` view (Private/Scene/SceneResources.h), reached via
  // the friended `detail::SceneResourcesAccess` hook below. The camera
  // introspection below is NVRHI-free; the V2.A.5 volume getters are the
  // one surviving raw-NVRHI exception (retained as-is: no render pass
  // consumes them yet — the volume-integrator follow-up decides whether
  // they migrate into SceneResources or stay).
  [[nodiscard]] const CameraDesc&        GetCamera() const noexcept;
  [[nodiscard]] bool                     HasCamera() const noexcept;

  // V2.A.5 — render-side accessor for the per-volume Texture3D. Sparse
  // (includes dead slots). The volume-integrator follow-up will walk
  // these to populate a bindless slot; until then the textures are
  // alive on the GPU but unbound to any shader. Returns nullptr for
  // dead / un-uploaded entries.
  [[nodiscard]] uint32_t                 GetVolumeCount() const noexcept;
  [[nodiscard]] nvrhi::ITexture*         GetVolumeTextureAt(uint32_t volumeSlot) const noexcept;

private:
  // PIMPL: NVRHI handles, entry-table vectors, per-frame ring slots
  // live behind this pointer so the public header stays NVRHI- and
  // STL-container-free. §18.9 ABI rule.
  struct Impl;
  std::unique_ptr<Impl> _impl;

  // RFC 0003 — the ONLY hook through the PIMPL: the renderer-internal
  // SceneResources accessor (Private/Scene/SceneResources.h) builds the
  // render passes' borrowed-resource view from Impl. No other
  // public-surface addition.
  friend struct detail::SceneResourcesAccess;
};

}  // namespace pyxis
