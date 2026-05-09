// Pyxis Hydra — HdRenderDelegate (private — only the plugin entry
// from Public/ is reachable by Hydra hosts).
//
// Plan §7. Reports supported Rprim / Sprim / Bprim types and
// constructs the Pyxis-side prim wrappers that translate Hydra's
// dirty-bit-driven Sync calls into GpuScene mutations.
//
// M4 stub level: registration + supported-types report + Sync
// no-ops on every prim type (mesh / camera / light / material /
// renderBuffer). Real Sync impls land progressively as the §24
// dirty-bit mapping is wired in subsequent commits on this branch.

#pragma once

#include <pxr/imaging/hd/renderDelegate.h>

#include <memory>

namespace pyxis {
class GpuScene;
class Profiler;
}  // namespace pyxis

PXR_NAMESPACE_OPEN_SCOPE

class HdPyxisRenderParam;

class HdPyxisRenderDelegate final : public HdRenderDelegate {
 public:
  HdPyxisRenderDelegate();
  explicit HdPyxisRenderDelegate(HdRenderSettingsMap const& settingsMap);
  ~HdPyxisRenderDelegate() override;

  HdPyxisRenderDelegate(const HdPyxisRenderDelegate&) = delete;
  HdPyxisRenderDelegate& operator=(const HdPyxisRenderDelegate&) = delete;

  // Pyxis-specific: HydraEngine calls this once after constructing the
  // delegate to wire the per-render state (GpuScene + Profiler the
  // Sync impls reach through). Sync impls cast the HdRenderParam* to
  // HdPyxisRenderParam* and read the borrowed pointers. Both args
  // must outlive every HdEngine::Execute call against this delegate.
  void SetEngineState(pyxis::GpuScene* scene, pyxis::Profiler* profiler) noexcept;

  // ---- Supported-types reports -------------------------------------
  [[nodiscard]] const TfTokenVector& GetSupportedRprimTypes() const override;
  [[nodiscard]] const TfTokenVector& GetSupportedSprimTypes() const override;
  [[nodiscard]] const TfTokenVector& GetSupportedBprimTypes() const override;

  // ---- Render-pass / RenderParam lifecycle -------------------------
  [[nodiscard]] HdRenderPassSharedPtr CreateRenderPass(HdRenderIndex* index,
                                                       HdRprimCollection const& collection) override;
  [[nodiscard]] HdRenderParam* GetRenderParam() const override;

  [[nodiscard]] HdResourceRegistrySharedPtr GetResourceRegistry() const override;

  // ---- Rprim / Sprim / Bprim factories -----------------------------
  [[nodiscard]] HdRprim* CreateRprim(TfToken const& typeId, SdfPath const& primId) override;
  void DestroyRprim(HdRprim* rprim) override;

  [[nodiscard]] HdSprim* CreateSprim(TfToken const& typeId, SdfPath const& sprimId) override;
  [[nodiscard]] HdSprim* CreateFallbackSprim(TfToken const& typeId) override;
  void DestroySprim(HdSprim* sprim) override;

  [[nodiscard]] HdBprim* CreateBprim(TfToken const& typeId, SdfPath const& bprimId) override;
  [[nodiscard]] HdBprim* CreateFallbackBprim(TfToken const& typeId) override;
  void DestroyBprim(HdBprim* bprim) override;

  // ---- Instancer factory -------------------------------------------
  [[nodiscard]] HdInstancer* CreateInstancer(HdSceneDelegate* delegate,
                                             SdfPath const& primId) override;
  void DestroyInstancer(HdInstancer* instancer) override;

  // ---- Frame boundary ----------------------------------------------
  void CommitResources(HdChangeTracker* tracker) override;

 private:
  HdResourceRegistrySharedPtr _resourceRegistry;
  std::unique_ptr<HdPyxisRenderParam> _renderParam;
};

PXR_NAMESPACE_CLOSE_SCOPE
