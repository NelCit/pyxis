// Pyxis Hydra — HdRenderDelegate.

#include "HdPyxisRenderDelegate.h"

#include "HdPyxisRenderParam.h"

#include <Pyxis/Renderer/Descs/CameraDesc.h>
#include <Pyxis/Renderer/Descs/InstanceDesc.h>
#include <Pyxis/Renderer/Descs/MeshDesc.h>
#include <Pyxis/Renderer/GpuScene.h>

#include <pxr/imaging/hd/mesh.h>
#include <pxr/imaging/hd/camera.h>
#include <pxr/imaging/hd/material.h>
#include <pxr/imaging/hd/meshTopology.h>
#include <pxr/imaging/hd/renderBuffer.h>
#include <pxr/imaging/hd/renderPass.h>
#include <pxr/imaging/hd/resourceRegistry.h>
#include <pxr/imaging/hd/sceneDelegate.h>
#include <pxr/imaging/hd/tokens.h>
#include <pxr/imaging/hd/instancer.h>
#include <pxr/base/gf/matrix4d.h>
#include <pxr/base/gf/matrix4f.h>
#include <pxr/base/gf/vec3f.h>
#include <pxr/base/vt/array.h>
#include <pxr/base/vt/value.h>
#include <pxr/usd/sdf/path.h>

#include <hlsl++.h>

#include <string>
#include <vector>

PXR_NAMESPACE_OPEN_SCOPE

namespace {

const TfTokenVector& GetSupportedRprimTypesImpl() {
  static const TfTokenVector SUPPORTED_TYPES = {HdPrimTypeTokens->mesh};
  return SUPPORTED_TYPES;
}

const TfTokenVector& GetSupportedSprimTypesImpl() {
  // Plan §25.A: camera + the three light kinds Pyxis supports +
  // material. extComputation / simpleLight / cylinderLight /
  // sphereLight are deferred (sphere/cylinder may map to rect/distant
  // fallbacks at M9, see plan §25.A note).
  static const TfTokenVector SUPPORTED_TYPES = {
      HdPrimTypeTokens->camera,
      HdPrimTypeTokens->distantLight,
      HdPrimTypeTokens->domeLight,
      HdPrimTypeTokens->rectLight,
      HdPrimTypeTokens->material,
  };
  return SUPPORTED_TYPES;
}

const TfTokenVector& GetSupportedBprimTypesImpl() {
  // Plan §25.A: renderBuffer for AOVs only — texture-resource-as-Bprim
  // is skipped (the renderer's TextureCache owns texture lifetime).
  static const TfTokenVector SUPPORTED_TYPES = {HdPrimTypeTokens->renderBuffer};
  return SUPPORTED_TYPES;
}

// -------------------------------------------------------------------
// Convert USD's row-major double-precision matrix to Pyxis's
// column-vector + row-major float4x4 (plan §10). Same transposition
// pyxis_usd_ingest::StageWalker uses, so both adapters produce the
// same instance transforms on the same .usd input — the §25.O.3
// byte-equal P0 invariant.
// -------------------------------------------------------------------
hlslpp::float4x4 ToPyxisMatrix(GfMatrix4d const& usdMatrix) noexcept {
  return hlslpp::float4x4(
      hlslpp::float4{static_cast<float>(usdMatrix[0][0]),
                     static_cast<float>(usdMatrix[1][0]),
                     static_cast<float>(usdMatrix[2][0]),
                     static_cast<float>(usdMatrix[3][0])},
      hlslpp::float4{static_cast<float>(usdMatrix[0][1]),
                     static_cast<float>(usdMatrix[1][1]),
                     static_cast<float>(usdMatrix[2][1]),
                     static_cast<float>(usdMatrix[3][1])},
      hlslpp::float4{static_cast<float>(usdMatrix[0][2]),
                     static_cast<float>(usdMatrix[1][2]),
                     static_cast<float>(usdMatrix[2][2]),
                     static_cast<float>(usdMatrix[3][2])},
      hlslpp::float4{static_cast<float>(usdMatrix[0][3]),
                     static_cast<float>(usdMatrix[1][3]),
                     static_cast<float>(usdMatrix[2][3]),
                     static_cast<float>(usdMatrix[3][3])});
}

// -------------------------------------------------------------------
// HdPyxisMesh — translates Hydra mesh dirty events into GpuScene
// mutations. M4 constraint: triangle-list meshes only (matches
// pyxis_usd_ingest::StageWalker's M4 stub triangulator). Quads /
// ngons fan-triangulate at M5+.
// -------------------------------------------------------------------

class HdPyxisMesh final : public HdMesh {
 public:
  explicit HdPyxisMesh(SdfPath const& primId) : HdMesh(primId) {}

