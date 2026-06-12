// Pyxis renderer — GpuScene private implementation header.
//
// Defines `struct GpuScene::Impl` (the PIMPL body) shared between every
// per-verb .cpp file. The Flecs component PODs live in Components.h and the
// `gpuscene_detail` hashing / packing / handle / buffer helpers in Detail.h
// (both included below); this header keeps just the Impl struct. The split
// (one verb-group per file: Mesh, Material, Texture, Instance, Light, Commit)
// keeps the single 2200-line GpuScene.cpp from growing unboundedly while
// preserving the public PIMPL contract — the public header still sees only
// `struct Impl;` + `std::unique_ptr<Impl>`. Each verb file declares member
// functions on Impl; GpuScene.cpp's public methods forward one line to
// `_impl->Verb(...)`.
//
// This header is PRIVATE — only files under Private/GpuScene/ may
// include it. Per §30 / §18.9 it must never appear in any Public/
// header transitively.

#pragma once

#include <Pyxis/Renderer/GpuScene.h>

#include <Pyxis/Platform/Logging/Log.h>
#include <Pyxis/Platform/Logging/LogCategories.h>
#include <Pyxis/Renderer/Descs/FrameStats.h>
#include <Pyxis/Renderer/Forward.h>
#include <Pyxis/Renderer/Profiler.h>

// ShaderInterop.slang lives in resources/shaders/ — the
// pyxis_renderer target's PRIVATE include path puts that directory
// on the search path so the C++ side here gets the same
// OpenPBRMaterialGPU / LightGpu layouts the shaders read. See
// pyxis_renderer/CMakeLists.txt for the wiring rationale.
#include "ShaderInterop.slang"

#include <nvrhi/nvrhi.h>

// RFC 0009 — the Flecs SceneWorld is the scene representation. P1 migrates LIGHTS:
// they live as entities in `sceneWorld` (component below) rather than a std::vector,
// with HandleBimap owning slot/generation. flecs is a PRIVATE renderer dep, so this
// header (Private only, §18.9) may include it; no Flecs type reaches Public/.
#include "GpuScene/GpuSlotMap.h"
#include "Scene/Components/Dirty.h"
#include "Scene/HandleBimap.h"
#include "Scene/Phases.h"  // RFC 0009 — §30.11 phase tags + RegisterPhasePipeline.

#include <flecs.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "GpuScene/Components.h"  // RFC 0009 — Flecs component POD structs (split out).
#include "GpuScene/Detail.h"      // RFC 0009 — gpuscene_detail hashing / packing / buffer helpers.

namespace pyxis {

// PIMPL body. Defined here so per-verb .cpp files can declare member
// functions on it; the public header (GpuScene.h) only forward-
// declares this. All NVRHI handles + STL containers live behind this
// boundary per §18.9.
//
// Field order here groups related data for cache locality +
// readability (per-resource sections, dirty-flag bools clustered
// at the end); optimal-packing reorder would scatter related
// fields apart and hurt the cognitive map without changing
// runtime hot-path costs (Impl is allocated once per renderer
// instance, not in any loop).
// NOLINTNEXTLINE(clang-analyzer-optin.performance.Padding)
struct GpuScene::Impl
{
  // RFC 0009 P4 — per-mesh resource record (the heavy CPU/GPU data). Slot-indexed in
  // `meshResources`, paired with a GpuMeshComponent entity (meshSlots owns
  // slot/generation/liveness; DirtyTopology marks (re)upload+BLAS). The old
  // live/quarantined/generation/needsGpuUpload/needsBlasBuild fields moved to
  // meshSlots + the DirtyTopology tag.
  struct MeshResource
  {
    std::vector<hlslpp::float3>  positions;
    std::vector<std::uint32_t>   indices;
    std::vector<hlslpp::float3>  normals;
    std::vector<hlslpp::float4>  tangents;
    std::vector<hlslpp::float2>  uv0;
    std::string                  debugName;

    // §14.5 pooled geometry pages (P2 packing) — the mesh no longer owns per-mesh
    // vertex/index buffers; it records its ranges in the two scene-wide pools
    // (vertexPoolBuffer / indexPoolBuffer below). BLAS builds take buffer+offset.
    // `residentInPool` flips true once UploadPendingMeshes appended the data;
    // DestroyMesh leaks the ranges until Clear() releases the pools wholesale.
    std::uint64_t                vertexPoolByteOffset = 0;
    std::uint32_t                indexPoolElemOffset  = 0;
    bool                         residentInPool       = false;
    std::uint32_t                vertexCount = 0;
    std::uint32_t                indexCount  = 0;
    nvrhi::rt::AccelStructHandle blas;

