// Pyxis renderer — GpuScene per-frame commit + scene-wide reset.
//
// Per-verb split off GpuScene.cpp; declarations live on
// `GpuScene::Impl` in Internal.h. Both verbs touch every entry
// table, so they cohabit one file rather than splitting Clear off.
//
// Commit.cpp is the only verb file that pulls in stb_image +
// tinyexr — pixel decode happens lazily inside CommitResources, not
// at AcquireTexture time, so Texture.cpp stays decode-free.
//
// CommitResources is an orchestrator: it zeros per-frame counters,
// validates inputs, and then forwards through the per-resource-type
// member functions below in order. Each one services exactly one
// upload / build phase; PYXIS_TRY propagates GPU-creation failures
// up the chain unchanged.

#include "GpuScene/Internal.h"

#include "GpuScene/BcEncoder.h"
#include "GpuScene/DdsParser.h"
#include "GpuScene/MeshUvPack.h"
#include "Materials/MaterialFlag.h"

#include <stb_image.h>
#include <tinyexr.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <utility>
#include <vector>

namespace pyxis {

using namespace gpuscene_detail;

namespace {

// V2.B.* — gamma-aware 2× box-downsample of an RGBA8 image, used to
// build a CPU mip chain for uncompressed material textures. Without
// mips, minified tiled textures (e.g. the World Lobby's tiling wood
// walls) point-sample mip 0 and alias into herringbone/chevron moiré
// — the ray-cone LOD helper in closesthit selects a mip level that
// doesn't exist and clamps to 0. sRGB-encoded roles (BaseColor /
// Emission) are averaged in LINEAR space then re-encoded; linear
// roles average bytes directly. Alpha always averages linearly.
// Odd dimensions floor-halve (drop the trailing texel) — standard for
// a box mip chain.
[[nodiscard]] std::vector<std::uint8_t> DownsampleRgba8Half(
    const std::uint8_t* src, std::uint32_t srcW, std::uint32_t srcH, bool isSrgb,
    std::uint32_t& outW, std::uint32_t& outH) noexcept
{
  outW = std::max(1u, srcW / 2u);
  outH = std::max(1u, srcH / 2u);
  std::vector<std::uint8_t> dst(static_cast<std::size_t>(outW) * outH * 4u);

  // sRGB↔linear via precomputed lookup tables instead of std::pow per
  // channel per pixel. The pow-based version dominated first-frame load
  // (~57 s on the World Lobby's 8K textures: ~15 pow calls per output
  // texel). byte→linear is an exact 256-entry table; linear→byte uses a
  // 4097-entry table indexed by the averaged linear value (≤ 1/4096
  // quantisation — invisible in a mip). Both built once (thread-safe
  // function-local statics), so parallel callers share them.
  static const std::array<float, 256> SRGB_TO_LINEAR = [] {
    std::array<float, 256> table{};
    for (int idx = 0; idx < 256; ++idx)
    {
      const float chan = static_cast<float>(idx) / 255.0f;
      table[static_cast<std::size_t>(idx)] =
          (chan <= 0.04045f) ? chan / 12.92f
                             : std::pow((chan + 0.055f) / 1.055f, 2.4f);
    }
    return table;
  }();
  static const std::array<std::uint8_t, 4097> LINEAR_TO_SRGB_BYTE = [] {
    std::array<std::uint8_t, 4097> table{};
    for (int idx = 0; idx <= 4096; ++idx)
    {
      const float lin = static_cast<float>(idx) / 4096.0f;
      const float srgb = (lin <= 0.0031308f)
                             ? lin * 12.92f
                             : 1.055f * std::pow(lin, 1.0f / 2.4f) - 0.055f;
      table[static_cast<std::size_t>(idx)] =
          static_cast<std::uint8_t>(std::lround(std::clamp(srgb, 0.0f, 1.0f) * 255.0f));
    }
    return table;
  }();

  for (std::uint32_t dstY = 0; dstY < outH; ++dstY)
  {
    const std::uint32_t srcY0 = dstY * 2u;
    const std::uint32_t srcY1 = std::min(srcY0 + 1u, srcH - 1u);
    for (std::uint32_t dstX = 0; dstX < outW; ++dstX)
    {
      const std::uint32_t srcX0 = dstX * 2u;
      const std::uint32_t srcX1 = std::min(srcX0 + 1u, srcW - 1u);
      const std::size_t o00 = (static_cast<std::size_t>(srcY0) * srcW + srcX0) * 4u;
      const std::size_t o10 = (static_cast<std::size_t>(srcY0) * srcW + srcX1) * 4u;
      const std::size_t o01 = (static_cast<std::size_t>(srcY1) * srcW + srcX0) * 4u;
      const std::size_t o11 = (static_cast<std::size_t>(srcY1) * srcW + srcX1) * 4u;
      std::uint8_t* outPix =
          dst.data() + (static_cast<std::size_t>(dstY) * outW + dstX) * 4u;
      for (std::size_t chIdx = 0; chIdx < 4u; ++chIdx)
      {
        if (isSrgb && chIdx < 3u)  // gamma-correct average; alpha stays linear
        {
          const float averaged = 0.25f
              * (SRGB_TO_LINEAR[src[o00 + chIdx]] + SRGB_TO_LINEAR[src[o10 + chIdx]]
                 + SRGB_TO_LINEAR[src[o01 + chIdx]] + SRGB_TO_LINEAR[src[o11 + chIdx]]);
          outPix[chIdx] = LINEAR_TO_SRGB_BYTE[static_cast<std::size_t>(
              std::lround(std::clamp(averaged, 0.0f, 1.0f) * 4096.0f))];
        }
        else
        {
          const std::uint32_t sum = static_cast<std::uint32_t>(src[o00 + chIdx])
                                  + src[o10 + chIdx] + src[o01 + chIdx] + src[o11 + chIdx];
          outPix[chIdx] = static_cast<std::uint8_t>((sum + 2u) / 4u);
        }
      }
    }
  }
  return dst;
}

}  // namespace

// ============================================================================
// Clear
// ============================================================================

void GpuScene::Impl::Clear() noexcept
{
  // Reset to the post-construction shape. Every NVRHI handle owned by
  // the impl is reference-counted (nvrhi::RefCountPtr); dropping the
  // handles releases the ref and NVRHI's deferred-destruction queue
  // reclaims the underlying Vulkan objects on the next garbage
  // collection tick. Caller must have waited the device idle so no
  // in-flight command buffer still references these.

  // Mesh / instance / light / material / texture tables back to "slot
  // 0 sentinel only", matching ctor.
  // RFC 0009 P4 — meshes are Flecs entities; Reset destructs them + clears the
  // slot-indexed resource records.
  meshSlots.Reset();
  meshResources.clear();

  // RFC 0009 P5 — instances are Flecs entities; Reset destructs them + clears the
  // slot-indexed resource records.
  instanceSlots.Reset();
  instanceResources.clear();

  // RFC 0009 P1 — lights are Flecs entities; delete them all + reset the slot map
  // (no slot-0 sentinel: HandleBimap encodes slot+1 so handle 0 stays Invalid).
  sceneWorld.delete_with<GpuLightComponent>();
  lightHandles = scene::HandleBimap{};

  // RFC 0009 P2 — materials are Flecs entities; Reset destructs them + restores the
  // slot-0-only sentinel state, and clears the slot-indexed sourcePrim table.
  materialSlots.Reset();
  materialSourcePrims.clear();

  // RFC 0009 P3 — textures are Flecs entities; Reset destructs them + clears the
  // slot-indexed GPU-resource side tables.
  textureSlots.Reset();
  textureResources.clear();
  texturePixelData.clear();
  textureResolvedPaths.clear();

  // RFC 0009 P6 — volumes are Flecs entities; Reset destructs them + clears records.
  volumeSlots.Reset();
  volumeResources.clear();

  materialDescHashToHandle.clear();
  textureKeyHashToHandle.clear();
  meshDescHashToHandle.clear();

  volumesNeedGpuUpload = false;

  // GPU buffers: drop refs. CommitResources will lazily re-allocate
  // on the first AcquireMaterial / AppendInstance / AddLight after
  // Clear (the same lazy path used at scene-construction time).
  materialGpuBuffer = nullptr;
  materialsNeedGpuUpload = false;
  instanceMaterialBuffer = nullptr;
  lightsGpuBuffer = nullptr;
  lightsNeedGpuUpload = false;
  meshFaceNormalsBuffer = nullptr;
  meshFaceOffsetsBuffer = nullptr;
  meshFaceNormalsNeedUpload = false;
  meshUvsBuffer = nullptr;
  meshUvOffsetsBuffer = nullptr;
  meshIndicesBuffer = nullptr;
  meshIndexOffsetsBuffer = nullptr;
  meshUvsNeedUpload = false;
  meshIndicesNeedUpload = false;
  meshVertexNormalsBuffer = nullptr;
  meshVertexNormalOffsetsBuffer = nullptr;
  meshVertexNormalsNeedUpload = false;
  meshTangentsBuffer = nullptr;
  meshTangentOffsetsBuffer = nullptr;
  meshTangentsNeedUpload = false;
  instanceMeshBuffer = nullptr;

  // Sampler + missingTexture are scene-lifetime singletons that the
  // first CommitResources after Clear will re-create on demand. Drop
  // the refs so memory isn't held longer than needed across a reload.
  bindlessSampler = nullptr;
  domeSampler = nullptr;
  missingTexture = nullptr;

  // TLAS + camera + dirty flags.
  tlas = nullptr;
  tlasNeedsRebuild = false;
  tlasStructureChanged = true;   // RFC 0009 — fresh scene → next build is a full rebuild.
  tlasAllowsUpdate = false;
  instanceMaterialNeedsUpload = false;
  hasCamera = false;
  cameraDesc = CameraDesc{};

  // Per-frame stat counters back to zero. Cumulative counters
  // (meshCount / instanceCount / etc.) recompute on read from the
  // live tables so they don't need explicit reset here, but the
  // FrameStats POD itself is replaced wholesale to clear bytes /
  // pendingUploads / degraded / staleHandleDrops in one shot.
  lastFrameStats = FrameStats{};
}

// ============================================================================
// CommitResources orchestrator
// ============================================================================

Expected<void> GpuScene::Impl::CommitResources(nvrhi::ICommandList* commandList)
{
  // Zero the per-frame counters at the start of every commit so the
  // value `LastFrameStats()` reports between this commit and the
  // next is exactly what happened during the current in-progress
  // frame. That honours the §18.5 / FrameStats.h "this frame"
  // contract on `staleHandleDrops` / `pendingUploads` /
  // `pendingBlasBuilds`. Cumulative counters (`meshCount`,
  // `instanceCount`, ...) are recomputed on read from the live
  // tables so they don't need to be reset here.
  lastFrameStats.staleHandleDrops = 0;
  lastFrameStats.pendingUploads = 0;
  lastFrameStats.pendingBlasBuilds = 0;

  if (commandList == nullptr)
  {
    return std::unexpected{
        PYXIS_ERROR(ErrorKind::InvalidArgument, "GpuScene::CommitResources: commandList is null")};
  }
  if (device == nullptr)
  {
    return std::unexpected{PYXIS_ERROR(ErrorKind::InvalidState,
                                       "GpuScene::CommitResources: scene has no device "
                                       "(constructed in CPU-only test mode)")};
  }

  const Profiler::CpuScope commitScope(*profiler, "render.commitResources");

  // RFC 0009 follow-up — the §30.11 phase pipeline now executes the commit for real:
  // the systems registered in RegisterCommitPipeline run via sceneWorld.progress() in
  // phase order (UploadTextures → UploadMaterials → ExtractMeshes → BuildBlas →
  // RebuildTlas → UpdateBindless → ClearDirty). The load-bearing dependencies are
  // preserved by that order (textures' bindless slots before material packing; mesh
  // vertex/index before BLAS; BLAS before TLAS); the reordered steps are independent
  // GPU buffers, so the output is byte-identical (verified against the golden suite).
  RegisterCommitPipeline();  // idempotent.
  currentCommandList = commandList;
  commitError = {};
  sceneWorld.progress();  // runs the commit systems in phase order.
  currentCommandList = nullptr;
  return commitError;
}

void GpuScene::Impl::RegisterCommitPipeline() noexcept
{
  if (commitPipelineRegistered)
    return;
  commitPipelineRegistered = true;

  scene::RegisterPhasePipeline(sceneWorld);  // §30.11 phases + the active pipeline.

  // Each system runs one commit step, short-circuiting once a prior step errored.
  // `this` is captured: the Impl (and its sceneWorld) outlive every system.
  auto runStep = [this](Expected<void> (Impl::*commitFn)(nvrhi::ICommandList*)) {
    if (!commitError)
      return;  // a prior step already failed — skip.
    commitError = (this->*commitFn)(currentCommandList);
  };

  // PhaseUploadTextures — bindless fallbacks (slot-0 magenta) before texture decode.
  sceneWorld.system("Sys_BindlessFallbacks").kind<scene::PhaseUploadTextures>().run(
      [runStep](flecs::iter&) { runStep(&Impl::EnsureBindlessFallbacks); });
  sceneWorld.system("Sys_UploadTextures").kind<scene::PhaseUploadTextures>().run(
      [runStep](flecs::iter&) { runStep(&Impl::UploadPendingTextures); });

  // PhaseUploadMaterials — material pack reads the resolved texture bindless slots.
  sceneWorld.system("Sys_UploadMaterials").kind<scene::PhaseUploadMaterials>().run(
      [runStep](flecs::iter&) { runStep(&Impl::UploadMaterialBuffer); });
  sceneWorld.system("Sys_UploadLights").kind<scene::PhaseUploadMaterials>().run(
      [runStep](flecs::iter&) { runStep(&Impl::UploadLightBuffer); });
  sceneWorld.system("Sys_InstanceSideTables").kind<scene::PhaseUploadMaterials>().run(
      [runStep](flecs::iter&) { runStep(&Impl::UploadInstanceSideTables); });
  sceneWorld.system("Sys_UploadVolumes").kind<scene::PhaseUploadMaterials>().run(
      [runStep](flecs::iter&) { runStep(&Impl::UploadPendingVolumes); });

  // PhaseExtractMeshes — vertex/index + the per-mesh side-table buffers.
  sceneWorld.system("Sys_UploadMeshes").kind<scene::PhaseExtractMeshes>().run(
      [runStep](flecs::iter&) { runStep(&Impl::UploadPendingMeshes); });
  sceneWorld.system("Sys_MeshFaceNormals").kind<scene::PhaseExtractMeshes>().run(
      [runStep](flecs::iter&) { runStep(&Impl::UploadMeshFaceNormals); });
  sceneWorld.system("Sys_MeshUvs").kind<scene::PhaseExtractMeshes>().run(
      [runStep](flecs::iter&) { runStep(&Impl::UploadMeshUvs); });
  sceneWorld.system("Sys_MeshIndices").kind<scene::PhaseExtractMeshes>().run(
      [runStep](flecs::iter&) { runStep(&Impl::UploadMeshIndices); });
  sceneWorld.system("Sys_MeshVertexNormals").kind<scene::PhaseExtractMeshes>().run(
      [runStep](flecs::iter&) { runStep(&Impl::UploadMeshVertexNormals); });
  sceneWorld.system("Sys_MeshTangents").kind<scene::PhaseExtractMeshes>().run(
      [runStep](flecs::iter&) { runStep(&Impl::UploadMeshTangents); });

  // PhaseBuildBlas — needs the vertex/index buffers from ExtractMeshes.
  sceneWorld.system("Sys_BuildBlas").kind<scene::PhaseBuildBlas>().run(
      [runStep](flecs::iter&) { runStep(&Impl::BuildPendingBlas); });

  // PhaseRebuildTlas — needs the BLAS handles from BuildBlas.
  sceneWorld.system("Sys_RebuildTlas").kind<scene::PhaseRebuildTlas>().run(
      [runStep](flecs::iter&) { runStep(&Impl::RebuildTlasIfDirty); });
}

// ============================================================================
// Per-resource-type uploaders
// ============================================================================

Expected<void> GpuScene::Impl::UploadPendingMeshes(nvrhi::ICommandList* commandList)
{
  // RFC 0009 P4 — upload vertex/index buffers for DirtyTopology meshes. The tag is
  // NOT cleared here: the BLAS-build pass (which runs after, same commit, and needs
  // these buffers) clears DirtyTopology once the BLAS is built.
  for (uint32_t slot = 1; slot < meshSlots.SlotCount(); ++slot)
  {
    if (!meshSlots.IsLive(slot) || !meshSlots.EntityAtSlot(slot).has<scene::DirtyTopology>())
      continue;
    MeshResource& entry = meshResources[slot];

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
    entry.vertexBuffer = device->createBuffer(vertexDesc);
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
    entry.indexBuffer = device->createBuffer(indexDesc);
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
  }
  return {};
}

Expected<void> GpuScene::Impl::EnsureBindlessFallbacks(nvrhi::ICommandList* commandList)
{
  // The magenta 4×4 fallback lives in slot 0 of the bindless texture
  // table; every material whose resolved path failed to decode
  // points at it via INVALID_BINDLESS_TEXTURE → fallback gating in
  // the closesthit. Created once on the first commit that has a
  // device, reused for the lifetime of the scene.
  if (!missingTexture)
  {
    nvrhi::TextureDesc missingDesc;
    missingDesc.width = 4;
    missingDesc.height = 4;
    missingDesc.format = nvrhi::Format::RGBA8_UNORM;
    missingDesc.dimension = nvrhi::TextureDimension::Texture2D;
    missingDesc.debugName = "scene.missingTexture";
    missingDesc.initialState = nvrhi::ResourceStates::ShaderResource;
    missingDesc.keepInitialState = true;
    missingTexture = device->createTexture(missingDesc);
    if (!missingTexture)
    {
      return std::unexpected{
          PYXIS_ERROR(ErrorKind::OutOfMemoryGpu,
                      "CommitResources: createTexture(missingTexture 4x4) failed")};
    }
    // Magenta + black checker — visibly broken to debug eyes.
    static constexpr std::uint8_t MAGENTA[4] = {255, 0, 255, 255};
    static constexpr std::uint8_t BLACK[4]   = {0, 0, 0, 255};
    std::uint8_t pixels[4 * 4 * 4];
    for (std::size_t row = 0; row < 4; ++row)
    {
      for (std::size_t col = 0; col < 4; ++col)
      {
        const auto& src = ((col ^ row) & 1u) ? MAGENTA : BLACK;
        std::memcpy(&pixels[(row * 4u + col) * 4u], src, 4);
      }
    }
    commandList->writeTexture(missingTexture.Get(), 0, 0, pixels,
                              static_cast<std::size_t>(4u * 4u));
  }
  if (!bindlessSampler)
  {
    // Material textures (baseColor / normal / metallic / roughness /
    // emission). Wrap-Wrap covers tiling architectural surfaces;
    // mip-filter ON is what makes the M9-fidelity ray-cone LOD
    // helper actually trilinear-blend instead of point-sampling.
    nvrhi::SamplerDesc samplerDesc;
    samplerDesc.minFilter = true;
    samplerDesc.magFilter = true;
    samplerDesc.mipFilter = true;
    samplerDesc.addressU = nvrhi::SamplerAddressMode::Wrap;
    samplerDesc.addressV = nvrhi::SamplerAddressMode::Wrap;
    samplerDesc.addressW = nvrhi::SamplerAddressMode::Wrap;
    bindlessSampler = device->createSampler(samplerDesc);
  }
  if (!domeSampler)
  {
    // M9-fidelity per-role samplers. HDRI dome lat-long mapping
    // wants Wrap-U (azimuth wraps) + Clamp-V (elevation poles must
    // not bleed into the opposite hemisphere — a Wrap-V would
    // mirror +Y onto -Y at the seam). Otherwise identical to
    // bindlessSampler.
    nvrhi::SamplerDesc domeDesc;
    domeDesc.minFilter = true;
    domeDesc.magFilter = true;
    domeDesc.mipFilter = true;
    domeDesc.addressU = nvrhi::SamplerAddressMode::Wrap;
    domeDesc.addressV = nvrhi::SamplerAddressMode::Clamp;
    domeDesc.addressW = nvrhi::SamplerAddressMode::Wrap;
    domeSampler = device->createSampler(domeDesc);
  }
  return {};
}

Expected<void> GpuScene::Impl::UploadPendingTextures(nvrhi::ICommandList* commandList)
{
  // Synchronous decode on the render thread (§31 async I/O pool
  // wires at M8). LDR (.png/.jpg) goes through stb_image as RGBA8;
  // HDR (.exr — added at M7 for the dome environment map) goes
  // through tinyexr as RGBA32F. Format selection happens per entry
  // based on extension.
  //
  // V2.B — per-phase CPU timing. This pass dominated first-frame load on
  // the World Lobby (~57 s before the LUT mip-gen). We accumulate the
  // three CPU phases — decode (stb/tinyexr), mip build + BCn encode, and
  // GPU command record (createTexture + writeTexture into the staging
  // ring) — and log the breakdown so the cost is attributable.
  const auto texPassStart = std::chrono::steady_clock::now();

  // Collect the dirty entries up front so the CPU-bound work (decode +
  // gamma-aware mip build + BCn encode) can fan across the OpenMP pool.
  // NVRHI device/command-list calls are NOT thread-safe and must stay on
  // the render thread, so this pass is two-phase:
  //   phase 1 (parallel): decode + mip + BCn into a per-entry staging POD
  //   phase 2 (serial)  : createTexture + writeTexture from that POD
  // Decode dominated first-frame load on the World Lobby (~16 s of the
  // 27 s pass); spreading it over the asset-decode pool is the win.
  // RFC 0009 P3 — snapshot dirty texture slots (entities tagged DirtyTexture that
  // have a resolved path) up front. The parallel decode below touches NO Flecs /
  // component state — only this snapshot + the local prep[] — because flecs is not
  // safe for concurrent component access. Component metadata is written serially.
  struct DirtyTex
  {
    uint32_t          slot = 0;
    std::string       path;
    TextureKey::Role  role = TextureKey::Role::BaseColor;
  };
  std::vector<DirtyTex> dirty;
  textureSlots.ForEachLiveSlot([&](uint32_t slot, flecs::entity entity) {
    if (!entity.has<scene::DirtyTexture>())
      return;
    if (slot >= textureResolvedPaths.size() || textureResolvedPaths[slot].empty())
      return;
    dirty.push_back({slot, textureResolvedPaths[slot],
                     entity.get<GpuTextureComponent>().keyCopy.role});
  });

  struct PreparedTexUpload
  {
    std::vector<std::vector<std::uint8_t>> levelBytes;
    std::vector<std::size_t>               levelPitch;
    nvrhi::Format                          texFormat = nvrhi::Format::UNKNOWN;
    std::uint32_t                          width     = 0;
    std::uint32_t                          height    = 0;
    bool                                   ready     = false;  // false ⇒ decode failed.
  };
  std::vector<PreparedTexUpload> prep(dirty.size());

  // Per-entry CPU preparation. Pure compute + thread-safe library calls (stb_image,
  // tinyexr LoadEXR, EncodeBCn, DownsampleRgba8Half) over the snapshot; each
  // invocation owns its DirtyTex + PreparedTexUpload slot, no cross-entry sharing,
  // no Flecs access. spdlog (Logging::Get()) is thread-safe.
  const bool compressTextures = desc.compressTextures;
  const auto prepareEntry = [compressTextures](const DirtyTex& dirtyTex, PreparedTexUpload& out)
  {
    // Sniff extension — `.exr` → tinyexr; `.dds` → V2.A.14 BCn
    // passthrough; everything else → stb_image.
    const std::string& path = dirtyTex.path;
    const bool isExr = path.size() >= 4
        && (path.compare(path.size() - 4, 4, ".exr") == 0
            || path.compare(path.size() - 4, 4, ".EXR") == 0);
    const bool isDds = path.size() >= 4
        && (path.compare(path.size() - 4, 4, ".dds") == 0
            || path.compare(path.size() - 4, 4, ".DDS") == 0);

    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> decodedPixels;
    nvrhi::Format pixelFormat = nvrhi::Format::UNKNOWN;
    std::size_t rowPitchBytes = 0;

    if (isDds)
    {
      // V2.A.14 — DDS BCn passthrough. The header parse + format map
      // lives in `DdsParser.cpp` (testable without a Vulkan device);
      // we own the file I/O + slot allocation here.
      std::ifstream ddsFile(path, std::ios::binary | std::ios::ate);
      if (!ddsFile)
      {
        Logging::Get().Warn(log::RENDER,
                            std::string{"TextureCache: DDS open failed for "} + path
                                + " — falling back to missing-texture (slot 0).");
        return;
      }
      const auto fileSize = static_cast<std::size_t>(ddsFile.tellg());
      ddsFile.seekg(0);
      std::vector<std::uint8_t> ddsBytes(fileSize);
      ddsFile.read(reinterpret_cast<char*>(ddsBytes.data()),
                   static_cast<std::streamsize>(fileSize));
      const auto parsed = pyxis::gpuscene_detail::ParseDds(
          std::span<const std::uint8_t>{ddsBytes.data(), ddsBytes.size()},
          dirtyTex.role);
      if (!parsed.success)
      {
        Logging::Get().Warn(log::RENDER,
                            std::string{"TextureCache: DDS parse failed for "} + path
                                + " (unsupported FourCC / DXGI / truncated).");
        return;
      }
      width  = static_cast<int>(parsed.width);
      height = static_cast<int>(parsed.height);
      pixelFormat = parsed.format;
      decodedPixels.assign(ddsBytes.begin() + parsed.pixelOffset, ddsBytes.end());
      rowPitchBytes = static_cast<std::size_t>((width + 3) / 4) * parsed.bytesPerBlock;
    }
    else if (isExr)
    {
      // tinyexr LoadEXR: malloc's float[w*h*4] in RGBA order. We
      // upload directly as RGBA32_FLOAT — 16 B/pixel, so a 1024×512
      // dome env-map costs ~8 MB GPU (fine for v1; M9 polish drops
      // to RGBA16_FLOAT once the half-float pack lands).
      float* exrPixels = nullptr;
      const char* exrErr = nullptr;
      const int loadResult =
          LoadEXR(&exrPixels, &width, &height, path.c_str(), &exrErr);
      if (loadResult != TINYEXR_SUCCESS || exrPixels == nullptr || width <= 0
          || height <= 0)
      {
        Logging::Get().Warn(log::RENDER,
                            std::string{"TextureCache: LoadEXR failed for "} + path
                                + (exrErr ? std::string{" — "} + exrErr : std::string{})
                                + " — falling back to missing-texture (slot 0).");
        if (exrErr)
          FreeEXRErrorMessage(exrErr);
        if (exrPixels)
          std::free(exrPixels);
        return;
      }
      const std::size_t pixelByteCount = static_cast<std::size_t>(width) * height * 4u
                                       * sizeof(float);
      decodedPixels.assign(reinterpret_cast<std::uint8_t*>(exrPixels),
                           reinterpret_cast<std::uint8_t*>(exrPixels) + pixelByteCount);
      std::free(exrPixels);
      pixelFormat = nvrhi::Format::RGBA32_FLOAT;
      rowPitchBytes = static_cast<std::size_t>(width) * 4u * sizeof(float);
    }
    else
    {
      int channelsInFile = 0;
      stbi_uc* decoded = stbi_load(path.c_str(), &width, &height, &channelsInFile, 4);
      if (decoded == nullptr || width <= 0 || height <= 0)
      {
        Logging::Get().Warn(log::RENDER,
                            std::string{"TextureCache: stbi_load failed for "} + path
                                + " — falling back to missing-texture (slot 0).");
        if (decoded)
          stbi_image_free(decoded);
        return;
      }
      const auto pixelByteCount = static_cast<std::size_t>(width) * height * 4u;
      decodedPixels.assign(decoded, decoded + pixelByteCount);
      stbi_image_free(decoded);
      // BaseColor + Emission go through the sRGB→linear EOTF on
      // sample; everything else is linear data (normal maps, ORM
      // packs, env-maps via the EXR path above, etc.). §13.
      pixelFormat = (dirtyTex.role == TextureKey::Role::BaseColor
                     || dirtyTex.role == TextureKey::Role::Emission)
                        ? nvrhi::Format::SRGBA8_UNORM
                        : nvrhi::Format::RGBA8_UNORM;
      rowPitchBytes = static_cast<std::size_t>(width) * 4u;
    }

    out.width = static_cast<std::uint32_t>(width);
    out.height = static_cast<std::uint32_t>(height);

    // Unified mip-chain build + upload. Three decode paths feed here:
    //   * stbi 8-bit RGBA (pixelFormat RGBA8/SRGBA8): build a gamma-
    //     aware box mip chain, then optionally BCn-encode each level.
    //   * DDS (already block-compressed) + EXR (float env-map): a single
    //     mip 0, uploaded as decoded.
    // Mips are essential: closesthit's ray-cone LOD selects a mip level,
    // and with only mip 0 present every minified tiling texture aliases
    // into herringbone/sparkle moiré (wood walls, felt normal maps).
    //
    // V2.A.14 — BCn compression now runs PER MIP (BC1/BC4/BC5 by role)
    // so compressed normal/roughness maps keep their mip chain. sRGB
    // roles (BaseColor/Emission) skip BCn: BC1's RGB565 endpoints
    // quantise R/B at 5 bits vs G at 6, warm-casting sRGB data. The
    // BCn chain stops at the first mip whose dims aren't 4-divisible
    // (BC blocks are 4×4); below that the sampler clamps to the last
    // encoded level.
    const bool isRgba8 = (pixelFormat == nvrhi::Format::RGBA8_UNORM
                          || pixelFormat == nvrhi::Format::SRGBA8_UNORM);
    const bool isSrgb  = (pixelFormat == nvrhi::Format::SRGBA8_UNORM);
    const bool roleIsSRgb = (dirtyTex.role == TextureKey::Role::BaseColor)
                            || (dirtyTex.role == TextureKey::Role::Emission);

    std::vector<std::vector<std::uint8_t>> levelBytes;
    std::vector<std::size_t>               levelPitch;
    nvrhi::Format                          texFormat = pixelFormat;

    if (!isRgba8)
    {
      // DDS / EXR: data is already final; one level.
      levelBytes.push_back(std::move(decodedPixels));
      levelPitch.push_back(rowPitchBytes);
    }
    else
    {
      // Build the gamma-aware RGBA8 box mip chain (down to 1×1).
      std::vector<std::vector<std::uint8_t>> rgbaMips;
      std::vector<std::uint32_t>             rgbaMipW;
      std::vector<std::uint32_t>             rgbaMipH;
      rgbaMips.push_back(std::move(decodedPixels));
      rgbaMipW.push_back(out.width);
      rgbaMipH.push_back(out.height);
      while (rgbaMipW.back() > 1u || rgbaMipH.back() > 1u)
      {
        std::uint32_t nextW = 0u;
        std::uint32_t nextH = 0u;
        std::vector<std::uint8_t> half =
            DownsampleRgba8Half(rgbaMips.back().data(), rgbaMipW.back(),
                                rgbaMipH.back(), isSrgb, nextW, nextH);
        rgbaMips.push_back(std::move(half));
        rgbaMipW.push_back(nextW);
        rgbaMipH.push_back(nextH);
      }

      bool encoded = false;
      if (compressTextures && !roleIsSRgb)
      {
        for (std::size_t lvl = 0; lvl < rgbaMips.size(); ++lvl)
        {
          if (rgbaMipW[lvl] < 4u || rgbaMipH[lvl] < 4u
              || (rgbaMipW[lvl] % 4u) != 0u || (rgbaMipH[lvl] % 4u) != 0u)
            break;
          std::vector<std::uint8_t> blocks;
          nvrhi::Format             bcFormat     = nvrhi::Format::UNKNOWN;
          std::size_t               bcBlockBytes = 0;
          gpuscene_detail::EncodeBCn(rgbaMips[lvl].data(), rgbaMipW[lvl],
                                     rgbaMipH[lvl], dirtyTex.role,
                                     blocks, bcFormat, bcBlockBytes);
          if (bcFormat == nvrhi::Format::UNKNOWN || blocks.empty())
            break;
          texFormat = bcFormat;
          levelPitch.push_back(static_cast<std::size_t>(rgbaMipW[lvl] / 4u) * bcBlockBytes);
          levelBytes.push_back(std::move(blocks));
          encoded = true;
        }
      }
      if (!encoded)
      {
        // Uncompressed (sRGB role, compression off, or BCn declined):
        // upload every RGBA8 level.
        texFormat = pixelFormat;
        levelBytes.clear();
        levelPitch.clear();
        for (std::size_t lvl = 0; lvl < rgbaMips.size(); ++lvl)
        {
          levelPitch.push_back(static_cast<std::size_t>(rgbaMipW[lvl]) * 4u);
          levelBytes.push_back(std::move(rgbaMips[lvl]));
        }
      }
    }

    out.levelBytes = std::move(levelBytes);
    out.levelPitch = std::move(levelPitch);
    out.texFormat  = texFormat;
    out.ready      = true;
  };

  // Phase 1 — fan the decode/mip/BCn work across the OpenMP pool. Each iteration
  // owns a distinct DirtyTex snapshot + PreparedTexUpload slot; no Flecs access.
#ifdef _OPENMP
#  pragma omp parallel for schedule(dynamic)
#endif
  for (int entryIdx = 0; entryIdx < static_cast<int>(dirty.size()); ++entryIdx)
    prepareEntry(dirty[static_cast<std::size_t>(entryIdx)],
                 prep[static_cast<std::size_t>(entryIdx)]);

  const auto   prepEnd = std::chrono::steady_clock::now();
  const double prepMs =
      std::chrono::duration<double, std::milli>(prepEnd - texPassStart).count();

  // Phase 2 — serial GPU resource creation + upload on the render thread. Writes the
  // GPU handle into the slot-indexed side table + the component metadata, then drops
  // the DirtyTexture tag. Decode failures (ready == false) fall back to bindless slot
  // 0 and also drop the tag (no retry), matching the old needsGpuUpload=false path.
  int texProcessed = 0;
  for (std::size_t slotIdx = 0; slotIdx < dirty.size(); ++slotIdx)
  {
    const DirtyTex&    dirtyTex = dirty[slotIdx];
    PreparedTexUpload& prepared = prep[slotIdx];
    const flecs::entity entity = textureSlots.EntityAtSlot(dirtyTex.slot);
    if (entity.id() == 0)
      continue;  // destroyed mid-commit (single-writer makes this unreachable).
    GpuTextureComponent component = entity.get<GpuTextureComponent>();
    if (!prepared.ready)
    {
      component.bindlessSlot = 0;  // decode failed → magenta fallback (slot 0).
      entity.set<GpuTextureComponent>(component);
      entity.remove<scene::DirtyTexture>();
      continue;
    }
    component.width = prepared.width;
    component.height = prepared.height;
    component.format = prepared.texFormat;
    component.bindlessSlot = dirtyTex.slot;
    const std::uint32_t mipLevels = static_cast<std::uint32_t>(prepared.levelBytes.size());

    nvrhi::TextureDesc texDesc;
    texDesc.width = component.width;
    texDesc.height = component.height;
    texDesc.format = component.format;
    texDesc.mipLevels = mipLevels;
    texDesc.dimension = nvrhi::TextureDimension::Texture2D;
    texDesc.debugName = textureResolvedPaths[dirtyTex.slot];
    texDesc.initialState = nvrhi::ResourceStates::ShaderResource;
    texDesc.keepInitialState = true;
    const nvrhi::TextureHandle created = device->createTexture(texDesc);
    if (!created)
    {
      return std::unexpected{PYXIS_ERROR(ErrorKind::OutOfMemoryGpu,
                                         "CommitResources: createTexture failed for '%s'",
                                         textureResolvedPaths[dirtyTex.slot].c_str())};
    }
    for (std::uint32_t level = 0; level < mipLevels; ++level)
      commandList->writeTexture(created.Get(), 0, level,
                                prepared.levelBytes[level].data(),
                                prepared.levelPitch[level]);
    textureResources[dirtyTex.slot] = created;
    entity.set<GpuTextureComponent>(component);
    texturePixelData[dirtyTex.slot].clear();
    texturePixelData[dirtyTex.slot].shrink_to_fit();
    entity.remove<scene::DirtyTexture>();
    ++texProcessed;
  }
  if (texProcessed > 0)
  {
    const double gpuRecordMs = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - prepEnd).count();
    const double totalMs = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - texPassStart).count();
    Logging::Get().Info(
        log::RENDER,
        "UploadPendingTextures: " + std::to_string(texProcessed) + " textures in "
            + std::to_string(static_cast<int>(totalMs))
            + " ms (parallel decode+mip+BCn "
            + std::to_string(static_cast<int>(prepMs)) + " + serial GPU-record "
            + std::to_string(static_cast<int>(gpuRecordMs)) + " ms)");
  }
  return {};
}