  void Sync(HdSceneDelegate* sceneDelegate, HdRenderParam* renderParam,
            HdDirtyBits* dirtyBits, TfToken const& /*reprToken*/) override {
    auto* pyxisParam = static_cast<HdPyxisRenderParam*>(renderParam);
    pyxis::GpuScene* scene = pyxisParam ? pyxisParam->GetGpuScene() : nullptr;

    if (scene == nullptr || sceneDelegate == nullptr)
    {
      *dirtyBits = HdChangeTracker::Clean;
      return;
    }

    // M4: only handle the initial sync (DirtyTopology / DirtyPoints /
    // DirtyTransform). Per-frame transform updates land at M6 when
    // animation arrives.
    if (HdChangeTracker::IsTopologyDirty(*dirtyBits, GetId())
        || HdChangeTracker::IsPrimvarDirty(*dirtyBits, GetId(), HdTokens->points))
    {
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

    // Topology — face counts + face vertex indices.
    const HdMeshTopology topology = sceneDelegate->GetMeshTopology(primId);
    const VtIntArray& faceCounts = topology.GetFaceVertexCounts();
    const VtIntArray& faceIndices = topology.GetFaceVertexIndices();

    // M4 stub triangulation: every face must be a triangle. Mirrors
    // the StageWalker M4 constraint so both adapters agree on what
    // they emit.
    for (const int faceCount : faceCounts)
    {
      if (faceCount != 3)
      {
        return;  // Skip non-triangle meshes at M4.
      }
    }

    // Points primvar.
    const VtValue pointsVal = sceneDelegate->Get(primId, HdTokens->points);
    if (!pointsVal.IsHolding<VtVec3fArray>())
      return;
    const VtVec3fArray& pointsArray = pointsVal.UncheckedGet<VtVec3fArray>();
    if (pointsArray.empty() || faceIndices.empty())
      return;

    std::vector<hlslpp::float3> positions;
    positions.reserve(pointsArray.size());
    for (const GfVec3f& point : pointsArray)
      positions.emplace_back(point[0], point[1], point[2]);

    std::vector<uint32_t> indices;
    indices.reserve(faceIndices.size());
    for (const int idx : faceIndices)
      indices.push_back(static_cast<uint32_t>(idx));

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
    const auto instanceHandle = scene.AppendInstance(instanceDesc);
    (void)instanceHandle;  // Failure is silent at M4 — TLAS-cap exhaustion
                           // surfaces via FrameStats::degraded at M6.
  }
};

// -------------------------------------------------------------------
// HdPyxisCamera — translates Hydra camera Sync into GpuScene::SetCamera.
// Plan §25.D maps HdCamera (transform + projection) to pyxis::CameraDesc.
// -------------------------------------------------------------------

class HdPyxisCamera final : public HdCamera {
 public:
  explicit HdPyxisCamera(SdfPath const& primId) : HdCamera(primId) {}

  void Sync(HdSceneDelegate* sceneDelegate, HdRenderParam* renderParam,
            HdDirtyBits* dirtyBits) override {
    HdCamera::Sync(sceneDelegate, renderParam, dirtyBits);

    auto* pyxisParam = static_cast<HdPyxisRenderParam*>(renderParam);
    pyxis::GpuScene* scene = pyxisParam ? pyxisParam->GetGpuScene() : nullptr;

    if (scene != nullptr && sceneDelegate != nullptr)
    {
      EmitToScene(sceneDelegate, *scene);
    }

    *dirtyBits = HdChangeTracker::Clean;
  }