    // M7 NdotL: per-triangle face normals in object space, computed
    // from positions + indices at CreateMesh time. Used by the
    // closesthit's Lambert pass via the gMeshFaceNormals flat buffer
    // (offset = gMeshFaceOffsets[meshSlot] + PrimitiveIndex()).
    // float4 instead of float3 — std430-style 16-byte alignment +
    // closesthit reads .xyz only; the w slot is reserved for an
    // M9 per-face flag (alpha-test, double-sided override, etc.).
    std::vector<hlslpp::float4>  faceNormals;

    // §15 content-dedup: FNV1a-64 of (positions + indices + optional
    // attributes) at CreateMesh time. Stored on the entry so
    // DestroyMesh can erase the matching map entry without re-hashing
    // the (possibly-cleared) buffers.
    std::uint64_t                descHash       = 0;
  };

  // RFC 0009 P5 — per-instance resource record (slot-indexed in `instanceResources`,
  // paired with a GpuInstanceComponent entity). live/quarantined/generation moved to
  // instanceSlots; DirtyTransform marks a transform change (future TLAS refit).
  struct InstanceResource
  {
    MeshHandle       mesh        = MeshHandle::Invalid;
    MaterialHandle   material    = MaterialHandle::Invalid;
    hlslpp::float4x4 worldFromLocal{};
    bool             visible     = true;
    bool             doubleSided = false;  // V2.A.x — UsdGeomGprim::doubleSided.
    std::string      debugName;
  };

  // RFC 0009 P1 — lights are no longer a std::vector<LightEntry>; they live as
  // GpuLightComponent entities in `sceneWorld` (see members below). LightEntry,
  // the `lights` vector, `freeLightSlots`, LookupLight and ResolveLight are gone.

  // V2.A.5 — UsdVolVolume / OpenVDBAsset slot. Owns the dense float
  // voxel buffer (CPU-side, dropped after upload) + the NVRHI 3D
  // texture (R32_FLOAT). The closesthit doesn't sample the texture
  // in v2 — the entry exists so the volume-integrator follow-up has
  // a stable per-volume container to bind from. Size + transform
  // metadata is kept on the entry so the future integrator pass can
  // build a per-volume cbuffer without re-reading the source .vdb.
  // RFC 0009 P6 — per-volume resource record (slot-indexed in `volumeResources`,
  // paired with a GpuVolumeComponent entity; volumeSlots owns slot/generation).
  // needsGpuUpload stays per-record (volumes are rare; no DirtyVolume tag needed).
  struct VolumeResource
  {
    bool                    needsGpuUpload = false;
    std::array<uint32_t, 3> dimensions{0, 0, 0};
    std::array<float, 3>    bboxMin{0, 0, 0};
    std::array<float, 3>    bboxMax{0, 0, 0};
    std::array<float, 16>   indexToWorld{};
    std::vector<float>      voxelData;     // dropped after upload commits
    nvrhi::TextureHandle    texture;
    std::uint64_t           bytesOnGpu     = 0;  // textureBytes contribution.
    std::string             debugName;
  };

  // M5: material entry. Holds the CPU-side OpenPBRMaterialDesc copy
  // + the slot index inside the material GPU buffer (the bindless
  // table the closesthit reads via instanceCustomIndex). `descHash`
  // is the FNV1a-64 hash of the descriptor body and is used by the
  // dedup map; identical descs collapse to the same MaterialHandle
  // per the §11 dedup rule.
  // RFC 0009 P2 — materials are GpuMaterialComponent entities (see GpuSlotMap
  // materialSlots below), not a std::vector<MaterialEntry>. The struct is gone.

  // M5: texture entry. The TextureKey copy + the NVRHI texture +
  // the bindless slot index used by the closesthit's
  // `baseColorMaps[material.baseColorTex]` lookup. Decode is
  // synchronous at M5 (the §31 async decode pool wires at M8 when
  // texture-load latency starts to dominate); CPU-side decoded
  // pixels are dropped after the GPU upload retires.
  // RFC 0009 P3 — textures are GpuTextureComponent entities (textureSlots below)
  // plus the slot-indexed GPU-resource side tables; the TextureEntry struct is gone.

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

