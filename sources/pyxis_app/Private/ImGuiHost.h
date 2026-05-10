// Pyxis app — ImGui dockable-panel host.
//
// Plan §41 M1 exit: "ImGui dockable panel with FPS shows". Owns the
// ImGui context + Vulkan + GLFW backends; reaches the raw Vulkan
// handles through IDeviceManager::GetVulkanContext(). Dynamic-rendering
// path only — matches the swapchain we set up in VkDeviceManager
// (Vulkan 1.3 core, no legacy VkRenderPass).

#pragma once

#include <Pyxis/Renderer/Descs/CameraDesc.h>
#include <Pyxis/Renderer/Descs/FrameProfile.h>
#include <Pyxis/Renderer/Descs/FrameStats.h>
#include <Pyxis/Renderer/Descs/PickResult.h>
#include <Pyxis/Renderer/Descs/RenderSettings.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace nvrhi {
class ICommandList;
class ITexture;
}  // namespace nvrhi

namespace pyxis {
class IDeviceManager;
class IWindow;
class GpuScene;
}  // namespace pyxis

namespace pyxis::app {

class ImGuiHost {
 public:
  ImGuiHost() = default;
  ~ImGuiHost();
  ImGuiHost(const ImGuiHost&) = delete;
  ImGuiHost& operator=(const ImGuiHost&) = delete;

  // Set up the ImGui context, the GLFW backend, and the Vulkan backend.
  // Returns true on success. On any failure the caller should run the
  // viewer with ImGui disabled — IsReady() will report false and every
  // other method becomes a no-op.
  [[nodiscard]] bool Init(IWindow* window, IDeviceManager* deviceManager) noexcept;

  void Shutdown() noexcept;

  [[nodiscard]] bool IsReady() const noexcept { return _ready; }

  // ----- Per-frame plumbing -----------------------------------------
  // BeginFrame: backend NewFrame + ImGui::NewFrame.
  // BuildFpsPanel: dock-friendly window with FPS / CPU ms / GPU ms.
  // Render: ImGui::Render() — finalises draw data, called once per
  // frame regardless of whether any panels are visible.
  // Submit: encodes ImDrawData into the supplied command list against
  // colorTarget (must already be in nvrhi::ResourceStates::ColorAttachment).
  void BeginFrame() noexcept;
  // Builds the dockable Performance panel. Shows the totals + the
  // pre-order scope tree from the supplied FrameProfile snapshot.
  void BuildFpsPanel(const FrameProfile& frameProfile) noexcept;

  // Builds the dockable Scene panel — counts (meshes / materials /
  // textures / instances / lights / BLAS), GPU memory by category
  // (vertex / index / texture / TLAS / BLAS bytes), and the
  // per-frame health row (pending uploads / BLAS builds, stale
  // handle drops, degraded sentinel). Reads from the snapshot the
  // caller pulled via GpuScene::LastFrameStats().
  void BuildScenePanel(const FrameStats& sceneStats) noexcept;

  // Editor panel — live-edit camera + dome / lights + materials.
  // Reads the current state from GpuScene's introspection accessors
  // (GetCamera / GetLightDescAt / GetMaterialDescAt) and pushes
  // edits back via the matching Update verbs. Mutations are
  // synchronous against the supplied scene; the renderer picks up
  // the changes the next CommitResources tick.
  //
  // Takes a non-const reference because the editor calls SetCamera /
  // UpdateLight / UpdateMaterial. Caller must ensure the scene is
  // accessed only from the render thread (single-writer §31; in
  // viewer mode this is the main thread = render thread).
  void BuildEditorPanel(GpuScene& scene) noexcept;
  void Render() noexcept;

  // Notify the ImGui Vulkan backend that the swapchain was recreated
  // (resize, fullscreen toggle, etc.). Must be called whenever the
  // backbuffer image count changes — without this, ImGui's internal
  // frame ring keeps believing the old count and stalls one frame
  // after every rebuild. Caller must ensure the GPU is idle.
  void OnSwapchainRebuilt(uint32_t imageCount) noexcept;
  void Submit(nvrhi::ICommandList* commandList, nvrhi::ITexture* colorTarget) noexcept;

