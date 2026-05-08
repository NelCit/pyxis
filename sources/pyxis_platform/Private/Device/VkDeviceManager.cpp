// Pyxis platform — windowed VkDeviceManager (M1).
//
// Plan §5.c / §5.b. Picks an adapter, enables the §5.b required extensions
// + VK_KHR_swapchain, creates the VkDevice + queues, wraps it in
// nvrhi::IDevice, then builds a VkSurfaceKHR from the IWindow and a
// matching VkSwapchainKHR. Each swapchain VkImage is wrapped into an
// nvrhi::ITexture so renderer passes can write into it like any other
// AOV.

#include "Device/VkDeviceManager.h"

#include "Device/VulkanFeatureCheck.h"

#include <Pyxis/Platform/Logging/Log.h>
#include <Pyxis/Platform/Logging/LogCategories.h>
#include <Pyxis/Platform/Window/IWindow.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <string>
#include <vector>

namespace pyxis {

// Three-stage init for NVRHI's Vulkan-Hpp dispatch loader. Defined in
// Private/Device/VulkanHppStorage.cpp so vulkan.hpp doesn't leak into
// any other TU.
void VulkanHppInitFromLoader  (PFN_vkGetInstanceProcAddr) noexcept;
void VulkanHppInitFromInstance(VkInstance) noexcept;
void VulkanHppInitFromDevice  (VkDevice)   noexcept;

namespace {

constexpr uint32_t VULKAN_API_VERSION = VK_API_VERSION_1_3;

const char* StatusName(DeviceManagerCreateStatus s) noexcept {
    switch (s) {
        case DeviceManagerCreateStatus::Ok:                       return "Ok";
        case DeviceManagerCreateStatus::NoVulkanDriver:           return "NoVulkanDriver";
        case DeviceManagerCreateStatus::NoCompatibleAdapter:      return "NoCompatibleAdapter";
        case DeviceManagerCreateStatus::FeatureMissing:           return "FeatureMissing";
        case DeviceManagerCreateStatus::InstanceCreationFailed:   return "InstanceCreationFailed";
        case DeviceManagerCreateStatus::DeviceCreationFailed:     return "DeviceCreationFailed";
        case DeviceManagerCreateStatus::SurfaceCreationFailed:    return "SurfaceCreationFailed";
        case DeviceManagerCreateStatus::SwapchainCreationFailed:  return "SwapchainCreationFailed";
        case DeviceManagerCreateStatus::OutOfMemory:              return "OutOfMemory";
        case DeviceManagerCreateStatus::Unknown:                  return "Unknown";
    }
    return "?";
}

VkInstance CreateInstance(bool enableValidation,
                          std::string_view appName, uint32_t appVersion,
                          DeviceManagerCreateStatus* outStatus) noexcept {
    VkApplicationInfo appInfo{};
    appInfo.sType              = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    const std::string namebuf{ appName };
    appInfo.pApplicationName   = namebuf.c_str();
    appInfo.applicationVersion = appVersion;
    appInfo.pEngineName        = "Pyxis";
    appInfo.engineVersion      = 0;
    appInfo.apiVersion         = VULKAN_API_VERSION;

    // M1 requires the surface + swapchain extensions for the windowed mode.
    std::vector<const char*> extensions;
    extensions.push_back(VK_KHR_SURFACE_EXTENSION_NAME);
    extensions.push_back(VK_KHR_WIN32_SURFACE_EXTENSION_NAME);

    std::vector<const char*> layers;
    if (enableValidation) {
        layers.push_back("VK_LAYER_KHRONOS_validation");
    }

    VkInstanceCreateInfo info{};
    info.sType                   = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    info.pApplicationInfo        = &appInfo;
    info.enabledLayerCount       = static_cast<uint32_t>(layers.size());
    info.ppEnabledLayerNames     = layers.data();
    info.enabledExtensionCount   = static_cast<uint32_t>(extensions.size());
    info.ppEnabledExtensionNames = extensions.data();

    VkInstance instance = VK_NULL_HANDLE;
    if (vkCreateInstance(&info, nullptr, &instance) != VK_SUCCESS) {
        if (outStatus) *outStatus = DeviceManagerCreateStatus::InstanceCreationFailed;
        return VK_NULL_HANDLE;
    }
    return instance;
}

// nvrhi MessageCallback adapter — routes NVRHI diagnostics through the
// §33.10 cross-DLL Logger.
class NvrhiMessageCallback final : public nvrhi::IMessageCallback {
public:
    void message(nvrhi::MessageSeverity severity, const char* messageText) override {
        auto& log = Logging::Get();
        const std::string_view msg{ messageText ? messageText : "" };
        switch (severity) {
            case nvrhi::MessageSeverity::Info:    log.Info   (log::PLATFORM, msg); break;
            case nvrhi::MessageSeverity::Warning: log.Warn   (log::PLATFORM, msg); break;
            case nvrhi::MessageSeverity::Error:   log.Error  (log::PLATFORM, msg); break;
            case nvrhi::MessageSeverity::Fatal:   log.Critical(log::PLATFORM, msg); break;
        }
    }
};

NvrhiMessageCallback& NvrhiCallback() {
    static NvrhiMessageCallback cb;
    return cb;
}

}  // namespace

VkDeviceManager::VkDeviceManager(const DeviceCreationParams& params,
                                 IWindow*                    window,
                                 const Resolution&           initialBackbuffer,
                                 DeviceManagerCreateStatus*  outStatus) noexcept {
    const DeviceManagerCreateStatus status = Bringup(params, window, initialBackbuffer);
    if (outStatus) *outStatus = status;
    _ready = (status == DeviceManagerCreateStatus::Ok);
}

VkDeviceManager::~VkDeviceManager() {
    Teardown();
}

DeviceManagerCreateStatus VkDeviceManager::Bringup(const DeviceCreationParams& params,
                                                   IWindow*                    window,
                                                   const Resolution&           initialBackbuffer) noexcept {
    auto& log = Logging::Get();
    _backbuffer     = initialBackbuffer;
    // M1 pins the active runtime to 1 frame in flight — see VkDeviceManager.h.
    // The §33.1 compile-time cap of 3 still bounds future growth.
    _framesInFlight = 1;
    (void)params.framesInFlight;
    _window         = window;

    if (!window) {
        log.Error(log::PLATFORM, "VkDeviceManager: IWindow* is null (viewer mode requires a window)");
        return DeviceManagerCreateStatus::SurfaceCreationFailed;
    }

    // ---- Vulkan-Hpp loader (stage 1) -------------------------------------
    // NVRHI uses vulkan.hpp's dynamic dispatcher; it has to be init'd from
    // vkGetInstanceProcAddr before *any* vk:: helper is called, otherwise
    // vulkan.hpp's per-call assertion fires.
    VulkanHppInitFromLoader(&vkGetInstanceProcAddr);

    // ---- VkInstance ------------------------------------------------------
    DeviceManagerCreateStatus instStatus = DeviceManagerCreateStatus::Ok;
    _instance = CreateInstance(params.enableValidation,
                               params.applicationName, params.applicationVersion,
                               &instStatus);
    if (_instance == VK_NULL_HANDLE) {
        log.Error(log::PLATFORM, "VkDeviceManager: VkInstance creation failed");
        return instStatus;
    }
    VulkanHppInitFromInstance(_instance);   // stage 2

    // ---- Surface ---------------------------------------------------------
    _surface = window->CreateVulkanSurface(_instance);
    if (_surface == VK_NULL_HANDLE) {
        log.Error(log::PLATFORM, "VkDeviceManager: surface creation failed");
        return DeviceManagerCreateStatus::SurfaceCreationFailed;
    }

    // ---- Pick a physical device -----------------------------------------
    uint32_t deviceCount = 0;
    vkEnumeratePhysicalDevices(_instance, &deviceCount, nullptr);
    if (deviceCount == 0) {
        log.Error(log::PLATFORM, "VkDeviceManager: no Vulkan-capable GPU found");
        return DeviceManagerCreateStatus::NoCompatibleAdapter;
    }
    std::vector<VkPhysicalDevice> physicalDevices(deviceCount);
    vkEnumeratePhysicalDevices(_instance, &deviceCount, physicalDevices.data());

    int32_t bestIndex = -1;
    uint64_t bestVram = 0;
    AdapterInfo bestAdapter{};
    for (uint32_t i = 0; i < deviceCount; ++i) {
        AdapterInfo info{};
        if (!QueryAdapterFeatures(physicalDevices[i], info)) continue;
        const bool isExplicit = (params.adapterIndex == static_cast<int32_t>(i));
        if (isExplicit) { bestIndex = static_cast<int32_t>(i); bestAdapter = info; break; }
        if (info.totalDeviceLocalBytes > bestVram) {
            bestVram    = info.totalDeviceLocalBytes;
            bestIndex   = static_cast<int32_t>(i);
            bestAdapter = info;
        }
    }
    if (bestIndex < 0) {
        log.Error(log::PLATFORM, "VkDeviceManager: no GPU satisfies plan-5.b required features");
        return DeviceManagerCreateStatus::FeatureMissing;
    }

    _physicalDevice = physicalDevices[static_cast<std::size_t>(bestIndex)];
    _adapter        = bestAdapter;

    // ---- Locate a graphics queue family that supports our surface -------
    uint32_t qfCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(_physicalDevice, &qfCount, nullptr);
    std::vector<VkQueueFamilyProperties> qfs(qfCount);
    vkGetPhysicalDeviceQueueFamilyProperties(_physicalDevice, &qfCount, qfs.data());

    bool foundGraphics = false;
    for (uint32_t i = 0; i < qfCount; ++i) {
        if (!(qfs[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) continue;
        VkBool32 presentSupport = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(_physicalDevice, i, _surface, &presentSupport);
        if (!presentSupport) continue;
        _graphicsFamily = i;
        foundGraphics   = true;
        break;
    }
    if (!foundGraphics) {
        log.Error(log::PLATFORM, "VkDeviceManager: no graphics+present queue family on this surface");
        return DeviceManagerCreateStatus::FeatureMissing;
    }

    // ---- VkDevice --------------------------------------------------------
    const float queuePriority = 1.0f;
    VkDeviceQueueCreateInfo qInfo{};
    qInfo.sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qInfo.queueFamilyIndex = _graphicsFamily;
    qInfo.queueCount       = 1;
    qInfo.pQueuePriorities = &queuePriority;

    std::array<const char*, 8> requiredExt{
        VK_KHR_SWAPCHAIN_EXTENSION_NAME,
        VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME,
        VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME,
        VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME,
        VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME,
        VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME,
        VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME,
        VK_EXT_HOST_QUERY_RESET_EXTENSION_NAME,
    };

    // Vulkan 1.3 features chain — sync2 + dynamic rendering + timeline semaphores
    // are mandatory per §5.b.
    VkPhysicalDeviceVulkan13Features v13{};
    v13.sType            = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    v13.synchronization2 = VK_TRUE;
    v13.dynamicRendering = VK_TRUE;

    VkPhysicalDeviceVulkan12Features v12{};
    v12.sType                                       = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    v12.timelineSemaphore                           = VK_TRUE;
    v12.bufferDeviceAddress                         = VK_TRUE;
    v12.descriptorIndexing                          = VK_TRUE;
    v12.runtimeDescriptorArray                      = VK_TRUE;
    v12.descriptorBindingPartiallyBound             = VK_TRUE;
    v12.shaderSampledImageArrayNonUniformIndexing   = VK_TRUE;
    v12.shaderStorageBufferArrayNonUniformIndexing  = VK_TRUE;
    v12.hostQueryReset                              = VK_TRUE;
    v12.pNext                                       = &v13;

    // Slang's emitted SPIR-V for the SV_VertexID-driven triangle vertex
    // shader (resources/shaders/triangle.vert.slang) declares the
    // `DrawParameters` capability. Without this v1.1 feature
    // VUID-VkShaderModuleCreateInfo-pCode-08740 fires at
    // vkCreateShaderModule.
    VkPhysicalDeviceVulkan11Features v11{};
    v11.sType                = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
    v11.shaderDrawParameters = VK_TRUE;
    v11.pNext                = &v12;

    VkPhysicalDeviceAccelerationStructureFeaturesKHR asFeats{};
    asFeats.sType                                   = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR;
    asFeats.accelerationStructure                   = VK_TRUE;
    asFeats.pNext                                   = &v11;

    VkPhysicalDeviceRayTracingPipelineFeaturesKHR rtFeats{};
    rtFeats.sType                                   = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR;
    rtFeats.rayTracingPipeline                      = VK_TRUE;
    rtFeats.pNext                                   = &asFeats;

    VkPhysicalDeviceFeatures2 features2{};
    features2.sType                                 = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    features2.features.shaderInt64                  = VK_TRUE;
    features2.features.shaderStorageImageReadWithoutFormat = VK_TRUE;
    features2.pNext                                 = &rtFeats;

    VkDeviceCreateInfo dInfo{};
    dInfo.sType                   = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dInfo.queueCreateInfoCount    = 1;
    dInfo.pQueueCreateInfos       = &qInfo;
    dInfo.enabledExtensionCount   = static_cast<uint32_t>(requiredExt.size());
    dInfo.ppEnabledExtensionNames = requiredExt.data();
    dInfo.pNext                   = &features2;

    if (vkCreateDevice(_physicalDevice, &dInfo, nullptr, &_device) != VK_SUCCESS) {
        log.Error(log::PLATFORM, "VkDeviceManager: vkCreateDevice failed");
        return DeviceManagerCreateStatus::DeviceCreationFailed;
    }
    vkGetDeviceQueue(_device, _graphicsFamily, 0, &_graphicsQueue);
    VulkanHppInitFromDevice(_device);       // stage 3

    // ---- nvrhi::IDevice wrap --------------------------------------------
    nvrhi::vulkan::DeviceDesc nvrhiDesc{};
    nvrhiDesc.errorCB             = &NvrhiCallback();
    nvrhiDesc.instance            = _instance;
    nvrhiDesc.physicalDevice      = _physicalDevice;
    nvrhiDesc.device              = _device;
    nvrhiDesc.graphicsQueue       = _graphicsQueue;
    nvrhiDesc.graphicsQueueIndex  = _graphicsFamily;
    nvrhiDesc.bufferDeviceAddressSupported = true;

    // Non-constexpr because nvrhi::vulkan::DeviceDesc::deviceExtensions
    // is `const char**` (pointer-to-non-const), so the elements must be
    // plain `const char*` (not `const char* const`). misc-const-correctness
    // would suggest making the elements `const char* const`, but that
    // breaks the assignment to NVRHI's field.
    static const char* nvrhiExts[] = {  // NOLINT(readability-identifier-naming, misc-const-correctness)
        VK_KHR_SWAPCHAIN_EXTENSION_NAME,
        VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME,
        VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME,
        VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME,
        VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME,
        VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME,
        VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME,
        VK_EXT_HOST_QUERY_RESET_EXTENSION_NAME,
    };
    nvrhiDesc.deviceExtensions    = nvrhiExts;
    nvrhiDesc.numDeviceExtensions = std::size(nvrhiExts);

    _nvrhiDevice = nvrhi::vulkan::createDevice(nvrhiDesc);
    if (!_nvrhiDevice) {
        log.Error(log::PLATFORM, "VkDeviceManager: nvrhi::vulkan::createDevice failed");
        return DeviceManagerCreateStatus::DeviceCreationFailed;
    }
    _nvrhiVulkan = static_cast<nvrhi::vulkan::IDevice*>(_nvrhiDevice.Get());

    // ---- Swapchain -------------------------------------------------------
    if (!CreateSwapchain(_backbuffer.width, _backbuffer.height)) {
        return DeviceManagerCreateStatus::SwapchainCreationFailed;
    }

    // ---- Swapchain sync semaphores --------------------------------------
    // framesInFlight = 1, so a single binary acquire/present pair is
    // enough — the timeline below makes sure each is GPU-retired before
    // its next reuse.
    VkSemaphoreCreateInfo binarySemInfo{};
    binarySemInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    vkCreateSemaphore(_device, &binarySemInfo, nullptr, &_acquireSem);
    vkCreateSemaphore(_device, &binarySemInfo, nullptr, &_presentSem);

    // CPU-throttle timeline. Initial value 0; signalled to _frameValue
    // alongside each frame's swapchain submit. BeginFrame waits on
    // (_frameValue - 1) before reusing _acquireSem.
    VkSemaphoreTypeCreateInfo timelineInfo{};
    timelineInfo.sType         = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
    timelineInfo.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    timelineInfo.initialValue  = 0;
    VkSemaphoreCreateInfo timelineSemInfo{};
    timelineSemInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    timelineSemInfo.pNext = &timelineInfo;
    vkCreateSemaphore(_device, &timelineSemInfo, nullptr, &_frameTimeline);
    _frameValue = 0;

    {
        std::string msg = "Vulkan device ready: ";
        msg.append(_adapter.NameView());
        msg += "  driver=";
        msg += std::to_string(_adapter.driverVersionRaw);
        msg += "  vram=";
        msg += std::to_string(_adapter.totalDeviceLocalBytes / (1024ull * 1024ull));
        msg += " MiB  swapchain=";
        msg += std::to_string(_swapchainExtent.width) + "x" + std::to_string(_swapchainExtent.height);
        msg += " (" + std::to_string(_swapchainTextures.size()) + " images)";
        log.Info(log::PLATFORM, msg);
    }
    return DeviceManagerCreateStatus::Ok;
}

bool VkDeviceManager::CreateSwapchain(uint32_t width, uint32_t height) noexcept {
    auto& log = Logging::Get();

    VkSurfaceCapabilitiesKHR caps{};
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(_physicalDevice, _surface, &caps);

    uint32_t formatCount = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(_physicalDevice, _surface, &formatCount, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(formatCount);
    vkGetPhysicalDeviceSurfaceFormatsKHR(_physicalDevice, _surface, &formatCount, formats.data());
    // §5: pin to BGRA8_UNORM_SRGB.
    VkSurfaceFormatKHR chosenFormat = formats[0];
    for (const auto& f : formats) {
        if (f.format == VK_FORMAT_B8G8R8A8_SRGB && f.colorSpace == VK_COLORSPACE_SRGB_NONLINEAR_KHR) {
            chosenFormat = f;
            break;
        }
    }

    uint32_t imageCount = caps.minImageCount + 1;
    if (caps.maxImageCount > 0 && imageCount > caps.maxImageCount) imageCount = caps.maxImageCount;

    VkExtent2D extent = caps.currentExtent;
    if (extent.width == UINT32_MAX) {
        extent.width  = std::clamp(width,  caps.minImageExtent.width,  caps.maxImageExtent.width);
        extent.height = std::clamp(height, caps.minImageExtent.height, caps.maxImageExtent.height);
    }

    VkSwapchainCreateInfoKHR scInfo{};
    scInfo.sType            = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    scInfo.surface          = _surface;
    scInfo.minImageCount    = imageCount;
    scInfo.imageFormat      = chosenFormat.format;
    scInfo.imageColorSpace  = chosenFormat.colorSpace;
    scInfo.imageExtent      = extent;
    scInfo.imageArrayLayers = 1;
    // TRANSFER_SRC enables --screenshot's vkCmdCopyImageToBuffer readback.
    // Most drivers grant it implicitly, but the spec doesn't require that;
    // validation flags VUID-vkCmdCopyImageToBuffer-srcImage-00186 without
    // it, and the cost (an extra usage bit on the swapchain images) is
    // negligible.
    scInfo.imageUsage       = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
                            | VK_IMAGE_USAGE_TRANSFER_DST_BIT
                            | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    scInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    scInfo.preTransform     = caps.currentTransform;
    scInfo.compositeAlpha   = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    scInfo.presentMode      = VK_PRESENT_MODE_FIFO_KHR;     // §28 default; mailbox/immediate are M2+
    scInfo.clipped          = VK_TRUE;
    scInfo.oldSwapchain     = VK_NULL_HANDLE;

    if (vkCreateSwapchainKHR(_device, &scInfo, nullptr, &_swapchain) != VK_SUCCESS) {
        log.Error(log::PLATFORM, "VkDeviceManager: vkCreateSwapchainKHR failed");
        return false;
    }
    _swapchainFormat = chosenFormat.format;
    _swapchainExtent = extent;

    uint32_t actualCount = 0;
    vkGetSwapchainImagesKHR(_device, _swapchain, &actualCount, nullptr);
    _swapchainImages.resize(actualCount);
    vkGetSwapchainImagesKHR(_device, _swapchain, &actualCount, _swapchainImages.data());

    // Wrap each VkImage in an nvrhi::ITexture so renderer code treats the
    // backbuffer like any other render target.
    _swapchainTextures.clear();
    _swapchainTextures.reserve(_swapchainImages.size());
    for (uint32_t i = 0; i < actualCount; ++i) {
        nvrhi::TextureDesc tdesc{};
        tdesc.format     = nvrhi::Format::SBGRA8_UNORM;
        tdesc.width      = extent.width;
        tdesc.height     = extent.height;
        tdesc.dimension  = nvrhi::TextureDimension::Texture2D;
        tdesc.isRenderTarget = true;
        tdesc.debugName  = "swapchain-image-" + std::to_string(i);
        tdesc.initialState     = nvrhi::ResourceStates::Present;
        tdesc.keepInitialState = true;
        nvrhi::TextureHandle h = _nvrhiVulkan->createHandleForNativeTexture(
            nvrhi::ObjectTypes::VK_Image,
            nvrhi::Object(_swapchainImages[i]),
            tdesc);
        if (!h) {
            log.Error(log::PLATFORM, "VkDeviceManager: nvrhi createHandleForNativeTexture failed");
            return false;
        }
        _swapchainTextures.push_back(std::move(h));
    }
    return true;
}

void VkDeviceManager::DestroySwapchain() noexcept {
    if (_device == VK_NULL_HANDLE) return;
    if (_acquireSem != VK_NULL_HANDLE) {
        vkDestroySemaphore(_device, _acquireSem, nullptr);
        _acquireSem = VK_NULL_HANDLE;
    }
    if (_presentSem != VK_NULL_HANDLE) {
        vkDestroySemaphore(_device, _presentSem, nullptr);
        _presentSem = VK_NULL_HANDLE;
    }
    _swapchainTextures.clear();
    _swapchainImages.clear();
    if (_swapchain != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(_device, _swapchain, nullptr);
        _swapchain = VK_NULL_HANDLE;
    }
}

void VkDeviceManager::Teardown() noexcept {
    if (_device != VK_NULL_HANDLE) vkDeviceWaitIdle(_device);
    DestroySwapchain();
    if (_device != VK_NULL_HANDLE && _frameTimeline != VK_NULL_HANDLE) {
        vkDestroySemaphore(_device, _frameTimeline, nullptr);
        _frameTimeline = VK_NULL_HANDLE;
    }
    _nvrhiDevice = nullptr;
    _nvrhiVulkan = nullptr;
    if (_device != VK_NULL_HANDLE) {
        vkDestroyDevice(_device, nullptr);
        _device = VK_NULL_HANDLE;
    }
    if (_surface != VK_NULL_HANDLE) {
        vkDestroySurfaceKHR(_instance, _surface, nullptr);
        _surface = VK_NULL_HANDLE;
    }
    if (_instance != VK_NULL_HANDLE) {
        vkDestroyInstance(_instance, nullptr);
        _instance = VK_NULL_HANDLE;
    }
}

nvrhi::IDevice* VkDeviceManager::GetDevice() const noexcept { return _nvrhiDevice.Get(); }
const AdapterInfo& VkDeviceManager::GetAdapterInfo() const noexcept { return _adapter; }
uint32_t VkDeviceManager::GetFramesInFlight() const noexcept { return _framesInFlight; }

nvrhi::ITexture* VkDeviceManager::GetCurrentBackbuffer() const noexcept {
    if (_swapchainTextures.empty()) return nullptr;
    return _swapchainTextures[_currentImage].Get();
}

nvrhi::ITexture* VkDeviceManager::GetBackbuffer(uint32_t index) const noexcept {
    if (index >= _swapchainTextures.size()) return nullptr;
    return _swapchainTextures[index].Get();
}

void VkDeviceManager::BeginFrame() {
    if (_swapchain == VK_NULL_HANDLE) return;

    // CPU-side throttle: wait for the previous frame's GPU submission to
    // retire before reusing _acquireSem. With framesInFlight = 1 this
    // serialises CPU and GPU per-frame, which is exactly the M1 contract.
    // Keeps validation's VUID-vkAcquireNextImageKHR-semaphore-01779
    // ("semaphore must not have pending operations") clean.
    if (_frameValue >= 1) {
        const uint64_t targetValue = _frameValue;
        VkSemaphoreWaitInfo wait{};
        wait.sType          = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
        wait.semaphoreCount = 1;
        wait.pSemaphores    = &_frameTimeline;
        wait.pValues        = &targetValue;
        vkWaitSemaphores(_device, &wait, UINT64_MAX);
    }

    ++_frameValue;

    const VkResult r = vkAcquireNextImageKHR(_device, _swapchain, UINT64_MAX,
                                             _acquireSem, VK_NULL_HANDLE, &_currentImage);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) {
        // The semaphore is *not* signalled in this case. Skip queuing waits
        // and trigger a swapchain rebuild on the next frame boundary.
        _resizePending = true;
        return;
    }
    if (r == VK_SUBOPTIMAL_KHR) {
        // Suboptimal: image is usable, but we'll rebuild after EndFrame.
        _resizePending = true;
    }

    if (_nvrhiVulkan) {
        // Pre-queue everything the renderer's executeCommandList will pick
        // up: wait on the acquire, signal the present sem (binary, used by
        // vkQueuePresentKHR), and signal the timeline (CPU throttle).
        _nvrhiVulkan->queueWaitForSemaphore  (nvrhi::CommandQueue::Graphics, _acquireSem,    0);
        _nvrhiVulkan->queueSignalSemaphore   (nvrhi::CommandQueue::Graphics, _presentSem,    0);
        _nvrhiVulkan->queueSignalSemaphore   (nvrhi::CommandQueue::Graphics, _frameTimeline, _frameValue);
    }
}

void VkDeviceManager::EndFrame() {
    if (_swapchain == VK_NULL_HANDLE) return;

    // The renderer's executeCommandList already happened with the wait +
    // signal we pre-queued in BeginFrame. Just present.
    VkPresentInfoKHR present{};
    present.sType              = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    present.waitSemaphoreCount = 1;
    present.pWaitSemaphores    = &_presentSem;
    present.swapchainCount     = 1;
    present.pSwapchains        = &_swapchain;
    present.pImageIndices      = &_currentImage;
    const VkResult r = vkQueuePresentKHR(_graphicsQueue, &present);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) {
        _resizePending = true;
    }

    if (_resizePending && _window) {
        const uint32_t w = _window->Width();
        const uint32_t h = _window->Height();
        if (w > 0 && h > 0) {
            vkDeviceWaitIdle(_device);
            DestroySwapchain();
            CreateSwapchain(w, h);
            // Sem pair was destroyed by DestroySwapchain — recreate.
            VkSemaphoreCreateInfo semInfo{};
            semInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
            vkCreateSemaphore(_device, &semInfo, nullptr, &_acquireSem);
            vkCreateSemaphore(_device, &semInfo, nullptr, &_presentSem);
            _resizePending = false;
            // The timeline persists across rebuilds; no reset needed.
        }
    }
}

void VkDeviceManager::WaitIdle() {
    if (_device != VK_NULL_HANDLE) vkDeviceWaitIdle(_device);
}

VulkanContext VkDeviceManager::GetVulkanContext() const noexcept {
    VulkanContext c{};
    c.instance       = static_cast<void*>(_instance);
    c.physicalDevice = static_cast<void*>(_physicalDevice);
    c.device         = static_cast<void*>(_device);
    c.graphicsQueue  = static_cast<void*>(_graphicsQueue);
    c.graphicsFamily = _graphicsFamily;
    c.colorFormat    = static_cast<uint32_t>(_swapchainFormat);
    return c;
}

IDeviceManager::~IDeviceManager() = default;

IDeviceManager* CreateWindowedDeviceManager(const DeviceCreationParams& params,
                                            IWindow*                    window,
                                            const Resolution&           initialBackbuffer,
                                            DeviceManagerCreateStatus*  status) noexcept {
    DeviceManagerCreateStatus localStatus = DeviceManagerCreateStatus::Unknown;
    auto* dm = new VkDeviceManager(params, window, initialBackbuffer, &localStatus);
    if (status) *status = localStatus;
    if (localStatus != DeviceManagerCreateStatus::Ok) {
        Logging::Get().Error(log::PLATFORM,
            std::string{"CreateWindowedDeviceManager: failed with status="} + StatusName(localStatus));
        delete dm;
        return nullptr;
    }
    return dm;
}

}  // namespace pyxis
