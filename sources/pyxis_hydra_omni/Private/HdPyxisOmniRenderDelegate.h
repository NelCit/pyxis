// Pyxis Omniverse Hydra delegate — RFC 0004 Stage 3.
//
// Mirror of sources/pyxis_hydra (the usdview/desktop delegate) built out-of-tree
// against Kit's nv-usd 25.11. The delegate owns a PyxisEngine (its own Pyxis
// Vulkan device + GpuScene + PyxisRenderer + GpuInteropExporter); prim Sync impls
// push to the shared GpuScene via HdPyxisOmniRenderParam, and the render pass
// drives PyxisEngine::RenderFrame.

#pragma once

#include <pxr/imaging/hd/renderDelegate.h>
#include <pxr/imaging/hd/renderPass.h>
#include <pxr/pxr.h>

#include <memory>

namespace pyxis_omni {
class PyxisEngine;
}

PXR_NAMESPACE_OPEN_SCOPE

class HdPyxisOmniRenderParam;

// Drives PyxisEngine::RenderFrame (which commits the synced GpuScene + renders
// into the exportable image + signals the timeline). RFC 0004 §4.
class HdPyxisOmniRenderPass final : public HdRenderPass {
 public:
  HdPyxisOmniRenderPass(HdRenderIndex* index, HdRprimCollection const& collection,
                        pyxis_omni::PyxisEngine* engine);
  ~HdPyxisOmniRenderPass() override;

 protected:
  void _Execute(HdRenderPassStateSharedPtr const& renderPassState,
                TfTokenVector const& renderTags) override;

 private:
  pyxis_omni::PyxisEngine* _engine = nullptr;  // borrowed; owned by the delegate.
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

  // Borrowed — the engine the render pass drives + prims push to. May be null
  // if device/interop init failed.
  [[nodiscard]] pyxis_omni::PyxisEngine* Engine() const noexcept { return _engine.get(); }

 private:
  void _Initialize();

  HdResourceRegistrySharedPtr _resourceRegistry;
  TfTokenVector _supportedRprimTypes;
  TfTokenVector _supportedSprimTypes;
  TfTokenVector _supportedBprimTypes;
  std::unique_ptr<pyxis_omni::PyxisEngine> _engine;
  std::unique_ptr<HdPyxisOmniRenderParam> _renderParam;
};

PXR_NAMESPACE_CLOSE_SCOPE
