// Pyxis app — ImGui host implementation.

#include "ImGuiHost.h"

#include "Output/SaveFilePicker.h"  // M11: "Save profile JSON..." button

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

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <imgui.h>
#include <ios>

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
    #include <shobjidl.h>
    #include <objbase.h>
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

// Pyxis theme — warm dark-grey background with a light-orange accent
// (Pyxis is the "compass" constellation; the accent matches a sodium-
// vapor / sunset orange glow against a dim instrument-panel background).
// Same engineering-tool aesthetic as NVIDIA Donut, just our palette
// instead of NVIDIA green.
//
// Pyxis accent palette (sRGB → linear-ish, ImGui takes 0..1 floats):
//   Pyxis orange       0.961, 0.627, 0.290   (#f5a04a)
//   Pyxis orange dim   .. with alpha 0.56
//   Pyxis orange hot   1.000, 0.690, 0.400   (#ffb066)
//   Window background  0.110, 0.110, 0.110   (#1c1c1c)
//   Frame background   0.145, 0.145, 0.145   (#252525)
//   Frame hovered      0.180, 0.180, 0.180   (#2e2e2e)
//   Frame active       0.230, 0.230, 0.230   (#3a3a3a)
//   Border             0.230, 0.230, 0.230   (#3a3a3a)
//   Text               0.880, 0.880, 0.880   (#e0e0e0)
//   Title BG active    0.227, 0.157, 0.078   (#3a2814) — orange-tinted
void ApplyPyxisTheme() noexcept {
  ImGuiStyle& style = ImGui::GetStyle();
  ImVec4*     colors = style.Colors;

  // More orange across the panel — palette dialed warmer + accent
  // colour now appears on borders, separators, title bars in every
  // state, frame backgrounds (subtle tint), and text-tint for emphasis.
  const ImVec4 pyxOrange       = ImVec4(1.000f, 0.620f, 0.260f, 1.000f); // brighter accent
  const ImVec4 pyxOrangeDim    = ImVec4(1.000f, 0.620f, 0.260f, 0.620f);
  const ImVec4 pyxOrangeHot    = ImVec4(1.000f, 0.730f, 0.420f, 1.000f);
  const ImVec4 pyxOrangeMuted  = ImVec4(0.560f, 0.340f, 0.150f, 1.000f);
  const ImVec4 pyxOrangeDeep   = ImVec4(0.310f, 0.190f, 0.090f, 1.000f); // deep brown-orange
  const ImVec4 windowBg        = ImVec4(0.140f, 0.110f, 0.085f, 1.000f); // warm dark-grey-orange
  const ImVec4 childBg         = ImVec4(0.115f, 0.090f, 0.065f, 1.000f);
  const ImVec4 popupBg         = ImVec4(0.165f, 0.130f, 0.095f, 0.980f);
  const ImVec4 frameBg         = ImVec4(0.190f, 0.140f, 0.090f, 1.000f); // orange-tinted frame
  const ImVec4 frameHover      = ImVec4(0.260f, 0.180f, 0.110f, 1.000f);
  const ImVec4 frameActive     = ImVec4(0.330f, 0.220f, 0.130f, 1.000f);
  const ImVec4 border          = ImVec4(0.420f, 0.270f, 0.140f, 1.000f); // warm orange-brown
  const ImVec4 borderShadow    = ImVec4(0.000f, 0.000f, 0.000f, 0.000f);
  const ImVec4 text            = ImVec4(1.000f, 0.910f, 0.820f, 1.000f); // warm-tinted text
  const ImVec4 textDisabled    = ImVec4(0.580f, 0.500f, 0.420f, 1.000f);
  const ImVec4 separator       = ImVec4(0.470f, 0.300f, 0.150f, 1.000f);
  const ImVec4 headerBg        = ImVec4(0.250f, 0.170f, 0.100f, 1.000f); // collapsing-header bg
  const ImVec4 titleActive     = pyxOrangeDeep;

  colors[ImGuiCol_Text]                  = text;
  colors[ImGuiCol_TextDisabled]          = textDisabled;
  colors[ImGuiCol_WindowBg]              = windowBg;
  colors[ImGuiCol_ChildBg]               = childBg;
  colors[ImGuiCol_PopupBg]               = popupBg;
  colors[ImGuiCol_Border]                = border;
  colors[ImGuiCol_BorderShadow]          = borderShadow;
  colors[ImGuiCol_FrameBg]               = frameBg;
  colors[ImGuiCol_FrameBgHovered]        = frameHover;
  colors[ImGuiCol_FrameBgActive]         = frameActive;
  colors[ImGuiCol_TitleBg]               = ImVec4(0.080f, 0.080f, 0.080f, 1.000f);
  colors[ImGuiCol_TitleBgActive]         = titleActive;
  colors[ImGuiCol_TitleBgCollapsed]      = ImVec4(0.080f, 0.080f, 0.080f, 0.760f);
  colors[ImGuiCol_MenuBarBg]             = ImVec4(0.135f, 0.135f, 0.135f, 1.000f);
  colors[ImGuiCol_ScrollbarBg]           = ImVec4(0.080f, 0.080f, 0.080f, 0.530f);
  colors[ImGuiCol_ScrollbarGrab]         = ImVec4(0.310f, 0.310f, 0.310f, 1.000f);
  colors[ImGuiCol_ScrollbarGrabHovered]  = ImVec4(0.410f, 0.410f, 0.410f, 1.000f);
  colors[ImGuiCol_ScrollbarGrabActive]   = pyxOrange;
  colors[ImGuiCol_CheckMark]             = pyxOrange;
  colors[ImGuiCol_SliderGrab]            = pyxOrange;
  colors[ImGuiCol_SliderGrabActive]      = pyxOrangeHot;
  colors[ImGuiCol_Button]                = frameBg;
  colors[ImGuiCol_ButtonHovered]         = pyxOrangeDim;
  colors[ImGuiCol_ButtonActive]          = pyxOrange;
  colors[ImGuiCol_Header]                = headerBg;
  colors[ImGuiCol_HeaderHovered]         = pyxOrangeDim;
  colors[ImGuiCol_HeaderActive]          = pyxOrange;
  colors[ImGuiCol_Separator]             = separator;
  colors[ImGuiCol_SeparatorHovered]      = pyxOrangeDim;
  colors[ImGuiCol_SeparatorActive]       = pyxOrange;
  colors[ImGuiCol_ResizeGrip]            = ImVec4(0.620f, 0.405f, 0.190f, 0.250f);
  colors[ImGuiCol_ResizeGripHovered]     = pyxOrangeDim;
  colors[ImGuiCol_ResizeGripActive]      = pyxOrange;
  colors[ImGuiCol_Tab]                   = headerBg;
  colors[ImGuiCol_TabHovered]            = pyxOrangeDim;
  colors[ImGuiCol_TabSelected]           = pyxOrangeMuted;
  colors[ImGuiCol_TabDimmed]             = ImVec4(0.080f, 0.080f, 0.080f, 0.970f);
  colors[ImGuiCol_TabDimmedSelected]     = ImVec4(0.300f, 0.200f, 0.090f, 1.000f);
  colors[ImGuiCol_PlotLines]             = pyxOrange;
  colors[ImGuiCol_PlotLinesHovered]      = pyxOrangeHot;
  colors[ImGuiCol_PlotHistogram]         = pyxOrange;
  colors[ImGuiCol_PlotHistogramHovered]  = pyxOrangeHot;
  colors[ImGuiCol_TextSelectedBg]        = ImVec4(0.470f, 0.305f, 0.140f, 0.350f);
  colors[ImGuiCol_DragDropTarget]        = pyxOrange;
  colors[ImGuiCol_NavCursor]             = pyxOrange;
  colors[ImGuiCol_NavWindowingHighlight] = ImVec4(1.000f, 1.000f, 1.000f, 0.700f);
  colors[ImGuiCol_NavWindowingDimBg]     = ImVec4(0.800f, 0.800f, 0.800f, 0.200f);
  colors[ImGuiCol_ModalWindowDimBg]      = ImVec4(0.800f, 0.800f, 0.800f, 0.350f);

  // Geometry: slim padding + small rounding for an engineering look.
  style.WindowPadding     = ImVec2(8.0f, 8.0f);
  style.WindowRounding    = 3.0f;
  style.WindowBorderSize  = 1.0f;
  style.FramePadding      = ImVec2(6.0f, 4.0f);
  style.FrameRounding     = 3.0f;
  style.FrameBorderSize   = 0.0f;
  style.ItemSpacing       = ImVec2(8.0f, 4.0f);
  style.ItemInnerSpacing  = ImVec2(6.0f, 4.0f);
  style.IndentSpacing     = 16.0f;
  style.ScrollbarSize     = 12.0f;
  style.ScrollbarRounding = 3.0f;
  style.GrabMinSize       = 8.0f;
  style.GrabRounding      = 2.0f;
  style.TabRounding       = 3.0f;
}