  // RFC 0009 — the Flecs SceneWorld. Entities live here; the per-type handle tables
  // map handle<->entity. Declaration order matters: tables/queries reference
  // sceneWorld, so it must be declared FIRST (destruct last).
  flecs::world                      sceneWorld;
  // P1 — lights (GpuLightComponent). HandleBimap (§8.2) owns slot/generation; the
  // query is built once in the ctor (§30.11, no per-frame build).
  scene::HandleBimap                lightHandles;
  flecs::query<GpuLightComponent>   lightQuery;
  // RFC 0009 follow-up — cached queries over the transient Dirty<T> tags, built once
  // in the ctor (§30.11: never per-frame). Sys_ClearDirty iterates them to remove the
  // tags at the end of each commit, completing the §30.11 "Dirty<T> cleared after each
  // phase" contract (replaces the deleted facade's System_ClearDirtyFlags).
  flecs::query<>                    dirtyTransformQuery;
  flecs::query<>                    dirtyVisibilityQuery;
  flecs::query<>                    dirtyMaterialQuery;
  flecs::query<>                    dirtyLightQuery;
  // RFC 0009 follow-up — a removal (RemoveLight / DestroyInstance) re-packs the whole
  // light buffer / TLAS, but the removed entity is gone so it can't carry a Dirty tag.
  // The verb tags this persistent sentinel instead, so the `count<Dirty*>() > 0` upload
  // gate still fires; Sys_ClearDirty clears it like any other tagged entity.
  flecs::entity                     removalSentinel;
  // RFC 0009 follow-up — CommitResources runs the §30.11 phase pipeline for real via
  // sceneWorld.progress(). The per-commit command list + first-error live here (the
  // registered systems are ctor lambdas that capture `this`, so no FrameContext
  // singleton component is needed). `commitPipelineRegistered` guards one-time setup.
  nvrhi::ICommandList*              currentCommandList = nullptr;
  Expected<void>                    commitError{};
  bool                              commitPipelineRegistered = false;
  // Review fix #3 — lowest DirtyTopology mesh slot, computed ONCE per commit so the
  // mesh side-table uploaders don't each rescan the whole mesh table.
  uint32_t                          commitLowestDirtyMeshSlot = 0;
  // P2 — materials (GpuMaterialComponent). GpuSlotMap keeps slot == GPU buffer index
  // (gpuscene_detail encoding) so the packed material buffer + instance side-table
  // are byte-identical. `materialSourcePrims` is the slot-indexed owned-string table
  // backing each desc's sourcePrim view (variable-length data → a GpuScene table).
  GpuSlotMap                        materialSlots{sceneWorld};
  std::vector<std::string>          materialSourcePrims;
  // P3 — textures (GpuTextureComponent). Slot-indexed side tables hold the non-POD
  // GPU resource + transient decode state (slot == bindless slot for M5). Distinct
  // slots are written from distinct OpenMP threads during decode (thread-safe);
  // component metadata is written serially.
  GpuSlotMap                        textureSlots{sceneWorld};
  std::vector<nvrhi::TextureHandle> textureResources;     // the GPU texture per slot.
  std::vector<std::vector<std::uint8_t>> texturePixelData;  // transient; dropped post-upload.
  std::vector<std::string>          textureResolvedPaths;  // owned backing for keyCopy.resolvedPath.
  // Review fix #4 — LRU access tick per texture slot. Mutable hot metadata lives in a
  // side table (not GpuTextureComponent) so a cache hit bumps it with a plain write
  // instead of a get<>()+set<>() round-trip that also re-persisted a fragile view.
  std::vector<std::uint64_t>        textureLastAccessTick;
  // RFC 0003 — slot-indexed ITexture* scratch behind SceneResources::bindlessTextures.
  // Refreshed by SceneResourcesAccess::Get each call (nullptr for sentinel / dead /
  // not-yet-decoded slots — the old GetBindlessTextureAt semantics); reuses capacity,
  // so steady-state frames are allocation-free.
  std::vector<nvrhi::ITexture*>     bindlessTextureScratch;
  // P4 — meshes (GpuMeshComponent). meshResources is the slot-indexed heavy CPU/GPU
  // data; meshSlots owns slot/generation/liveness; DirtyTopology marks (re)upload+BLAS.
  GpuSlotMap                        meshSlots{sceneWorld};
  std::vector<MeshResource>         meshResources;
  // P5 — instances (GpuInstanceComponent). instanceResources is the slot-indexed
  // data; instanceSlots owns slot/generation/liveness (slot == instanceCustomIndex);
  // DirtyTransform marks a transform change.
  GpuSlotMap                        instanceSlots{sceneWorld};
  std::vector<InstanceResource>     instanceResources;
  // P6 — volumes (GpuVolumeComponent). volumeResources is the slot-indexed data;
  // volumeSlots owns slot/generation/liveness.
  GpuSlotMap                        volumeSlots{sceneWorld};
  std::vector<VolumeResource>       volumeResources;

  // RFC 0009 — all per-type free-slot recycling now lives inside each GpuSlotMap
  // (gpuscene_detail encoding, LIFO reuse), so the old free-slot vectors are gone.
  // V2.A.5 — set when a fresh AddVolume needs CommitResources to
  // create the matching nvrhi::TextureHandle + write the dense
  // float buffer into it via writeTexture(). Cleared after
  // UploadPendingVolumes drains the queue.
  bool                       volumesNeedGpuUpload = false;

