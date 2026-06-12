// Pyxis renderer — incremental mesh side-table upload byte-identity test (RFC 0009).
//
// The per-mesh side-table buffers (face normals / UVs / interleaved vertex
// attribs — each a concatenation whose per-slot offsets live in the packed
// MeshInfoGpu table — plus the §14.5 index pool) are uploaded by an incremental
// fast path: when the only dirty meshes are NEW tail slots and the buffers still
// have capacity, UploadMeshSideTable writes ONLY the new tail region instead of
// re-packing every mesh (the audit's quadratic-load fix), and the geometry pools
// append-only by construction. The golden suite only ever does a SINGLE bulk
// commit, so it exercises the full-rebuild path but never the tail-append path.
// This test pins the load-bearing invariant the optimization rests on: a scene
// built across MANY commits (mesh-add-then-commit, repeatedly — driving the
// append + geometric-grow branches) produces byte-identical buffers to the same
// scene built in one bulk commit.
//
// P2 packing: the buffers are reached through the renderer-internal
// SceneResources accessor (RFC 0003) — the public Get* getters are gone. The
// unit-test harness is the documented §35 exception allowed into Private/.
//
// Needs a real (RT-capable) Vulkan device because the upload happens inside
// CommitResources; skips on CPU-only CI.

#include <gtest/gtest.h>

#include <Pyxis/Platform/Device/DeviceCreationParams.h>
#include <Pyxis/Platform/Device/IDeviceManager.h>
#include <Pyxis/Platform/Device/Resolution.h>
#include <Pyxis/Renderer/Descs/GpuSceneCreateDesc.h>
#include <Pyxis/Renderer/Descs/MeshDesc.h>
#include <Pyxis/Renderer/GpuScene.h>
#include <Pyxis/Renderer/Profiler.h>

#include "Scene/SceneResources.h"  // Private — see the §35 exception note above.

#include <nvrhi/nvrhi.h>

#include <cstdint>
#include <cstring>
#include <hlsl++.h>
#include <memory>
#include <vector>

using namespace pyxis;

namespace {

// One distinct mesh per index: `index + 1` independent triangles (so vertex/index
// counts vary across meshes → the per-slot offsets are non-trivial and a wrong
// tail-start byte offset would corrupt the comparison). Attribute presence is
// rotated by index so the vertex-normal / tangent / UV pad-vs-copy branches in
// the append path all get exercised across the scene.
struct MeshData {
  std::vector<hlslpp::float3> positions;
  std::vector<uint32_t>       indices;
  std::vector<hlslpp::float3> normals;
  std::vector<hlslpp::float4> tangents;
  std::vector<hlslpp::float2> uv0;

  explicit MeshData(uint32_t index) {
    const uint32_t triCount = index + 1u;
    const float    seed     = 1.0f + static_cast<float>(index);
    for (uint32_t tri = 0; tri < triCount; ++tri) {
      const float base = seed + static_cast<float>(tri) * 0.25f;
      positions.push_back(hlslpp::float3{base, 0.0f, 0.0f});
      positions.push_back(hlslpp::float3{base, 1.0f, 0.0f});
      positions.push_back(hlslpp::float3{base, 0.0f, 1.0f});
      indices.push_back(tri * 3u + 0u);
      indices.push_back(tri * 3u + 1u);
      indices.push_back(tri * 3u + 2u);
    }
    const auto vertexCount = static_cast<uint32_t>(positions.size());
    // Rotate which optional attributes are authored — exercises both the
    // copy branch (authored) and the zero-pad branch (omitted) in the
    // vertex-normal / tangent / UV side-table append logic.
    if (index % 2u == 0u)
      for (uint32_t vert = 0; vert < vertexCount; ++vert)
        normals.push_back(hlslpp::float3{0.0f, 1.0f, 0.0f});
    if (index % 2u == 1u)
      for (uint32_t vert = 0; vert < vertexCount; ++vert)
        tangents.push_back(hlslpp::float4{1.0f, 0.0f, 0.0f, 1.0f});
    if (index % 3u == 0u)
      for (uint32_t vert = 0; vert < vertexCount; ++vert)
        uv0.push_back(hlslpp::float2{static_cast<float>(vert) * 0.1f, 0.5f});
  }