Expected<void> GpuScene::Impl::UploadMaterialBuffer(nvrhi::ICommandList* commandList)
{
  // Re-uploaded whenever any material was added or updated this
  // frame (or when the materials vector grew). Small enough at v1
  // (~80 bytes per material × hundreds-of-thousands materials cap
  // = a few MiB worst case) that we always re-upload the whole
  // table rather than tracking dirty ranges.
  if (!materialsNeedGpuUpload || materialSlots.SlotCount() <= 1)
    return {};

  // RFC 0009 P2 — pack from the Flecs material entities by slot (was: iterate the
  // `materials` vector). buffer[slot] layout unchanged → byte-identical.
  std::vector<shaderinterop::OpenPBRMaterialGPU> packed;
  packed.resize(materialSlots.SlotCount());
  for (std::uint32_t slot = 0; slot < materialSlots.SlotCount(); ++slot)
  {
    if (slot == 0 || !materialSlots.IsLive(slot))
    {
      // Sentinel slot 0 + dead materials → magenta-fallback
      // material so the closesthit reads valid bytes regardless.
      packed[slot] = PackMaterialGpu(
          OpenPBRMaterialDesc{},
          static_cast<std::uint32_t>(MaterialFlag::None),
          shaderinterop::INVALID_BINDLESS_TEXTURE,
          shaderinterop::INVALID_BINDLESS_TEXTURE,
          shaderinterop::INVALID_BINDLESS_TEXTURE,
          shaderinterop::INVALID_BINDLESS_TEXTURE,
          shaderinterop::INVALID_BINDLESS_TEXTURE,
          shaderinterop::INVALID_BINDLESS_TEXTURE,
          shaderinterop::INVALID_BINDLESS_TEXTURE,
          shaderinterop::INVALID_BINDLESS_TEXTURE);
      continue;
    }
    const OpenPBRMaterialDesc& descCopy = materialSlots.EntityAtSlot(slot)
                                              .get<GpuMaterialComponent>()
                                              .desc;
    // Compute MaterialFlag bits from the desc + the texture
    // handles. The closesthit reads `flags` to short-circuit the
    // bindless lookup so a missing texture renders the scalar
    // fallback rather than the magenta missingTexture.
    std::uint32_t flags = 0;
    if (descCopy.opacity < 1.0f) flags |= MaterialFlag::AlphaTested;
    if (descCopy.coatWeight > 0.0f) flags |= MaterialFlag::CoatEnabled;
    if (descCopy.transmissionWeight > 0.0f)
      flags |= MaterialFlag::TransmissionEnabled;
    if (descCopy.emissionLuminance > 0.0f) flags |= MaterialFlag::Emissive;
    // V2.A.24 — normal-map tangent flips + non-identity UV transform.
    if (descCopy.flipTangentU != 0u) flags |= MaterialFlag::FlipTangentU;
    if (descCopy.flipTangentV != 0u) flags |= MaterialFlag::FlipTangentV;
    {
      const OpenPBRMaterialDesc& mdesc = descCopy;
      const bool nonIdentityUv =
          mdesc.baseColorUvScaleX != 1.0f || mdesc.baseColorUvScaleY != 1.0f
          || mdesc.baseColorUvRotationDeg != 0.0f
          || mdesc.baseColorUvTranslationX != 0.0f
          || mdesc.baseColorUvTranslationY != 0.0f;
      if (nonIdentityUv) flags |= MaterialFlag::HasUvTransform;
    }
    // RFC 0005 — planar projection mode → exactly one flag bit.
    // 1 = world-space, 2 = object-space (projection follows the instance).
    if (descCopy.projectionMode == 1u)
      flags |= MaterialFlag::WorldProjection;
    else if (descCopy.projectionMode == 2u)
      flags |= MaterialFlag::ObjectProjection;

    const std::uint32_t baseColorSlot =
        ResolveTextureBindlessSlot(descCopy.baseColorMap);
    const std::uint32_t normalSlot =
        ResolveTextureBindlessSlot(descCopy.normalMap);
    const std::uint32_t metallicSlot =
        ResolveTextureBindlessSlot(descCopy.metallicMap);
    const std::uint32_t roughnessSlot =
        ResolveTextureBindlessSlot(descCopy.roughnessMap);
    const std::uint32_t emissionSlot =
        ResolveTextureBindlessSlot(descCopy.emissionMap);
    const std::uint32_t opacitySlot =
        ResolveTextureBindlessSlot(descCopy.opacityMap);
    const std::uint32_t transmissionSlot =
        ResolveTextureBindlessSlot(descCopy.transmissionMap);
    const std::uint32_t coatRoughnessSlot =
        ResolveTextureBindlessSlot(descCopy.coatRoughnessMap);

    if (baseColorSlot != shaderinterop::INVALID_BINDLESS_TEXTURE)
      flags |= MaterialFlag::HasBaseColorMap;
    if (normalSlot != shaderinterop::INVALID_BINDLESS_TEXTURE)
      flags |= MaterialFlag::HasNormalMap;
    if (metallicSlot != shaderinterop::INVALID_BINDLESS_TEXTURE)
      flags |= MaterialFlag::HasMetallicMap;
    if (roughnessSlot != shaderinterop::INVALID_BINDLESS_TEXTURE)
      flags |= MaterialFlag::HasRoughnessMap;
    if (emissionSlot != shaderinterop::INVALID_BINDLESS_TEXTURE)
      flags |= MaterialFlag::HasEmissionMap;
    if (opacitySlot != shaderinterop::INVALID_BINDLESS_TEXTURE)
      flags |= MaterialFlag::HasOpacityMap;
    if (transmissionSlot != shaderinterop::INVALID_BINDLESS_TEXTURE)
      flags |= MaterialFlag::HasTransmissionMap;
    if (coatRoughnessSlot != shaderinterop::INVALID_BINDLESS_TEXTURE)
      flags |= MaterialFlag::HasCoatRoughnessMap;

    packed[slot] = PackMaterialGpu(
        descCopy, flags,
        baseColorSlot, normalSlot, metallicSlot, roughnessSlot,
        emissionSlot, opacitySlot, transmissionSlot, coatRoughnessSlot);
  }

  const std::size_t bufferBytes = packed.size() * sizeof(shaderinterop::OpenPBRMaterialGPU);
  // (Re)create the GPU buffer if it doesn't exist or grew. M5 stub
  // grows monotonically — DestroyMaterial leaves a hole rather
  // than reclaiming the slot index. M8 perf sweep adds compaction.
  PYXIS_TRY(EnsureStructuredBuffer(device, materialGpuBuffer, bufferBytes,
                                   sizeof(shaderinterop::OpenPBRMaterialGPU),
                                   "scene.materials", "materials"));
  commandList->writeBuffer(materialGpuBuffer.Get(), packed.data(), bufferBytes);
  materialsNeedGpuUpload = false;
  return {};
}

