// Pyxis app — ImGui dockable-panel host.
//
// Plan §41 M1 exit: "ImGui dockable panel with FPS shows". Owns the
// ImGui context + Vulkan + GLFW backends; reaches the raw Vulkan
// handles through IDeviceManager::GetVulkanContext(). Dynamic-rendering
// path only — matches the swapchain we set up in VkDeviceManager
// (Vulkan 1.3 core, no legacy VkRenderPass).

#pragma once

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

  // Editor's "Reload shaders" button latches this flag; ViewerMode
  // reads it via TakeShaderReloadRequest() each frame, calls the
  // renderer-side reload, and clears the flag. Decouples the editor
  // panel from the renderer (BuildEditorPanel only knows about
  // GpuScene; the actual shader-reload lives across the API
  // boundary on PyxisRenderer).
  bool                                  _editorReloadShadersRequested = false;

  // Editor's "Open scene..." button latches the picked-file path here;
  // ViewerMode drains it via TakeSceneReloadRequest() each frame.
  // Empty string = no pending request. Owned std::string because the
  // path comes out of an OS file dialog whose buffer disappears with
  // the dialog.
  std::string                           _editorPendingScenePath;

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
  // Latest pick readback the viewer pushed from PyxisRenderer's
  // LastPickResult(); displayed in the Editor panel as a hover
  // readout (color, normal, depth, instance id).
  PickResult                            _editorLastPick{};
  // "Save current AOV..." latch — set by the editor button to the
  // requested file path; ViewerMode drains it via TakeSaveAovRequest()
  // and dispatches a TextureReadback + ExrWriter write. Empty = no
  // pending request.
  std::string                           _editorPendingSaveAovPath;

  // Aspect ratio (renderWidth / renderHeight) of the AOV the renderer
  // dispatches into. ViewerMode pushes this each frame via
  // SetRenderAspect so the Camera section can rebuild projFromView
  // from FOV + near/far when the user drags a slider. Defaults to 1.0
  // (square) so the first frame's editor sliders don't divide by zero.
  float                                 _renderAspect = 1.0f;

 public:
  // Called by ViewerMode each frame; returns true and clears the
  // latched request iff the editor's "Reload shaders" button was
  // clicked since the last call.
  [[nodiscard]] bool TakeShaderReloadRequest() noexcept {
    const bool requested = _editorReloadShadersRequested;
    _editorReloadShadersRequested = false;
    return requested;
  }

  // Called by ViewerMode each frame; if the editor has a pending
  // scene-reload path, moves it into `outPath` and returns true. The
  // internal slot resets to empty in either branch so a subsequent
  // call returns false until the user picks another file.
  [[nodiscard]] bool TakeSceneReloadRequest(std::string& outPath) noexcept {
    if (_editorPendingScenePath.empty())
      return false;
    outPath = std::move(_editorPendingScenePath);
    _editorPendingScenePath.clear();
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

  // ViewerMode pushes the renderer's LastPickResult() into the panel
  // each frame; the Editor displays the hover-pixel values.
  void SetLastPickResult(const PickResult& pick) noexcept { _editorLastPick = pick; }

  // ViewerMode pushes the renderer's current AOV aspect ratio
  // (width / height) each frame. The Camera section's FOV / focal-
  // length sliders use this to rebuild projFromView on edit so the
  // result matches the dispatched render dims at the time of the
  // edit. Sub-1.0 floors at a tiny epsilon to avoid divide-by-zero
  // before the first AOV is allocated.
  void SetRenderAspect(float aspect) noexcept {
    _renderAspect = (aspect > 1e-3f) ? aspect : 1.0f;
  }

  // ViewerMode drains a pending save-AOV path each frame. Returns
  // true and clears the latch iff the user clicked "Save AOV..." since
  // the last call. The current debug view selects which AOV gets
  // saved (Color/Normal/Depth/InstanceID).
  [[nodiscard]] bool TakeSaveAovRequest(std::string& outPath) noexcept {
    if (_editorPendingSaveAovPath.empty())
      return false;
    outPath = std::move(_editorPendingSaveAovPath);
    _editorPendingSaveAovPath.clear();
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
