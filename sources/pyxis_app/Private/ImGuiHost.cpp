// Pyxis app — ImGui host implementation.

#include "ImGuiHost.h"

#include <Pyxis/Platform/Device/IDeviceManager.h>
#include <Pyxis/Platform/Device/VulkanContext.h>
#include <Pyxis/Platform/Logging/Log.h>
#include <Pyxis/Platform/Logging/LogCategories.h>
#include <Pyxis/Platform/Window/IWindow.h>

#include <nvrhi/nvrhi.h>

#include <vulkan/vulkan.h>

#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_vulkan.h>

#include <cstring>

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
    if (err == VK_SUCCESS) return;
    auto& log = pyxis::Logging::Get();
    char  msg[96];
    std::snprintf(msg, sizeof(msg), "ImGui Vulkan VkResult = %d", static_cast<int>(err));
    log.Warn(pyxis::log::APP, msg);
}

}  // namespace

ImGuiHost::~ImGuiHost() { Shutdown(); }

bool ImGuiHost::Init(IWindow* window, IDeviceManager* deviceManager) noexcept {
    auto& log = Logging::Get();
    if (!window || !deviceManager) {
        log.Error(log::APP, "ImGuiHost::Init: null window or device manager");
        return false;
    }

    const VulkanContext vk = deviceManager->GetVulkanContext();
    if (!vk.instance || !vk.physicalDevice || !vk.device || !vk.graphicsQueue) {
        log.Error(log::APP, "ImGuiHost::Init: device manager surfaced null Vulkan handles");
        return false;
    }

    _instance = vk.instance;
    auto vkInstance = static_cast<VkInstance>(vk.instance);
    auto vkPhys     = static_cast<VkPhysicalDevice>(vk.physicalDevice);
    auto vkDevice   = static_cast<VkDevice>(vk.device);
    auto vkQueue    = static_cast<VkQueue>(vk.graphicsQueue);

    // -- ImGui context ----------------------------------------------------
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;                                  // no imgui.ini side-files
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    ImGui::StyleColorsDark();

    // -- GLFW backend -----------------------------------------------------
    auto* glfwWindow = static_cast<GLFWwindow*>(window->NativeHandle());
    if (!glfwWindow || !ImGui_ImplGlfw_InitForVulkan(glfwWindow, /*install_callbacks*/true)) {
        log.Error(log::APP, "ImGuiHost::Init: ImGui_ImplGlfw_InitForVulkan failed");
        Shutdown();
        return false;
    }

    // -- Vulkan backend ---------------------------------------------------
    if (!ImGui_ImplVulkan_LoadFunctions(VK_API_VERSION_1_3, &ImGuiVulkanLoader, _instance)) {
        log.Error(log::APP, "ImGuiHost::Init: ImGui_ImplVulkan_LoadFunctions failed");
        Shutdown();
        return false;
    }

    const auto colorFormat = static_cast<VkFormat>(vk.colorFormat);

    ImGui_ImplVulkan_InitInfo init{};
    init.ApiVersion        = VK_API_VERSION_1_3;
    init.Instance          = vkInstance;
    init.PhysicalDevice    = vkPhys;
    init.Device            = vkDevice;
    init.QueueFamily       = vk.graphicsFamily;
    init.Queue             = vkQueue;
    init.DescriptorPool     = VK_NULL_HANDLE;
    init.DescriptorPoolSize = IMGUI_DESCRIPTOR_POOL_SIZE;   // ImGui builds its own pool with the right types.
    init.MinImageCount      = 2;
    init.ImageCount         = deviceManager->GetBackbufferCount();
    init.UseDynamicRendering = true;
    init.CheckVkResultFn   = &ImGuiVulkanCheckResult;

    init.PipelineInfoMain.PipelineRenderingCreateInfo.sType =
        VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR;
    init.PipelineInfoMain.PipelineRenderingCreateInfo.colorAttachmentCount = 1;
    init.PipelineInfoMain.PipelineRenderingCreateInfo.pColorAttachmentFormats = &colorFormat;
    init.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;

    if (!ImGui_ImplVulkan_Init(&init)) {
        log.Error(log::APP, "ImGuiHost::Init: ImGui_ImplVulkan_Init failed");
        Shutdown();
        return false;
    }

    _ready = true;
    log.Info(log::APP, "ImGuiHost: docking + Vulkan backend ready");
    return true;
}