 private:
  bool _ready = false;
  void* _instance = nullptr;        // VkInstance (borrowed) — kept for the function loader
  void* _physicalDevice = nullptr;  // VkPhysicalDevice (borrowed) — VRAM query in BuildFpsPanel

  // Editor-panel state (M7 follow-up). The materials + lights
  // editors' "currently picked entry" lives here so the combo
  // selection survives across frames. Reset to 0 if the live count
  // drops below the picked index between frames (catches
  // DestroyMaterial / RemoveLight happening between renders).
  uint32_t _editorMaterialIndex = 0;
  uint32_t _editorLightIndex    = 0;

  // Layout state (M7 follow-up). Scene's rendered height from the
  // PREVIOUS frame, used to position the Editor panel directly
  // beneath it without overlap. ImGui auto-sizes Scene each frame;
  // we read the size right after BuildScenePanel and reuse it next
  // frame. First-frame fallback: 200 px (reasonable lower bound).
  float _layoutScenePanelHeight = 200.0f;

  // Loading-time profile snapshot. Latched on the first BuildFpsPanel
  // call where the FrameProfile reports a non-zero CPU time — that's
  // effectively the load profile (mesh upload + BLAS build + TLAS
  // rebuild + first PathTracePass dispatch all happen on frame 0).
  // Displayed in the Performance panel's "Loading" CollapsingHeader.
  // The full pass breakdown is COPIED into _loadingPasses (the live
  // FrameProfile.passes span gets recycled at every frame's
  // BeginFrame; persisting needs an owned copy).
  bool                                  _loadingProfileLatched = false;
  float                                 _loadingCpuMs          = 0.0f;
  float                                 _loadingGpuMs          = 0.0f;
  std::vector<FrameProfile::PassTiming> _loadingPasses;

  // EditorLatches — every "set on event in BuildEditorPanel, drained
  // by ViewerMode (or BuildEditorPanel itself) on the next frame"
  // signal grouped in one POD. Pre-refactor each was a separate
  // field + setter + Take* method; consolidating made the latch
  // pattern explicit and keeps related state on one cache line.
  //
  // - reloadShaders     : "Reload shaders" button -> ViewerMode
  // - sceneReloadPath   : "Open scene..." picked path -> ViewerMode
  // - saveAovPath       : "Save current AOV..." path -> ViewerMode
  // - clickInstanceSlot : LMB-click instance id -> BuildEditorPanel
  //                       drains internally (snaps Material combo)
  struct EditorLatches {
    bool        reloadShaders          = false;
    std::string sceneReloadPath;
    std::string saveAovPath;
    uint32_t    clickInstanceSlot      = INSTANCE_ID_NONE;
    // Scene-camera combo selection. -1 = no pending request; the
    // editor sets this to the index into _sceneCameras when the user
    // picks an entry; ViewerMode drains via TakeSnapToSceneCameraRequest
    // and calls FlyCameraController::SnapToCamera with the matching desc.
    int         snapToSceneCameraIndex = -1;
  };
  EditorLatches                         _latches;

  // Last-completed ingest source label + per-stage timing — set by
  // ViewerMode via SetIngestProfile() right after engine.Load()
  // returns. Surfaced in the Performance panel's Loading section so
  // the user sees USD parsing cost alongside the first-frame mesh
  // upload + BLAS build totals (which the FrameProfile-derived
  // _loadingCpuMs already covers). All zero / empty source = no
  // ingest has run yet.
  // The five sub-stage durations (stageOpenMs ... meshLightCameraMs)
  // mirror IngestStats's fields and break out where the load latency
  // actually goes — supports the user's "more detailed loading"
  // request and tracks the §34 KPI sub-budgets.
  float                                 _ingestTotalMs           = 0.0f;
  float                                 _ingestStageOpenMs       = 0.0f;
  float                                 _ingestTraverseSortMs    = 0.0f;
  float                                 _ingestMaterialPassMs    = 0.0f;
  float                                 _ingestInstancerPassMs   = 0.0f;
  float                                 _ingestMeshLightCameraMs = 0.0f;
  std::string                           _ingestSourceLabel;

