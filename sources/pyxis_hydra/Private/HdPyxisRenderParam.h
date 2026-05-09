// Pyxis Hydra — HdRenderParam carrying the shared per-render state.
//
// Plan §7. The HdRenderParam Hydra hands to every Sync call. Holds
// borrowed pointers to the renderer-side GpuScene + Profiler so each
// HdPyxisMesh / HdPyxisCamera Sync impl can translate dirty-bits into
// GpuScene mutations + GPU-side profiler scopes without each prim
// having to find them itself.
//
// Lifetime: the HydraEngine constructs the param once + hands ownership
// to HdPyxisRenderDelegate, which returns it from GetRenderParam(). Both
// pointers are non-owning views into objects whose lifetime is bounded
// by the HydraEngine instance.

#pragma once

#include <pxr/imaging/hd/renderDelegate.h>

namespace pyxis {
class GpuScene;
class Profiler;
}  // namespace pyxis

PXR_NAMESPACE_OPEN_SCOPE

class HdPyxisRenderParam final : public HdRenderParam {
 public:
  HdPyxisRenderParam(pyxis::GpuScene* scene, pyxis::Profiler* profiler) noexcept
      : _scene(scene), _profiler(profiler) {}
  ~HdPyxisRenderParam() override = default;

  HdPyxisRenderParam(const HdPyxisRenderParam&) = delete;
  HdPyxisRenderParam& operator=(const HdPyxisRenderParam&) = delete;

  [[nodiscard]] pyxis::GpuScene* GetGpuScene() const noexcept { return _scene; }
  [[nodiscard]] pyxis::Profiler* GetProfiler() const noexcept { return _profiler; }

 private:
  pyxis::GpuScene* _scene;
  pyxis::Profiler* _profiler;
};

PXR_NAMESPACE_CLOSE_SCOPE