// (File-save / file-open COM dialogs moved to EditorPanel.cpp where
// the only call sites live — see the audit-driven split.)

// M11 — Performance panel "Save profile JSON..." button. Writes a
// snapshot of the current 240-frame rolling stats + frame index +
// timestamp. Distinct from headless `--profile` (which carries
// adapter / driver / config / bench metadata for perf_compare's
// rolling median); this viewer-side dump is dev-triage-only and
// stays format-compatible only at the `bench.passes` level so
// perf_compare can ingest both. Hand-rolled JSON keeps the pyxis_app
// `/EHs-c-` perimeter exception-free.
void WriteViewerProfileJson(const std::string& path,
                            std::uint64_t frameIndex,
                            std::span<const Profiler::RollingStat> rollingStats) noexcept {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out)
  {
    Logging::Get().Warn(log::APP,
        "viewer: profile JSON write failed (could not open " + path + ")");
    return;
  }
  const auto unixSec = std::chrono::duration_cast<std::chrono::seconds>(
                           std::chrono::system_clock::now().time_since_epoch())
                           .count();
  out << "{\n";
  out << "  \"source\": \"viewer\",\n";
  out << "  \"timestamp_unix\": " << unixSec << ",\n";
  out << "  \"frame_index\": " << frameIndex << ",\n";
  out << "  \"bench\": {\n";
  out << "    \"frames\": " << Profiler::ROLLING_WINDOW_FRAMES << ",\n";
  out << "    \"passes\": [";
  bool firstPass = true;
  char numBuf[64];
  for (const Profiler::RollingStat& stat : rollingStats)
  {
    if (stat.name.size == 0 || stat.sampleCount == 0)
      continue;
    const std::string_view nameView = stat.name.View();
    out << (firstPass ? "\n      " : ",\n      ");
    firstPass = false;
    out << "{\"name\": \"";
    out.write(nameView.data(), static_cast<std::streamsize>(nameView.size()));
    out << "\", \"kind\": \""
        << (stat.kind == FrameProfile::ScopeKind::Gpu ? "Gpu" : "Cpu") << "\", ";
    std::snprintf(numBuf, sizeof(numBuf), "%.6f", stat.p50Ms);
    out << "\"p50_ms\": " << numBuf << ", ";
    std::snprintf(numBuf, sizeof(numBuf), "%.6f", stat.p99Ms);
    out << "\"p99_ms\": " << numBuf << ", ";
    std::snprintf(numBuf, sizeof(numBuf), "%.6f", stat.maxMs);
    out << "\"max_ms\": " << numBuf << ", ";
    out << "\"sample_count\": " << stat.sampleCount << "}";
  }
  out << (firstPass ? "]" : "\n    ]");
  out << "\n  }\n";
  out << "}\n";
  Logging::Get().Info(log::APP, "viewer: profile JSON written to " + path);
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
  ApplyPyxisTheme();

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

