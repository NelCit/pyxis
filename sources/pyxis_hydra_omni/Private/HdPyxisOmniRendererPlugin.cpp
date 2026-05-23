// Pyxis Omniverse Hydra delegate — plugin registration. RFC 0004 Stage 3.
//
// Registers HdPyxisOmniRendererPlugin with the Hydra renderer-plugin registry
// so a Kit viewport (or usdview) discovers "Pyxis" via plugInfo.json and lets
// the user select it as the active renderer.

#include "HdPyxisOmniRenderDelegate.h"

#include <pxr/imaging/hd/rendererPlugin.h>
#include <pxr/imaging/hd/rendererPluginRegistry.h>
#include <pxr/pxr.h>

PXR_NAMESPACE_OPEN_SCOPE

class HdPyxisOmniRendererPlugin final : public HdRendererPlugin {
 public:
  HdPyxisOmniRendererPlugin() = default;
  ~HdPyxisOmniRendererPlugin() override = default;

  HdPyxisOmniRendererPlugin(const HdPyxisOmniRendererPlugin&) = delete;
  HdPyxisOmniRendererPlugin& operator=(const HdPyxisOmniRendererPlugin&) = delete;

  HdRenderDelegate* CreateRenderDelegate() override { return new HdPyxisOmniRenderDelegate(); }

  HdRenderDelegate* CreateRenderDelegate(HdRenderSettingsMap const& settingsMap) override {
    return new HdPyxisOmniRenderDelegate(settingsMap);
  }

  void DeleteRenderDelegate(HdRenderDelegate* renderDelegate) override {
    delete renderDelegate;
  }

  // nv-usd 25.11 uses NVIDIA's extended signature (HdRendererCreateArgs +
  // reasonWhyNot) rather than stock USD's `bool gpuEnabled`. Pyxis is GPU-only
  // and always reports supported here; the device-UUID / external-memory check
  // (RFC 0004 §4) happens when the delegate actually creates its NVRHI device.
  bool IsSupported(HdRendererCreateArgs const& /*rendererCreateArgs*/,
                   std::string* /*reasonWhyNot*/) const override {
    return true;
  }
};

TF_REGISTRY_FUNCTION(TfType) {
  HdRendererPluginRegistry::Define<HdPyxisOmniRendererPlugin>();
}

PXR_NAMESPACE_CLOSE_SCOPE
