// Pyxis USD-direct ingest — StageWalker.
//
// Plan §25.O. One-shot importer (no UsdNotice listener — see §25.O.2):
// opens a UsdStage, walks every prim in SdfPath-sorted order, and
// calls into a supplied GpuScene to materialise meshes / camera /
// lights / materials. The renderer never holds the UsdStage* after
// `Walk` returns; subsequent edits go through the pyxis_renderer
// public API (§18.5), not USD notices.
//
// SdfPath-sorted ordering is the §25.O.3 P0 invariant: pyxis_hydra
// must produce instances in the same order so the byte-equal EXR
// regression test passes.

#pragma once

#include <Pyxis/Renderer/Descs/CameraDesc.h>
#include <Pyxis/UsdIngest/UsdIngestApi.h>

#include <pxr/usd/usd/stage.h>

#include <cstdint>
#include <memory>
#include <string_view>

namespace pyxis {
class GpuScene;
}  // namespace pyxis

namespace pyxis::usd_ingest {

// Per-stage timing breakdown (milliseconds, steady_clock based).
// Surfaced in the viewer's Performance / Loading panel so users can
// see where USD-load latency goes. Zero on a stage that didn't run
// (e.g. stageOpenMs is 0 when WalkStage is called with an already-
// opened stage). totalMs covers WalkFile / WalkStage end-to-end.
struct PYXIS_USD_INGEST_API IngestTimings {
  float stageOpenMs       = 0.0f;  // pxr::UsdStage::Open
  float traverseSortMs    = 0.0f;  // stage->Traverse + stable_sort
  float prewarmPassMs     = 0.0f;  // parallel attribute pre-warm
  float materialPassMs    = 0.0f;  // pass 1: materials
  float instancerPassMs   = 0.0f;  // pass 2: instancers
  float meshLightCameraMs = 0.0f;  // pass 3: meshes / camera / lights
  float totalMs           = 0.0f;
};

// Inline-string POD for camera names (mirrors §18.5 ErrorMessage /
// ScopeName pattern). Holds an SdfPath up to 255 chars; longer paths
// truncate. Cameras with very long paths are diagnostics-only — the
// active-camera selection lookup uses the string only to match the
// root layer's `boundCamera` hint, which itself is bounded by USD's
// SdfPath limits.
struct PYXIS_USD_INGEST_API NamedCameraView {
  CameraDesc  desc;          // viewFromWorld + projFromView + intrinsics
  char        name[256] = {0};  // null-terminated, truncated if > 255 chars
};

// Counters + timings only — no STL containers cross the DLL boundary.
// Camera list lives behind PIMPL on `IngestResult` (below); query it
// via `GetCameraCount()` + `GetCameraAt(...)`. §18.9 ABI rule.
struct PYXIS_USD_INGEST_API IngestStats {
  uint32_t      meshesEmitted     = 0;
  uint32_t      instancesEmitted  = 0;
  uint32_t      instancersEmitted = 0;  // M6: UsdGeomPointInstancer prims expanded
  uint32_t      lightsEmitted     = 0;
  uint32_t      materialsEmitted  = 0;
  uint32_t      camerasEmitted    = 0;
  uint32_t      skipped           = 0;  // unsupported prim types
  int           activeCameraIndex = -1; // index into the result's camera list, -1 if none
  IngestTimings timings           = {};
};

// Opaque PIMPL holder for the camera list. STL containers live inside
// `Impl` so the public class body stays NVRHI-/STL-container-free per
// §18.9. Construct via `StageWalker::Walk*`; destructor walks the
// PIMPL back through the SHARED-DLL allocator.
class PYXIS_USD_INGEST_API IngestResult final {
 public:
  IngestResult();
  ~IngestResult();
  IngestResult(const IngestResult&)            = delete;
  IngestResult& operator=(const IngestResult&) = delete;
  IngestResult(IngestResult&&) noexcept;
  IngestResult& operator=(IngestResult&&) noexcept;

  [[nodiscard]] const IngestStats& Stats() const noexcept;

  // Camera-list iteration. Cameras are stored in SdfPath-sorted order
  // (§25.O.3 invariant). `index < GetCameraCount()` returns true on
  // success and writes into `*out`; out-of-range returns false and
  // leaves `*out` untouched.
  [[nodiscard]] uint32_t GetCameraCount() const noexcept;
  [[nodiscard]] bool     GetCameraAt(uint32_t index, NamedCameraView* out) const noexcept;

  // Renderer-side accessor for the StageWalker pass to populate the
  // result. Internal — callers must not rely on the symbol staying
  // exported. Hidden behind `_impl` so out-of-line.
  struct Impl;
  Impl& GetImpl() noexcept;

 private:
  std::unique_ptr<Impl> _impl;
};

class PYXIS_USD_INGEST_API StageWalker final {
 public:
  StageWalker() = default;
  ~StageWalker() = default;

  StageWalker(const StageWalker&)            = delete;
  StageWalker& operator=(const StageWalker&) = delete;

  // Open `usdPath`, walk all prims, push mutations into `scene`.
  // Returns an IngestResult: counters + timings + opaque camera list.
  // Errors (file not found, bad layer, etc.) surface via spdlog and
  // produce a result with every counter zero + an empty camera list.
  //
  // V2.A.13 — `frameNumber` selects the USD time code at which all
  // attributes are evaluated. Negative = `UsdTimeCode::Default()`
  // (no animation). xformOps, camera attrs, light attrs, mesh
  // points, and primvars all read at that time. The closesthit
  // shader is time-agnostic — animation drives the load-time state
  // only in v2 (per-frame re-walk is the operator's responsibility).
  IngestResult WalkFile(std::string_view usdPath, GpuScene& scene,
                        double frameNumber = -1.0);

  // Variant taking an already-opened stage. Used by the cross-adapter
  // regression harness so both paths share a single stage open. The
  // pxr::UsdStageRefPtr argument is USD's reference-counted handle —
  // the renderer never persists it past `Walk` (§25.O.2 contract).
  IngestResult WalkStage(const pxr::UsdStageRefPtr& stage, GpuScene& scene,
                         double frameNumber = -1.0);
};

}  // namespace pyxis::usd_ingest
