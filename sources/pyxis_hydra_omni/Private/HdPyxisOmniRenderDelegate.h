// Pyxis Omniverse Hydra delegate — RFC 0004 Stage 3.
//
// Mirror of sources/pyxis_hydra (the usdview/desktop delegate) but built
// out-of-tree against Kit's nv-usd 25.11 (Packman) instead of vcpkg USD 26.3.
// This first cut is the "C1" toolchain-proof milestone: a real
// HdRenderDelegate subclass + plugin registration that compiles and links
// against nv-usd 25.11, loads in a Kit viewport, and appears in the renderer
// list. The FSD prim adapters + GpuInteropImporter wiring land on top of this.

#pragma once

#include <pxr/imaging/hd/renderDelegate.h>
#include <pxr/imaging/hd/renderPass.h>
#include <pxr/pxr.h>

#include <memory>

PXR_NAMESPACE_OPEN_SCOPE

// Minimal render pass — the real one drives PyxisRenderer::RenderFrame against
// the imported Kit render buffer (RFC 0004 §4). C1: a no-op execute so the
// delegate is loadable and selectable.
class HdPyxisOmniRenderPass final : public HdRenderPass {
 public:
  HdPyxisOmniRenderPass(HdRenderIndex* index, HdRprimCollection const& collection);
  ~HdPyxisOmniRenderPass() override;

 protected:
  void _Execute(HdRenderPassStateSharedPtr const& renderPassState,
                TfTokenVector const& renderTags) override;
};

class HdPyxisOmniRenderDelegate final : public HdRenderDelegate {
 public:
  HdPyxisOmniRenderDelegate();
  explicit HdPyxisOmniRenderDelegate(HdRenderSettingsMap const& settingsMap);
  ~HdPyxisOmniRenderDelegate() override;

  HdPyxisOmniRenderDelegate(const HdPyxisOmniRenderDelegate&) = delete;
  HdPyxisOmniRenderDelegate& operator=(const HdPyxisOmniRenderDelegate&) = delete;

  [[nodiscard]] const TfTokenVector& GetSupportedRprimTypes() const override;
  [[nodiscard]] const TfTokenVector& GetSupportedSprimTypes() const override;
  [[nodiscard]] const TfTokenVector& GetSupportedBprimTypes() const override;

  [[nodiscard]] HdRenderParam* GetRenderParam() const override;
  [[nodiscard]] HdResourceRegistrySharedPtr GetResourceRegistry() const override;

  [[nodiscard]] HdRenderPassSharedPtr CreateRenderPass(
      HdRenderIndex* index, HdRprimCollection const& collection) override;

  [[nodiscard]] HdInstancer* CreateInstancer(HdSceneDelegate* delegate,
                                             SdfPath const& id) override;
  void DestroyInstancer(HdInstancer* instancer) override;

  [[nodiscard]] HdRprim* CreateRprim(TfToken const& typeId, SdfPath const& rprimId) override;
  void DestroyRprim(HdRprim* rprim) override;

  [[nodiscard]] HdSprim* CreateSprim(TfToken const& typeId, SdfPath const& sprimId) override;
  [[nodiscard]] HdSprim* CreateFallbackSprim(TfToken const& typeId) override;
  void DestroySprim(HdSprim* sprim) override;

  [[nodiscard]] HdBprim* CreateBprim(TfToken const& typeId, SdfPath const& bprimId) override;
  [[nodiscard]] HdBprim* CreateFallbackBprim(TfToken const& typeId) override;
  void DestroyBprim(HdBprim* bprim) override;

  void CommitResources(HdChangeTracker* tracker) override;

 private:
  void _Initialize();

  HdResourceRegistrySharedPtr _resourceRegistry;
  TfTokenVector _supportedRprimTypes;
  TfTokenVector _supportedSprimTypes;
  TfTokenVector _supportedBprimTypes;
};

PXR_NAMESPACE_CLOSE_SCOPE
