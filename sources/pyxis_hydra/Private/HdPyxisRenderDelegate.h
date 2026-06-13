// Pyxis Hydra — HdRenderDelegate (private — only the plugin entry from Public/
// is reachable by Hydra hosts).
//
// Plan §7 / RFC 0006. The single Pyxis Hydra delegate, loaded as a USD plugin by
// any HdEngine host (usdview, Omniverse Kit). It OWNS a PyxisEngine (its own
// Pyxis Vulkan device + GpuScene + PyxisRenderer + GpuInteropExporter); prim
// Sync impls push to the shared GpuScene via HdPyxisRenderParam, and the render
// pass drives PyxisEngine::RenderFrame, compositing into the host's color AOV.
//
// (RFC 0006 collapsed the former pyxis_hydra_omni delegate fork onto this source
// once both adapters shared Kit's nv-usd — the ABI split that forced the fork.)

#pragma once

#include <pxr/imaging/hd/renderDelegate.h>
#include <pxr/pxr.h>
#include <pxr/usd/usd/stage.h>  // UsdStageWeakPtr (SetStage — §25.O.3 shared FromUsdShade)

#include <memory>

namespace pyxis::hydra {
class PyxisEngine;
}

PXR_NAMESPACE_OPEN_SCOPE

class HdPyxisRenderParam;
class Hgi;  // host render driver (captured in SetDrivers; RFC 0008 GL-interop AOV)

// HdPyxisRenderPass (drives PyxisEngine::RenderFrame + composites to the host's
// color AOV) is private to the .cpp — CreateRenderPass returns it as the base
// HdRenderPassSharedPtr, so it needs no declaration here.

class HdPyxisRenderDelegate final : public HdRenderDelegate {
 public:
  HdPyxisRenderDelegate();
  explicit HdPyxisRenderDelegate(HdRenderSettingsMap const& settingsMap);
  ~HdPyxisRenderDelegate() override;

  HdPyxisRenderDelegate(const HdPyxisRenderDelegate&) = delete;
  HdPyxisRenderDelegate& operator=(const HdPyxisRenderDelegate&) = delete;

  // Advertise our render settings so the pxr engine bridges carb <-> the
  // delegate's Get/SetRenderSetting using these descriptors. WITHOUT this the
  // carb-stamped values (the Render Settings panel / __init__.py) never reach
  // GetRenderSetting, so pyxis:persistEngine / pyxis:stageToken stay at their
  // defaults. (RFC 0006; mirrors how Synopsys/ANSYS AVxcelerate advertises.)
  [[nodiscard]] HdRenderSettingDescriptorList GetRenderSettingDescriptors() const override;

  // The pxr engine calls this when the Render Settings panel toggle changes
  // (carb -> here). Override only to make the change OBSERVABLE (PYXIS_OMNI_DBG)
  // and to store via the base impl; the persist value itself is re-read at
  // teardown so the toggle frees/keeps the resident engine on the next switch.
  void SetRenderSetting(TfToken const& key, VtValue const& value) override;

  // ---- Supported-types reports -------------------------------------
  [[nodiscard]] const TfTokenVector& GetSupportedRprimTypes() const override;
  [[nodiscard]] const TfTokenVector& GetSupportedSprimTypes() const override;
  [[nodiscard]] const TfTokenVector& GetSupportedBprimTypes() const override;

  // Advertise which AOVs Pyxis can fill. WITHOUT this, HdxTaskController (the
  // viewport's render-task setup, incl. omni.hydra.pxr) thinks the delegate has
  // no renderable AOVs, so it creates NO render buffers and binds NO AOVs — the
  // render pass then has nowhere to composite and the viewport is black. (§7.)
  [[nodiscard]] HdAovDescriptor GetDefaultAovDescriptor(TfToken const& name) const override;

  // ---- Render-pass / RenderParam lifecycle -------------------------
  [[nodiscard]] HdRenderPassSharedPtr CreateRenderPass(HdRenderIndex* index,
                                                       HdRprimCollection const& collection) override;
  [[nodiscard]] HdRenderParam* GetRenderParam() const override;

  [[nodiscard]] HdResourceRegistrySharedPtr GetResourceRegistry() const override;

  // Host hands us its HdDriver list (incl. the Hgi render driver) here. We capture
  // the Hgi so we can probe its backend (Vulkan/GL) for the direct-AOV question.
  void SetDrivers(HdDriverVector const& drivers) override;

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