Expected<void> GpuScene::Impl::UploadLightBuffer(nvrhi::ICommandList* commandList)
{
  // Packs every LIVE LightEntry into a tightly-packed LightGpu buffer
  // bound at PathTracePass binding 5. Sparse / dead slots are
  // omitted — the closesthit iterates the buffer's full length, so
  // emitting only live lights keeps the per-hit loop tight. The
  // simple shading model in closesthit.slang ignores `intensity ==
  // 0` so a fallback 1-element zero buffer (used by PathTracePass
  // when the scene has no lights) contributes nothing.
  if (!lightsNeedGpuUpload)
    return {};

  // RFC 0009 P1 — pack from the slot-sorted Flecs query (was: iterate `lights`).
  // Same order (live lights in slot order) → byte-identical LightGpu buffer.
  std::vector<std::pair<uint32_t, LightDesc>> liveLights;
  CollectLiveLightsSorted(liveLights);
  std::vector<shaderinterop::LightGpu> packedLights;
  packedLights.reserve(liveLights.size());
  for (const auto& entry : liveLights)
  {
    const std::uint32_t envMapSlot = ResolveTextureBindlessSlot(entry.second.envMap);
    packedLights.push_back(PackLightGpu(entry.second, envMapSlot));
  }
  if (!packedLights.empty())
  {
    const std::size_t lightBytes =
        packedLights.size() * sizeof(shaderinterop::LightGpu);
    PYXIS_TRY(EnsureStructuredBuffer(device, lightsGpuBuffer, lightBytes,
                                     sizeof(shaderinterop::LightGpu),
                                     "scene.lights", "lights"));
    commandList->writeBuffer(lightsGpuBuffer.Get(), packedLights.data(), lightBytes);
  }
  lightsNeedGpuUpload = false;
  return {};
}

