// Pyxis Hydra — HdRendererPlugin registration.
//
// Hooks the Pyxis delegate into the Hd plugin registry so usdview /
// any Hydra host discovers it via plugInfo.json. Implementation is
// the minimum viable shape at M4: returns a real
// HdPyxisRenderDelegate that creates the supported Rprim/Sprim/Bprim
// types but renders into the supplied AOVs as M3-cube-fallback
// pixels until M5+ wires the OpenPBR closesthit branch.

#include "Pyxis/Hydra/HdPyxisRendererPlugin.h"

#include "HdPyxisRenderDelegate.h"

#include <pxr/imaging/hd/rendererPluginRegistry.h>

PXR_NAMESPACE_OPEN_SCOPE

TF_REGISTRY_FUNCTION(TfType) {
  HdRendererPluginRegistry::Define<HdPyxisRendererPlugin>();
}

HdPyxisRendererPlugin::HdPyxisRendererPlugin() = default;
HdPyxisRendererPlugin::~HdPyxisRendererPlugin() = default;

HdRenderDelegate* HdPyxisRendererPlugin::CreateRenderDelegate() {
  return new HdPyxisRenderDelegate();
}

HdRenderDelegate* HdPyxisRendererPlugin::CreateRenderDelegate(
    HdRenderSettingsMap const& settingsMap) {
  return new HdPyxisRenderDelegate(settingsMap);
}

void HdPyxisRendererPlugin::DeleteRenderDelegate(HdRenderDelegate* renderDelegate) {
  delete renderDelegate;
}

bool HdPyxisRendererPlugin::IsSupported(HdRendererCreateArgs const& /*rendererCreateArgs*/,
                                        std::string* /*reasonWhyNot*/) const {
  // M4 stub: report supported unconditionally; device-creation
  // failure surfaces later if the host actually tries to render.
  // Real impl inspects rendererCreateArgs (HdRendererCreateArgsSchema)
  // to confirm a Vulkan-capable, RT-capable adapter is available.
  return true;
}

PXR_NAMESPACE_CLOSE_SCOPE