  // M5 dedup maps: hash → handle. AcquireMaterial / AcquireTexture
  // hash their input desc / key, look up here, and return the
  // existing handle on a hit. The §11 OpenPBR architecture rule
  // ("hashed via XXH3_64bits, deduplicated") relies on these maps
  // collapsing identical materials in a World Lobby-scale scene where
  // the same UsdShadeMaterial is bound to thousands of meshes.
  // M5 stub uses FNV1a-64 (10-line inline impl below); XXH3
  // upgrade is on the M8 perf-sweep checklist.
  std::unordered_map<std::uint64_t, MaterialHandle> materialDescHashToHandle;
  std::unordered_map<std::uint64_t, TextureHandle>  textureKeyHashToHandle;

  // V2.A.12 — LRU tick counter. Bumped on every AcquireTexture (hit
  // or miss); written onto the entry's `lastAccessTick`. Wraps at
  // 64-bit (i.e. effectively never). Eviction policy reads the
  // delta against this counter when the future `EvictColdTextures`
  // pass lands.
  std::uint64_t nextTextureAccessTick = 0;
  std::uint64_t lruHitCount  = 0;  // texture acquires that returned an existing handle
  std::uint64_t lruMissCount = 0;  // acquires that allocated a fresh slot
  // §15 content-dedup map: hash → MeshHandle. CreateMesh hashes the
  // input MeshDesc's geometry, looks up here, and returns the
  // existing handle on a hit so identical mesh content authored under
  // different SdfPaths shares one MeshHandle → one BLAS. Required for
  // PointInstancer-with-shared-prototype + identical-content scenes
  // (default.usd's three spheres) to actually share BLAS the way
  // §15 promises.
  std::unordered_map<std::uint64_t, MeshHandle>     meshDescHashToHandle;

  // M5 GPU pools.
  // - materialGpuBuffer: structured buffer of OpenPBRMaterialGPU,
  //   indexed by material slot. Re-uploaded on every commit if any
  //   material has needsGpuUpload — small enough (M5 stub: <1 MiB)
  //   that we don't bother with partial updates.
  // - instanceInfoBuffer (M6 / P2 packing): structured buffer of
  //   InstanceInfoGpu indexed by instance slot — one packed record
  //   replaces the old parallel instance→material + instance→mesh
  //   uint tables. Row 3 carries the material slot (the closesthit
  //   reads `materials[gInstanceInfo[InstanceID()].materialSlot]` —
  //   the §15 indirection that frees instanceCustomIndex for the
  //   INSTANCE slot) + the M7-NdotL mesh slot; rows 0-2 carry the
  //   instance object→world 3×4 (populated for the P4 split; no
  //   shader reads them yet). `instanceInfo` is the CPU mirror,
  //   sized to instanceSlots.SlotCount(): PackInstanceInfo fills the
  //   slot halves (gated on instanceMaterialNeedsUpload), the TLAS
  //   instance walk fills the transforms, and the buffer re-uploads
  //   at the end of RebuildTlasIfDirty when EITHER fired.
  // - bindlessSampler: a single shared linear-clamp sampler. Per-
  //   role samplers (sRGB filtering, anisotropic for tangent maps,
  //   etc.) are an M9 polish item.
  nvrhi::BufferHandle  materialGpuBuffer;  // re-packed when any DirtyMaterial tag is present.
  nvrhi::BufferHandle  instanceInfoBuffer;
  std::vector<shaderinterop::InstanceInfoGpu> instanceInfo;  // CPU mirror, slot-indexed.
  bool                 instanceInfoNeedsUpload = false;  // set by PackInstanceInfo.
  nvrhi::SamplerHandle bindlessSampler;
  // M9-fidelity per-role samplers. `bindlessSampler` (above) is
  // Wrap-Wrap-Wrap for tiling material textures; `domeSampler` is
  // Wrap-Clamp-Wrap for the HDRI dome's lat-long mapping (V-axis
  // clamp prevents the elevation seam at the poles from mirroring
  // +Y onto -Y).
  nvrhi::SamplerHandle domeSampler;

  // M7: structured buffer of LightGpu entries the closesthit reads
  // via the simple per-light contribution loop at binding 5. Sized
  // to the count of LIVE LightEntry slots (sparse storage with
  // holes is fine — the closesthit iterates the buffer's full
  // length, but we only emit live lights, so dead slots never
  // appear in the upload). Re-packed when any DirtyLight tag is present
  // (AddLight / UpdateLight tag the light; RemoveLight tags removalSentinel).
  nvrhi::BufferHandle  lightsGpuBuffer;

