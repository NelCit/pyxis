// Pyxis Hydra — HdRenderParam carrying the shared per-render state.
//
// Plan §7. The HdRenderParam Hydra hands to every Sync call. Holds borrowed
// pointers to the delegate-owned PyxisEngine's GpuScene + Profiler so each
// HdPyxisMesh / HdPyxisCamera / HdPyxisLight Sync impl can translate dirty-bits
// into GpuScene mutations + GPU-side profiler scopes without each prim having to
// find them itself.
//
// Lifetime: HdPyxisRenderDelegate constructs the param from its PyxisEngine and
// returns it from GetRenderParam(). Both pointers are non-owning views into
// objects whose lifetime is bounded by the delegate's engine.

#pragma once

#include <pxr/imaging/hd/renderDelegate.h>
#include <pxr/usd/usd/stage.h>

#include <hlsl++.h>

namespace pyxis {
class GpuScene;
class Profiler;
namespace hydra {
class PyxisEngine;
}  // namespace hydra
}  // namespace pyxis

PXR_NAMESPACE_OPEN_SCOPE

class HdPyxisRenderParam final : public HdRenderParam {
 public:
  HdPyxisRenderParam(pyxis::GpuScene* scene, pyxis::Profiler* profiler,
                     pyxis::hydra::PyxisEngine* engine, bool persistEngine) noexcept
      : _scene(scene), _profiler(profiler), _engine(engine), _persistEngine(persistEngine) {}
  ~HdPyxisRenderParam() override = default;

  HdPyxisRenderParam(const HdPyxisRenderParam&) = delete;
  HdPyxisRenderParam& operator=(const HdPyxisRenderParam&) = delete;

  [[nodiscard]] pyxis::GpuScene* GetGpuScene() const noexcept { return _scene; }
  [[nodiscard]] pyxis::Profiler* GetProfiler() const noexcept { return _profiler; }

  // The active PyxisEngine (borrowed) — prim Sync needs it to reach Scene()
  // for the stage-change reset (engine->Scene()->Clear()).
  [[nodiscard]] pyxis::hydra::PyxisEngine* GetEngine() const noexcept { return _engine; }

  // Whether the engine is persisted process-wide (pyxis:persistEngine, minus
  // the PYXIS_OMNI_NO_PERSIST override). When false, prim Sync bypasses the
  // content-hash dedup map and uses today's CreateMesh + AppendInstance path.
  [[nodiscard]] bool PersistEngine() const noexcept { return _persistEngine; }

  // NOTE: there is NO scene mutex anymore. Under the RFC 0007 StageWalker-only
  // ingest, every prim Sync (HdPyxisMesh/Light/Camera) is a no-op — ALL GpuScene
  // mutation happens single-threaded in HdPyxisRenderPass::_Execute (one
  // StageWalker::WalkStage on the render thread). So the §30.11 single-writer
  // invariant holds by construction and Hydra's parallel Sync can't race the
  // GpuScene. (The old per-prim-Sync path needed a mutex here; it's gone.)

  // Stage-to-world correction (metersPerUnit scale + Z-up->Y-up rotation),
  // matching pyxis_usd_ingest's StageWalker::BuildStageContext. Hydra's
  // GetTransform returns transforms in the stage's authored units/up-axis
  // (e.g. centimetres, Z-up for the World Lobby); StageWalker bakes this
  // correction onto every prim so the renderer's world is metres + Y-up.
  // The host sets this from the stage's metadata on SetStage; until then it
  // is identity (no correction). The closesthit's absolute-unit constants
  // (2 m dome-AO range, 0.01 m^2 inverse-square clamp) only behave correctly
  // when the scene is in metres, so this is required for §25.O.3 brightness
  // parity, not merely cosmetic. Default identity preserves prior behaviour
  // for hosts that don't set it.
  void SetStageToWorld(const hlslpp::float4x4& stageToWorld) noexcept {
    _stageToWorld = stageToWorld;
  }
  [[nodiscard]] const hlslpp::float4x4& StageToWorld() const noexcept { return _stageToWorld; }

  // The open UsdStage the delegate is rendering (borrowed, weak). §25.O.3: a
  // material's Sync resolves the bound material prim on this stage and runs it
  // through the SHARED pyxis_material_translation::FromUsdShade — the SAME path
  // pyxis_usd_ingest's StageWalker uses — so MDL / MaterialX / UsdPreviewSurface
  // networks translate byte-identically across both ingest adapters. Hosts set
  // it on SetStage. Null (no stage) => the delegate falls back to reading Hydra's
  // flattened material network (UsdPreviewSurface only, which loses MDL tints).
  void SetStage(const UsdStageRefPtr& stage) noexcept { _stage = stage; }
  [[nodiscard]] const UsdStageRefPtr& Stage() const noexcept { return _stage; }

 private:
  pyxis::GpuScene* _scene;
  pyxis::Profiler* _profiler;
  pyxis::hydra::PyxisEngine* _engine;
  bool _persistEngine;
  hlslpp::float4x4 _stageToWorld = hlslpp::float4x4::identity();
  UsdStageRefPtr _stage;
};

PXR_NAMESPACE_CLOSE_SCOPE
