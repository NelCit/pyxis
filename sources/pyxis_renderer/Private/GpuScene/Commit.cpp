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

#include "GpuScene/DdsParser.h"
#include "Materials/MaterialFlag.h"

#include <stb_image.h>
#include <tinyexr.h>

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <utility>
#include <vector>

namespace pyxis {

using namespace gpuscene_detail;

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
  meshes.clear();
  meshes.emplace_back();
  meshes[0].quarantined = true;

  instances.clear();
  instances.emplace_back();
  instances[0].quarantined = true;

  lights.clear();
  lights.emplace_back();
  lights[0].quarantined = true;

  materials.clear();
  textures.clear();
  volumes.clear();

  materialDescHashToHandle.clear();
  textureKeyHashToHandle.clear();
  meshDescHashToHandle.clear();

  // Clear the slot-recycle free lists symmetrically with their
  // entry vectors. Without this, slot indices from the prior scene
  // would survive Clear() and the next Acquire/Append would pop a
  // slot that's now out of range for the cleared entry vector →
  // out-of-bounds `entries[slot]` → UB.
  freeMeshSlots.clear();
  freeInstanceSlots.clear();
  freeMaterialSlots.clear();
  freeTextureSlots.clear();
  freeLightSlots.clear();
  freeVolumeSlots.clear();
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

  // Order matters: meshes upload first (BLAS build needs the buffers
  // to exist), bindless fallbacks before texture decode (texture
  // entries fall back to slot 0), texture decode before material pack
  // (material-flag bits depend on resolved bindless slots), TLAS
  // rebuild before instance side-tables (the side-tables mirror the
  // instance vector shape, not strictly TLAS-dependent, but keeping
  // them adjacent makes the dirty-flag dependency obvious).
  PYXIS_TRY(UploadPendingMeshes(commandList));
  PYXIS_TRY(EnsureBindlessFallbacks(commandList));
  PYXIS_TRY(UploadPendingTextures(commandList));
  PYXIS_TRY(UploadMaterialBuffer(commandList));
  PYXIS_TRY(UploadLightBuffer(commandList));
  PYXIS_TRY(BuildPendingBlas(commandList));
  PYXIS_TRY(RebuildTlasIfDirty(commandList));
  PYXIS_TRY(UploadInstanceSideTables(commandList));
  PYXIS_TRY(UploadMeshFaceNormals(commandList));
  PYXIS_TRY(UploadMeshUvs(commandList));
  PYXIS_TRY(UploadMeshIndices(commandList));
  PYXIS_TRY(UploadMeshVertexNormals(commandList));
  PYXIS_TRY(UploadMeshTangents(commandList));
  PYXIS_TRY(UploadPendingVolumes(commandList));
  return {};
}

// ============================================================================
// Per-resource-type uploaders
// ============================================================================