  // M7 NdotL: per-mesh face normals concatenated into one flat
  // buffer. Closesthit reads:
  //   nLocal = gMeshFaceNormals[gMeshInfo[meshSlot].faceOffset
  //                            + PrimitiveIndex()].xyz
  // Then transforms via Vulkan's `ObjectToWorld3x4()` to get
  // world-space N for the Lambert pass. Uploaded alongside BLAS
  // builds (computed in CreateMesh; flushed at CommitResources when a
  // DirtyTopology mesh exists — LowestDirtyMeshSlot gates every side table).
  nvrhi::BufferHandle  meshFaceNormalsBuffer;
  // RFC 0009 follow-up — # mesh slots already concatenated into the buffer above.
  // A new upload appends only slots >= this (the tail fast path) when no lower slot
  // changed; see UploadMeshSideTable. One tracker per concatenated buffer (they share
  // a lifecycle but each owns its packed extent, so there is no cross-buffer hazard).
  uint32_t             meshFaceNormalsPackedSlots = 0;

  // M8a UV pipeline: per-mesh UVs concatenated into one flat
  // structured buffer. Closesthit reads (offsets from gMeshInfo):
  //   v0,v1,v2    = gMeshIndices[mi.indexOffset + PrimitiveIndex()*3 + {0,1,2}]
  //   uv0,uv1,uv2 = gMeshUvs[mi.uvOffset + v_i]
  //   uv          = barycentric_interp(uv0, uv1, uv2, attribs.bary)
  // Then samples bindless gBindlessTextures[mat.baseColorTex] at uv.
  // Sized + uploaded by UploadMeshUvs in Commit.cpp; gated by
  // DirtyTopology like meshFaceNormals. (gMeshIndices is the §14.5
  // index pool below — the old flat duplicate is gone.)
  nvrhi::BufferHandle  meshUvsBuffer;
  uint32_t             meshUvsPackedSlots = 0;  // see meshFaceNormalsPackedSlots.

  // P2 packing — interleaved per-vertex shading attributes
  // (VertexAttribGpu = {normal, tangent}); merges the old parallel
  // per-vertex normal + tangent float4 streams. Exactly vertexCount
  // entries per mesh; zero-magnitude xyz stays the independent
  // "not authored" sentinel per field (a mesh can have normals but
  // no tangents). Closesthit reads
  //   gMeshVertexAttribs[gMeshInfo[meshSlot].vertexAttribOffset + v_i].
  nvrhi::BufferHandle  meshVertexAttribsBuffer;
  uint32_t             meshVertexAttribsPackedSlots = 0;  // see meshFaceNormalsPackedSlots.

  // P2 packing — one MeshInfoGpu per mesh slot (binding 6) replaces the
  // five parallel per-mesh uint offset tables. Fully rewritten whenever
  // a DirtyTopology mesh exists (offset tables always were); built by
  // UploadMeshInfo in ONE walk computing every running sum.
  nvrhi::BufferHandle  meshInfoBuffer;

  // §14.5 pooled geometry pages (P2 packing) — two grow-only pools
  // replace the per-mesh vertex/index buffers (2N buffers → 2). BLAS
  // builds reference buffer+offset; growth (2×, copy live range, swap
  // handle) happens in UploadPendingMeshes, strictly before
  // BuildPendingBlas records builds against the pool. The index pool
  // doubles as the gMeshIndices structured view (binding 26), deleting
  // the old flat duplicate + its upload path. DestroyMesh leaks its
  // ranges until Clear() releases the pools wholesale.
  nvrhi::BufferHandle  vertexPoolBuffer;
  nvrhi::BufferHandle  indexPoolBuffer;
  std::uint64_t        vertexPoolUsedBytes    = 0;
  std::uint64_t        indexPoolUsedElements  = 0;

  // Magenta 4x4 fallback texture — slot 0 in the bindless table is
  // permanently the "missing texture" colour so any material whose
  // resolved path failed to decode renders visibly-broken instead
  // of black-or-undefined-memory. The texture is created on first
  // CommitResources and reused for the lifetime of the scene.
  nvrhi::TextureHandle missingTexture;

  bool       hasCamera = false;
  CameraDesc cameraDesc{};