  // M7 follow-up — AOV inspector + pixel picker.
  // _editorDebugView mirrors RenderSettings::DebugView; ViewerMode
  // reads it each frame to populate RenderSettings before
  // PyxisRenderer::RenderFrame. Default Color so the viewer looks
  // like the M7 baseline until the user picks a different AOV.
  RenderSettings::DebugView             _editorDebugView =
      RenderSettings::DebugView::Color;
  // WorldPos AOV display period (scene units). Editor exposes a
  // slider when the WorldPos display mode is selected; ViewerMode
  // pushes this into RenderSettings::worldPosPeriod each frame.
  // Default 10 m matches the pre-slider hardcoded behaviour.
  float                                 _editorWorldPosPeriod = 10.0f;
  // ShaderMake rebuild status — pushed by ViewerMode each frame.
  // True while the worker thread is spawning cmake / waiting for
  // exit; the editor's Reload Shaders button shows "Rebuilding..."
  // and disables itself so a second click can't queue parallel
  // rebuilds. Pre-async this was always false — the click-handler
  // blocked the main thread for ~2 s.
  bool                                  _shaderRebuildInFlight = false;
  // Latest pick readback the viewer pushed from PyxisRenderer's
  // LastPickResult(); displayed in the Editor panel as a hover
  // readout (color, normal, depth, instance id).
  PickResult                            _editorLastPick{};
  // (Save-AOV path latch moved into _latches.saveAovPath above.)

  // Render dims of the AOV the renderer dispatches into. ViewerMode
  // pushes these each frame so:
  //   - the Camera section can rebuild projFromView from FOV +
  //     near/far when the user drags a slider (uses _renderAspect),
  //   - the picker-pin toggle can normalise the pinned pixel into
  //     [0, 1] UV (uses _renderWidth/_renderHeight) so the pin
  //     survives a window resize.
  // Defaults sized so the first frame's sliders don't divide by zero.
  float                                 _renderAspect = 1.0f;
  uint32_t                              _renderWidth  = 1u;
  uint32_t                              _renderHeight = 1u;

  // (Click-instance slot latch moved into _latches.clickInstanceSlot above.)

  // Picker pin/follow toggle (M7 follow-up). When `_pickerPinned`
  // is true, ViewerMode pushes `_pickerPinnedU/V` (denormalised to
  // the current AOV dims) to RenderSettings instead of the live
  // cursor pixel — the readout in the Editor stays frozen at the
  // pixel the user pinned, so they can move the camera and see how
  // the values at that exact world point change. F-key toggles via
  // ImGui::IsKeyPressed inside the AOV inspector.
  //
  // Stored as NORMALISED [0, 1] UV (not raw pixel coords) so a window
  // resize after pinning keeps the pin at roughly the same screen
  // location — pre-fix the raw pinned pixel could go out-of-bounds
  // after a resize and the pin silently died.
  bool                                  _pickerPinned = false;
  float                                 _pickerPinnedU = 0.0f;
  float                                 _pickerPinnedV = 0.0f;

  // Scene cameras (M8a follow-up). Populated by ViewerMode after
  // each scene load via SetSceneCameras() with the StageWalker's
  // cameras list. The editor renders a combo of names; selecting an
  // entry latches an index into EditorLatches.snapToSceneCameraIndex.
  // The SceneCameraEntry struct is public (declared below alongside
  // IngestProfile) so ViewerMode can name it when assembling the
  // vector before handing it in.
 public:
  struct SceneCameraEntry {
    std::string name;       // SdfPath
    CameraDesc  desc;       // viewFromWorld + projFromView + intrinsics
  };
 private:
  std::vector<SceneCameraEntry>         _sceneCameras;
  int                                   _sceneCameraSelectedIndex = -1;

  // FlyCameraController linear-translation speed (metres/sec). The
  // editor's slider mutates this; ViewerMode reads it each frame and
  // pushes via FlyCameraController::SetMoveSpeed. Default matches
  // the controller's own initial value so the slider position is
  // visually correct on first render.
  float                                 _moveSpeed = 5.0f;