namespace {

// ImPlot-style dual-line chart drawn directly via ImDrawList. Two
// overlaid series (CPU green + GPU blue), auto-scaled Y axis with
// 4 horizontal grid lines + tick labels, top-right legend. Reproduces
// the visual of ImPlot::PlotLine without taking the dep — see the
// $ImGui-1.92-incompat block in _cmake/Thirdparty.cmake for why.
//
// Reads from a single ring buffer per series, head-offset-aware so the
// line "scrolls" left as new samples push in. Y range is computed
// across both rings + rounded up to the next 5 ms bucket so a one-
// frame spike doesn't rescale every other frame. ImGui ID via the
// implicit window ID + DummyItem reserve so multiple instances per
// window don't collide.
void DrawDualLineChart(const float* seriesA, const float* seriesB,
                       std::size_t sampleCount, std::size_t headOffset,
                       const char* labelA, const char* labelB,
                       float chartHeight) noexcept {
  if (sampleCount == 0)
    return;

  // Reserve canvas space; ImGui assigns the rect.
  const ImVec2 canvasOrigin = ImGui::GetCursorScreenPos();
  const float  canvasWidth  = ImGui::GetContentRegionAvail().x;
  const ImVec2 canvasSize(canvasWidth, chartHeight);
  ImGui::Dummy(canvasSize);
  ImDrawList* drawList = ImGui::GetWindowDrawList();

  // Padding inside the canvas: leave room on the left for Y-axis tick
  // labels and on the bottom for the X-axis baseline.
  const float padLeft   = 38.0f;
  const float padRight  =  6.0f;
  const float padTop    =  6.0f;
  const float padBottom = 14.0f;
  const ImVec2 plotMin(canvasOrigin.x + padLeft, canvasOrigin.y + padTop);
  const ImVec2 plotMax(canvasOrigin.x + canvasSize.x - padRight,
                       canvasOrigin.y + canvasSize.y - padBottom);
  const float  plotW = plotMax.x - plotMin.x;
  const float  plotH = plotMax.y - plotMin.y;

  // Background panel + frame.
  const ImU32 bgColor      = IM_COL32(20, 20, 20, 255);
  const ImU32 frameColor   = IM_COL32(58, 58, 58, 255);
  const ImU32 gridColor    = IM_COL32(45, 45, 45, 255);
  const ImU32 axisColor    = IM_COL32(90, 90, 90, 255);
  const ImU32 textColor    = IM_COL32(180, 180, 180, 255);
  const ImU32 cpuColor     = IM_COL32(245, 160, 74, 255);  // Pyxis orange
  const ImU32 gpuColor     = IM_COL32( 95, 175, 240, 255); // cool blue
  drawList->AddRectFilled(canvasOrigin,
                          ImVec2(canvasOrigin.x + canvasSize.x, canvasOrigin.y + canvasSize.y),
                          bgColor, 3.0f);
  drawList->AddRect(canvasOrigin,
                    ImVec2(canvasOrigin.x + canvasSize.x, canvasOrigin.y + canvasSize.y),
                    frameColor, 3.0f);

  // Auto-scale Y range. Combine both series for a single shared axis
  // so CPU + GPU read at the same scale.
  float maxMs = 1.0f;
  for (std::size_t i = 0; i < sampleCount; ++i)
  {
    if (seriesA[i] > maxMs) maxMs = seriesA[i];
    if (seriesB[i] > maxMs) maxMs = seriesB[i];
  }
  maxMs = std::ceil(maxMs / 5.0f) * 5.0f;
  if (maxMs < 5.0f) maxMs = 5.0f;

  // Y-axis grid + tick labels (4 ticks: 0, max/4, max/2, 3*max/4, max).
  for (int tick = 0; tick <= 4; ++tick)
  {
    const float frac = static_cast<float>(tick) / 4.0f;
    const float yPos = plotMax.y - frac * plotH;
    const ImU32 lineCol = (tick == 0) ? axisColor : gridColor;
    drawList->AddLine(ImVec2(plotMin.x, yPos), ImVec2(plotMax.x, yPos), lineCol, 1.0f);
    char tickLabel[16];
    std::snprintf(tickLabel, sizeof(tickLabel), "%.0fms", frac * maxMs);
    const ImVec2 textSize = ImGui::CalcTextSize(tickLabel);
    drawList->AddText(ImVec2(plotMin.x - textSize.x - 4.0f, yPos - textSize.y * 0.5f),
                      textColor, tickLabel);
  }

  // X-axis baseline + frame's left edge.
  drawList->AddLine(ImVec2(plotMin.x, plotMin.y),
                    ImVec2(plotMin.x, plotMax.y), axisColor, 1.0f);

  // Plot a single ring buffer as a polyline. headOffset = next-write
  // index, so the OLDEST sample is at headOffset; we walk
  // sampleCount steps wrapping mod sampleCount to render
  // chronologically left→right.
  auto plotSeries = [&](const float* series, ImU32 color) {
    if (sampleCount < 2)
      return;
    std::vector<ImVec2> points;
    points.reserve(sampleCount);
    for (std::size_t i = 0; i < sampleCount; ++i)
    {
      const std::size_t idx = (headOffset + i) % sampleCount;
      const float       value = series[idx];
      const float lerp = static_cast<float>(i) / static_cast<float>(sampleCount - 1);
      const float xPos = plotMin.x + lerp * plotW;
      const float yPos = plotMax.y - (value / maxMs) * plotH;
      points.emplace_back(xPos, yPos);
    }
    drawList->AddPolyline(points.data(), static_cast<int>(points.size()), color, 1.5f, 0);
  };
  plotSeries(seriesA, cpuColor);
  plotSeries(seriesB, gpuColor);

  // Legend (top-right): two color swatches + labels.
  const float legendPad = 6.0f;
  const ImVec2 legendOrigin(plotMax.x - 100.0f, plotMin.y + legendPad);
  const float swatchW = 12.0f;
  const float swatchH = 8.0f;
  drawList->AddRectFilled(legendOrigin,
                          ImVec2(legendOrigin.x + swatchW, legendOrigin.y + swatchH),
                          cpuColor);
  drawList->AddText(ImVec2(legendOrigin.x + swatchW + 4.0f, legendOrigin.y - 2.0f),
                    textColor, labelA);
  drawList->AddRectFilled(ImVec2(legendOrigin.x, legendOrigin.y + swatchH + 4.0f),
                          ImVec2(legendOrigin.x + swatchW,
                                 legendOrigin.y + swatchH + 4.0f + swatchH),
                          gpuColor);
  drawList->AddText(ImVec2(legendOrigin.x + swatchW + 4.0f,
                           legendOrigin.y + swatchH + 2.0f),
                    textColor, labelB);
}

}  // namespace