void ImGuiHost::Shutdown() noexcept {
    if (_ready) {
        ImGui_ImplVulkan_Shutdown();
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();
        _ready = false;
    }
    _instance = nullptr;
}

void ImGuiHost::BeginFrame() noexcept {
    if (!_ready) return;
    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();
}

void ImGuiHost::BuildFpsPanel(double cpuFrameMs,
                              double gpuFrameMs,
                              uint64_t frameIndex) noexcept {
    if (!_ready) return;

    // A dockable panel — flagged as dock-friendly via no special flags;
    // the user can drag it anywhere or dock it against the viewport
    // edges thanks to ImGuiConfigFlags_DockingEnable.
    if (ImGui::Begin("Performance")) {
        const ImGuiIO& io  = ImGui::GetIO();
        ImGui::Text("Frame: %llu", static_cast<unsigned long long>(frameIndex));
        ImGui::Text("FPS  : %.1f", static_cast<double>(io.Framerate));
        ImGui::Separator();
        ImGui::Text("CPU  : %.3f ms", cpuFrameMs);
        ImGui::Text("GPU  : %.3f ms", gpuFrameMs);
    }
    ImGui::End();
}

void ImGuiHost::Render() noexcept {
    if (!_ready) return;
    ImGui::Render();
}

void ImGuiHost::Submit(nvrhi::ICommandList* commandList, nvrhi::ITexture* colorTarget) noexcept {
    if (!_ready || !commandList || !colorTarget) return;

    ImDrawData* drawData = ImGui::GetDrawData();
    if (!drawData || drawData->CmdListsCount == 0) return;

    // Lift the raw Vulkan handles out of NVRHI. We're inside an open
    // command list (commandList->open() was called by the caller);
    // render-pass boundaries are NVRHI's responsibility — we begin our
    // own dynamic-rendering block here on top of NVRHI's command buffer.
    // nvrhi::Object has a templated `operator T*()` so the native handle
    // converts directly to its Vulkan pointer-typed alias (VkCommandBuffer
    // is `struct VkCommandBuffer_T*` etc.). No reinterpret_cast needed.
    VkCommandBuffer vkCmd = commandList->getNativeObject(nvrhi::ObjectTypes::VK_CommandBuffer);
    VkImageView     vkView = colorTarget->getNativeView(
        nvrhi::ObjectTypes::VK_ImageView,
        nvrhi::Format::UNKNOWN,
        nvrhi::AllSubresources,
        nvrhi::TextureDimension::Texture2D,
        /*isReadOnlyDSV*/ false);
    if (!vkCmd || !vkView) return;

    const auto& tdesc = colorTarget->getDesc();

    VkRenderingAttachmentInfo color{};
    color.sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    color.imageView   = vkView;
    color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp      = VK_ATTACHMENT_LOAD_OP_LOAD;     // preserve renderer's content
    color.storeOp     = VK_ATTACHMENT_STORE_OP_STORE;

    VkRenderingInfo ri{};
    ri.sType                = VK_STRUCTURE_TYPE_RENDERING_INFO;
    ri.renderArea.offset    = { 0, 0 };
    ri.renderArea.extent    = { tdesc.width, tdesc.height };
    ri.layerCount           = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments    = &color;

    vkCmdBeginRendering(vkCmd, &ri);
    ImGui_ImplVulkan_RenderDrawData(drawData, vkCmd);
    vkCmdEndRendering(vkCmd);
}

}  // namespace pyxis::app