 public:
  // Called by ViewerMode each frame; returns true and clears the
  // latched request iff the editor's "Reload shaders" button was
  // clicked since the last call.
  [[nodiscard]] bool TakeShaderReloadRequest() noexcept {
    const bool requested = _latches.reloadShaders;
    _latches.reloadShaders = false;
    return requested;
  }

  // Called by ViewerMode each frame; if the editor has a pending
  // scene-reload path, moves it into `outPath` and returns true. The
  // internal slot resets to empty in either branch so a subsequent
  // call returns false until the user picks another file.
  [[nodiscard]] bool TakeSceneReloadRequest(std::string& outPath) noexcept {
    if (_latches.sceneReloadPath.empty())
      return false;
    outPath = std::move(_latches.sceneReloadPath);
    _latches.sceneReloadPath.clear();
    return true;
  }

  // Per-stage ingest breakdown, mirrored 1:1 from
  // pyxis::usd_ingest::IngestStats's timing fields. Lives in this
  // header (rather than just including IngestStats) so ImGuiHost
  // doesn't acquire a build-time dep on pyxis_usd_ingest's public
  // header — the panel-side struct is intentionally thin.
  struct IngestProfile {
    float totalMs           = 0.0f;
    float stageOpenMs       = 0.0f;
    float traverseSortMs    = 0.0f;
    float materialPassMs    = 0.0f;
    float instancerPassMs   = 0.0f;
    float meshLightCameraMs = 0.0f;
  };

  // M7 follow-up — AOV inspector accessors.
  // ViewerMode reads this to populate RenderSettings::debugView
  // each frame before PyxisRenderer::RenderFrame.
  [[nodiscard]] RenderSettings::DebugView GetDebugView() const noexcept {
    return _editorDebugView;
  }
  // Editor's WorldPos period slider value (scene units). ViewerMode
  // pushes this into RenderSettings::worldPosPeriod each frame.
  [[nodiscard]] float GetWorldPosPeriod() const noexcept { return _editorWorldPosPeriod; }

  // ViewerMode pushes the live rebuild state each frame so the
  // editor's Reload Shaders button can show "Rebuilding..." +
  // disable itself while the worker thread is in flight.
  void SetShaderRebuildInFlight(bool inFlight) noexcept {
    _shaderRebuildInFlight = inFlight;
  }

  // ViewerMode pushes the renderer's LastPickResult() into the panel
  // each frame; the Editor displays the hover-pixel values.
  void SetLastPickResult(const PickResult& pick) noexcept { _editorLastPick = pick; }

  // ViewerMode pushes the renderer's current AOV dims each frame.
  // The Camera section's FOV / focal-length sliders use the aspect
  // to rebuild projFromView on edit; the picker-pin toggle uses
  // {width, height} to normalise the pinned pixel to UV so it
  // survives a resize. Sub-1 dims floor to 1 to avoid divide-by-zero.
  void SetRenderDims(uint32_t width, uint32_t height) noexcept {
    _renderWidth  = (width  > 0) ? width  : 1u;
    _renderHeight = (height > 0) ? height : 1u;
    _renderAspect = static_cast<float>(_renderWidth) / static_cast<float>(_renderHeight);
  }

  // ViewerMode calls this on a left-button click that wasn't a drag
  // (LMB-up displacement < small threshold). The slot comes from the
  // most recent picker readback; BuildEditorPanel drains it next
  // frame and snaps the Material combo to that instance's material.
  // INSTANCE_ID_NONE = "no instance under cursor" — silently dropped.
  void SetClickedInstance(uint32_t instanceSlot) noexcept {
    _latches.clickInstanceSlot = instanceSlot;
  }

  // ViewerMode pushes the scene's camera list (from IngestStats)
  // after a successful scene load so the editor's Scene Camera combo
  // can list them. Empty = no cameras (cube fixture etc.). The active
  // index is informational — the FlyCam already snapped to it via
  // the standard SeedFromScene path.
  void SetSceneCameras(std::vector<SceneCameraEntry> cameras,
                       int activeIndex) noexcept {
    _sceneCameras = std::move(cameras);
    _sceneCameraSelectedIndex = activeIndex;
  }

