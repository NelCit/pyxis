// Pyxis platform — headless VkDeviceManager (M0 skeleton).

#include "Device/VkDeviceManagerHeadless.h"

#include "Device/VulkanFeatureCheck.h"

#include <Pyxis/Platform/Logging/Log.h>
#include <Pyxis/Platform/Logging/LogCategories.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <string>
#include <vector>

namespace pyxis {

namespace {

constexpr uint32_t VULKAN_API_VERSION = VK_API_VERSION_1_3;

VkInstance CreateInstance(bool enableValidation,
                          std::string_view appName, uint32_t appVersion) noexcept {
    VkApplicationInfo appInfo{};
    appInfo.sType              = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    const std::string namebuf{ appName };
    appInfo.pApplicationName   = namebuf.c_str();
    appInfo.applicationVersion = appVersion;
    appInfo.pEngineName        = "Pyxis";
    appInfo.engineVersion      = 0;
    appInfo.apiVersion         = VULKAN_API_VERSION;

    std::vector<const char*> layers;
    if (enableValidation) {
        layers.push_back("VK_LAYER_KHRONOS_validation");
    }

    VkInstanceCreateInfo info{};
    info.sType                   = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    info.pApplicationInfo        = &appInfo;
    info.enabledLayerCount       = static_cast<uint32_t>(layers.size());
    info.ppEnabledLayerNames     = layers.data();

    VkInstance instance = VK_NULL_HANDLE;
    if (vkCreateInstance(&info, nullptr, &instance) != VK_SUCCESS) {
        return VK_NULL_HANDLE;
    }
    return instance;
}

}  // namespace

VkDeviceManagerHeadless::VkDeviceManagerHeadless(const DeviceCreationParams& params,
                                                 const Resolution&           initialBackbuffer,
                                                 DeviceManagerCreateStatus*  outStatus) noexcept {
    const DeviceManagerCreateStatus status = Bringup(params, initialBackbuffer);
    if (outStatus) *outStatus = status;
}

VkDeviceManagerHeadless::~VkDeviceManagerHeadless() {
    Teardown();
}

DeviceManagerCreateStatus VkDeviceManagerHeadless::Bringup(
    const DeviceCreationParams& params,
    const Resolution&           initialBackbuffer) noexcept {

    auto& log = Logging::Get();

    // Headless pins 3 frames in flight (§33.7).
    _framesInFlight = 3;
    _backbuffer     = initialBackbuffer;

    _instance = CreateInstance(params.enableValidation,
                               params.applicationName, params.applicationVersion);
    if (_instance == VK_NULL_HANDLE) {
        log.Error(log::PLATFORM, "VkDeviceManagerHeadless: VkInstance creation failed");
        return DeviceManagerCreateStatus::InstanceCreationFailed;
    }

    uint32_t deviceCount = 0;
    vkEnumeratePhysicalDevices(_instance, &deviceCount, nullptr);
    if (deviceCount == 0) {
        log.Error(log::PLATFORM, "VkDeviceManagerHeadless: no Vulkan-capable GPU found");
        return DeviceManagerCreateStatus::NoCompatibleAdapter;
    }
    std::vector<VkPhysicalDevice> physicalDevices(deviceCount);
    vkEnumeratePhysicalDevices(_instance, &deviceCount, physicalDevices.data());

    int32_t      bestIndex = -1;
    uint64_t     bestVram  = 0;
    AdapterInfo  bestAdapter{};
    for (uint32_t i = 0; i < deviceCount; ++i) {
        AdapterInfo info{};
        if (!QueryAdapterFeatures(physicalDevices[i], info)) continue;
        const bool isExplicit = (params.adapterIndex == static_cast<int32_t>(i));
        if (isExplicit) { bestIndex = static_cast<int32_t>(i); bestAdapter = info; break; }
        if (info.totalDeviceLocalBytes > bestVram) {
            bestVram   = info.totalDeviceLocalBytes;
            bestIndex  = static_cast<int32_t>(i);
            bestAdapter = info;
        }
    }
    if (bestIndex < 0) {
        log.Error(log::PLATFORM, "VkDeviceManagerHeadless: no GPU satisfies §5.b features");
        return DeviceManagerCreateStatus::FeatureMissing;
    }

    _physicalDevice = physicalDevices[static_cast<std::size_t>(bestIndex)];
    _adapter        = bestAdapter;

    uint32_t qfCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(_physicalDevice, &qfCount, nullptr);
    std::vector<VkQueueFamilyProperties> qfs(qfCount);
    vkGetPhysicalDeviceQueueFamilyProperties(_physicalDevice, &qfCount, qfs.data());
    bool foundGraphics = false;
    for (uint32_t i = 0; i < qfCount; ++i) {
        if (qfs[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            _graphicsFamily = i;
            foundGraphics   = true;
            break;
        }
    }
    if (!foundGraphics) {
        log.Error(log::PLATFORM, "VkDeviceManagerHeadless: no graphics queue family");
        return DeviceManagerCreateStatus::FeatureMissing;
    }

    const float qPriority = 1.0f;
    VkDeviceQueueCreateInfo qInfo{};
    qInfo.sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qInfo.queueFamilyIndex = _graphicsFamily;
    qInfo.queueCount       = 1;
    qInfo.pQueuePriorities = &qPriority;

    // Headless explicitly DOES NOT request VK_KHR_swapchain (§5.c) so it can
    // run on stripped-down driver stacks (Windows Server 2022 containers).
    std::array<const char*, 7> requiredExt{
        VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME,
        VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME,
        VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME,
        VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME,
        VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME,
        VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME,
        VK_EXT_HOST_QUERY_RESET_EXTENSION_NAME,
    };

    VkPhysicalDeviceFeatures coreFeatures{};
    coreFeatures.shaderInt64 = VK_TRUE;
    coreFeatures.shaderStorageImageReadWithoutFormat = VK_TRUE;

    VkDeviceCreateInfo dInfo{};
    dInfo.sType                   = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dInfo.queueCreateInfoCount    = 1;
    dInfo.pQueueCreateInfos       = &qInfo;
    dInfo.enabledExtensionCount   = static_cast<uint32_t>(requiredExt.size());
    dInfo.ppEnabledExtensionNames = requiredExt.data();
    dInfo.pEnabledFeatures        = &coreFeatures;

    if (vkCreateDevice(_physicalDevice, &dInfo, nullptr, &_device) != VK_SUCCESS) {
        log.Error(log::PLATFORM, "VkDeviceManagerHeadless: vkCreateDevice failed");
        return DeviceManagerCreateStatus::DeviceCreationFailed;
    }
    vkGetDeviceQueue(_device, _graphicsFamily, 0, &_graphicsQueue);

    {
        std::string msg = "Vulkan headless device ready: ";
        msg.append(_adapter.NameView());
        msg += "  driver=";
        msg += std::to_string(_adapter.driverVersionRaw);
        msg += "  vram=";
        msg += std::to_string(_adapter.totalDeviceLocalBytes / (1024ull * 1024ull));
        msg += " MiB  framesInFlight=";
        msg += std::to_string(_framesInFlight);
        log.Info(log::PLATFORM, msg);
    }

    return DeviceManagerCreateStatus::Ok;
}

void VkDeviceManagerHeadless::Teardown() noexcept {
    if (_device != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(_device);
        vkDestroyDevice(_device, nullptr);
        _device = VK_NULL_HANDLE;
    }
    if (_instance != VK_NULL_HANDLE) {
        vkDestroyInstance(_instance, nullptr);
        _instance = VK_NULL_HANDLE;
    }
}

nvrhi::IDevice*    VkDeviceManagerHeadless::GetDevice()         const noexcept { return _nvrhiDevice; }
const AdapterInfo& VkDeviceManagerHeadless::GetAdapterInfo()    const noexcept { return _adapter; }
uint32_t           VkDeviceManagerHeadless::GetFramesInFlight() const noexcept { return _framesInFlight; }

void VkDeviceManagerHeadless::BeginFrame() {}  // M0: no-op (no acquire).
void VkDeviceManagerHeadless::EndFrame()   {}  // M0: no-op (no present, no readback yet).

void VkDeviceManagerHeadless::WaitIdle() {
    if (_device != VK_NULL_HANDLE) vkDeviceWaitIdle(_device);
}

IDeviceManager* CreateHeadlessDeviceManager(const DeviceCreationParams&  params,
                                            const Resolution&            initialBackbuffer,
                                            DeviceManagerCreateStatus*   status) noexcept {
    DeviceManagerCreateStatus localStatus = DeviceManagerCreateStatus::Unknown;
    auto* dm = new VkDeviceManagerHeadless(params, initialBackbuffer, &localStatus);
    if (status) *status = localStatus;
    if (localStatus != DeviceManagerCreateStatus::Ok) {
        delete dm;
        return nullptr;
    }
    return dm;
}

}  // namespace pyxis
