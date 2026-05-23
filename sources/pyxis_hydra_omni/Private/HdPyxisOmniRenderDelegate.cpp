// Pyxis Omniverse Hydra delegate — RFC 0004 Stage 3 (C1 toolchain-proof).

#include "HdPyxisOmniRenderDelegate.h"

#include <pxr/imaging/hd/bprim.h>
#include <pxr/imaging/hd/camera.h>
#include <pxr/imaging/hd/instancer.h>
#include <pxr/imaging/hd/mesh.h>
#include <pxr/imaging/hd/resourceRegistry.h>
#include <pxr/imaging/hd/rprim.h>
#include <pxr/imaging/hd/sprim.h>
#include <pxr/imaging/hd/tokens.h>

PXR_NAMESPACE_OPEN_SCOPE

// ---- Render pass ---------------------------------------------------------

HdPyxisOmniRenderPass::HdPyxisOmniRenderPass(HdRenderIndex* index,
                                             HdRprimCollection const& collection)
    : HdRenderPass(index, collection) {}

HdPyxisOmniRenderPass::~HdPyxisOmniRenderPass() = default;

void HdPyxisOmniRenderPass::_Execute(HdRenderPassStateSharedPtr const& /*renderPassState*/,
                                     TfTokenVector const& /*renderTags*/) {
  // C1: no-op. The real pass drives PyxisRenderer::RenderFrame into the
  // imported Kit render buffer (RFC 0004 §4), waiting on the exported timeline
  // semaphore before signalling the buffer is ready.
}

// ---- Render delegate -----------------------------------------------------

HdPyxisOmniRenderDelegate::HdPyxisOmniRenderDelegate() { _Initialize(); }

HdPyxisOmniRenderDelegate::HdPyxisOmniRenderDelegate(HdRenderSettingsMap const& settingsMap)
    : HdRenderDelegate(settingsMap) {
  _Initialize();
}

HdPyxisOmniRenderDelegate::~HdPyxisOmniRenderDelegate() = default;

void HdPyxisOmniRenderDelegate::_Initialize() {
  _resourceRegistry = std::make_shared<HdResourceRegistry>();
  // C1 advertises only the prim types we will wire next; geometry/material/
  // light adapters arrive with the FSD conformance pass (RFC 0004 §3). Keeping
  // the lists explicit (not empty) makes the delegate select cleanly in the
  // Kit viewport's renderer list.
  _supportedRprimTypes = {HdPrimTypeTokens->mesh};
  _supportedSprimTypes = {HdPrimTypeTokens->camera};
  _supportedBprimTypes = {HdPrimTypeTokens->renderBuffer};
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

HdRenderParam* HdPyxisOmniRenderDelegate::GetRenderParam() const { return nullptr; }

HdResourceRegistrySharedPtr HdPyxisOmniRenderDelegate::GetResourceRegistry() const {
  return _resourceRegistry;
}

HdRenderPassSharedPtr HdPyxisOmniRenderDelegate::CreateRenderPass(
    HdRenderIndex* index, HdRprimCollection const& collection) {
  return HdRenderPassSharedPtr(new HdPyxisOmniRenderPass(index, collection));
}

HdInstancer* HdPyxisOmniRenderDelegate::CreateInstancer(HdSceneDelegate* /*delegate*/,
                                                        SdfPath const& /*id*/) {
  return nullptr;
}
void HdPyxisOmniRenderDelegate::DestroyInstancer(HdInstancer* instancer) { delete instancer; }

HdRprim* HdPyxisOmniRenderDelegate::CreateRprim(TfToken const& /*typeId*/,
                                                SdfPath const& /*rprimId*/) {
  // C1: no Rprim wrapper yet — returns null. The mesh adapter (GpuScene
  // ingest) lands next.
  return nullptr;
}
void HdPyxisOmniRenderDelegate::DestroyRprim(HdRprim* rprim) { delete rprim; }

HdSprim* HdPyxisOmniRenderDelegate::CreateSprim(TfToken const& /*typeId*/,
                                                SdfPath const& /*sprimId*/) {
  return nullptr;
}
HdSprim* HdPyxisOmniRenderDelegate::CreateFallbackSprim(TfToken const& /*typeId*/) {
  return nullptr;
}
void HdPyxisOmniRenderDelegate::DestroySprim(HdSprim* sprim) { delete sprim; }

HdBprim* HdPyxisOmniRenderDelegate::CreateBprim(TfToken const& /*typeId*/,
                                                SdfPath const& /*bprimId*/) {
  return nullptr;
}
HdBprim* HdPyxisOmniRenderDelegate::CreateFallbackBprim(TfToken const& /*typeId*/) {
  return nullptr;
}
void HdPyxisOmniRenderDelegate::DestroyBprim(HdBprim* bprim) { delete bprim; }

void HdPyxisOmniRenderDelegate::CommitResources(HdChangeTracker* /*tracker*/) {
  // C1: no-op. Real impl drains GpuScene uploads + builds BLAS/TLAS (RFC §K).
}

PXR_NAMESPACE_CLOSE_SCOPE