  // Top-level acceleration structure. Allocated lazily on the first
  // TLAS rebuild so an empty scene doesn't pay for it.
  nvrhi::rt::AccelStructHandle tlas;
  // RFC 0009 — TLAS dirty/refit (§16 "rebuilt every frame if dirty; refit otherwise"),
  // driven by the instance Dirty<T> tags: DirtyVisibility (Append/Destroy/SetVisibility
  // — the instance SET or its BLAS pointers change) forces a full rebuild; a
  // transform-only edit (UpdateInstanceTransform → DirtyTransform, no DirtyVisibility)
  // is eligible for a cheap refit. `tlasAllowsUpdate` tracks whether the live TLAS was
  // built with the AllowUpdate flag. It stays false for static scenes (which only ever
  // tag DirtyVisibility via appends), so their TLAS build is byte-identical to before;
  // the AllowUpdate variant is only created once a transform edit needs refit capability.
  bool                         tlasAllowsUpdate     = false;
  // Review fix #1 — instance count of the last TLAS build. A refit (PerformUpdate)
  // is only valid against an identical instance count/topology, so a transform-only
  // tick whose gathered count differs (e.g. DestroyMesh dropped a still-referenced
  // mesh's BLAS, which does NOT tag DirtyVisibility) must full-rebuild, not refit.
  uint32_t                     tlasBuiltInstanceCount = 0;
  // Review fix #2 — cached single-dome env-map texture, refreshed once per commit
  // (RefreshDomeEnvMapCache). SceneResources::domeEnvMapTexture is read every frame
  // by the RaytracedLightingPass binding path; recomputing there allocated + sorted the
  // live-light set per frame.
  nvrhi::ITexture*             domeEnvMapTexture = nullptr;

  // M6 audit closeout: the instance→material/mesh side-table buffer (binding 4) has
  // FOUR heterogeneous triggers — AppendInstance, DestroyInstance, SetInstanceVisibility
  // (all also tag DirtyVisibility → TLAS rebuild) and UpdateInstanceMaterial (side-table
  // ONLY — must NOT rebuild the TLAS). Because that last trigger maps to no TLAS dirty
  // tag, the side-table keeps its own coarse bool rather than folding into DirtyVisibility
  // (which would force a needless TLAS rebuild on a material-binding change).
  bool                         instanceMaterialNeedsUpload = false;

  // ---- Handle resolution -----------------------------------------------
  //
  // Two layers per entry type:
  //
  //   LookupX  — const, no side effects. Returns nullptr for any
  //              invalid case (Invalid sentinel, slot out of range,
  //              recycled-generation, dead). Used by Has* predicates +
  //              read-only paths where the §18.5 stale-handle counter
  //              must NOT bump.
  //
  //   ResolveX — mutating, counter-aware. Same nullptr cases as Lookup
  //              but bumps `staleHandleDrops` whenever a non-Invalid
  //              handle failed to resolve. Used by void-returning
  //              Update* / Destroy* verbs per the §18.5 contract:
  //              Invalid silently no-ops (no counter bump); recycled /
  //              out-of-range bumps the counter and returns nullptr.
  //
  // Resolve composes from Lookup so the underlying validation logic
  // stays in one place.

  template <typename Entry>
  [[nodiscard]] static const Entry* LookupEntryImpl(uint32_t handleValue,
                                                    const std::vector<Entry>& entries) noexcept
  {
    if (handleValue == 0)
      return nullptr;
    const uint32_t slot = gpuscene_detail::HandleSlot(handleValue);
    if (slot == 0 || slot >= entries.size())
      return nullptr;
    const Entry& entry = entries[slot];
    if (!entry.live || entry.quarantined
        || entry.generation != gpuscene_detail::HandleGeneration(handleValue))
      return nullptr;
    return &entry;
  }

  // Promote a non-Invalid lookup miss into a stale-handle counter
  // bump per §18.5. Returns the same pointer for transparent chaining.
  template <typename Entry, typename Handle>
  [[nodiscard]] Entry* BumpIfStaleAndReturn(Handle handle, const Entry* entry) noexcept
  {
    if (entry == nullptr && static_cast<uint32_t>(handle) != 0)
      ++lastFrameStats.staleHandleDrops;
    return const_cast<Entry*>(entry);
  }

  // RFC 0009 P6 — resolve a VolumeHandle to its slot-indexed VolumeResource.
  [[nodiscard]] VolumeResource* ResolveVolumeResource(VolumeHandle handle) noexcept
  {
    const flecs::entity entity = volumeSlots.Resolve(static_cast<uint32_t>(handle));
    if (entity.id() == 0)
      return nullptr;
    const uint32_t slot = GpuSlotMap::SlotOf(static_cast<uint32_t>(handle));
    return slot < volumeResources.size() ? &volumeResources[slot] : nullptr;
  }

  // RFC 0009 P5 — resolve an InstanceHandle to its slot-indexed InstanceResource (or
  // null for Invalid/stale). Does NOT bump staleHandleDrops; callers that need the
  // §18.5 count do it explicitly (mirrors the per-verb pattern of the other types).
  [[nodiscard]] InstanceResource* ResolveInstanceResource(InstanceHandle handle) noexcept
  {
    const flecs::entity entity = instanceSlots.Resolve(static_cast<uint32_t>(handle));
    if (entity.id() == 0)
      return nullptr;
    const uint32_t slot = GpuSlotMap::SlotOf(static_cast<uint32_t>(handle));
    return slot < instanceResources.size() ? &instanceResources[slot] : nullptr;
  }