Expected<void> GpuScene::Impl::BuildPendingBlas(nvrhi::ICommandList* commandList)
{
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
  // RFC 0009 P4 — build the BLAS for DirtyTopology meshes (vertex/index buffers were
  // uploaded by the earlier pass this commit), then clear the tag.
  for (uint32_t slot = 1; slot < meshSlots.SlotCount(); ++slot)
  {
    const flecs::entity meshEntity = meshSlots.EntityAtSlot(slot);
    if (!meshSlots.IsLive(slot) || !meshEntity.has<scene::DirtyTopology>())
      continue;
    MeshResource& entry = meshResources[slot];
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

    // M9: drop the Opaque flag globally so the anyhit shader fires
    // on every hit. Anyhit reads the bound material and calls
    // IgnoreHit() for semi-translucent / transmissive / alpha-tested
    // materials (M9 invisibility-as-translucency stub) — without the
    // flag drop the GPU would short-circuit straight to closesthit
    // and translucent geometry would block light incorrectly.
    // Opaque-material cost is one extra anyhit invocation that
    // returns immediately; well within the §34 KPI budget.
    nvrhi::rt::GeometryDesc geometry;
    geometry.setTriangles(triangles).setFlags(nvrhi::rt::GeometryFlags::None);

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
    entry.blas = device->createAccelStruct(blasDesc);
    if (!entry.blas)
    {
      return std::unexpected{
          PYXIS_ERROR(ErrorKind::AccelStructBuildFailed,
                      "CommitResources: createAccelStruct(BLAS) failed for '%s' (triCount=%u)",
                      entry.debugName.c_str(), triangleCount)};
    }

    commandList->buildBottomLevelAccelStruct(entry.blas.Get(), &geometry, /*numGeometries*/ 1,
                                             buildFlags);
    meshEntity.remove<scene::DirtyTopology>();  // upload + BLAS both done this commit.
  }
  return {};
}

