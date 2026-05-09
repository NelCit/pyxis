// Pyxis app — ImGui host implementation.

#include "ImGuiHost.h"

#include <Pyxis/Platform/Device/IDeviceManager.h>
#include <Pyxis/Platform/Device/VulkanContext.h>
#include <Pyxis/Platform/Logging/Log.h>
#include <Pyxis/Platform/Logging/LogCategories.h>
#include <Pyxis/Platform/Window/IWindow.h>
#include <Pyxis/Renderer/Descs/CameraDesc.h>
#include <Pyxis/Renderer/Descs/LightDesc.h>
#include <Pyxis/Renderer/Descs/OpenPBRMaterialDesc.h>
#include <Pyxis/Renderer/Forward.h>
#include <Pyxis/Renderer/GpuScene.h>

#include <nvrhi/nvrhi.h>
#include <vulkan/vulkan.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <imgui.h>

// Windows process-memory query for the perf panel's RAM row.
// Pyxis is Windows-only v1 (plan §3); this stays inside an #ifdef
// so a future cross-platform port can fan out to mach_task_basic_info
// (macOS) or /proc/self/status VmRSS (Linux) without disturbing the
// existing path.
#if defined(_WIN32)
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #include <windows.h>
    #include <psapi.h>
#endif
#include <imgui_impl_glfw.h>
#include <imgui_impl_vulkan.h>