  [[nodiscard]] MeshDesc Desc() const noexcept {
    MeshDesc desc;
    desc.positions = positions;
    desc.indices   = indices;
    desc.normals   = normals;
    desc.tangents  = tangents;
    desc.uv0       = uv0;
    desc.debugName = "incremental-upload.mesh";
    return desc;
  }
};

// Flush the scene's pending GPU work on a fresh command list (one CommitResources).
[[nodiscard]] bool Commit(nvrhi::IDevice* device, GpuScene& scene) {
  const nvrhi::CommandListHandle commandList = device->createCommandList();
  commandList->open();
  const Expected<void> result = scene.CommitResources(commandList.Get());
  commandList->close();
  device->executeCommandList(commandList.Get());
  device->waitForIdle();
  return result.has_value();
}

// Read `bytes` from the head of a device buffer back to host memory.
std::vector<uint8_t> Readback(nvrhi::IDevice* device, nvrhi::IBuffer* source,
                              std::size_t bytes) {
  nvrhi::BufferDesc stagingDesc;
  stagingDesc.byteSize   = bytes;
  stagingDesc.cpuAccess  = nvrhi::CpuAccessMode::Read;
  stagingDesc.debugName  = "incremental-upload.readback";
  const nvrhi::BufferHandle staging = device->createBuffer(stagingDesc);
  const nvrhi::CommandListHandle commandList = device->createCommandList();
  commandList->open();
  commandList->copyBuffer(staging.Get(), 0, source, 0, bytes);
  commandList->close();
  device->executeCommandList(commandList.Get());
  device->waitForIdle();
  std::vector<uint8_t> out(bytes);
  const void* mapped = device->mapBuffer(staging.Get(), nvrhi::CpuAccessMode::Read);
  std::memcpy(out.data(), mapped, bytes);
  device->unmapBuffer(staging.Get());
  return out;
}

// Assert the VALID prefix of an incrementally-built buffer matches the bulk one.
// The bulk buffer is sized exactly (its byteSize == valid bytes); the incremental
// buffer may be larger (geometric capacity growth), so we compare the bulk-sized
// prefix — exactly the region the closesthit ever reads.
void ExpectBufferPrefixEqual(nvrhi::IDevice* device, const char* label,
                             nvrhi::IBuffer* bulk, nvrhi::IBuffer* incremental) {
  ASSERT_NE(bulk, nullptr) << label << " (bulk) is null";
  ASSERT_NE(incremental, nullptr) << label << " (incremental) is null";
  const std::size_t validBytes = bulk->getDesc().byteSize;
  ASSERT_GE(incremental->getDesc().byteSize, validBytes)
      << label << " incremental buffer smaller than bulk";
  const std::vector<uint8_t> bulkBytes        = Readback(device, bulk, validBytes);
  const std::vector<uint8_t> incrementalBytes = Readback(device, incremental, validBytes);
  EXPECT_EQ(bulkBytes, incrementalBytes) << label << " differs (append != full pack)";
}

}  // namespace

TEST(GpuSceneIncrementalUpload, IncrementalCommitsMatchBulkCommitByteForByte) {
  // Six meshes is enough to drive the geometric-grow rebuild AND the in-headroom
  // tail-append branch over the run of one-at-a-time commits.
  constexpr uint32_t MESH_COUNT = 6;

  DeviceCreationParams params;
  params.framesInFlight  = 1;
  params.applicationName = "pyxis_incremental_upload_test";
  const Resolution res{64, 64};
  DeviceManagerCreateStatus status = DeviceManagerCreateStatus::Unknown;
  const std::unique_ptr<IDeviceManager> deviceManager(
      CreateHeadlessDeviceManager(params, res, &status));
  if (!deviceManager || status != DeviceManagerCreateStatus::Ok
      || deviceManager->GetDevice() == nullptr)
    GTEST_SKIP() << "No Vulkan device (CPU-only CI).";
  nvrhi::IDevice* device = deviceManager->GetDevice();

  std::vector<MeshData> meshes;
  meshes.reserve(MESH_COUNT);
  for (uint32_t index = 0; index < MESH_COUNT; ++index)
    meshes.emplace_back(index);

  // Scene A — single bulk commit (full-rebuild path; buffers sized exactly).
  Profiler           bulkProfiler{device};
  GpuScene           bulkScene{device, bulkProfiler, GpuSceneCreateDesc{}};
  for (const MeshData& mesh : meshes)
    ASSERT_TRUE(bulkScene.CreateMesh(mesh.Desc()).has_value());
  if (!Commit(device, bulkScene))
    GTEST_SKIP() << "CommitResources failed (device lacks ray tracing).";

  // Scene B — mesh-add-then-commit, repeated (append + geometric-grow path).
  Profiler           incrementalProfiler{device};
  GpuScene           incrementalScene{device, incrementalProfiler, GpuSceneCreateDesc{}};
  for (const MeshData& mesh : meshes) {
    ASSERT_TRUE(incrementalScene.CreateMesh(mesh.Desc()).has_value());
    ASSERT_TRUE(Commit(device, incrementalScene));
  }

  // Every packed/concatenated mesh buffer must be byte-identical. The bulk scene's
  // single commit sizes each buffer exactly (and the §14.5 pools allocate exact-fit
  // on first use thanks to UploadPendingMeshes' per-commit pre-pass), so the bulk
  // byteSize is precisely the valid range; the incremental buffers may be larger
  // (geometric growth) and only the prefix is compared.
  const SceneResources bulkRes        = detail::SceneResourcesAccess::Get(bulkScene);
  const SceneResources incrementalRes = detail::SceneResourcesAccess::Get(incrementalScene);
  ExpectBufferPrefixEqual(device, "MeshFaceNormals", bulkRes.meshFaceNormalsBuffer,
                          incrementalRes.meshFaceNormalsBuffer);
  ExpectBufferPrefixEqual(device, "MeshUvs", bulkRes.meshUvsBuffer,
                          incrementalRes.meshUvsBuffer);
  // §14.5 index pool (gMeshIndices view) — append-only, so bulk vs incremental
  // prefixes match by construction.
  ExpectBufferPrefixEqual(device, "MeshIndices(pool)", bulkRes.meshIndicesBuffer,
                          incrementalRes.meshIndicesBuffer);
  // P2 interleaved per-vertex normal+tangent stream.
  ExpectBufferPrefixEqual(device, "MeshVertexAttribs", bulkRes.meshVertexAttribsBuffer,
                          incrementalRes.meshVertexAttribsBuffer);
  // P2 packed MeshInfoGpu table — carries EVERY per-slot offset (face / UV /
  // vertex-attrib running sums + index-pool offsets) + vertexCount, replacing the
  // five old offset-table comparisons.
  ExpectBufferPrefixEqual(device, "MeshInfo", bulkRes.meshInfoBuffer,
                          incrementalRes.meshInfoBuffer);
}
