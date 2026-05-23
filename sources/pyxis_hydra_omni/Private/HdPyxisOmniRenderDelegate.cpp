// Pyxis Omniverse Hydra delegate — RFC 0004 Stage 3 (C4).
//
// Ported from sources/pyxis_hydra (the vcpkg-USD desktop delegate), adapted to
// build out-of-tree against Kit's nv-usd 25.11. Mesh + camera prim Sync impls
// push to the delegate-owned PyxisEngine's GpuScene; the render pass drives
// PyxisEngine::RenderFrame.

#include "HdPyxisOmniRenderDelegate.h"

#include "HdPyxisOmniRenderParam.h"
#include "PyxisEngine.h"

#include <Pyxis/Renderer/Descs/CameraDesc.h>
#include <Pyxis/Renderer/Descs/InstanceDesc.h>
#include <Pyxis/Renderer/Descs/MeshDesc.h>
#include <Pyxis/Renderer/GpuScene.h>

#include <pxr/base/gf/matrix4d.h>
#include <pxr/base/gf/vec3f.h>
#include <pxr/base/vt/array.h>
#include <pxr/base/vt/value.h>
#include <pxr/imaging/hd/bprim.h>
#include <pxr/imaging/hd/camera.h>
#include <pxr/imaging/hd/instancer.h>
#include <pxr/imaging/hd/material.h>
#include <pxr/imaging/hd/mesh.h>
#include <pxr/imaging/hd/meshTopology.h>
#include <pxr/imaging/hd/renderBuffer.h>
#include <pxr/imaging/hd/resourceRegistry.h>
#include <pxr/imaging/hd/sceneDelegate.h>
#include <pxr/imaging/hd/sprim.h>
#include <pxr/imaging/hd/tokens.h>
#include <pxr/usd/sdf/path.h>

#include <hlsl++.h>

#include <cstdint>
#include <string>
#include <vector>

PXR_NAMESPACE_OPEN_SCOPE

namespace {

// USD row-major double matrix -> Pyxis column-vector row-major float4x4 (§10).
// Same transposition both Pyxis adapters use (§25.O.3 byte-equal invariant).
hlslpp::float4x4 ToPyxisMatrix(GfMatrix4d const& usd) noexcept {
  return hlslpp::float4x4(
      hlslpp::float4{static_cast<float>(usd[0][0]), static_cast<float>(usd[1][0]),
                     static_cast<float>(usd[2][0]), static_cast<float>(usd[3][0])},
      hlslpp::float4{static_cast<float>(usd[0][1]), static_cast<float>(usd[1][1]),
                     static_cast<float>(usd[2][1]), static_cast<float>(usd[3][1])},
      hlslpp::float4{static_cast<float>(usd[0][2]), static_cast<float>(usd[1][2]),
                     static_cast<float>(usd[2][2]), static_cast<float>(usd[3][2])},
      hlslpp::float4{static_cast<float>(usd[0][3]), static_cast<float>(usd[1][3]),
                     static_cast<float>(usd[2][3]), static_cast<float>(usd[3][3])});
}

[[nodiscard]] pyxis::GpuScene* SceneOf(HdRenderParam* renderParam) noexcept {
  auto* param = static_cast<HdPyxisOmniRenderParam*>(renderParam);
  return param ? param->GetGpuScene() : nullptr;
}

// ---- Mesh: Hydra mesh -> GpuScene::CreateMesh + AppendInstance ------------
class HdPyxisOmniMesh final : public HdMesh {
 public:
  explicit HdPyxisOmniMesh(SdfPath const& primId) : HdMesh(primId) {}

  void Sync(HdSceneDelegate* sceneDelegate, HdRenderParam* renderParam, HdDirtyBits* dirtyBits,
            TfToken const& /*reprToken*/) override {
    pyxis::GpuScene* scene = SceneOf(renderParam);
    if (scene == nullptr || sceneDelegate == nullptr) {
      *dirtyBits = HdChangeTracker::Clean;
      return;
    }
    if (HdChangeTracker::IsTopologyDirty(*dirtyBits, GetId()) ||
        HdChangeTracker::IsPrimvarDirty(*dirtyBits, GetId(), HdTokens->points)) {
      EmitToScene(sceneDelegate, *scene);
    }
    *dirtyBits = HdChangeTracker::Clean;
  }

