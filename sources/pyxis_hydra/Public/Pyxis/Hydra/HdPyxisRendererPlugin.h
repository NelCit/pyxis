// Pyxis Hydra — HdRendererPlugin entry point.
//
// Plan §7 / §25.A. The single public symbol of pyxis_hydra; usdview
// (and any other Hydra host) discovers Pyxis as a render delegate
// through the plugInfo.json descriptor that registers
// `HdPyxisRendererPlugin` as an `HdRendererPlugin` subclass.
//
// Implementation is private (Private/HdPyxisRendererPlugin.cpp); this
// header exposes nothing the host actually calls — it just declares
// the type so the matching plugInfo.json's `bases` reference resolves.

#pragma once

#include <Pyxis/Hydra/HydraApi.h>

#include <pxr/imaging/hd/rendererPlugin.h>

PXR_NAMESPACE_OPEN_SCOPE

class PYXIS_HYDRA_API HdPyxisRendererPlugin final : public HdRendererPlugin {
 public:
  HdPyxisRendererPlugin();
  ~HdPyxisRendererPlugin() override;

  HdPyxisRendererPlugin(const HdPyxisRendererPlugin&) = delete;
  HdPyxisRendererPlugin& operator=(const HdPyxisRendererPlugin&) = delete;

  [[nodiscard]] HdRenderDelegate* CreateRenderDelegate() override;
  [[nodiscard]] HdRenderDelegate* CreateRenderDelegate(
      HdRenderSettingsMap const& settingsMap) override;
  void DeleteRenderDelegate(HdRenderDelegate* renderDelegate) override;

  // USD 26.x added a Hydra-2.0-shaped pure virtual that takes
  // HdRendererCreateArgs (a SceneIndex-style "what's available" bag)
  // and an out-param explanation. The legacy `bool gpuEnabled`
  // overload still exists with a default impl that delegates to this
  // one.
  [[nodiscard]] bool IsSupported(HdRendererCreateArgs const& rendererCreateArgs,
                                 std::string* reasonWhyNot = nullptr) const override;
};

PXR_NAMESPACE_CLOSE_SCOPE
