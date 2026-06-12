// Pyxis renderer — renderer-internal SceneResources view (RFC 0003).
//
// The borrowed raw-NVRHI-resource set the render passes bind into their
// descriptor sets, populated from `GpuScene::Impl` once per use via
// `detail::SceneResourcesAccess::Get`. Replaces the 16 public
// `GpuScene::Get*` getters the M8a audit flagged as a §18.1 narrow-surface
// violation: ingest adapters and the app hold a `GpuScene*` but have no
// business binding raw NVRHI resources — this header lives in Private/ and
// never crosses the §18 public surface.
//
// Field set tracks the P2 packed buffers (design D3): the two per-instance
// uint tables collapsed into `instanceInfoBuffer` (InstanceInfoGpu), the
// five per-mesh offset tables into `meshInfoBuffer` (MeshInfoGpu), the
// per-vertex normal+tangent streams into `meshVertexAttribsBuffer`
// (VertexAttribGpu), and `meshIndicesBuffer` is the §14.5 index pool's
// structured view (the flat duplicate is gone).
//
// All pointers are BORROWED — valid until the next GpuScene mutation /
// CommitResources on the render thread (§31 single-writer). Passes that
// cache binding sets must snapshot-compare these pointers (PathTracePass's
// BindingsSnapshot) exactly as they did against the old getters.
//
// PRIVATE header (§18.9) — `SceneResourcesAccess::Get` is exported from the
// DLL only so the §35 unit-test harness (the documented exception allowed to
// peek into Private/) can reach the packed buffers; it is NOT part of the
// §18.1 public surface.

#pragma once

#include <Pyxis/Renderer/RendererApi.h>

#include <cstdint>

namespace nvrhi {
class IBuffer;
class ITexture;
class ISampler;
namespace rt {
class IAccelStruct;
}  // namespace rt
}  // namespace nvrhi

namespace pyxis {

class GpuScene;

struct SceneResources
{
  nvrhi::rt::IAccelStruct* tlas                    = nullptr;
  nvrhi::IBuffer*          materialBuffer          = nullptr;  // OpenPBRMaterialGPU, binding 3
  nvrhi::IBuffer*          instanceInfoBuffer      = nullptr;  // InstanceInfoGpu, binding 4
  nvrhi::IBuffer*          lightBuffer             = nullptr;  // LightGpu, binding 5
  nvrhi::IBuffer*          meshInfoBuffer          = nullptr;  // MeshInfoGpu, binding 6
  nvrhi::IBuffer*          meshFaceNormalsBuffer   = nullptr;  // float4, binding 7
  nvrhi::IBuffer*          meshUvsBuffer           = nullptr;  // tight float2, binding 24
  nvrhi::IBuffer*          meshIndicesBuffer       = nullptr;  // §14.5 index pool view, binding 26
  nvrhi::IBuffer*          meshVertexAttribsBuffer = nullptr;  // VertexAttribGpu, binding 29
  nvrhi::ITexture*         domeEnvMapTexture       = nullptr;  // binding 9 (null = no dome)
  nvrhi::ISampler*         bindlessSampler         = nullptr;  // binding 10
  nvrhi::ISampler*         domeSampler             = nullptr;  // binding 33
  nvrhi::ITexture*         missingTexture          = nullptr;  // bindless slot 0 (binding 28)
  // Bindless texture iteration (count + per-slot pointer; binding 28). Sparse:
  // entries are nullptr for the sentinel slot 0 and dead / not-yet-decoded slots —
  // same semantics as the old GetBindlessTextureCount / GetBindlessTextureAt pair.
  // The array aliases GpuScene::Impl scratch storage; same borrow rules as above.
  uint32_t                 bindlessTextureCount    = 0;
  nvrhi::ITexture* const*  bindlessTextures        = nullptr;
};

namespace detail {

// The single friend hook through GpuScene's PIMPL (the public header friends this
// struct; no other public-surface addition). Builds the view from GpuScene::Impl.
struct PYXIS_RENDERER_API SceneResourcesAccess
{
  [[nodiscard]] static SceneResources Get(GpuScene& scene) noexcept;
};

}  // namespace detail

}  // namespace pyxis
