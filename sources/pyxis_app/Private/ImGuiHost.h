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
  void* _instance = nullptr;  // VkInstance (borrowed) — kept for the function loader
};

}  // namespace pyxis::app