 private:
  void EmitToScene(HdSceneDelegate* sceneDelegate, pyxis::GpuScene& scene) {
    const SdfPath& primId = GetId();
    const GfMatrix4d worldFromLocal = sceneDelegate->GetTransform(primId);
    const GfMatrix4d viewFromWorld = worldFromLocal.GetInverse();

    pyxis::CameraDesc cameraDesc;
    cameraDesc.viewFromWorld = ToPyxisMatrix(viewFromWorld);
    // M4: identity projection (the StageWalker also has a stub
    // projection — both agree). Real projection wiring at M6 alongside
    // animation; for the byte-equal regression at M4 it's enough that
    // both adapters set IDENTICAL camera state.
    cameraDesc.projFromView = ToPyxisMatrix(ComputeProjectionMatrix());
    cameraDesc.focalLengthMm = GetFocalLength();
    cameraDesc.apertureFStop = GetFStop();
    cameraDesc.focusDistance = GetFocusDistance();
    const GfRange1f clipRange = GetClippingRange();
    cameraDesc.nearClip = clipRange.GetMin();
    cameraDesc.farClip = clipRange.GetMax();
    scene.SetCamera(cameraDesc);
  }
};

class StubMaterial final : public HdMaterial {
 public:
  explicit StubMaterial(SdfPath const& primId) : HdMaterial(primId) {}

  void Sync(HdSceneDelegate* /*sceneDelegate*/, HdRenderParam* /*renderParam*/,
            HdDirtyBits* dirtyBits) override {
    *dirtyBits = HdChangeTracker::Clean;
  }
  [[nodiscard]] HdDirtyBits GetInitialDirtyBitsMask() const override {
    return HdMaterial::AllDirty;
  }
};

class StubLight final : public HdSprim {
 public:
  explicit StubLight(SdfPath const& primId) : HdSprim(primId) {}

  void Sync(HdSceneDelegate* /*sceneDelegate*/, HdRenderParam* /*renderParam*/,
            HdDirtyBits* dirtyBits) override {
    *dirtyBits = HdChangeTracker::Clean;
  }
  [[nodiscard]] HdDirtyBits GetInitialDirtyBitsMask() const override {
    return HdChangeTracker::AllDirty;
  }
};

class StubRenderBuffer final : public HdRenderBuffer {
 public:
  explicit StubRenderBuffer(SdfPath const& primId) : HdRenderBuffer(primId) {}

  bool Allocate(GfVec3i const& /*dimensions*/, HdFormat /*format*/,
                bool /*multiSampled*/) override {
    return true;
  }
  [[nodiscard]] unsigned int GetWidth() const override { return 0; }
  [[nodiscard]] unsigned int GetHeight() const override { return 0; }
  [[nodiscard]] unsigned int GetDepth() const override { return 0; }
  [[nodiscard]] HdFormat GetFormat() const override { return HdFormatInvalid; }
  [[nodiscard]] bool IsMultiSampled() const override { return false; }
  [[nodiscard]] void* Map() override { return nullptr; }
  void Unmap() override {}
  [[nodiscard]] bool IsMapped() const override { return false; }
  void Resolve() override {}
  [[nodiscard]] bool IsConverged() const override { return true; }

 protected:
  void _Deallocate() override {}
};

class StubRenderPass final : public HdRenderPass {
 public:
  StubRenderPass(HdRenderIndex* index, HdRprimCollection const& collection)
      : HdRenderPass(index, collection) {}