  [[nodiscard]] HdDirtyBits GetInitialDirtyBitsMask() const override {
    return HdChangeTracker::AllSceneDirtyBits;
  }

 protected:
  HdDirtyBits _PropagateDirtyBits(HdDirtyBits bits) const override { return bits; }
  void _InitRepr(TfToken const& /*reprToken*/, HdDirtyBits* /*dirtyBits*/) override {}

 private:
  void EmitToScene(HdSceneDelegate* sceneDelegate, pyxis::GpuScene& scene) {
    const SdfPath& primId = GetId();
    const HdMeshTopology topology = sceneDelegate->GetMeshTopology(primId);
    const VtIntArray& faceCounts = topology.GetFaceVertexCounts();
    const VtIntArray& faceIndices = topology.GetFaceVertexIndices();

    // Triangulate fan-style so quads/ngons render too (the desktop adapter's
    // M4 stub only took triangles; here we fan-triangulate any convex face).
    std::vector<uint32_t> indices;
    {
      size_t cursor = 0;
      for (const int faceCount : faceCounts) {
        if (faceCount >= 3) {
          for (int v = 1; v + 1 < faceCount; ++v) {
            indices.push_back(static_cast<uint32_t>(faceIndices[cursor]));
            indices.push_back(static_cast<uint32_t>(faceIndices[cursor + v]));
            indices.push_back(static_cast<uint32_t>(faceIndices[cursor + v + 1]));
          }
        }
        cursor += static_cast<size_t>(faceCount);
      }
    }

    const VtValue pointsVal = sceneDelegate->Get(primId, HdTokens->points);
    if (!pointsVal.IsHolding<VtVec3fArray>())
      return;
    const VtVec3fArray& pointsArray = pointsVal.UncheckedGet<VtVec3fArray>();
    if (pointsArray.empty() || indices.empty())
      return;

    std::vector<hlslpp::float3> positions;
    positions.reserve(pointsArray.size());
    for (const GfVec3f& point : pointsArray)
      positions.emplace_back(point[0], point[1], point[2]);

    const std::string debugName = primId.GetString();
    pyxis::MeshDesc meshDesc;
    meshDesc.positions = positions;
    meshDesc.indices = indices;
    meshDesc.debugName = debugName;
    const auto meshHandle = scene.CreateMesh(meshDesc);
    if (!meshHandle.has_value())
      return;

    const GfMatrix4d worldFromLocal = sceneDelegate->GetTransform(primId);
    pyxis::InstanceDesc instanceDesc;
    instanceDesc.mesh = *meshHandle;
    instanceDesc.worldFromLocal = ToPyxisMatrix(worldFromLocal);
    instanceDesc.debugName = debugName;
    (void)scene.AppendInstance(instanceDesc);  // TLAS-cap exhaustion surfaces in FrameStats.
  }
};

// ---- Camera: Hydra camera -> GpuScene::SetCamera --------------------------
class HdPyxisOmniCamera final : public HdCamera {
 public:
  explicit HdPyxisOmniCamera(SdfPath const& primId) : HdCamera(primId) {}