  // Borrowed — the active engine the render pass drives + prims push to. May be
  // null if device/interop init failed. When the engine is persisted
  // process-wide (the pxr engine destroys/recreates this delegate on every
  // viewport renderer switch — see RFC 0006 "Persistent engine across pxr
  // renderer switches"), this returns the borrowed pointer into the static
  // holder; otherwise it returns the delegate-owned engine.
  [[nodiscard]] pyxis::hydra::PyxisEngine* Engine() const noexcept { return _engine; }

  // Supply the stage's unit/up-axis metadata so prim, light, and camera
  // transforms are baked into the renderer's metres + Y-up frame (matching
  // pyxis_usd_ingest's StageWalker). Hosts call this from SetStage; without it
  // the correction is identity (raw stage units), which leaves the World Lobby
  // in centimetres and breaks the closesthit's metre-calibrated dome-AO /
  // inverse-square constants (interior renders blown-out or too dark). Builds
  // the same matrix as StageWalker::BuildStageContext. §25.O.3 parity.
  void SetStageToWorld(double metersPerUnit, bool stageIsZUp) noexcept;

  // §25.O.3 — supply the open UsdStage so material Sync resolves the bound
  // material prim and runs it through the SHARED FromUsdShade (the same path as
  // pyxis_usd_ingest's StageWalker), giving byte-identical MDL / MaterialX /
  // UsdPreviewSurface translation across both ingest adapters. Hosts call this
  // from SetStage. Without it the delegate falls back to reading Hydra's
  // flattened material network (UsdPreviewSurface only — loses MDL diffuse tints).
  void SetStage(const UsdStageRefPtr& stage) noexcept;

  // Q3 OpenPBR-complete — compose RenderSettings::openPbrFeatureMask from the
  // six pyxis:openpbr* BOOL render settings (Kit Render Settings panel rows;
  // carb-bridged like pyxis:persistEngine). Absent / unknown-typed settings
  // default to ON, so a bare usdview run (no carb) gets the all-on reference
  // look (0x3F) and §25.O.3 parity holds. Called per-frame by the render pass
  // (NOT once at Init — carb propagation timing makes Init-time reads stale)
  // and pushed into the engine via PyxisEngine::SetOpenPbrFeatureMask.
  [[nodiscard]] uint32_t ReadOpenPbrFeatureMask() const;

  // Auto-exposure render settings (pyxis:autoExposure / pyxis:autoExposureKey),
  // re-read per-frame by the render pass (same rationale as the OpenPBR gates)
  // and pushed into the engine via PyxisEngine::SetAutoExposure. Default
  // OFF / 0.18 so a bare usdview run keeps the parity-stable reference look.
  [[nodiscard]] bool ReadAutoExposure() const;
  [[nodiscard]] float ReadAutoExposureKey() const;

 private:
  void Init();
  // Resolve pyxis:persistEngine (render setting, default true) with the
  // PYXIS_OMNI_NO_PERSIST env override (forces false). USD-native — no carb dep.
  [[nodiscard]] bool ReadPersistEngineSetting() const;

  HdResourceRegistrySharedPtr _resourceRegistry;
  TfTokenVector _supportedRprimTypes;
  TfTokenVector _supportedSprimTypes;
  TfTokenVector _supportedBprimTypes;
  HdRenderSettingDescriptorList _settingDescriptors;

  // The engine the render pass drives + prims push to. Either:
  //   - persist ON  -> _engine BORROWS the process-static holder's engine
  //                    (_ownedEngine stays null, _ownsEngine == false). The
  //                    holder keeps it resident across delegate destroy/recreate
  //                    so switch-back reuses warm textures/geometry/BLAS.
  //   - persist OFF -> _ownedEngine owns it (today's behaviour); _engine aliases
  //                    _ownedEngine.get() (_ownsEngine == true).
  std::unique_ptr<pyxis::hydra::PyxisEngine> _ownedEngine;  // non-persist only
  pyxis::hydra::PyxisEngine* _engine = nullptr;             // active (borrowed or owned)
  bool _ownsEngine = false;
  bool _persistEngine = true;
  std::unique_ptr<HdPyxisRenderParam> _renderParam;
  // Host Hgi (borrowed; captured in SetDrivers). When it is HgiGL, the color render
  // buffer can expose a GL texture via GetResource() and the render pass feeds it
  // from the exported image (RFC 0008), skipping the CPU readback. Null otherwise.
  Hgi* _hgi = nullptr;
};

PXR_NAMESPACE_CLOSE_SCOPE
