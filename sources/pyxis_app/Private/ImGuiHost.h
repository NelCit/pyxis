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

#include <cstdint>

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
  // Displayed in the Performance panel's "Loading" CollapsingHeader
  // so the user can see what the first-frame cost was even after
  // many frames of steady-state rendering.
  bool        _loadingProfileLatched = false;
  float       _loadingCpuMs          = 0.0f;
  float       _loadingGpuMs          = 0.0f;

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