  void Sync(HdSceneDelegate* sceneDelegate, HdRenderParam* renderParam,
            HdDirtyBits* dirtyBits) override {
    HdCamera::Sync(sceneDelegate, renderParam, dirtyBits);
    pyxis::GpuScene* scene = SceneOf(renderParam);
    if (scene != nullptr && sceneDelegate != nullptr) {
      const SdfPath& primId = GetId();
      const GfMatrix4d worldFromLocal = sceneDelegate->GetTransform(primId);
      pyxis::CameraDesc cameraDesc;
      cameraDesc.viewFromWorld = ToPyxisMatrix(worldFromLocal.GetInverse());
      cameraDesc.projFromView = ToPyxisMatrix(ComputeProjectionMatrix());
      const GfRange1f clipRange = GetClippingRange();
      cameraDesc.nearClip = clipRange.GetMin();
      cameraDesc.farClip = clipRange.GetMax();
      scene->SetCamera(cameraDesc);
    }
    *dirtyBits = HdChangeTracker::Clean;
  }
};

// ---- Functional stubs: material / light / render buffer -------------------
class StubMaterial final : public HdMaterial {
 public:
  explicit StubMaterial(SdfPath const& primId) : HdMaterial(primId) {}
  void Sync(HdSceneDelegate*, HdRenderParam*, HdDirtyBits* dirtyBits) override {
    *dirtyBits = HdChangeTracker::Clean;
  }
  [[nodiscard]] HdDirtyBits GetInitialDirtyBitsMask() const override { return HdMaterial::AllDirty; }
};

class StubLight final : public HdSprim {
 public:
  explicit StubLight(SdfPath const& primId) : HdSprim(primId) {}
  void Sync(HdSceneDelegate*, HdRenderParam*, HdDirtyBits* dirtyBits) override {
    *dirtyBits = HdChangeTracker::Clean;
  }
  [[nodiscard]] HdDirtyBits GetInitialDirtyBitsMask() const override {
    return HdChangeTracker::AllDirty;
  }
};

// Minimal CPU-backed render buffer so HdEngine's AOV machinery doesn't crash.
// Pyxis renders into its own exportable image (PyxisEngine), so this buffer is
// not the render target; the C4-full step aliases it to the imported image.
class StubRenderBuffer final : public HdRenderBuffer {
 public:
  explicit StubRenderBuffer(SdfPath const& primId) : HdRenderBuffer(primId) {}

  bool Allocate(GfVec3i const& dimensions, HdFormat format, bool /*multiSampled*/) override {
    _width = static_cast<uint32_t>(dimensions[0]);
    _height = static_cast<uint32_t>(dimensions[1]);
    _format = format;
    _data.assign(static_cast<size_t>(_width) * _height * HdDataSizeOfFormat(format), 0);
    return true;
  }
  unsigned int GetWidth() const override { return _width; }
  unsigned int GetHeight() const override { return _height; }
  unsigned int GetDepth() const override { return 1; }
  HdFormat GetFormat() const override { return _format; }
  bool IsMultiSampled() const override { return false; }
  void* Map() override {
    _mapped = true;
    return _data.data();
  }
  void Unmap() override { _mapped = false; }
  bool IsMapped() const override { return _mapped; }
  void Resolve() override {}
  bool IsConverged() const override { return true; }

 protected:
  void _Deallocate() override {
    _data.clear();
    _width = _height = 0;
  }

 private:
  uint32_t _width = 0;
  uint32_t _height = 0;
  HdFormat _format = HdFormatUNorm8Vec4;
  bool _mapped = false;
  std::vector<uint8_t> _data;
};

}  // namespace

// ---- Render pass ----------------------------------------------------------
HdPyxisOmniRenderPass::HdPyxisOmniRenderPass(HdRenderIndex* index,
                                             HdRprimCollection const& collection,
                                             pyxis_omni::PyxisEngine* engine)
    : HdRenderPass(index, collection), _engine(engine) {}

HdPyxisOmniRenderPass::~HdPyxisOmniRenderPass() = default;

void HdPyxisOmniRenderPass::_Execute(HdRenderPassStateSharedPtr const& /*renderPassState*/,
                                     TfTokenVector const& /*renderTags*/) {
  // The prims have already Sync'd into the engine's GpuScene; RenderFrame
  // commits it (builds BLAS/TLAS) and renders into the exportable image.
  if (_engine != nullptr && _engine->IsValid())
    _engine->RenderFrame();
}

// ---- Render delegate ------------------------------------------------------
HdPyxisOmniRenderDelegate::HdPyxisOmniRenderDelegate() { _Initialize(); }

HdPyxisOmniRenderDelegate::HdPyxisOmniRenderDelegate(HdRenderSettingsMap const& settingsMap)
    : HdRenderDelegate(settingsMap) {
  _Initialize();
}

HdPyxisOmniRenderDelegate::~HdPyxisOmniRenderDelegate() = default;

void HdPyxisOmniRenderDelegate::_Initialize() {
  _resourceRegistry = std::make_shared<HdResourceRegistry>();
  _supportedRprimTypes = {HdPrimTypeTokens->mesh};
  _supportedSprimTypes = {HdPrimTypeTokens->camera, HdPrimTypeTokens->material,
                          HdPrimTypeTokens->distantLight, HdPrimTypeTokens->domeLight,
                          HdPrimTypeTokens->rectLight};
  _supportedBprimTypes = {HdPrimTypeTokens->renderBuffer};

  // Stand up the Pyxis render engine (own Vulkan device + scene + renderer +
  // exporter). Default size; a real viewport resizes via the render buffer.
  _engine = std::make_unique<pyxis_omni::PyxisEngine>();
  if (!_engine->Initialize(1280, 720)) {
    _engine.reset();
    return;
  }
  _renderParam =
      std::make_unique<HdPyxisOmniRenderParam>(_engine->Scene(), _engine->ProfilerPtr());
}

