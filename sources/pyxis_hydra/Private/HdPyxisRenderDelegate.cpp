// Pyxis Hydra — HdRenderDelegate stub.

#include "HdPyxisRenderDelegate.h"

#include <pxr/imaging/hd/mesh.h>
#include <pxr/imaging/hd/camera.h>
#include <pxr/imaging/hd/material.h>
#include <pxr/imaging/hd/renderBuffer.h>
#include <pxr/imaging/hd/renderPass.h>
#include <pxr/imaging/hd/resourceRegistry.h>
#include <pxr/imaging/hd/tokens.h>
#include <pxr/imaging/hd/instancer.h>

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
// Stub Rprim / Sprim / Bprim. Each accepts the Hydra Sync calls but
// no-ops at M4 — the real translation to GpuScene mutations lands in
// follow-up commits on this milestone branch (mesh sync first, then
// camera, then lights/material/renderBuffer).
// -------------------------------------------------------------------

class StubMesh final : public HdMesh {
 public:
  explicit StubMesh(SdfPath const& primId) : HdMesh(primId) {}

  void Sync(HdSceneDelegate* /*sceneDelegate*/, HdRenderParam* /*renderParam*/,
            HdDirtyBits* dirtyBits, TfToken const& /*reprToken*/) override {
    // Clear all bits so the change tracker doesn't loop. Real mesh
    // ingest reads positions / topology / material binding here and
    // calls into GpuScene::CreateMesh + AppendInstance.
    *dirtyBits = HdChangeTracker::Clean;
  }
  [[nodiscard]] HdDirtyBits GetInitialDirtyBitsMask() const override {
    return HdChangeTracker::AllSceneDirtyBits;
  }

 protected:
  HdDirtyBits _PropagateDirtyBits(HdDirtyBits bits) const override { return bits; }
  void _InitRepr(TfToken const& /*reprToken*/, HdDirtyBits* /*dirtyBits*/) override {}
};

class StubCamera final : public HdCamera {
 public:
  explicit StubCamera(SdfPath const& primId) : HdCamera(primId) {}

  void Sync(HdSceneDelegate* sceneDelegate, HdRenderParam* renderParam,
            HdDirtyBits* dirtyBits) override {
    HdCamera::Sync(sceneDelegate, renderParam, dirtyBits);
    *dirtyBits = HdChangeTracker::Clean;
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
  // M4 stub: GpuScene*/Profiler* live elsewhere (HydraEngine wires
  // them); the delegate's RenderParam is null at this milestone.
  // Real RenderParam lands when Sync impls need to reach the renderer.
  return nullptr;
}

HdResourceRegistrySharedPtr HdPyxisRenderDelegate::GetResourceRegistry() const {
  return _resourceRegistry;
}

HdRprim* HdPyxisRenderDelegate::CreateRprim(TfToken const& typeId, SdfPath const& primId) {
  if (typeId == HdPrimTypeTokens->mesh)
    return new StubMesh(primId);
  TF_CODING_ERROR("Unknown Rprim type %s", typeId.GetText());
  return nullptr;
}
void HdPyxisRenderDelegate::DestroyRprim(HdRprim* rprim) { delete rprim; }

HdSprim* HdPyxisRenderDelegate::CreateSprim(TfToken const& typeId, SdfPath const& primId) {
  if (typeId == HdPrimTypeTokens->camera)
    return new StubCamera(primId);
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
    return new StubCamera(SdfPath::EmptyPath());
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
