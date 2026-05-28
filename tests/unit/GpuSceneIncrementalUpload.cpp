// Pyxis renderer — incremental mesh side-table upload byte-identity test (RFC 0009).
//
// The per-mesh side-table buffers (face normals / UVs / indices / vertex normals /
// tangents, each a concatenation + per-slot offset table) are uploaded by an
// incremental fast path: when the only dirty meshes are NEW tail slots and the
// buffers still have capacity, UploadMeshSideTable writes ONLY the new tail region
// instead of re-packing every mesh (the audit's quadratic-load fix). The golden
// suite only ever does a SINGLE bulk commit, so it exercises the full-rebuild path
// but never the tail-append path. This test pins the load-bearing invariant the
// optimization rests on: a scene built across MANY commits (mesh-add-then-commit,
// repeatedly — driving the append + geometric-grow branches) produces byte-identical
// side-table buffers to the same scene built in one bulk commit.
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

  // Every per-mesh side-table buffer + its offset table must be byte-identical.
  ExpectBufferPrefixEqual(device, "MeshFaceNormals", bulkScene.GetMeshFaceNormalsBuffer(),
                          incrementalScene.GetMeshFaceNormalsBuffer());
  ExpectBufferPrefixEqual(device, "MeshFaceOffsets", bulkScene.GetMeshFaceOffsetsBuffer(),
                          incrementalScene.GetMeshFaceOffsetsBuffer());
  ExpectBufferPrefixEqual(device, "MeshUvs", bulkScene.GetMeshUvsBuffer(),
                          incrementalScene.GetMeshUvsBuffer());
  ExpectBufferPrefixEqual(device, "MeshUvOffsets", bulkScene.GetMeshUvOffsetsBuffer(),
                          incrementalScene.GetMeshUvOffsetsBuffer());
  ExpectBufferPrefixEqual(device, "MeshIndices", bulkScene.GetMeshIndicesBuffer(),
                          incrementalScene.GetMeshIndicesBuffer());
  ExpectBufferPrefixEqual(device, "MeshIndexOffsets", bulkScene.GetMeshIndexOffsetsBuffer(),
                          incrementalScene.GetMeshIndexOffsetsBuffer());
  ExpectBufferPrefixEqual(device, "MeshVertexNormals", bulkScene.GetMeshVertexNormalsBuffer(),
                          incrementalScene.GetMeshVertexNormalsBuffer());
  ExpectBufferPrefixEqual(device, "MeshVertexNormalOffsets",
                          bulkScene.GetMeshVertexNormalOffsetsBuffer(),
                          incrementalScene.GetMeshVertexNormalOffsetsBuffer());
  ExpectBufferPrefixEqual(device, "MeshTangents", bulkScene.GetMeshTangentsBuffer(),
                          incrementalScene.GetMeshTangentsBuffer());
  ExpectBufferPrefixEqual(device, "MeshTangentOffsets", bulkScene.GetMeshTangentOffsetsBuffer(),
                          incrementalScene.GetMeshTangentOffsetsBuffer());
}
