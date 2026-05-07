// Pyxis platform — windowed VkDeviceManager (M0 skeleton).
//
// M0 deliverable per plan §41:
//   - Picks the highest-VRAM ray-tracing-capable adapter (or --adapter <i>).
//   - Logs adapter name + driver + VRAM via pyxis::Logging::Get().
//   - Creates VkInstance + VkDevice + the graphics queue.
//   - Skeleton no-op for swapchain/window — wired in M1.
//
// NOTE: NVRHI device wrap is gated by PYXIS_HAVE_NVRHI (set when
// Thirdparty.cmake's NVRHI fetch is pinned). Until then GetDevice() returns
// nullptr and the renderer's M0 unit test SceneWorldInit doesn't try to
// touch it.

#include "Device/VkDeviceManager.h"

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

constexpr uint32_t kVulkanApiVersion = VK_API_VERSION_1_3;

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
    std::string namebuf{ appName };
    appInfo.pApplicationName   = namebuf.c_str();
    appInfo.applicationVersion = appVersion;
    appInfo.pEngineName        = "Pyxis";
    appInfo.engineVersion      = 0;
    appInfo.apiVersion         = kVulkanApiVersion;

    std::vector<const char*> extensions;
    // M0: no surface yet; viewer mode adds VK_KHR_surface + VK_KHR_win32_surface in M1.

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

}  // namespace

VkDeviceManager::VkDeviceManager(const DeviceCreationParams& params,
                                 const Resolution&           initialBackbuffer,
                                 DeviceManagerCreateStatus*  outStatus) noexcept {
    DeviceManagerCreateStatus status = Bringup(params, initialBackbuffer);
    if (outStatus) *outStatus = status;
    _ready = (status == DeviceManagerCreateStatus::Ok);
}

VkDeviceManager::~VkDeviceManager() {
    Teardown();
}

DeviceManagerCreateStatus VkDeviceManager::Bringup(const DeviceCreationParams& params,
                                                   const Resolution&           initialBackbuffer) noexcept {
    auto& log = Logging::Get();
    _backbuffer    = initialBackbuffer;
    _framesInFlight = std::clamp<uint32_t>(params.framesInFlight, 1, 3);

    // ---- VkInstance ------------------------------------------------------
    DeviceManagerCreateStatus instStatus = DeviceManagerCreateStatus::Ok;
    _instance = CreateInstance(params.enableValidation,
                               params.applicationName, params.applicationVersion,
                               &instStatus);
    if (_instance == VK_NULL_HANDLE) {
        log.Error(log::kPlatform, "VkDeviceManager: VkInstance creation failed");
        return instStatus;
    }

    // ---- Pick a physical device -----------------------------------------
    uint32_t deviceCount = 0;
    vkEnumeratePhysicalDevices(_instance, &deviceCount, nullptr);
    if (deviceCount == 0) {
        log.Error(log::kPlatform, "VkDeviceManager: no Vulkan-capable GPU found");
        return DeviceManagerCreateStatus::NoCompatibleAdapter;
    }
    std::vector<VkPhysicalDevice> physicalDevices(deviceCount);
    vkEnumeratePhysicalDevices(_instance, &deviceCount, physicalDevices.data());

    int32_t bestIndex = -1;
    uint64_t bestVram = 0;
    AdapterInfo  bestAdapter{};
    for (uint32_t i = 0; i < deviceCount; ++i) {
        AdapterInfo info{};
        const bool  ok = QueryAdapterFeatures(physicalDevices[i], info);
        if (!ok) continue;  // Hard-required §5.b features missing; skip.

        const bool isExplicit = (params.adapterIndex == static_cast<int32_t>(i));
        if (isExplicit) {
            bestIndex   = static_cast<int32_t>(i);
            bestAdapter = info;
            break;
        }
        if (info.totalDeviceLocalBytes > bestVram) {
            bestVram    = info.totalDeviceLocalBytes;
            bestIndex   = static_cast<int32_t>(i);
            bestAdapter = info;
        }
    }
    if (bestIndex < 0) {
        log.Error(log::kPlatform, "VkDeviceManager: no GPU satisfies §5.b required features");
        return DeviceManagerCreateStatus::FeatureMissing;
    }

    _physicalDevice = physicalDevices[static_cast<std::size_t>(bestIndex)];
    _adapter        = bestAdapter;

    // ---- Locate a graphics queue family ----------------------------------
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
        log.Error(log::kPlatform, "VkDeviceManager: chosen GPU has no graphics queue family");
        return DeviceManagerCreateStatus::FeatureMissing;
    }

    // ---- VkDevice --------------------------------------------------------
    const float queuePriority = 1.0f;
    VkDeviceQueueCreateInfo qInfo{};
    qInfo.sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qInfo.queueFamilyIndex = _graphicsFamily;
    qInfo.queueCount       = 1;
    qInfo.pQueuePriorities = &queuePriority;

    // M0 enables only the extensions strictly required for device creation
    // and the §5.b mandatory feature set. M1+ adds the swapchain extension.
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
        log.Error(log::kPlatform, "VkDeviceManager: vkCreateDevice failed");
        return DeviceManagerCreateStatus::DeviceCreationFailed;
    }
    vkGetDeviceQueue(_device, _graphicsFamily, 0, &_graphicsQueue);

    // ---- Log the adapter (M0 exit-criterion proof) ----------------------
    {
        std::string msg = "Vulkan device ready: ";
        msg.append(_adapter.NameView());
        msg += "  driver=";
        msg += std::to_string(_adapter.driverVersionRaw);
        msg += "  vram=";
        msg += std::to_string(_adapter.totalDeviceLocalBytes / (1024ull * 1024ull));
        msg += " MiB";
        log.Info(log::kPlatform, msg);
    }

    // M0: NVRHI wrap is deferred until the FetchContent SHA in
    // _cmake/Thirdparty.cmake is pinned (§49). _nvrhiDevice stays nullptr;
    // the M0 exit criterion is "logs adapter, driver, VRAM" which is met
    // above. Renderer/SceneWorld unit tests do not touch nvrhi at M0.
    _nvrhiDevice = nullptr;

    return DeviceManagerCreateStatus::Ok;
}

void VkDeviceManager::Teardown() noexcept {
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

nvrhi::IDevice* VkDeviceManager::GetDevice() const noexcept {
    return _nvrhiDevice;
}

const AdapterInfo& VkDeviceManager::GetAdapterInfo() const noexcept {
    return _adapter;
}

uint32_t VkDeviceManager::GetFramesInFlight() const noexcept {
    return _framesInFlight;
}

void VkDeviceManager::BeginFrame() {
    // M0: no swapchain → no acquire. Wired in M1.
}

void VkDeviceManager::EndFrame() {
    // M0: no swapchain → no present. Wired in M1.
}

void VkDeviceManager::WaitIdle() {
    if (_device != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(_device);
    }
}

IDeviceManager::~IDeviceManager() = default;

IDeviceManager* CreateWindowedDeviceManager(const DeviceCreationParams&  params,
                                            const Resolution&            initialBackbuffer,
                                            DeviceManagerCreateStatus*   status) noexcept {
    DeviceManagerCreateStatus localStatus = DeviceManagerCreateStatus::Unknown;
    auto* dm = new VkDeviceManager(params, initialBackbuffer, &localStatus);
    if (status) *status = localStatus;
    if (localStatus != DeviceManagerCreateStatus::Ok) {
        Logging::Get().Error(log::kPlatform,
            std::string{"CreateWindowedDeviceManager: failed with status="} + StatusName(localStatus));
        delete dm;
        return nullptr;
    }
    return dm;
}

}  // namespace pyxis