const TfTokenVector& HdPyxisOmniRenderDelegate::GetSupportedRprimTypes() const {
  return _supportedRprimTypes;
}
const TfTokenVector& HdPyxisOmniRenderDelegate::GetSupportedSprimTypes() const {
  return _supportedSprimTypes;
}
const TfTokenVector& HdPyxisOmniRenderDelegate::GetSupportedBprimTypes() const {
  return _supportedBprimTypes;
}

HdRenderParam* HdPyxisOmniRenderDelegate::GetRenderParam() const { return _renderParam.get(); }

HdResourceRegistrySharedPtr HdPyxisOmniRenderDelegate::GetResourceRegistry() const {
  return _resourceRegistry;
}

HdRenderPassSharedPtr HdPyxisOmniRenderDelegate::CreateRenderPass(
    HdRenderIndex* index, HdRprimCollection const& collection) {
  return HdRenderPassSharedPtr(new HdPyxisOmniRenderPass(index, collection, _engine.get()));
}

HdInstancer* HdPyxisOmniRenderDelegate::CreateInstancer(HdSceneDelegate* /*delegate*/,
                                                        SdfPath const& /*id*/) {
  return nullptr;
}
void HdPyxisOmniRenderDelegate::DestroyInstancer(HdInstancer* instancer) { delete instancer; }

HdRprim* HdPyxisOmniRenderDelegate::CreateRprim(TfToken const& typeId, SdfPath const& rprimId) {
  if (typeId == HdPrimTypeTokens->mesh)
    return new HdPyxisOmniMesh(rprimId);
  return nullptr;
}
void HdPyxisOmniRenderDelegate::DestroyRprim(HdRprim* rprim) { delete rprim; }

HdSprim* HdPyxisOmniRenderDelegate::CreateSprim(TfToken const& typeId, SdfPath const& sprimId) {
  if (typeId == HdPrimTypeTokens->camera)
    return new HdPyxisOmniCamera(sprimId);
  if (typeId == HdPrimTypeTokens->material)
    return new StubMaterial(sprimId);
  if (typeId == HdPrimTypeTokens->distantLight || typeId == HdPrimTypeTokens->domeLight ||
      typeId == HdPrimTypeTokens->rectLight)
    return new StubLight(sprimId);
  return nullptr;
}
HdSprim* HdPyxisOmniRenderDelegate::CreateFallbackSprim(TfToken const& typeId) {
  if (typeId == HdPrimTypeTokens->camera)
    return new HdPyxisOmniCamera(SdfPath::EmptyPath());
  if (typeId == HdPrimTypeTokens->material)
    return new StubMaterial(SdfPath::EmptyPath());
  return new StubLight(SdfPath::EmptyPath());
}
void HdPyxisOmniRenderDelegate::DestroySprim(HdSprim* sprim) { delete sprim; }

HdBprim* HdPyxisOmniRenderDelegate::CreateBprim(TfToken const& typeId, SdfPath const& bprimId) {
  if (typeId == HdPrimTypeTokens->renderBuffer)
    return new StubRenderBuffer(bprimId);
  return nullptr;
}
HdBprim* HdPyxisOmniRenderDelegate::CreateFallbackBprim(TfToken const& typeId) {
  if (typeId == HdPrimTypeTokens->renderBuffer)
    return new StubRenderBuffer(SdfPath::EmptyPath());
  return nullptr;
}
void HdPyxisOmniRenderDelegate::DestroyBprim(HdBprim* bprim) { delete bprim; }

void HdPyxisOmniRenderDelegate::CommitResources(HdChangeTracker* /*tracker*/) {
  // GpuScene commit happens inside PyxisEngine::RenderFrame (on the render
  // pass) so it lands on the same command list as the render.
}

PXR_NAMESPACE_CLOSE_SCOPE