Expected<void> GpuScene::Impl::RebuildTlasIfDirty(nvrhi::ICommandList* commandList)
{
  // Lazy-allocate the TLAS on first need; size it to a fixed
  // M3-friendly capacity (TLAS_MAX_INSTANCES). M6+ grows this with
  // the scene budget.
  if (!tlasNeedsRebuild)
    return {};

  // RFC 0009 / §16 — refit when only instance transforms changed (no add/remove/
  // visibility since the last build) AND the live TLAS supports update. Otherwise a
  // full rebuild. A transform-only edit on a TLAS that lacks AllowUpdate triggers ONE
  // full rebuild that recreates it with AllowUpdate, enabling refit thereafter.
  // Static scenes never reach the AllowUpdate path → their build stays byte-identical.
  const bool transformOnly = !tlasStructureChanged;
  const bool wantAllowUpdate = transformOnly;  // dynamic transforms want refit capability.
  const bool canRefit = transformOnly && tlas && tlasAllowsUpdate;

  if (!tlas || (wantAllowUpdate && !tlasAllowsUpdate))
  {
    nvrhi::rt::AccelStructDesc tlasDesc;
    tlasDesc.isTopLevel = true;
    tlasDesc.topLevelMaxInstances = TLAS_MAX_INSTANCES;
    tlasDesc.buildFlags = nvrhi::rt::AccelStructBuildFlags::PreferFastTrace
                          | (wantAllowUpdate ? nvrhi::rt::AccelStructBuildFlags::AllowUpdate
                                             : nvrhi::rt::AccelStructBuildFlags::None);
    tlasDesc.debugName = "scene.tlas";
    tlas = device->createAccelStruct(tlasDesc);
    if (!tlas)
    {
      return std::unexpected{
          PYXIS_ERROR(ErrorKind::AccelStructBuildFailed,
                      "CommitResources: createAccelStruct(TLAS, max=%zu) failed",
                      TLAS_MAX_INSTANCES)};
    }
    tlasAllowsUpdate = wantAllowUpdate;
  }

  // Gather one nvrhi::rt::InstanceDesc per live + visible instance
  // whose mesh has a live BLAS. Skipping instances whose BLAS
  // isn't ready yet is the right behaviour during partial
  // mid-frame ingest — they'll join on the next CommitResources
  // tick.
  std::vector<nvrhi::rt::InstanceDesc> instanceDescs;
  instanceDescs.reserve(instanceSlots.SlotCount());
  for (uint32_t slot = 1; slot < instanceSlots.SlotCount(); ++slot)
  {
    if (!instanceSlots.IsLive(slot))
      continue;
    const InstanceResource& inst = instanceResources[slot];
    if (!inst.visible)
      continue;
    const auto meshValue = static_cast<uint32_t>(inst.mesh);
    const uint32_t meshSlot = HandleSlot(meshValue);
    if (meshSlot == 0 || meshSlot >= meshSlots.SlotCount() || !meshSlots.IsLive(meshSlot))
      continue;
    const MeshResource& mesh = meshResources[meshSlot];
    if (!mesh.blas)
      continue;

    nvrhi::rt::InstanceDesc desc;
    // §10 row-major + column-vector float4x4 → NVRHI's 3x4 affine
    // layout. NVRHI's AffineTransform is `float[12]` storing 3
    // rows of 4 columns in row-major order — Pyxis's
    // worldFromLocal rows 0..2 (drop row 3, the [0,0,0,1]
    // homogeneous padding) are byte-equivalent. hlslpp::store
    // writes 16 floats row-major; we keep the first 12.
    float worldRowMajor[16];
    hlslpp::store(worldRowMajor, inst.worldFromLocal);
    std::memcpy(&desc.transform, worldRowMajor, sizeof(nvrhi::rt::AffineTransform));
    desc.instanceMask = 0xFF;
    // Plan §15 — instanceCustomIndex carries the INSTANCE slot
    // (24-bit cap matches §19.7's HANDLE_SLOT_BITS). The
    // closesthit reads `instanceMaterial[InstanceID()]` to
    // resolve the material slot, then `materials[that]` to read
    // the OpenPBR fields. The indirection costs one extra buffer
    // load per closest-hit and frees the custom index for the
    // §41 M6 instanceId AOV + future picking (§19.4).
    desc.instanceID = slot;
    desc.instanceContributionToHitGroupIndex = 0;
    // UsdGeomGprim::doubleSided → disable triangle back-face culling so
    // rays hit the prim from either side. Essential for double-sided
    // translucent surfaces (glass panes, fabric, foliage cards): the
    // continuation/transmission ray spawned in the closesthit travels
    // through the front face and must still register the BACK face, and
    // primary rays that strike a back-facing triangle (interior of an
    // open shell) must shade rather than pass through invisibly. The
    // closesthit already face-forwards the shading normal, so a hit
    // from either side shades correctly once culling is off. Single-
    // sided prims keep default culling.
    desc.flags = inst.doubleSided
                     ? nvrhi::rt::InstanceFlags::TriangleCullDisable
                     : nvrhi::rt::InstanceFlags::None;
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

  auto buildFlags = nvrhi::rt::AccelStructBuildFlags::PreferFastTrace;
  if (tlasAllowsUpdate)
    buildFlags = buildFlags | nvrhi::rt::AccelStructBuildFlags::AllowUpdate;
  if (canRefit)
    buildFlags = buildFlags | nvrhi::rt::AccelStructBuildFlags::PerformUpdate;  // refit in place.
  commandList->buildTopLevelAccelStruct(tlas.Get(), instanceDescs.data(),
                                        instanceDescs.size(), buildFlags);
  tlasNeedsRebuild = false;
  tlasStructureChanged = false;  // next transform-only edit may refit.
  return {};
}

Expected<void> GpuScene::Impl::UploadInstanceSideTables(nvrhi::ICommandList* commandList)
{
  // Plan §15 / M6 P0 — upload the instance→material side-table.
  // Indexed by instance slot (so dead/sparse slots are present but
  // unread; they're never visited because the TLAS only contains
  // live instances). Each entry holds the material slot bound to
  // that instance; slot 0 always maps to material slot 0 (the
  // GpuScene sentinel grey material). Re-uploaded whenever the
  // dedicated dirty flag fires — independent of TLAS rebuild so
  // UpdateInstanceMaterial doesn't pointlessly rebuild the TLAS.
  if (!instanceMaterialNeedsUpload || instanceSlots.SlotCount() <= 1u)
    return {};

  const std::size_t instanceTableEntries = instanceSlots.SlotCount();
  std::vector<std::uint32_t> instanceMaterialTable(instanceTableEntries, 0u);
  for (std::size_t entrySlot = 1; entrySlot < instanceTableEntries; ++entrySlot)
  {
    if (!instanceSlots.IsLive(static_cast<uint32_t>(entrySlot)))
      continue;
    const InstanceResource& inst = instanceResources[entrySlot];
    const auto materialValue = static_cast<std::uint32_t>(inst.material);
    instanceMaterialTable[entrySlot] =
        (materialValue == 0) ? 0u : HandleSlot(materialValue);
  }
  const std::size_t instanceTableBytes =
      instanceMaterialTable.size() * sizeof(std::uint32_t);
  PYXIS_TRY(EnsureStructuredBuffer(device, instanceMaterialBuffer, instanceTableBytes,
                                   sizeof(std::uint32_t),
                                   "GpuScene.instanceMaterialBuffer",
                                   "instanceMaterialBuffer"));
  commandList->writeBuffer(instanceMaterialBuffer.Get(),
                           instanceMaterialTable.data(), instanceTableBytes);

  // M7 NdotL — instance→mesh side-table. Same shape + lifecycle
  // as instanceMaterialBuffer; piggy-backs on the same dirty flag
  // because instance ↔ mesh changes only happen when the TLAS
  // shape changes (AppendInstance / DestroyInstance / visibility
  // flip — UpdateInstanceMaterial doesn't touch instance.mesh).
  std::vector<std::uint32_t> instanceMeshTable(instanceTableEntries, 0u);
  for (std::size_t entrySlot = 1; entrySlot < instanceTableEntries; ++entrySlot)
  {
    if (!instanceSlots.IsLive(static_cast<uint32_t>(entrySlot)))
      continue;
    const InstanceResource& inst = instanceResources[entrySlot];
    const auto meshValue = static_cast<std::uint32_t>(inst.mesh);
    instanceMeshTable[entrySlot] =
        (meshValue == 0) ? 0u : HandleSlot(meshValue);
  }
  PYXIS_TRY(EnsureStructuredBuffer(device, instanceMeshBuffer, instanceTableBytes,
                                   sizeof(std::uint32_t),
                                   "GpuScene.instanceMeshBuffer",
                                   "instanceMeshBuffer"));
  commandList->writeBuffer(instanceMeshBuffer.Get(),
                           instanceMeshTable.data(), instanceTableBytes);

  instanceMaterialNeedsUpload = false;
  return {};
}

Expected<void> GpuScene::Impl::UploadMeshFaceNormals(nvrhi::ICommandList* commandList)
{
  // Concatenates every live mesh's per-triangle face normals into one
  // flat float4 buffer + a per-mesh-slot start-offset table. The
  // closesthit's NdotL Lambert pass reads:
  //   offset = gMeshFaceOffsets[meshSlot]
  //   nLocal = gMeshFaceNormals[offset + PrimitiveIndex()].xyz
  // The +1 in the offsets sizing reserves slot 0 (the §19.7
  // sentinel mesh handle) so the closesthit's "no mesh assigned"
  // path resolves to offset 0 with a black/zero normal entry.
  if (!meshFaceNormalsNeedUpload || meshSlots.SlotCount() <= 1u)
    return {};

  std::vector<hlslpp::float4> packedNormals;
  std::vector<std::uint32_t>  perMeshOffsets(meshSlots.SlotCount(), 0u);
  for (std::size_t meshSlot = 0; meshSlot < meshSlots.SlotCount(); ++meshSlot)
  {
    perMeshOffsets[meshSlot] = static_cast<std::uint32_t>(packedNormals.size());
    const MeshResource& mesh = meshResources[meshSlot];
    if (!meshSlots.IsLive(static_cast<uint32_t>(meshSlot)))
      continue;
    packedNormals.insert(packedNormals.end(), mesh.faceNormals.begin(),
                         mesh.faceNormals.end());
  }
  if (packedNormals.empty())
  {
    // Empty scene with no meshes registered yet — nothing to
    // upload. The PathTracePass fallback handles this.
    packedNormals.emplace_back(0.0f, 0.0f, 0.0f, 0.0f);
  }
  const std::size_t normalsBytes = packedNormals.size() * sizeof(hlslpp::float4);
  const std::size_t offsetsBytes = perMeshOffsets.size() * sizeof(std::uint32_t);

  PYXIS_TRY(EnsureStructuredBuffer(device, meshFaceNormalsBuffer, normalsBytes,
                                   sizeof(hlslpp::float4),
                                   "GpuScene.meshFaceNormalsBuffer",
                                   "meshFaceNormalsBuffer"));
  PYXIS_TRY(EnsureStructuredBuffer(device, meshFaceOffsetsBuffer, offsetsBytes,
                                   sizeof(std::uint32_t),
                                   "GpuScene.meshFaceOffsetsBuffer",
                                   "meshFaceOffsetsBuffer"));
  commandList->writeBuffer(meshFaceNormalsBuffer.Get(), packedNormals.data(),
                           normalsBytes);
  commandList->writeBuffer(meshFaceOffsetsBuffer.Get(), perMeshOffsets.data(),
                           offsetsBytes);
  meshFaceNormalsNeedUpload = false;
  return {};
}

// ---- M8a UV pipeline -------------------------------------------------
// Concatenates every live mesh's per-vertex UVs into one flat float2
// buffer + a per-mesh-slot start-offset table. Closesthit reads:
//   uvOffset = gMeshUvOffsets[meshSlot]
//   uv0/1/2  = gMeshUvs[uvOffset + vertexIndex]
// Vertex indices come from gMeshIndices (uploaded by the next phase).
//
// Meshes that authored no `primvars:st` (cube fixtures, tagged-empty
// authoring) contribute zero UVs — their offset stays the same as
// the previous mesh's, and the closesthit's HasBaseColorMap flag
// will fall through to the scalar baseColor anyway. Empty buffer
// gets a one-element fallback so the bindless layout is valid.
Expected<void> GpuScene::Impl::UploadMeshUvs(nvrhi::ICommandList* commandList)
{
  if (!meshUvsNeedUpload || meshSlots.SlotCount() <= 1u)
    return {};

  // Pack UVs TIGHT (8 bytes/element) to match the shader's
  // `StructuredBuffer<float2>` 8-byte stride. hlslpp::float2 is
  // SIMD-padded to 16 bytes, so a std::vector<hlslpp::float2> write gives
  // a 16-byte stride — the shader then reads every other element as the
  // padding (0,0), scrambling ALL mesh UVs (manifested as diagonal /
  // degenerate texturing on st-mapped surfaces, e.g. wood grain). The
  // float4 mesh buffers (normals/tangents/face-normals) are unaffected
  // because hlslpp::float4 is exactly 16 bytes. The per-mesh pack/pad
  // step lives in MeshUvPack.h so MeshUvPackV2 can pin the stride
  // contract directly.
  using gpuscene_detail::PackedUv;
  std::vector<PackedUv>       packedUvs;
  std::vector<std::uint32_t>  perMeshOffsets(meshSlots.SlotCount(), 0u);
  for (std::size_t meshSlot = 0; meshSlot < meshSlots.SlotCount(); ++meshSlot)
  {
    perMeshOffsets[meshSlot] = static_cast<std::uint32_t>(packedUvs.size());
    const MeshResource& mesh = meshResources[meshSlot];
    if (!meshSlots.IsLive(static_cast<uint32_t>(meshSlot)))
      continue;
    // Every mesh contributes EXACTLY vertexCount UV entries to the
    // flat buffer, even if mesh.uv0 is short. The closesthit reads
    // `gMeshUvs[gMeshUvOffsets[meshSlot] + v_i]` where v_i is a
    // vertex index in [0, vertexCount). Padding short UV arrays
    // with (0,0) keeps each mesh's UV slice aligned to its vertex
    // range — without it, the next mesh's UV data would overlap and
    // the closesthit would sample texels that don't belong to the
    // hit mesh.
    gpuscene_detail::AppendTightMeshUvs(mesh.uv0.data(), mesh.uv0.size(),
                                        mesh.vertexCount, packedUvs);
  }
  if (packedUvs.empty())
    packedUvs.push_back(PackedUv{0.0f, 0.0f});  // 1-element fallback

  const std::size_t uvsBytes     = packedUvs.size() * sizeof(PackedUv);
  const std::size_t offsetsBytes = perMeshOffsets.size() * sizeof(std::uint32_t);

  PYXIS_TRY(EnsureStructuredBuffer(device, meshUvsBuffer, uvsBytes,
                                   sizeof(PackedUv),
                                   "GpuScene.meshUvsBuffer",
                                   "meshUvsBuffer"));
  PYXIS_TRY(EnsureStructuredBuffer(device, meshUvOffsetsBuffer, offsetsBytes,
                                   sizeof(std::uint32_t),
                                   "GpuScene.meshUvOffsetsBuffer",
                                   "meshUvOffsetsBuffer"));
  commandList->writeBuffer(meshUvsBuffer.Get(),        packedUvs.data(),       uvsBytes);
  commandList->writeBuffer(meshUvOffsetsBuffer.Get(),  perMeshOffsets.data(),  offsetsBytes);
  meshUvsNeedUpload = false;
  return {};
}

// Concatenates every live mesh's triangle indices into one flat uint
// buffer + per-mesh-slot start-offset table. The same data lives in
// the per-mesh BLAS index buffer, but those are bound for AS-build
// (not as structured buffers for shader read). The duplication cost
// is one uint per triangle (~12 MB at World Lobby scale, acceptable).
//
// Closesthit reads three indices per hit:
//   ofs = gMeshIndexOffsets[meshSlot]
//   v0/1/2 = gMeshIndices[ofs + PrimitiveIndex()*3 + 0/1/2]
Expected<void> GpuScene::Impl::UploadMeshIndices(nvrhi::ICommandList* commandList)
{
  if (!meshIndicesNeedUpload || meshSlots.SlotCount() <= 1u)
    return {};

  std::vector<std::uint32_t> packedIndices;
  std::vector<std::uint32_t> perMeshOffsets(meshSlots.SlotCount(), 0u);
  for (std::size_t meshSlot = 0; meshSlot < meshSlots.SlotCount(); ++meshSlot)
  {
    perMeshOffsets[meshSlot] = static_cast<std::uint32_t>(packedIndices.size());
    const MeshResource& mesh = meshResources[meshSlot];
    if (!meshSlots.IsLive(static_cast<uint32_t>(meshSlot)))
      continue;
    packedIndices.insert(packedIndices.end(), mesh.indices.begin(), mesh.indices.end());
  }
  if (packedIndices.empty())
    packedIndices.push_back(0u);  // 1-element fallback

  const std::size_t indicesBytes = packedIndices.size() * sizeof(std::uint32_t);
  const std::size_t offsetsBytes = perMeshOffsets.size() * sizeof(std::uint32_t);

  PYXIS_TRY(EnsureStructuredBuffer(device, meshIndicesBuffer, indicesBytes,
                                   sizeof(std::uint32_t),
                                   "GpuScene.meshIndicesBuffer",
                                   "meshIndicesBuffer"));
  PYXIS_TRY(EnsureStructuredBuffer(device, meshIndexOffsetsBuffer, offsetsBytes,
                                   sizeof(std::uint32_t),
                                   "GpuScene.meshIndexOffsetsBuffer",
                                   "meshIndexOffsetsBuffer"));
  commandList->writeBuffer(meshIndicesBuffer.Get(),       packedIndices.data(),  indicesBytes);
  commandList->writeBuffer(meshIndexOffsetsBuffer.Get(),  perMeshOffsets.data(), offsetsBytes);
  meshIndicesNeedUpload = false;
  return {};
}

// M9 smooth shading: per-vertex normals concatenated into one flat
// float4 buffer + per-mesh-slot start-offset table. Mirror of the
// per-triangle face-normal upload above but per-VERTEX so the
// closesthit can barycentric-interpolate three vertex normals at
// each hit.
//
// Like the UV path, every mesh contributes EXACTLY vertexCount
// entries — short / empty `mesh.normals` arrays pad with (0,0,0,0).
// Closesthit detects the zero-magnitude case and falls back to the
// face-normal path so meshes that authored no normals still render.
Expected<void> GpuScene::Impl::UploadMeshVertexNormals(nvrhi::ICommandList* commandList)
{
  if (!meshVertexNormalsNeedUpload || meshSlots.SlotCount() <= 1u)
    return {};

  std::vector<hlslpp::float4> packedNormals;
  std::vector<std::uint32_t>  perMeshOffsets(meshSlots.SlotCount(), 0u);
  for (std::size_t meshSlot = 0; meshSlot < meshSlots.SlotCount(); ++meshSlot)
  {
    perMeshOffsets[meshSlot] = static_cast<std::uint32_t>(packedNormals.size());
    const MeshResource& mesh = meshResources[meshSlot];
    if (!meshSlots.IsLive(static_cast<uint32_t>(meshSlot)))
      continue;
    const std::size_t copyCount =
        std::min<std::size_t>(mesh.normals.size(), mesh.vertexCount);
    for (std::size_t i = 0; i < copyCount; ++i)
    {
      packedNormals.emplace_back(mesh.normals[i].x, mesh.normals[i].y,
                                 mesh.normals[i].z, 0.0f);
    }
    if (copyCount < mesh.vertexCount)
    {
      const std::size_t padCount = mesh.vertexCount - copyCount;
      packedNormals.insert(packedNormals.end(), padCount,
                           hlslpp::float4{0.0f, 0.0f, 0.0f, 0.0f});
    }
  }
  if (packedNormals.empty())
    packedNormals.emplace_back(0.0f, 0.0f, 0.0f, 0.0f);  // 1-element fallback

  const std::size_t normalsBytes = packedNormals.size() * sizeof(hlslpp::float4);
  const std::size_t offsetsBytes = perMeshOffsets.size() * sizeof(std::uint32_t);

  PYXIS_TRY(EnsureStructuredBuffer(device, meshVertexNormalsBuffer, normalsBytes,
                                   sizeof(hlslpp::float4),
                                   "GpuScene.meshVertexNormalsBuffer",
                                   "meshVertexNormalsBuffer"));
  PYXIS_TRY(EnsureStructuredBuffer(device, meshVertexNormalOffsetsBuffer, offsetsBytes,
                                   sizeof(std::uint32_t),
                                   "GpuScene.meshVertexNormalOffsetsBuffer",
                                   "meshVertexNormalOffsetsBuffer"));
  commandList->writeBuffer(meshVertexNormalsBuffer.Get(),
                           packedNormals.data(), normalsBytes);
  commandList->writeBuffer(meshVertexNormalOffsetsBuffer.Get(),
                           perMeshOffsets.data(), offsetsBytes);
  meshVertexNormalsNeedUpload = false;
  return {};
}

// M9 normal mapping: per-vertex tangents (from MikkTSpace) packed
// into a flat float4 buffer + per-mesh start-offset table. xyz is
// the unit tangent; w is the bitangent sign (+/- 1) used by the
// closesthit's `bitangent = sign × cross(N, T)` construction. Same
// shape + padding policy as the vertex-normal upload above —
// meshes that didn't generate tangents (no UVs / no normals) pad
// with zeros, and the closesthit's normal-mapping branch detects
// the zero-magnitude case and skips its TBN construction.
Expected<void> GpuScene::Impl::UploadMeshTangents(nvrhi::ICommandList* commandList)
{
  if (!meshTangentsNeedUpload || meshSlots.SlotCount() <= 1u)
    return {};

  std::vector<hlslpp::float4> packedTangents;
  std::vector<std::uint32_t>  perMeshOffsets(meshSlots.SlotCount(), 0u);
  for (std::size_t meshSlot = 0; meshSlot < meshSlots.SlotCount(); ++meshSlot)
  {
    perMeshOffsets[meshSlot] = static_cast<std::uint32_t>(packedTangents.size());
    const MeshResource& mesh = meshResources[meshSlot];
    if (!meshSlots.IsLive(static_cast<uint32_t>(meshSlot)))
      continue;
    const std::size_t copyCount =
        std::min<std::size_t>(mesh.tangents.size(), mesh.vertexCount);
    for (std::size_t i = 0; i < copyCount; ++i)
    {
      packedTangents.push_back(mesh.tangents[i]);
    }
    if (copyCount < mesh.vertexCount)
    {
      const std::size_t padCount = mesh.vertexCount - copyCount;
      packedTangents.insert(packedTangents.end(), padCount,
                            hlslpp::float4{0.0f, 0.0f, 0.0f, 0.0f});
    }
  }
  if (packedTangents.empty())
    packedTangents.emplace_back(0.0f, 0.0f, 0.0f, 0.0f);  // 1-element fallback

  const std::size_t tangentsBytes = packedTangents.size() * sizeof(hlslpp::float4);
  const std::size_t offsetsBytes  = perMeshOffsets.size() * sizeof(std::uint32_t);

  PYXIS_TRY(EnsureStructuredBuffer(device, meshTangentsBuffer, tangentsBytes,
                                   sizeof(hlslpp::float4),
                                   "GpuScene.meshTangentsBuffer",
                                   "meshTangentsBuffer"));
  PYXIS_TRY(EnsureStructuredBuffer(device, meshTangentOffsetsBuffer, offsetsBytes,
                                   sizeof(std::uint32_t),
                                   "GpuScene.meshTangentOffsetsBuffer",
                                   "meshTangentOffsetsBuffer"));
  commandList->writeBuffer(meshTangentsBuffer.Get(),
                           packedTangents.data(), tangentsBytes);
  commandList->writeBuffer(meshTangentOffsetsBuffer.Get(),
                           perMeshOffsets.data(), offsetsBytes);
  meshTangentsNeedUpload = false;
  return {};
}

}  // namespace pyxis
