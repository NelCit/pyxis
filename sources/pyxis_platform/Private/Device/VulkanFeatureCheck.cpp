// Pyxis platform — Vulkan feature-check implementation.

#include "Device/VulkanFeatureCheck.h"

#include <Pyxis/Platform/Logging/Log.h>
#include <Pyxis/Platform/Logging/LogCategories.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

namespace pyxis {

namespace {

bool HasExtension(const std::vector<VkExtensionProperties>& props,
                  const char*                               name) noexcept {
    return std::any_of(props.begin(), props.end(),
                       [&](const VkExtensionProperties& prop) {
                           return std::strcmp(prop.extensionName, name) == 0;
                       });
}

}  // namespace

bool QueryAdapterFeatures(VkPhysicalDevice device, AdapterInfo& outInfo) noexcept {
    if (device == VK_NULL_HANDLE) return false;

    // ---- Properties ------------------------------------------------------
    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(device, &props);
    outInfo.vendorId          = props.vendorID;
    outInfo.deviceId          = props.deviceID;
    outInfo.driverVersionRaw  = props.driverVersion;
    std::memcpy(outInfo.pipelineCacheUuid.data(), props.pipelineCacheUUID, 16);

    switch (props.vendorID) {
        case 0x10DE: outInfo.vendor = AdapterVendor::Nvidia;   break;
        case 0x1002: outInfo.vendor = AdapterVendor::Amd;      break;
        case 0x8086: outInfo.vendor = AdapterVendor::Intel;    break;
        case 0x5143: outInfo.vendor = AdapterVendor::Qualcomm; break;
        default:     outInfo.vendor = AdapterVendor::Unknown;  break;
    }

    switch (props.deviceType) {
        case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:   outInfo.type = AdapterType::Discrete;   break;
        case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: outInfo.type = AdapterType::Integrated; break;
        case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:    outInfo.type = AdapterType::Virtual;    break;
        case VK_PHYSICAL_DEVICE_TYPE_CPU:            outInfo.type = AdapterType::Cpu;        break;
        default:                                     outInfo.type = AdapterType::Other;      break;
    }

    const std::size_t nameLen =
        std::min<std::size_t>(std::strlen(props.deviceName), AdapterInfo::NAME_CAPACITY - 1);
    std::memcpy(outInfo.name.data(), props.deviceName, nameLen);
    outInfo.name[nameLen] = '\0';
    outInfo.nameSize      = static_cast<uint16_t>(nameLen);

    // ---- Memory ----------------------------------------------------------
    VkPhysicalDeviceMemoryProperties memProps{};
    vkGetPhysicalDeviceMemoryProperties(device, &memProps);
    outInfo.totalDeviceLocalBytes = 0;
    outInfo.totalHostVisibleBytes = 0;
    for (uint32_t i = 0; i < memProps.memoryHeapCount; ++i) {
        const auto& heap = memProps.memoryHeaps[i];
        if (heap.flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) {
            outInfo.totalDeviceLocalBytes =
                std::max(outInfo.totalDeviceLocalBytes, heap.size);
        }
    }

    // ---- Extensions ------------------------------------------------------
    uint32_t extCount = 0;
    vkEnumerateDeviceExtensionProperties(device, nullptr, &extCount, nullptr);
    std::vector<VkExtensionProperties> exts(extCount);
    vkEnumerateDeviceExtensionProperties(device, nullptr, &extCount, exts.data());

    outInfo.supportsRayTracingPipeline =
        HasExtension(exts, VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME);
    outInfo.supportsAccelerationStructure =
        HasExtension(exts, VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME);
    outInfo.supportsBufferDeviceAddress =
        HasExtension(exts, VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME);
    outInfo.supportsDescriptorIndexing =
        HasExtension(exts, VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME);
    outInfo.supportsTimelineSemaphore =
        HasExtension(exts, VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME);
    outInfo.supportsHostQueryReset =
        HasExtension(exts, VK_EXT_HOST_QUERY_RESET_EXTENSION_NAME);
    outInfo.supportsMaintenance4 =
        HasExtension(exts, VK_KHR_MAINTENANCE_4_EXTENSION_NAME);
    outInfo.supportsMemoryBudget =
        HasExtension(exts, VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);

    // ---- Vulkan 1.3 core implies sync2 + dynamicRendering ----------------
    outInfo.supportsSync2 = (props.apiVersion >= VK_API_VERSION_1_3);

    // ---- Required core feature: shaderInt64 ------------------------------
    VkPhysicalDeviceFeatures features{};
    vkGetPhysicalDeviceFeatures(device, &features);
    outInfo.supportsShaderInt64 = features.shaderInt64 != 0;

    // ---- Validate required set -------------------------------------------
    bool allRequiredPresent = true;
    auto& log = Logging::Get();
    auto missing = [&](const char* name) {
        allRequiredPresent = false;
        std::string msg = "missing required feature/extension: ";
        msg += name;
        log.Error(log::PLATFORM, msg);
    };

    if (!outInfo.supportsRayTracingPipeline)     missing(VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME);
    if (!outInfo.supportsAccelerationStructure)  missing(VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME);
    if (!outInfo.supportsBufferDeviceAddress)    missing(VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME);
    if (!outInfo.supportsDescriptorIndexing)     missing(VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME);
    if (!outInfo.supportsTimelineSemaphore)      missing(VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME);
    if (!outInfo.supportsHostQueryReset)         missing(VK_EXT_HOST_QUERY_RESET_EXTENSION_NAME);
    if (!outInfo.supportsMaintenance4 && props.apiVersion < VK_API_VERSION_1_3)
        missing(VK_KHR_MAINTENANCE_4_EXTENSION_NAME);
    if (!outInfo.supportsShaderInt64)            missing("shaderInt64");
    if (!outInfo.supportsSync2)                  missing("Vulkan 1.3 core (synchronization2)");

    return allRequiredPresent;
}

}  // namespace pyxis