  // RFC 0009 P4 — resolve a MeshHandle to its slot-indexed MeshResource (or null for
  // Invalid/stale handles). Mirrors the old ResolveMesh contract via meshSlots.
  [[nodiscard]] MeshResource* ResolveMeshResource(MeshHandle handle) noexcept
  {
    const flecs::entity entity = meshSlots.Resolve(static_cast<uint32_t>(handle));
    if (entity.id() == 0)
      return nullptr;
    const uint32_t slot = GpuSlotMap::SlotOf(static_cast<uint32_t>(handle));
    return slot < meshResources.size() ? &meshResources[slot] : nullptr;
  }

  // RFC 0009 P1 — collect live lights as (encoded handle, desc) sorted by slot
  // index, the deterministic order the GPU light buffer + editor accessors need
  // (replaces iterating the old `lights` vector in slot order). ~30 lights, so the
  // collect+sort is negligible; called per-commit (packing) + per-frame (editor).
  void CollectLiveLightsSorted(std::vector<std::pair<uint32_t, LightDesc>>& out);

  // Resolve a TextureHandle to its bindless slot index, or to
  // INVALID_BINDLESS_TEXTURE for Invalid / out-of-range / dead handles. Used when
  // packing OpenPBRMaterialGPU entries. RFC 0009 P3 — reads the texture entity's
  // component (bindlessSlot reflects decode success: 0 = decode failed → fallback).
  [[nodiscard]] std::uint32_t ResolveTextureBindlessSlot(TextureHandle handle) const noexcept
  {
    const flecs::entity entity = textureSlots.Resolve(static_cast<uint32_t>(handle));
    if (entity.id() == 0)
      return shaderinterop::INVALID_BINDLESS_TEXTURE;
    // Review fix #8 — try_get (not get) so a live entity that somehow lacks the
    // component returns the fallback instead of dereferencing null: flecs get<T>()'s
    // presence assert is compiled out under NDEBUG (release).
    const GpuTextureComponent* component = entity.try_get<GpuTextureComponent>();
    return component ? component->bindlessSlot : shaderinterop::INVALID_BINDLESS_TEXTURE;
  }

  // ---- Verb member functions (defined in per-verb .cpp files) ----------
  // Each public `GpuScene::Verb()` in GpuScene.cpp forwards one line
  // to the matching `Impl::Verb()` here. The split keeps fields +
  // helpers visible without rewriting every `_impl->X` to `impl.X`.

  // Mesh.cpp
  Expected<MeshHandle> CreateMesh(const MeshDesc& meshDesc);
  Expected<void>       UpdateMesh(MeshHandle meshHandle, const MeshDesc& meshDesc);
  void                 DestroyMesh(MeshHandle meshHandle);
  [[nodiscard]] bool   HasMesh(MeshHandle meshHandle) const;

  // Material.cpp
  MaterialHandle       AcquireMaterial(const OpenPBRMaterialDesc& materialDesc);
  void                 UpdateMaterial(MaterialHandle materialHandle,
                                      const OpenPBRMaterialDesc& materialDesc);
  void                 DestroyMaterial(MaterialHandle materialHandle);
  [[nodiscard]] bool   HasMaterial(MaterialHandle materialHandle) const;

  // Texture.cpp
  TextureHandle        AcquireTexture(const TextureKey& textureKey);
  void                 DestroyTexture(TextureHandle textureHandle);
  [[nodiscard]] bool   HasTexture(TextureHandle textureHandle) const;
  // V2.A.12 — LRU eviction. Drops textures by ascending lastAccessTick
  // until total decoded byte count is below `targetBytes`. Returns the
  // count evicted. Safe to call between frames.
  std::uint32_t        EvictColdTextures(std::uint64_t targetBytes) noexcept;

  // Instance.cpp
  Expected<InstanceHandle> AppendInstance(const InstanceDesc& instanceDesc);
  void                     UpdateInstanceTransform(InstanceHandle instanceHandle,
                                                   const hlslpp::float4x4& worldFromLocal);
  void                     UpdateInstanceMaterial(InstanceHandle instanceHandle,
                                                  MaterialHandle materialHandle);
  void                     SetInstanceVisibility(InstanceHandle instanceHandle, bool visible);
  void                     DestroyInstance(InstanceHandle instanceHandle);
  [[nodiscard]] bool       HasInstance(InstanceHandle instanceHandle) const;

  // Light.cpp (camera + lights)
  void                 SetCamera(const CameraDesc& cameraDesc);
  LightHandle          AddLight(const LightDesc& lightDesc);
  void                 UpdateLight(LightHandle lightHandle, const LightDesc& lightDesc);
  void                 RemoveLight(LightHandle lightHandle);