  // Drained per-frame by ViewerMode. Returns true + writes the chosen
  // camera index iff the user picked an entry from the Scene Camera
  // combo since the last call. Caller looks up the matching CameraDesc
  // in the list ViewerMode supplied + calls FlyCameraController::SnapToCamera.
  [[nodiscard]] bool TakeSnapToSceneCameraRequest(int& outIndex) noexcept {
    if (_latches.snapToSceneCameraIndex < 0)
      return false;
    outIndex = _latches.snapToSceneCameraIndex;
    _latches.snapToSceneCameraIndex = -1;
    return true;
  }

  // Same list ViewerMode gave us. Reading it back so the camera-snap
  // drain can resolve the index without a parallel app-side copy.
  [[nodiscard]] const std::vector<SceneCameraEntry>& SceneCameras() const noexcept {
    return _sceneCameras;
  }

  // FlyCameraController move-speed slider value. ViewerMode reads this
  // each frame and propagates via FlyCameraController::SetMoveSpeed.
  [[nodiscard]] float MoveSpeed() const noexcept { return _moveSpeed; }

  // Picker pin accessors. ViewerMode reads these each frame to decide
  // whether to push the cursor pixel or the pinned pixel into
  // RenderSettings::mousePixel{X,Y}. When pinned, the raygen keeps
  // sampling the same pixel even as the user moves the cursor / camera,
  // so the picker readout reflects the world point the user locked in.
  // The UV is in normalised [0, 1] coords; ViewerMode denormalises
  // against the current AOV dims so a window resize after pinning
  // keeps the pin at roughly the same screen location.
  [[nodiscard]] bool IsPickerPinned() const noexcept { return _pickerPinned; }
  [[nodiscard]] float PickerPinnedU() const noexcept { return _pickerPinnedU; }
  [[nodiscard]] float PickerPinnedV() const noexcept { return _pickerPinnedV; }

  // ViewerMode drains a pending save-AOV path each frame. Returns
  // true and clears the latch iff the user clicked "Save AOV..." since
  // the last call. The current debug view selects which AOV gets
  // saved (Color/Normal/Depth/PrimID/...).
  [[nodiscard]] bool TakeSaveAovRequest(std::string& outPath) noexcept {
    if (_latches.saveAovPath.empty())
      return false;
    outPath = std::move(_latches.saveAovPath);
    _latches.saveAovPath.clear();
    return true;
  }

  // Pushed by ViewerMode after each successful (or failed-with-fallback)
  // ingest run. `sourceLabel` is the §29.4.a label
  // ("default_bundled" / "config_path" / "cli_path" / "cube_fallback")
  // or the ingest adapter ("usd_direct" / "hydra"); displayed verbatim
  // beside the timing in the Loading section.
  void SetIngestProfile(const IngestProfile& profile,
                        std::string_view sourceLabel) noexcept {
    _ingestTotalMs           = profile.totalMs;
    _ingestStageOpenMs       = profile.stageOpenMs;
    _ingestTraverseSortMs    = profile.traverseSortMs;
    _ingestMaterialPassMs    = profile.materialPassMs;
    _ingestInstancerPassMs   = profile.instancerPassMs;
    _ingestMeshLightCameraMs = profile.meshLightCameraMs;
    _ingestSourceLabel.assign(sourceLabel.data(), sourceLabel.size());
  }

 private:

  // Performance-panel rolling history. 240 frames @ 60 Hz = ~4 s of
  // visual context — enough to spot the per-pass cost shape of a
  // brief stutter without overwhelming the panel with raw noise. Push
  // happens at the top of BuildFpsPanel; ImGui::PlotLines reads the
  // arrays in-order via the head offset to render a wrap-aware line
  // chart. Sized for the v1 fixed-cap regime; the §34 ring lands
  // alongside the full per-frame profiler at M11.
  static constexpr std::size_t PERF_HISTORY_SIZE = 240;
  float _cpuMsHistory[PERF_HISTORY_SIZE]{};
  float _gpuMsHistory[PERF_HISTORY_SIZE]{};
  std::size_t _historyHead = 0;
};

}  // namespace pyxis::app