void ImGuiHost::BuildFpsPanel(const FrameProfile& frameProfile,
                              std::span<const Profiler::RollingStat> rollingStats) noexcept {
  if (!_ready)
    return;

  // Push the current frame's CPU + GPU ms into the rolling history
  // ring. Read by DrawDualLineChart below; the ring is sized to 240
  // frames (~4 s @ 60 Hz) — short enough to feel responsive,
  // long enough to spot a stutter without overwhelming the panel.
  _cpuMsHistory[_historyHead] = frameProfile.cpuFrameMs;
  _gpuMsHistory[_historyHead] = frameProfile.gpuFrameMs;
  _historyHead = (_historyHead + 1) % PERF_HISTORY_SIZE;

  // Latch the first non-zero CPU profile as the "loading" snapshot.
  // The render thread submits the load (mesh upload + BLAS build +
  // TLAS rebuild) on frame 0 — that frame's cpu/gpu cost IS the
  // load profile; subsequent frames are steady-state render only.
  // FrameProfile.passes is a span into Profiler-owned storage that
  // gets overwritten next BeginFrame, so we OWN-COPY it here for the
  // Loading section's pass breakdown to survive across frames.
  if (!_loadingProfileLatched && frameProfile.cpuFrameMs > 0.0f)
  {
    _loadingCpuMs = frameProfile.cpuFrameMs;
    _loadingGpuMs = frameProfile.gpuFrameMs;
    _loadingPasses.assign(frameProfile.passes.begin(), frameProfile.passes.end());
    _loadingProfileLatched = true;
  }

  // Layout: Performance is the TOP-RIGHT panel. Anchor to the right
  // viewport edge with a per-frame SetNextWindowPos so the panel
  // stays glued to the right when the window resizes. AlwaysAutoResize
  // sizes the window to its content; users still see the whole panel
  // without scrollbars.
  const ImGuiViewport* viewport = ImGui::GetMainViewport();
  const float perfPanelW = 440.0f;
  ImGui::SetNextWindowPos(
      ImVec2(viewport->WorkPos.x + viewport->WorkSize.x - perfPanelW - 10.0f,
             viewport->WorkPos.y + 10.0f),
      ImGuiCond_Always);
  ImGui::SetNextWindowSizeConstraints(ImVec2(perfPanelW, 0.0f),
                                      ImVec2(perfPanelW, FLT_MAX));
  if (ImGui::Begin("Performance", nullptr,
                   ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings))
  {
    const ImGuiIO& imguiIO = ImGui::GetIO();
    ImGui::Text("Frame: %llu",
                static_cast<unsigned long long>(frameProfile.frameIndex));
    ImGui::SameLine(180.0f);
    ImGui::Text("FPS: %.1f", static_cast<double>(imguiIO.Framerate));
    ImGui::Text("CPU: %.3f ms     GPU: %.3f ms", frameProfile.cpuFrameMs,
                frameProfile.gpuFrameMs);

    // Single combined CPU + GPU chart (replaces the two separate
    // PlotLines). NVIDIA-green CPU + cool-blue GPU on a shared Y-axis.
    DrawDualLineChart(_cpuMsHistory, _gpuMsHistory, PERF_HISTORY_SIZE,
                      _historyHead, "CPU", "GPU", 110.0f);

    // ----- Loading (one-shot snapshot of the first frame's cost) ------
    // The first frame includes mesh upload + BLAS build + TLAS
    // rebuild + first PathTracePass dispatch — effectively the load
    // profile. Subsequent frames are steady-state render only, so
    // these values stay frozen after the first non-zero capture.
    // The pass breakdown beneath each total mirrors the live "CPU
    // passes" / "GPU passes" sections; same indentation rules.
    if (ImGui::CollapsingHeader("Loading"))
    {
      if (_loadingProfileLatched)
      {
        ImGui::TextDisabled(
            "(captured on first frame; mesh upload + BLAS / TLAS build +\n"
            "  first PathTracePass dispatch all happen here)");

        // Ingest pass — populated by ViewerMode via SetIngestProfile()
        // right after the engine.Load() that ran BEFORE the frame loop
        // started. NOT part of the frame profile (the profiler only
        // sees scopes opened/closed inside Profiler::BeginFrame...
        // EndFrame); we surface it here so the user sees the full
        // load picture broken down by StageWalker sub-pass.
        if (!_ingestSourceLabel.empty())
        {
          ImGui::Spacing();
          ImGui::Text("Ingest (%s): %.1f ms total",
                      _ingestSourceLabel.c_str(), _ingestTotalMs);
          ImGui::Text("  UsdStage::Open      %.1f ms", _ingestStageOpenMs);
          ImGui::Text("  Traverse + sort     %.1f ms", _ingestTraverseSortMs);
          ImGui::Text("  Materials           %.1f ms", _ingestMaterialPassMs);
          ImGui::Text("  PointInstancers     %.1f ms", _ingestInstancerPassMs);
          ImGui::Text("  Meshes/lights/cam   %.1f ms", _ingestMeshLightCameraMs);
        }

        ImGui::Spacing();
        ImGui::Text("CPU load: %.3f ms", _loadingCpuMs);
        for (const FrameProfile::PassTiming& timing : _loadingPasses)
        {
          if (timing.kind != FrameProfile::ScopeKind::Cpu)
            continue;
          const std::string_view name = timing.name.View();
          if (name.empty())
            continue;
          ImGui::Text("  %*s%.*s   %.3f ms",
                      static_cast<int>(timing.depth * 2), "",
                      static_cast<int>(name.size()), name.data(),
                      timing.durationMs);
        }

        ImGui::Spacing();
        ImGui::Text("GPU load: %.3f ms", _loadingGpuMs);
        for (const FrameProfile::PassTiming& timing : _loadingPasses)
        {
          if (timing.kind != FrameProfile::ScopeKind::Gpu)
            continue;
          const std::string_view name = timing.name.View();
          if (name.empty())
            continue;
          ImGui::Text("  %*s%.*s   %.3f ms",
                      static_cast<int>(timing.depth * 2), "",
                      static_cast<int>(name.size()), name.data(),
                      timing.durationMs);
        }
      }
      else
      {
        ImGui::TextDisabled("(awaiting first non-zero frame profile)");
      }
    }

    // ----- CPU breakdown (collapsible like Lights/Materials) ----------
    // Skip empty-name and non-finite-duration entries: GPU timestamp
    // queries are async and can surface NaN / 0-name on the very first
    // few frames before the timestamp pool has wrapped through enough
    // samples, and clearing/recycling the ring can leave stale slots
    // until the next frame writes into them.
    auto drawPassRow = [](const FrameProfile::PassTiming& timing) {
      const std::string_view name = timing.name.View();
      if (name.empty())
        return;
      const float durationMs =
          std::isfinite(timing.durationMs) ? timing.durationMs : 0.0f;
      ImGui::Text("  %*s%.*s   %.3f ms",
                  static_cast<int>(timing.depth * 2), "",
                  static_cast<int>(name.size()), name.data(), durationMs);
    };
    if (ImGui::CollapsingHeader("CPU passes"))
    {
      for (const FrameProfile::PassTiming& timing : frameProfile.passes)
      {
        if (timing.kind == FrameProfile::ScopeKind::Cpu)
          drawPassRow(timing);
      }
    }

    // ----- GPU breakdown ----------------------------------------------
    if (ImGui::CollapsingHeader("GPU passes"))
    {
      for (const FrameProfile::PassTiming& timing : frameProfile.passes)
      {
        if (timing.kind == FrameProfile::ScopeKind::Gpu)
          drawPassRow(timing);
      }
    }

    // ----- M11 — rolling p50 / p99 / max (240-frame window) -----------
    // Plan §34.2 / §34.3 KPI surface. The Profiler maintains one ring
    // per named scope; we drain the current percentiles into the
    // panel without copying scope-name strings (RollingStat carries
    // its own inline name buffer). Empty span = profiler hasn't seen
    // a single drained frame yet — show a placeholder.
    if (ImGui::CollapsingHeader("Rolling (240f)"))
    {
      if (rollingStats.empty())
      {
        ImGui::TextDisabled("(warming up — no drained samples yet)");
      }
      else
      {
        ImGui::Text("%-30s  %-3s  %7s  %7s  %7s  %s",
                    "scope", "kind", "p50", "p99", "max", "n");
        for (const Profiler::RollingStat& stat : rollingStats)
        {
          const std::string_view name = stat.name.View();
          if (name.empty() || stat.sampleCount == 0)
            continue;
          ImGui::Text("%-30.*s  %-3s  %5.3fms  %5.3fms  %5.3fms  %u",
                      static_cast<int>(name.size()), name.data(),
                      stat.kind == FrameProfile::ScopeKind::Gpu ? "GPU" : "CPU",
                      stat.p50Ms, stat.p99Ms, stat.maxMs, stat.sampleCount);
        }

        // Save-profile-JSON button. Synchronous: opens a save dialog,
        // writes the JSON, returns. No latch / no ViewerMode drain
        // since the write is allocation-only and doesn't touch the
        // GPU or the scene. Format aligns with headless --profile's
        // bench.passes array so perf_compare ingests both.
        ImGui::Spacing();
        if (ImGui::Button("Save profile JSON..."))
        {
          SaveFilePickerSpec profileSpec{};
          profileSpec.title             = L"Pyxis - Save profile JSON";
          profileSpec.filterLabel       = L"Profile JSON (*.json)";
          profileSpec.filterGlob        = L"*.json";
          profileSpec.defaultExtension  = L"json";
          profileSpec.suggestedFileName = L"pyxis_profile.json";
          const std::string picked = SaveFilePickerDialog(profileSpec);
          if (!picked.empty())
            WriteViewerProfileJson(picked, frameProfile.frameIndex, rollingStats);
        }
      }
    }

    // ----- System (RAM + VRAM) ----------------------------------------
    if (ImGui::CollapsingHeader("System"))
    {
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
    }  // end "System" CollapsingHeader
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
  // Format bytes UNIFORMLY in MiB so adjacent rows are visually
  // additive (the user can mentally check "Vertex + Index + Texture
  // + BLAS + TLAS == Scene total" without converting between B / KiB
  // / MiB / GiB by eye). 4 decimal places keeps tiny-fixture values
  // readable down to ~100 B (~0.0001 MiB); World Lobby-class scenes with
  // hundreds of MiB still fit in the same column.
  // Pre-fix this auto-picked the unit per row; the audit flagged the
  // mixed-unit display as the underlying source of "Total doesn't
  // seem to add right" complaints.
  auto formatBytes = [](uint64_t bytes, char* buf, std::size_t bufSize) {
    const double mib = static_cast<double>(bytes) / static_cast<double>(1ull << 20);
    std::snprintf(buf, bufSize, "%9.4f MiB", mib);
  };
  char bytesBuf[32];

  // Layout: Scene is the TOP-LEFT panel. Editor stacks below it.
  // ImGuiCond_Always re-anchors every frame so the panel stays in
  // place when the OS window resizes; AlwaysAutoResize sizes the
  // window to its content (no scrollbars, no manual resize).
  const ImGuiViewport* viewport = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + 10.0f,
                                 viewport->WorkPos.y + 10.0f),
                          ImGuiCond_Always);
  if (ImGui::Begin("Stats", nullptr,
                   ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings))
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
    // Scene-only — does NOT include AOV textures, the swapchain
    // images, ImGui's descriptor pool, the path-tracer's shader
    // tables, or any other allocator outside GpuScene. The System
    // panel's VRAM row is the right number for "what's this process
    // actually consuming on the GPU"; this row is "what GpuScene's
    // tables added up to".
    ImGui::Text("Scene total : %s  (excl. AOVs / pipelines)", bytesBuf);

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
    // Capture the panel's rendered height so BuildEditorPanel can
    // anchor itself below us next frame without overlap.
    _layoutScenePanelHeight = ImGui::GetWindowHeight();
  }
  ImGui::End();
}

// `void ImGuiHost::BuildEditorPanel(GpuScene&)` lives in
// EditorPanel.cpp — same class, separate translation unit, full
// access to private fields. The split keeps this file focused on
// lifecycle / theme / Stats / Performance.
//
// The body that used to live here (Editor panel: top-level actions,
// AOV inspector, picker readout, F-key pin, viewport crosshair,
// Camera section, Lights section, Materials section + click-to-select
// auto-expand) is the SAME code, just relocated. No behavioural
// change — see the audit-driven file-split commit for the full move.
//
// PLACEHOLDER MARKER (do not delete): legacy body removed below.

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