 protected:
  void _Execute(HdRenderPassStateSharedPtr const& /*renderPassState*/,
                TfTokenVector const& /*renderTags*/) override {
    // M4 stub: real impl drives PyxisRenderer::RenderFrame against
    // the scene's GpuScene + the bound HdPyxisRenderBuffer AOV
    // textures. Wires in alongside the engine integration.
  }
};

class StubInstancer final : public HdInstancer {
 public:
  StubInstancer(HdSceneDelegate* delegate, SdfPath const& primId)
      : HdInstancer(delegate, primId) {}
};

}  // namespace

HdPyxisRenderDelegate::HdPyxisRenderDelegate()
    : _resourceRegistry(std::make_shared<HdResourceRegistry>()) {}

HdPyxisRenderDelegate::HdPyxisRenderDelegate(HdRenderSettingsMap const& /*settingsMap*/)
    : _resourceRegistry(std::make_shared<HdResourceRegistry>()) {}

HdPyxisRenderDelegate::~HdPyxisRenderDelegate() = default;

const TfTokenVector& HdPyxisRenderDelegate::GetSupportedRprimTypes() const {
  return GetSupportedRprimTypesImpl();
}
const TfTokenVector& HdPyxisRenderDelegate::GetSupportedSprimTypes() const {
  return GetSupportedSprimTypesImpl();
}
const TfTokenVector& HdPyxisRenderDelegate::GetSupportedBprimTypes() const {
  return GetSupportedBprimTypesImpl();
}

HdRenderPassSharedPtr HdPyxisRenderDelegate::CreateRenderPass(
    HdRenderIndex* index, HdRprimCollection const& collection) {
  return std::make_shared<StubRenderPass>(index, collection);
}

HdRenderParam* HdPyxisRenderDelegate::GetRenderParam() const {
  // HydraEngine calls SetEngineState() after constructing the
  // delegate; before that, returns null and Sync impls early-out.
  return _renderParam.get();
}

void HdPyxisRenderDelegate::SetEngineState(pyxis::GpuScene* scene,
                                           pyxis::Profiler* profiler) noexcept {
  _renderParam = std::make_unique<HdPyxisRenderParam>(scene, profiler);
}

HdResourceRegistrySharedPtr HdPyxisRenderDelegate::GetResourceRegistry() const {
  return _resourceRegistry;
}

HdRprim* HdPyxisRenderDelegate::CreateRprim(TfToken const& typeId, SdfPath const& primId) {
  if (typeId == HdPrimTypeTokens->mesh)
    return new HdPyxisMesh(primId);
  TF_CODING_ERROR("Unknown Rprim type %s", typeId.GetText());
  return nullptr;
}
void HdPyxisRenderDelegate::DestroyRprim(HdRprim* rprim) { delete rprim; }

HdSprim* HdPyxisRenderDelegate::CreateSprim(TfToken const& typeId, SdfPath const& primId) {
  if (typeId == HdPrimTypeTokens->camera)
    return new HdPyxisCamera(primId);
  if (typeId == HdPrimTypeTokens->material)
    return new StubMaterial(primId);
  if (typeId == HdPrimTypeTokens->distantLight || typeId == HdPrimTypeTokens->domeLight
      || typeId == HdPrimTypeTokens->rectLight)
  {
    return new StubLight(primId);
  }
  TF_CODING_ERROR("Unknown Sprim type %s", typeId.GetText());
  return nullptr;
}
HdSprim* HdPyxisRenderDelegate::CreateFallbackSprim(TfToken const& typeId) {
  // Return a generic Sprim of the requested type so missing material
  // bindings don't crash Hydra's render-index walks.
  if (typeId == HdPrimTypeTokens->camera)
    return new HdPyxisCamera(SdfPath::EmptyPath());
  if (typeId == HdPrimTypeTokens->material)
    return new StubMaterial(SdfPath::EmptyPath());
  return new StubLight(SdfPath::EmptyPath());
}
void HdPyxisRenderDelegate::DestroySprim(HdSprim* sprim) { delete sprim; }

HdBprim* HdPyxisRenderDelegate::CreateBprim(TfToken const& typeId, SdfPath const& primId) {
  if (typeId == HdPrimTypeTokens->renderBuffer)
    return new StubRenderBuffer(primId);
  TF_CODING_ERROR("Unknown Bprim type %s", typeId.GetText());
  return nullptr;
}
HdBprim* HdPyxisRenderDelegate::CreateFallbackBprim(TfToken const& typeId) {
  if (typeId == HdPrimTypeTokens->renderBuffer)
    return new StubRenderBuffer(SdfPath::EmptyPath());
  return nullptr;
}
void HdPyxisRenderDelegate::DestroyBprim(HdBprim* bprim) { delete bprim; }

HdInstancer* HdPyxisRenderDelegate::CreateInstancer(HdSceneDelegate* delegate,
                                                    SdfPath const& primId) {
  return new StubInstancer(delegate, primId);
}
void HdPyxisRenderDelegate::DestroyInstancer(HdInstancer* instancer) { delete instancer; }

void HdPyxisRenderDelegate::CommitResources(HdChangeTracker* /*tracker*/) {
  // M4 stub: real impl drains GpuScene pending mutations + builds
  // BLAS/TLAS via PyxisRenderer's commit pipeline.
}

PXR_NAMESPACE_CLOSE_SCOPE