  // Volume.cpp (V2.A.5)
  VolumeHandle         AddVolume(const VolumeDesc& volumeDesc);
  void                 RemoveVolume(VolumeHandle volumeHandle);
  [[nodiscard]] bool   HasVolume(VolumeHandle volumeHandle) const;

  // Commit.cpp (Clear + CommitResources — both touch every table).
  // CommitResources is an orchestrator over the per-resource-type
  // member functions below; each one services one upload/build phase
  // and propagates GPU-creation failures up through PYXIS_TRY.
  void                 Clear() noexcept;
  [[nodiscard]] Expected<void> CommitResources(nvrhi::ICommandList* commandList);
  // RFC 0009 follow-up — register the §30.11 phases + the commit systems on
  // sceneWorld (once). CommitResources then drives them via sceneWorld.progress().
  void                 RegisterCommitPipeline() noexcept;

  [[nodiscard]] Expected<void> UploadPendingMeshes(nvrhi::ICommandList* commandList);
  [[nodiscard]] Expected<void> EnsureBindlessFallbacks(nvrhi::ICommandList* commandList);
  [[nodiscard]] Expected<void> UploadPendingTextures(nvrhi::ICommandList* commandList);
  [[nodiscard]] Expected<void> UploadMaterialBuffer(nvrhi::ICommandList* commandList);
  [[nodiscard]] Expected<void> UploadLightBuffer(nvrhi::ICommandList* commandList);
  [[nodiscard]] Expected<void> BuildPendingBlas(nvrhi::ICommandList* commandList);
  [[nodiscard]] Expected<void> RebuildTlasIfDirty(nvrhi::ICommandList* commandList);
  // P2 packing — fills the materialSlot/meshSlot halves of the instanceInfo mirror
  // (replaces the old UploadInstanceSideTables; same walk, same dirty flag). The GPU
  // upload happens at the end of RebuildTlasIfDirty once the transform rows are
  // current.
  [[nodiscard]] Expected<void> PackInstanceInfo(nvrhi::ICommandList* commandList);
  // P2 packing — flushes the instanceInfo mirror to the GPU when either trigger
  // fired (slot-half repack OR TLAS rebuild/refit). Called from RebuildTlasIfDirty's
  // tail only (the transform rows must be current).
  [[nodiscard]] Expected<void> UploadInstanceInfoIfNeeded(nvrhi::ICommandList* commandList);
  [[nodiscard]] Expected<void> UploadMeshFaceNormals(nvrhi::ICommandList* commandList);
  [[nodiscard]] Expected<void> UploadMeshUvs(nvrhi::ICommandList* commandList);
  // P2 packing — interleaved per-vertex normal+tangent stream (replaces the old
  // UploadMeshVertexNormals + UploadMeshTangents pair).
  [[nodiscard]] Expected<void> UploadMeshVertexAttribs(nvrhi::ICommandList* commandList);
  // P2 packing — the packed MeshInfoGpu table (replaces the five offset tables).
  // Called from the tail of UploadPendingMeshes (NOT a separate system) because it
  // reads the index-pool offsets assigned there — a direct call keeps the dependency
  // off intra-phase system registration order (mirrors review fix #6).
  [[nodiscard]] Expected<void> UploadMeshInfo(nvrhi::ICommandList* commandList);
  [[nodiscard]] Expected<void> UploadPendingVolumes(nvrhi::ICommandList* commandList);

  // RFC 0009 follow-up — PhaseClearDirty system: removes the transient Dirty<T> tags
  // consumed this commit (DirtyTransform, DirtyVisibility) so they don't accumulate.
  // Per-entity tags consumed mid-pipeline (DirtyTopology, DirtyTexture, DirtyMaterial,
  // DirtyLight) are cleared by their own uploader/builder. Takes the command-list arg
  // to fit the commit-system signature; ignores it (no GPU work).
  [[nodiscard]] Expected<void> ClearDirtyFlags(nvrhi::ICommandList* commandList);

  // RFC 0009 follow-up — lowest live mesh slot carrying scene::DirtyTopology, or
  // meshSlots.SlotCount() when none are dirty. Drives the incremental side-table
  // append fast path: when this is >= the buffer's packedSlots, every dirty mesh is
  // a NEW tail slot and only that tail needs re-uploading (see UploadMeshSideTable).
  [[nodiscard]] uint32_t LowestDirtyMeshSlot() const noexcept;

  // Review fix #2 — recompute the cached single-dome env-map texture (the first live
  // Dome light with a resolved env-map, slot order). Called once at the end of each
  // CommitResources; SceneResources then carries the cached pointer.
  void RefreshDomeEnvMapCache() noexcept;
};

}  // namespace pyxis