Expected<void> GpuScene::Impl::UploadPendingMeshes(nvrhi::ICommandList* commandList)
{
  for (MeshEntry& entry : meshes)
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
    entry.needsGpuUpload = false;
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
  for (TextureEntry& entry : textures)
  {
    if (!entry.live || !entry.needsGpuUpload || entry.resolvedPath.empty())
      continue;

    // Sniff extension — `.exr` → tinyexr; `.dds` → V2.A.14 BCn
    // passthrough; everything else → stb_image.
    const std::string& path = entry.resolvedPath;
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
        entry.needsGpuUpload = false;
        entry.bindlessSlot = 0;
        continue;
      }
      const auto fileSize = static_cast<std::size_t>(ddsFile.tellg());
      ddsFile.seekg(0);
      std::vector<std::uint8_t> ddsBytes(fileSize);
      ddsFile.read(reinterpret_cast<char*>(ddsBytes.data()),
                   static_cast<std::streamsize>(fileSize));
      const auto parsed = pyxis::gpuscene_detail::ParseDds(
          std::span<const std::uint8_t>{ddsBytes.data(), ddsBytes.size()},
          entry.keyCopy.role);
      if (!parsed.success)
      {
        Logging::Get().Warn(log::RENDER,
                            std::string{"TextureCache: DDS parse failed for "} + path
                                + " (unsupported FourCC / DXGI / truncated).");
        entry.needsGpuUpload = false;
        entry.bindlessSlot = 0;
        continue;
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
        entry.needsGpuUpload = false;
        entry.bindlessSlot = 0;
        continue;
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
        entry.needsGpuUpload = false;
        entry.bindlessSlot = 0;
        if (decoded)
          stbi_image_free(decoded);
        continue;
      }
      const auto pixelByteCount = static_cast<std::size_t>(width) * height * 4u;
      decodedPixels.assign(decoded, decoded + pixelByteCount);
      stbi_image_free(decoded);
      // BaseColor + Emission go through the sRGB→linear EOTF on
      // sample; everything else is linear data (normal maps, ORM
      // packs, env-maps via the EXR path above, etc.). §13.
      pixelFormat = (entry.keyCopy.role == TextureKey::Role::BaseColor
                     || entry.keyCopy.role == TextureKey::Role::Emission)
                        ? nvrhi::Format::SRGBA8_UNORM
                        : nvrhi::Format::RGBA8_UNORM;
      rowPitchBytes = static_cast<std::size_t>(width) * 4u;
    }

    entry.pixelData = std::move(decodedPixels);
    entry.width = static_cast<std::uint32_t>(width);
    entry.height = static_cast<std::uint32_t>(height);
    entry.format = pixelFormat;

    nvrhi::TextureDesc texDesc;
    texDesc.width = entry.width;
    texDesc.height = entry.height;
    texDesc.format = entry.format;
    texDesc.dimension = nvrhi::TextureDimension::Texture2D;
    texDesc.debugName = entry.resolvedPath;
    texDesc.initialState = nvrhi::ResourceStates::ShaderResource;
    texDesc.keepInitialState = true;
    entry.texture = device->createTexture(texDesc);
    if (!entry.texture)
    {
      return std::unexpected{PYXIS_ERROR(ErrorKind::OutOfMemoryGpu,
                                         "CommitResources: createTexture failed for '%s'",
                                         entry.resolvedPath.c_str())};
    }
    commandList->writeTexture(entry.texture.Get(), 0, 0, entry.pixelData.data(),
                              rowPitchBytes);
    entry.pixelData.clear();
    entry.pixelData.shrink_to_fit();
    entry.needsGpuUpload = false;
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
  if (!materialsNeedGpuUpload || materials.size() <= 1)
    return {};

  std::vector<shaderinterop::OpenPBRMaterialGPU> packed;
  packed.resize(materials.size());
  for (std::uint32_t slot = 0; slot < materials.size(); ++slot)
  {
    const MaterialEntry& entry = materials[slot];
    if (slot == 0 || !entry.live)
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
    // Compute MaterialFlag bits from the desc + the texture
    // handles. The closesthit reads `flags` to short-circuit the
    // bindless lookup so a missing texture renders the scalar
    // fallback rather than the magenta missingTexture.
    std::uint32_t flags = 0;
    if (entry.descCopy.opacity < 1.0f) flags |= MaterialFlag::AlphaTested;
    if (entry.descCopy.coatWeight > 0.0f) flags |= MaterialFlag::CoatEnabled;
    if (entry.descCopy.transmissionWeight > 0.0f)
      flags |= MaterialFlag::TransmissionEnabled;
    if (entry.descCopy.emissionLuminance > 0.0f) flags |= MaterialFlag::Emissive;

    const std::uint32_t baseColorSlot =
        ResolveTextureBindlessSlot(entry.descCopy.baseColorMap);
    const std::uint32_t normalSlot =
        ResolveTextureBindlessSlot(entry.descCopy.normalMap);
    const std::uint32_t metallicSlot =
        ResolveTextureBindlessSlot(entry.descCopy.metallicMap);
    const std::uint32_t roughnessSlot =
        ResolveTextureBindlessSlot(entry.descCopy.roughnessMap);
    const std::uint32_t emissionSlot =
        ResolveTextureBindlessSlot(entry.descCopy.emissionMap);
    const std::uint32_t opacitySlot =
        ResolveTextureBindlessSlot(entry.descCopy.opacityMap);
    const std::uint32_t transmissionSlot =
        ResolveTextureBindlessSlot(entry.descCopy.transmissionMap);
    const std::uint32_t coatRoughnessSlot =
        ResolveTextureBindlessSlot(entry.descCopy.coatRoughnessMap);

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
        entry.descCopy, flags,
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
  // Per-entry needsGpuUpload flags are advisory only since we
  // always re-upload the whole table; clearing them keeps the
  // bookkeeping consistent so a future incremental-upload path
  // (M8+) can drop into place without semantic changes.
  for (MaterialEntry& entry : materials)
    entry.needsGpuUpload = false;
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

  std::vector<shaderinterop::LightGpu> packedLights;
  packedLights.reserve(lights.size());
  for (const LightEntry& entry : lights)
  {
    if (!entry.live)
      continue;
    const std::uint32_t envMapSlot =
        ResolveTextureBindlessSlot(entry.descCopy.envMap);
    packedLights.push_back(PackLightGpu(entry.descCopy, envMapSlot));
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
  for (MeshEntry& entry : meshes)
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
    entry.needsBlasBuild = false;
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

  if (!tlas)
  {
    nvrhi::rt::AccelStructDesc tlasDesc;
    tlasDesc.isTopLevel = true;
    tlasDesc.topLevelMaxInstances = TLAS_MAX_INSTANCES;
    tlasDesc.buildFlags = nvrhi::rt::AccelStructBuildFlags::PreferFastTrace;
    tlasDesc.debugName = "scene.tlas";
    tlas = device->createAccelStruct(tlasDesc);
    if (!tlas)
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
  instanceDescs.reserve(instances.size());
  for (uint32_t slot = 1; slot < instances.size(); ++slot)
  {
    const InstanceEntry& inst = instances[slot];
    if (!inst.live || !inst.visible)
      continue;
    const auto meshValue = static_cast<uint32_t>(inst.mesh);
    const uint32_t meshSlot = HandleSlot(meshValue);
    if (meshSlot == 0 || meshSlot >= meshes.size())
      continue;
    const MeshEntry& mesh = meshes[meshSlot];
    if (!mesh.live || !mesh.blas)
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

  commandList->buildTopLevelAccelStruct(tlas.Get(), instanceDescs.data(),
                                        instanceDescs.size(),
                                        nvrhi::rt::AccelStructBuildFlags::PreferFastTrace);
  tlasNeedsRebuild = false;
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
  if (!instanceMaterialNeedsUpload || instances.empty())
    return {};

  const std::size_t instanceTableEntries = instances.size();
  std::vector<std::uint32_t> instanceMaterialTable(instanceTableEntries, 0u);
  for (std::size_t entrySlot = 1; entrySlot < instanceTableEntries; ++entrySlot)
  {
    const InstanceEntry& inst = instances[entrySlot];
    if (!inst.live)
      continue;
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
    const InstanceEntry& inst = instances[entrySlot];
    if (!inst.live)
      continue;
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
  if (!meshFaceNormalsNeedUpload || meshes.empty())
    return {};

  std::vector<hlslpp::float4> packedNormals;
  std::vector<std::uint32_t>  perMeshOffsets(meshes.size(), 0u);
  for (std::size_t meshSlot = 0; meshSlot < meshes.size(); ++meshSlot)
  {
    perMeshOffsets[meshSlot] = static_cast<std::uint32_t>(packedNormals.size());
    const MeshEntry& mesh = meshes[meshSlot];
    if (!mesh.live)
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
  if (!meshUvsNeedUpload || meshes.empty())
    return {};

  std::vector<hlslpp::float2> packedUvs;
  std::vector<std::uint32_t>  perMeshOffsets(meshes.size(), 0u);
  for (std::size_t meshSlot = 0; meshSlot < meshes.size(); ++meshSlot)
  {
    perMeshOffsets[meshSlot] = static_cast<std::uint32_t>(packedUvs.size());
    const MeshEntry& mesh = meshes[meshSlot];
    if (!mesh.live)
      continue;
    // Every mesh contributes EXACTLY vertexCount UV entries to the
    // flat buffer, even if mesh.uv0 is short. The closesthit reads
    // `gMeshUvs[gMeshUvOffsets[meshSlot] + v_i]` where v_i is a
    // vertex index in [0, vertexCount). Padding short UV arrays
    // with (0,0) keeps each mesh's UV slice aligned to its vertex
    // range — without it, the next mesh's UV data would overlap and
    // the closesthit would sample texels that don't belong to the
    // hit mesh.
    packedUvs.insert(packedUvs.end(), mesh.uv0.begin(), mesh.uv0.end());
    if (mesh.uv0.size() < mesh.vertexCount)
    {
      const std::size_t padCount = mesh.vertexCount - mesh.uv0.size();
      packedUvs.insert(packedUvs.end(), padCount, hlslpp::float2{0.0f, 0.0f});
    }
  }
  if (packedUvs.empty())
    packedUvs.emplace_back(0.0f, 0.0f);  // 1-element fallback

  const std::size_t uvsBytes     = packedUvs.size() * sizeof(hlslpp::float2);
  const std::size_t offsetsBytes = perMeshOffsets.size() * sizeof(std::uint32_t);

  PYXIS_TRY(EnsureStructuredBuffer(device, meshUvsBuffer, uvsBytes,
                                   sizeof(hlslpp::float2),
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
  if (!meshIndicesNeedUpload || meshes.empty())
    return {};

  std::vector<std::uint32_t> packedIndices;
  std::vector<std::uint32_t> perMeshOffsets(meshes.size(), 0u);
  for (std::size_t meshSlot = 0; meshSlot < meshes.size(); ++meshSlot)
  {
    perMeshOffsets[meshSlot] = static_cast<std::uint32_t>(packedIndices.size());
    const MeshEntry& mesh = meshes[meshSlot];
    if (!mesh.live)
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
  if (!meshVertexNormalsNeedUpload || meshes.empty())
    return {};

  std::vector<hlslpp::float4> packedNormals;
  std::vector<std::uint32_t>  perMeshOffsets(meshes.size(), 0u);
  for (std::size_t meshSlot = 0; meshSlot < meshes.size(); ++meshSlot)
  {
    perMeshOffsets[meshSlot] = static_cast<std::uint32_t>(packedNormals.size());
    const MeshEntry& mesh = meshes[meshSlot];
    if (!mesh.live)
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
  if (!meshTangentsNeedUpload || meshes.empty())
    return {};

  std::vector<hlslpp::float4> packedTangents;
  std::vector<std::uint32_t>  perMeshOffsets(meshes.size(), 0u);
  for (std::size_t meshSlot = 0; meshSlot < meshes.size(); ++meshSlot)
  {
    perMeshOffsets[meshSlot] = static_cast<std::uint32_t>(packedTangents.size());
    const MeshEntry& mesh = meshes[meshSlot];
    if (!mesh.live)
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
