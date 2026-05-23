// Pyxis Omniverse Hydra delegate — HdRenderParam. RFC 0004 C4.
//
// Carries borrowed pointers to the shared GpuScene + Profiler (owned by the
// delegate's PyxisEngine) so each prim's Sync impl can translate Hydra
// dirty-bits into GpuScene mutations without finding them itself. Mirrors
// sources/pyxis_hydra's HdPyxisRenderParam.

#pragma once

#include <pxr/imaging/hd/renderDelegate.h>

namespace pyxis {
class GpuScene;
class Profiler;
}  // namespace pyxis

PXR_NAMESPACE_OPEN_SCOPE

class HdPyxisOmniRenderParam final : public HdRenderParam {
 public:
  HdPyxisOmniRenderParam(pyxis::GpuScene* scene, pyxis::Profiler* profiler) noexcept
      : _scene(scene), _profiler(profiler) {}
  ~HdPyxisOmniRenderParam() override = default;

  HdPyxisOmniRenderParam(const HdPyxisOmniRenderParam&) = delete;
  HdPyxisOmniRenderParam& operator=(const HdPyxisOmniRenderParam&) = delete;

  [[nodiscard]] pyxis::GpuScene* GetGpuScene() const noexcept { return _scene; }
  [[nodiscard]] pyxis::Profiler* GetProfiler() const noexcept { return _profiler; }

 private:
  pyxis::GpuScene* _scene;
  pyxis::Profiler* _profiler;
};

PXR_NAMESPACE_CLOSE_SCOPE