namespace pyxis::app {

namespace {

// Sized for the M1 dockable Performance panel — well under what ImGui's
// own bookkeeping needs even with multiple panels and a few user
// textures via ImGui_ImplVulkan_AddTexture(). The exact descriptor
// types ImGui allocates (SAMPLED_IMAGE + SAMPLER vs. legacy
// COMBINED_IMAGE_SAMPLER) flip across releases, so we let ImGui own
// the pool internally and just hand it a size hint.
constexpr uint32_t IMGUI_DESCRIPTOR_POOL_SIZE = 1024;

// ImGui's Vulkan backend, built with IMGUI_IMPL_VULKAN_NO_PROTOTYPES,
// needs every Vulkan entry point loaded through this callback. We bridge
// to the standard `vkGetInstanceProcAddr` exported by the Vulkan loader.
// VkInstance is itself a pointer-typed handle (struct VkInstance_T*), so
// the user-data slot can carry it directly without an extra indirection.
PFN_vkVoidFunction ImGuiVulkanLoader(const char* fnName, void* userInstance) {
  auto inst = reinterpret_cast<VkInstance>(userInstance);
  return vkGetInstanceProcAddr(inst, fnName);
}

void ImGuiVulkanCheckResult(VkResult err) {
  if (err == VK_SUCCESS)
    return;
  auto& log = pyxis::Logging::Get();
  char msg[96];
  std::snprintf(msg, sizeof(msg), "ImGui Vulkan VkResult = %d", static_cast<int>(err));
  log.Warn(pyxis::log::APP, msg);
}

}  // namespace

ImGuiHost::~ImGuiHost() {
  Shutdown();
}

bool ImGuiHost::Init(IWindow* window, IDeviceManager* deviceManager) noexcept {
  auto& log = Logging::Get();
  if (!window || !deviceManager)
  {
    log.Error(log::APP, "ImGuiHost::Init: null window or device manager");
    return false;
  }

  const VulkanContext vulkanContext = deviceManager->GetVulkanContext();
  if (!vulkanContext.instance || !vulkanContext.physicalDevice || !vulkanContext.device
      || !vulkanContext.graphicsQueue)
  {
    log.Error(log::APP, "ImGuiHost::Init: device manager surfaced null Vulkan handles");
    return false;
  }

  _instance = vulkanContext.instance;
  _physicalDevice = vulkanContext.physicalDevice;
  auto vkInstance = static_cast<VkInstance>(vulkanContext.instance);
  auto vkPhys = static_cast<VkPhysicalDevice>(vulkanContext.physicalDevice);
  auto vkDevice = static_cast<VkDevice>(vulkanContext.device);
  auto vkQueue = static_cast<VkQueue>(vulkanContext.graphicsQueue);

  // -- ImGui context ----------------------------------------------------
  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO& imguiIO = ImGui::GetIO();
  imguiIO.IniFilename = nullptr;  // no imgui.ini side-files
  imguiIO.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
  imguiIO.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

  ImGui::StyleColorsDark();

  // -- GLFW backend -----------------------------------------------------
  auto* glfwWindow = static_cast<GLFWwindow*>(window->NativeHandle());
  if (!glfwWindow || !ImGui_ImplGlfw_InitForVulkan(glfwWindow, /*install_callbacks*/ true))
  {
    log.Error(log::APP, "ImGuiHost::Init: ImGui_ImplGlfw_InitForVulkan failed");
    Shutdown();
    return false;
  }

  // -- Vulkan backend ---------------------------------------------------
  if (!ImGui_ImplVulkan_LoadFunctions(VK_API_VERSION_1_3, &ImGuiVulkanLoader, _instance))
  {
    log.Error(log::APP, "ImGuiHost::Init: ImGui_ImplVulkan_LoadFunctions failed");
    Shutdown();
    return false;
  }

  const auto colorFormat = static_cast<VkFormat>(vulkanContext.colorFormat);

  ImGui_ImplVulkan_InitInfo init{};
  init.ApiVersion = VK_API_VERSION_1_3;
  init.Instance = vkInstance;
  init.PhysicalDevice = vkPhys;
  init.Device = vkDevice;
  init.QueueFamily = vulkanContext.graphicsFamily;
  init.Queue = vkQueue;
  init.DescriptorPool = VK_NULL_HANDLE;
  init.DescriptorPoolSize =
      IMGUI_DESCRIPTOR_POOL_SIZE;  // ImGui builds its own pool with the right types.
  // Both image-count knobs match the actual swapchain count. Critically
  // MinImageCount must equal the count we'd ever pass to
  // ImGui_ImplVulkan_SetMinImageCount, otherwise that function trips
  // its own IM_ASSERT(0) marked "FIXME-VIEWPORT: Unsupported" — it
  // can't actually change MinImageCount post-init in this release.
  init.MinImageCount = deviceManager->GetBackbufferCount();
  init.ImageCount = deviceManager->GetBackbufferCount();
  init.UseDynamicRendering = true;
  init.CheckVkResultFn = &ImGuiVulkanCheckResult;

  init.PipelineInfoMain.PipelineRenderingCreateInfo.sType =
      VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR;
  init.PipelineInfoMain.PipelineRenderingCreateInfo.colorAttachmentCount = 1;
  init.PipelineInfoMain.PipelineRenderingCreateInfo.pColorAttachmentFormats = &colorFormat;
  init.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;

  if (!ImGui_ImplVulkan_Init(&init))
  {
    log.Error(log::APP, "ImGuiHost::Init: ImGui_ImplVulkan_Init failed");
    Shutdown();
    return false;
  }

  _ready = true;
  log.Info(log::APP, "ImGuiHost: docking + Vulkan backend ready");
  return true;
}

void ImGuiHost::Shutdown() noexcept {
  if (_ready)
  {
    ImGui_ImplVulkan_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    _ready = false;
  }
  _instance = nullptr;
}

void ImGuiHost::BeginFrame() noexcept {
  if (!_ready)
    return;
  ImGui_ImplVulkan_NewFrame();
  ImGui_ImplGlfw_NewFrame();
  ImGui::NewFrame();
}

void ImGuiHost::OnSwapchainRebuilt(uint32_t /*imageCount*/) noexcept {
  if (!_ready)
    return;
  // No-op for M1. We'd love to call ImGui_ImplVulkan_SetMinImageCount
  // here so ImGui's per-frame ring matches the new swapchain, but the
  // ImGui release we're pinned to (docking-branch c51f1a6e, see
  // _cmake/Thirdparty.cmake) has IM_ASSERT(0) inside that function
  // marked "FIXME-VIEWPORT: Unsupported. Need to recreate all swap
  // chains!" — calling it with a count different from the init-time
  // count crashes the app. Init now matches the swapchain count
  // exactly (see Init), and on Windows the count is stable across
  // resizes (Intel/NVidia drivers don't change it), so this hook
  // stays for the future when ImGui upstream fixes the FIXME.
}

void ImGuiHost::BuildFpsPanel(const FrameProfile& frameProfile) noexcept {
  if (!_ready)
    return;

  // Push the current frame's CPU + GPU ms into the rolling history
  // ring. Read by ImGui::PlotLines below; the ring is sized to 240
  // frames (~4 s @ 60 Hz) — short enough to feel responsive,
  // long enough to spot a stutter without overwhelming the panel.
  _cpuMsHistory[_historyHead] = frameProfile.cpuFrameMs;
  _gpuMsHistory[_historyHead] = frameProfile.gpuFrameMs;
  _historyHead = (_historyHead + 1) % PERF_HISTORY_SIZE;

  // Default position top-left so it doesn't overlap the Scene panel.
  // FirstUseEver = the user can drag the window anywhere after first
  // appearance and we won't teleport it back.
  ImGui::SetNextWindowPos(ImVec2(10.0f, 10.0f), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowSize(ImVec2(420.0f, 0.0f), ImGuiCond_FirstUseEver);
  if (ImGui::Begin("Performance", nullptr, ImGuiWindowFlags_NoSavedSettings))
  {
    const ImGuiIO& imguiIO = ImGui::GetIO();
    ImGui::Text("Frame: %llu",
                static_cast<unsigned long long>(frameProfile.frameIndex));
    ImGui::SameLine(180.0f);
    ImGui::Text("FPS: %.1f", static_cast<double>(imguiIO.Framerate));

    // Find the auto-scale upper bound for the graphs — max of CPU
    // OR GPU history ring, clamped to a 1 ms floor so a flat-zero
    // initial ring doesn't render as a single line at the top.
    float maxMs = 1.0f;
    for (std::size_t i = 0; i < PERF_HISTORY_SIZE; ++i)
    {
      maxMs = (_cpuMsHistory[i] > maxMs) ? _cpuMsHistory[i] : maxMs;
      maxMs = (_gpuMsHistory[i] > maxMs) ? _gpuMsHistory[i] : maxMs;
    }
    // Round up to the next 5 ms so the y-axis label rounds to a
    // human-friendly bucket and a one-frame spike doesn't rescale
    // every other frame.
    maxMs = std::ceil(maxMs / 5.0f) * 5.0f;

    // ----- CPU section ------------------------------------------------
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextColored(ImVec4(0.55f, 0.85f, 0.55f, 1.0f),
                       "CPU      %.3f ms", frameProfile.cpuFrameMs);
    char overlayCpu[32];
    std::snprintf(overlayCpu, sizeof(overlayCpu), "CPU ms (max %.1f)", maxMs);
    ImGui::PlotLines("##cpuMs", _cpuMsHistory, static_cast<int>(PERF_HISTORY_SIZE),
                     static_cast<int>(_historyHead), overlayCpu, 0.0f, maxMs,
                     ImVec2(0.0f, 60.0f));
    for (const FrameProfile::PassTiming& timing : frameProfile.passes)
    {
      if (timing.kind != FrameProfile::ScopeKind::Cpu)
        continue;
      const std::string_view name = timing.name.View();
      ImGui::Text("  %*s%.*s   %.3f ms", static_cast<int>(timing.depth * 2), "",
                  static_cast<int>(name.size()), name.data(), timing.durationMs);
    }

    // ----- GPU section ------------------------------------------------
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextColored(ImVec4(0.55f, 0.70f, 0.95f, 1.0f),
                       "GPU      %.3f ms", frameProfile.gpuFrameMs);
    char overlayGpu[32];
    std::snprintf(overlayGpu, sizeof(overlayGpu), "GPU ms (max %.1f)", maxMs);
    ImGui::PlotLines("##gpuMs", _gpuMsHistory, static_cast<int>(PERF_HISTORY_SIZE),
                     static_cast<int>(_historyHead), overlayGpu, 0.0f, maxMs,
                     ImVec2(0.0f, 60.0f));
    for (const FrameProfile::PassTiming& timing : frameProfile.passes)
    {
      if (timing.kind != FrameProfile::ScopeKind::Gpu)
        continue;
      const std::string_view name = timing.name.View();
      ImGui::Text("  %*s%.*s   %.3f ms", static_cast<int>(timing.depth * 2), "",
                  static_cast<int>(name.size()), name.data(), timing.durationMs);
    }

    // ----- System section ---------------------------------------------
    // Process-level memory footprint. Working-set bytes = the bytes
    // resident in RAM right now (not including paged-out code / data).
    // VRAM lands at Phase 5 alongside the Vulkan VK_EXT_memory_budget
    // query.
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Text("System");
#if defined(_WIN32)
    PROCESS_MEMORY_COUNTERS_EX memCounters{};
    memCounters.cb = sizeof(memCounters);
    if (GetProcessMemoryInfo(GetCurrentProcess(),
                             reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memCounters),
                             sizeof(memCounters)))
    {
      const double currentMiB =
          static_cast<double>(memCounters.WorkingSetSize) / (1024.0 * 1024.0);
      const double peakMiB =
          static_cast<double>(memCounters.PeakWorkingSetSize) / (1024.0 * 1024.0);
      const double commitMiB =
          static_cast<double>(memCounters.PrivateUsage) / (1024.0 * 1024.0);
      ImGui::Text("RAM      %7.1f MiB  (peak %.1f, commit %.1f)", currentMiB, peakMiB,
                  commitMiB);
    }
    else
    {
      ImGui::Text("RAM      query failed");
    }
#else
    ImGui::Text("RAM      (not implemented on this platform)");
#endif
    // VRAM — sum DEVICE_LOCAL heaps for total; query
    // VK_EXT_memory_budget for per-heap "in-use" if the extension is
    // available. The extension is enabled by default by NVRHI on
    // recent NVIDIA / AMD drivers; on a driver that didn't expose
    // it, the budget pNext gets ignored and we fall back to total
    // only.
    if (_physicalDevice != nullptr)
    {
      auto vkPhys = static_cast<VkPhysicalDevice>(_physicalDevice);
      VkPhysicalDeviceMemoryBudgetPropertiesEXT budgetProps{};
      budgetProps.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT;
      VkPhysicalDeviceMemoryProperties2 memProps2{};
      memProps2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2;
      memProps2.pNext = &budgetProps;
      vkGetPhysicalDeviceMemoryProperties2(vkPhys, &memProps2);

      uint64_t totalBytes = 0;
      uint64_t usedBytes = 0;
      uint64_t budgetBytes = 0;
      const auto& memProps = memProps2.memoryProperties;
      for (uint32_t heapIdx = 0; heapIdx < memProps.memoryHeapCount; ++heapIdx)
      {
        if ((memProps.memoryHeaps[heapIdx].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) == 0)
          continue;
        totalBytes += memProps.memoryHeaps[heapIdx].size;
        usedBytes += budgetProps.heapUsage[heapIdx];
        budgetBytes += budgetProps.heapBudget[heapIdx];
      }
      const double totalMiB =
          static_cast<double>(totalBytes) / (1024.0 * 1024.0);
      if (usedBytes > 0)
      {
        // Budget extension populated — show used / budget / total.
        const double usedMiB =
            static_cast<double>(usedBytes) / (1024.0 * 1024.0);
        const double budgetMiB =
            static_cast<double>(budgetBytes) / (1024.0 * 1024.0);
        ImGui::Text("VRAM     %7.1f MiB used / %.1f MiB budget / %.1f MiB total",
                    usedMiB, budgetMiB, totalMiB);
      }
      else
      {
        // Extension unavailable — fall back to total only.
        ImGui::Text("VRAM     %7.1f MiB total  (per-process used: ext unavailable)",
                    totalMiB);
      }
    }
    else
    {
      ImGui::Text("VRAM     (no physical device)");
    }
  }
  ImGui::End();
}

void ImGuiHost::BuildScenePanel(const FrameStats& sceneStats) noexcept {
  if (!_ready)
    return;

  // M7 audit follow-up: dockable Scene panel showing the §18.4
  // FrameStats snapshot. Counts go under the header section,
  // GPU-side memory under "GPU memory", per-frame health row at the
  // bottom (pending uploads / BLAS builds, stale handle drops,
  // degraded sentinel). All values come from a single
  // GpuScene::LastFrameStats() call the caller drained for us.
  //
  // Bytes formatted by absolute magnitude (B / KiB / MiB / GiB)
  // because raw byte counts are unreadable past ~1 KiB but the
  // smaller scenes (M3 cube, M5 / M6 / M7 fixtures) live well below
  // 1 MiB and would lose precision under a uniform MB / GB unit.
  auto formatBytes = [](uint64_t bytes, char* buf, std::size_t bufSize) {
    if (bytes >= (1ull << 30))
    {
      const double gib = static_cast<double>(bytes) / static_cast<double>(1ull << 30);
      std::snprintf(buf, bufSize, "%.2f GiB", gib);
    }
    else if (bytes >= (1ull << 20))
    {
      const double mib = static_cast<double>(bytes) / static_cast<double>(1ull << 20);
      std::snprintf(buf, bufSize, "%.2f MiB", mib);
    }
    else if (bytes >= (1ull << 10))
    {
      const double kib = static_cast<double>(bytes) / static_cast<double>(1ull << 10);
      std::snprintf(buf, bufSize, "%.2f KiB", kib);
    }
    else
    {
      std::snprintf(buf, bufSize, "%llu B", static_cast<unsigned long long>(bytes));
    }
  };
  char bytesBuf[32];

  // Default position to the right of the Performance panel so the
  // two don't overlap on first launch. FirstUseEver lets the user
  // drag freely after that.
  ImGui::SetNextWindowPos(ImVec2(280.0f, 10.0f), ImGuiCond_FirstUseEver);
  if (ImGui::Begin("Scene", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
  {
    ImGui::Text("Counts");
    ImGui::Separator();
    ImGui::Text("Meshes      : %llu", static_cast<unsigned long long>(sceneStats.meshCount));
    ImGui::Text("Materials   : %llu", static_cast<unsigned long long>(sceneStats.materialCount));
    ImGui::Text("Textures    : %llu", static_cast<unsigned long long>(sceneStats.textureCount));
    ImGui::Text("Instances   : %llu", static_cast<unsigned long long>(sceneStats.instanceCount));
    ImGui::Text("Lights      : %llu", static_cast<unsigned long long>(sceneStats.lightCount));
    ImGui::Text("BLAS        : %llu", static_cast<unsigned long long>(sceneStats.blasCount));
    // TLAS: GpuScene allocates exactly 0 or 1 TLAS in v1 (multi-tier
    // sharding is post-v1 §16.5); a non-zero tlasBytes implies one
    // TLAS exists. No FrameStats counter for it — display the boolean.
    ImGui::Text("TLAS        : %s", sceneStats.tlasBytes > 0 ? "1" : "0");

    ImGui::Spacing();
    ImGui::Text("GPU memory");
    ImGui::Separator();
    formatBytes(sceneStats.vertexBytes, bytesBuf, sizeof(bytesBuf));
    ImGui::Text("Vertex      : %s", bytesBuf);
    formatBytes(sceneStats.indexBytes, bytesBuf, sizeof(bytesBuf));
    ImGui::Text("Index       : %s", bytesBuf);
    formatBytes(sceneStats.textureBytes, bytesBuf, sizeof(bytesBuf));
    ImGui::Text("Texture     : %s", bytesBuf);
    formatBytes(sceneStats.blasBytes, bytesBuf, sizeof(bytesBuf));
    ImGui::Text("BLAS        : %s", bytesBuf);
    formatBytes(sceneStats.tlasBytes, bytesBuf, sizeof(bytesBuf));
    ImGui::Text("TLAS        : %s", bytesBuf);
    const uint64_t totalBytes = sceneStats.vertexBytes + sceneStats.indexBytes
                                + sceneStats.textureBytes + sceneStats.blasBytes
                                + sceneStats.tlasBytes;
    formatBytes(totalBytes, bytesBuf, sizeof(bytesBuf));
    ImGui::Text("Total       : %s", bytesBuf);

    ImGui::Spacing();
    ImGui::Text("This frame");
    ImGui::Separator();
    ImGui::Text("Pending uploads     : %llu",
                static_cast<unsigned long long>(sceneStats.pendingUploads));
    ImGui::Text("Pending BLAS builds : %llu",
                static_cast<unsigned long long>(sceneStats.pendingBlasBuilds));
    // staleHandleDrops + degraded are health signals — colour them
    // when non-zero / true so the user sees them immediately. Yellow
    // for the soft warning (drops > 0); red for the degraded sentinel.
    if (sceneStats.staleHandleDrops > 0)
    {
      ImGui::TextColored(ImVec4(0.95f, 0.85f, 0.20f, 1.0f),
                         "Stale handle drops  : %llu",
                         static_cast<unsigned long long>(sceneStats.staleHandleDrops));
    }
    else
    {
      ImGui::Text("Stale handle drops  : 0");
    }
    if (sceneStats.degraded)
    {
      ImGui::TextColored(ImVec4(0.95f, 0.30f, 0.20f, 1.0f), "Degraded            : YES");
    }
    else
    {
      ImGui::Text("Degraded            : no");
    }
  }
  ImGui::End();
}

void ImGuiHost::BuildEditorPanel(GpuScene& scene) noexcept {
  if (!_ready)
    return;

  // Default position: below the Scene panel (which is at 280, 10
  // with autoresize ~280 px tall). Editor goes below at y=320.
  ImGui::SetNextWindowPos(ImVec2(280.0f, 360.0f), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowSize(ImVec2(360.0f, 0.0f), ImGuiCond_FirstUseEver);
  if (ImGui::Begin("Editor", nullptr, ImGuiWindowFlags_NoSavedSettings))
  {
    // ---- Camera section ----------------------------------------------
    if (scene.HasCamera())
    {
      if (ImGui::CollapsingHeader("Camera", ImGuiTreeNodeFlags_DefaultOpen))
      {
        CameraDesc cameraDesc = scene.GetCamera();
        bool cameraEdited = false;

        // Most useful camera knobs for an interactive viewer:
        // focal length (FOV proxy), focus distance, near/far clip.
        // Position/orientation come from the FlyCameraController in
        // viewer mode — editing them via this panel would fight the
        // controller every frame, so we leave those out.
        ImGui::PushItemWidth(180.0f);
        if (ImGui::SliderFloat("Focal length (mm)", &cameraDesc.focalLengthMm, 12.0f,
                               200.0f, "%.1f"))
          cameraEdited = true;
        if (ImGui::SliderFloat("Focus distance",    &cameraDesc.focusDistance,    0.1f,
                               50.0f, "%.2f"))
          cameraEdited = true;
        if (ImGui::SliderFloat("Near clip",         &cameraDesc.nearClip,         0.01f,
                               1.0f, "%.3f"))
          cameraEdited = true;
        if (ImGui::SliderFloat("Far clip",          &cameraDesc.farClip,          10.0f,
                               5000.0f, "%.0f"))
          cameraEdited = true;
        ImGui::PopItemWidth();

        if (cameraEdited)
          scene.SetCamera(cameraDesc);
      }
    }

    // ---- Lights section ----------------------------------------------
    if (ImGui::CollapsingHeader("Lights", ImGuiTreeNodeFlags_DefaultOpen))
    {
      const uint32_t lightCount = scene.GetLiveLightCount();
      if (lightCount == 0)
      {
        ImGui::TextDisabled("(no lights authored)");
      }
      else
      {
        for (uint32_t i = 0; i < lightCount; ++i)
        {
          const LightHandle handle = scene.GetLightHandleAt(i);
          LightDesc desc = scene.GetLightDescAt(i);

          // Distinct ImGui ID per light so widgets don't collide.
          ImGui::PushID(static_cast<int>(i));
          const char* kindLabel = "Distant";
          if (desc.kind == LightDesc::Kind::Dome) kindLabel = "Dome";
          if (desc.kind == LightDesc::Kind::Rect) kindLabel = "Rect";
          ImGui::Text("Light #%u  %s", i, kindLabel);
          bool lightEdited = false;

          float color[3] = {static_cast<float>(desc.color.x),
                            static_cast<float>(desc.color.y),
                            static_cast<float>(desc.color.z)};
          if (ImGui::ColorEdit3("Color", color, ImGuiColorEditFlags_NoInputs))
          {
            desc.color = hlslpp::float3{color[0], color[1], color[2]};
            lightEdited = true;
          }
          if (ImGui::SliderFloat("Intensity", &desc.intensity, 0.0f, 10.0f, "%.2f"))
            lightEdited = true;

          if (lightEdited && handle != LightHandle::Invalid)
            scene.UpdateLight(handle, desc);

          ImGui::Separator();
          ImGui::PopID();
        }
      }
    }

    // ---- Materials section -------------------------------------------
    if (ImGui::CollapsingHeader("Materials"))
    {
      const uint32_t matCount = scene.GetLiveMaterialCount();
      if (matCount == 0)
      {
        ImGui::TextDisabled("(no materials authored)");
      }
      else
      {
        // Pick one material at a time via a slider. Dropdown would
        // need names, which we don't carry through OpenPBRMaterialDesc
        // for v1 (sourcePrim is debug-only + per the §11 dedup rule
        // not part of identity).
        if (_editorMaterialIndex >= matCount)
          _editorMaterialIndex = 0;
        int picked = static_cast<int>(_editorMaterialIndex);
        if (ImGui::SliderInt("Material #", &picked, 0, static_cast<int>(matCount) - 1))
        {
          _editorMaterialIndex = static_cast<uint32_t>(picked);
        }

        const MaterialHandle handle = scene.GetMaterialHandleAt(_editorMaterialIndex);
        OpenPBRMaterialDesc desc = scene.GetMaterialDescAt(_editorMaterialIndex);
        bool matEdited = false;

        float baseColor[3] = {static_cast<float>(desc.baseColor.x),
                              static_cast<float>(desc.baseColor.y),
                              static_cast<float>(desc.baseColor.z)};
        if (ImGui::ColorEdit3("Base color", baseColor, ImGuiColorEditFlags_NoInputs))
        {
          desc.baseColor = hlslpp::float3{baseColor[0], baseColor[1], baseColor[2]};
          matEdited = true;
        }
        if (ImGui::SliderFloat("Roughness", &desc.roughness, 0.0f, 1.0f, "%.3f"))
          matEdited = true;
        if (ImGui::SliderFloat("Metalness", &desc.metalness, 0.0f, 1.0f, "%.3f"))
          matEdited = true;
        if (ImGui::SliderFloat("Opacity",   &desc.opacity,   0.0f, 1.0f, "%.3f"))
          matEdited = true;
        if (ImGui::SliderFloat("Specular IoR", &desc.specularIor, 1.0f, 3.0f, "%.3f"))
          matEdited = true;

        if (matEdited && handle != MaterialHandle::Invalid)
          scene.UpdateMaterial(handle, desc);
      }
    }

    // ---- Scene loader (placeholder) ----------------------------------
    if (ImGui::CollapsingHeader("Scene"))
    {
      ImGui::TextDisabled(
          "Scene-reload (file picker + GpuScene::Clear) lands at the\n"
          "Phase-6 follow-up. Path overrides + restart-to-reload via\n"
          "`pyxis.exe --scene <path>` are the v1 workflow.");
    }
  }
  ImGui::End();
}

void ImGuiHost::Render() noexcept {
  if (!_ready)
    return;
  ImGui::Render();
}

void ImGuiHost::Submit(nvrhi::ICommandList* commandList, nvrhi::ITexture* colorTarget) noexcept {
  if (!_ready || !commandList || !colorTarget)
    return;

  ImDrawData* drawData = ImGui::GetDrawData();
  if (!drawData || drawData->CmdListsCount == 0)
    return;

  // Lift the raw Vulkan handles out of NVRHI. We're inside an open
  // command list (commandList->open() was called by the caller);
  // render-pass boundaries are NVRHI's responsibility — we begin our
  // own dynamic-rendering block here on top of NVRHI's command buffer.
  // nvrhi::Object has a templated `operator T*()` so the native handle
  // converts directly to its Vulkan pointer-typed alias (VkCommandBuffer
  // is `struct VkCommandBuffer_T*` etc.). No reinterpret_cast needed.
  VkCommandBuffer vkCmd = commandList->getNativeObject(nvrhi::ObjectTypes::VK_CommandBuffer);
  VkImageView vkView =
      colorTarget->getNativeView(nvrhi::ObjectTypes::VK_ImageView, nvrhi::Format::UNKNOWN,
                                 nvrhi::AllSubresources, nvrhi::TextureDimension::Texture2D,
                                 /*isReadOnlyDSV*/ false);
  if (!vkCmd || !vkView)
    return;

  const auto& tdesc = colorTarget->getDesc();

  VkRenderingAttachmentInfo color{};
  color.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
  color.imageView = vkView;
  color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  color.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;  // preserve renderer's content
  color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

  VkRenderingInfo renderingInfo{};
  renderingInfo.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
  renderingInfo.renderArea.offset = {0, 0};
  renderingInfo.renderArea.extent = {tdesc.width, tdesc.height};
  renderingInfo.layerCount = 1;
  renderingInfo.colorAttachmentCount = 1;
  renderingInfo.pColorAttachments = &color;

  vkCmdBeginRendering(vkCmd, &renderingInfo);
  ImGui_ImplVulkan_RenderDrawData(drawData, vkCmd);
  vkCmdEndRendering(vkCmd);
}

}  // namespace pyxis::app
