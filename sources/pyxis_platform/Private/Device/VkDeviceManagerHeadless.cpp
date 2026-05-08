// Pyxis platform — headless VkDeviceManager (M0 skeleton).

#include "Device/VkDeviceManagerHeadless.h"

#include "Device/NvrhiCallback.h"
#include "Device/VulkanFeatureCheck.h"

#include <Pyxis/Platform/Logging/Log.h>
#include <Pyxis/Platform/Logging/LogCategories.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <string>
#include <vector>

namespace pyxis {

void VulkanHppInitFromLoader  (PFN_vkGetInstanceProcAddr) noexcept;
void VulkanHppInitFromInstance(VkInstance) noexcept;
void VulkanHppInitFromDevice  (VkDevice)   noexcept;

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

    // §33.7 byte-identical EXR pin lands here at M2: headless honours the
    // params.framesInFlight value the caller supplies (HeadlessMode pins
    // it to 3, the §33.1 cap) instead of forcing 1 like M1 did. The
    // caller is expected to be in-range; we still clamp as a safety net
    // (this is a public-ish DLL boundary) but log a Warning so misuse
    // surfaces rather than getting silently rewritten.
    constexpr uint32_t MIN_FIF = 1u;
    constexpr uint32_t MAX_FIF = 3u;        // == MAX_FRAMES_IN_FLIGHT (§33.1).
    if (params.framesInFlight < MIN_FIF || params.framesInFlight > MAX_FIF) {
        log.Warn(log::PLATFORM,
                 std::string{"VkDeviceManagerHeadless: framesInFlight="} +
                 std::to_string(params.framesInFlight) +
                 " out of range [" + std::to_string(MIN_FIF) + ", " +
                 std::to_string(MAX_FIF) + "]; clamping. Determinism" +
                 " (§33.7) requires the caller to supply " +
                 std::to_string(MAX_FIF) + ".");
    }
    _framesInFlight = std::clamp<uint32_t>(params.framesInFlight, MIN_FIF, MAX_FIF);

    VulkanHppInitFromLoader(&vkGetInstanceProcAddr);

    _instance = CreateInstance(params.enableValidation,
                               params.applicationName, params.applicationVersion);
    if (_instance == VK_NULL_HANDLE) {
        log.Error(log::PLATFORM, "VkDeviceManagerHeadless: VkInstance creation failed");
        return DeviceManagerCreateStatus::InstanceCreationFailed;
    }
    VulkanHppInitFromInstance(_instance);

    uint32_t deviceCount = 0;
    vkEnumeratePhysicalDevices(_instance, &deviceCount, nullptr);
    if (deviceCount == 0) {
        log.Error(log::PLATFORM, "VkDeviceManagerHeadless: no Vulkan-capable GPU found");
        return DeviceManagerCreateStatus::NoCompatibleAdapter;
    }
    std::vector<VkPhysicalDevice> physicalDevices(deviceCount);
    vkEnumeratePhysicalDevices(_instance, &deviceCount, physicalDevices.data());

    // Same discrete-first ranking as VkDeviceManager: an Intel UMA iGPU
    // would otherwise out-rank a discrete RTX on raw deviceLocalBytes.
    auto adapterRank = [](const AdapterInfo& adapter) -> int {
        return adapter.type == AdapterType::Discrete ? 1 : 0;
    };

    int32_t      bestIndex = -1;
    int          bestRank  = -1;
    uint64_t     bestVram  = 0;
    AdapterInfo  bestAdapter{};
    for (uint32_t i = 0; i < deviceCount; ++i) {
        AdapterInfo info{};
        if (!QueryAdapterFeatures(physicalDevices[i], info)) continue;

        const char* typeName =
              info.type == AdapterType::Discrete   ? "discrete"
            : info.type == AdapterType::Integrated ? "integrated"
            : info.type == AdapterType::Virtual    ? "virtual"
            : info.type == AdapterType::Cpu        ? "cpu"
            :                                        "other";
        std::string adapterMsg = "  [" + std::to_string(i) + "] ";
        adapterMsg.append(info.NameView());
        adapterMsg += "  ";
        adapterMsg += typeName;
        adapterMsg += "  vram=";
        adapterMsg += std::to_string(info.totalDeviceLocalBytes / (1024ull * 1024ull));
        adapterMsg += " MiB";
        log.Info(log::PLATFORM, adapterMsg);

        const bool isExplicit = (params.adapterIndex == static_cast<int32_t>(i));
        if (isExplicit) { bestIndex = static_cast<int32_t>(i); bestAdapter = info; break; }

        const int rank = adapterRank(info);
        if (rank > bestRank ||
            (rank == bestRank && info.totalDeviceLocalBytes > bestVram)) {
            bestRank    = rank;
            bestVram    = info.totalDeviceLocalBytes;
            bestIndex   = static_cast<int32_t>(i);
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

    // Headless explicitly DOES NOT request VK_KHR_swapchain (§5.c) so it
    // can run on stripped-down driver stacks (Windows Server 2022
    // containers). Same single-source pattern as the windowed manager:
    // one array fed to vkCreateDevice + nvrhi::DeviceDesc::deviceExtensions
    // so NVRHI's capability flags stay in lockstep with what we actually
    // requested. NOLINT'd for the same reason as the windowed sibling
    // (NVRHI's deviceExtensions is `const char**`).
    static const char* deviceExtensions[] = {  // NOLINT(misc-const-correctness)
        VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME,
        VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME,
        VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME,
        VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME,
        VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME,
        VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME,
        VK_EXT_HOST_QUERY_RESET_EXTENSION_NAME,
    };

    // Same Vulkan 1.3 feature chain as VkDeviceManager (§5.b mandatory):
    // sync2 + dynamicRendering + timelineSemaphore + bufferDeviceAddress
    // + descriptorIndexing + shaderDrawParameters. NVRHI's renderer code
    // assumes all of these regardless of mode, and validation fires
    // VUID-vkCmdPipelineBarrier2-synchronization2-03848 etc. if any are
    // missing. M1 didn't notice because RunHeadless never actually
    // exercised the renderer; M2's render-to-EXR path does.
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
    dInfo.enabledExtensionCount   = static_cast<uint32_t>(std::size(deviceExtensions));
    dInfo.ppEnabledExtensionNames = deviceExtensions;
    dInfo.pNext                   = &features2;
    // dInfo.pEnabledFeatures stays null — features come through the
    // VkPhysicalDeviceFeatures2 chain (Vulkan 1.1+ pattern).

    if (vkCreateDevice(_physicalDevice, &dInfo, nullptr, &_device) != VK_SUCCESS) {
        log.Error(log::PLATFORM, "VkDeviceManagerHeadless: vkCreateDevice failed");
        return DeviceManagerCreateStatus::DeviceCreationFailed;
    }
    vkGetDeviceQueue(_device, _graphicsFamily, 0, &_graphicsQueue);
    VulkanHppInitFromDevice(_device);

    // Wrap the VkDevice in nvrhi::IDevice so the renderer can issue
    // command lists exactly the same way it does in viewer mode. Without
    // this, GetDevice() returns nullptr and any renderer-driven path
    // (M2's --config EXR) crashes on the first createCommandList.
    nvrhi::vulkan::DeviceDesc nvrhiDesc{};
    nvrhiDesc.errorCB             = &NvrhiCallback();
    nvrhiDesc.instance            = _instance;
    nvrhiDesc.physicalDevice      = _physicalDevice;
    nvrhiDesc.device              = _device;
    nvrhiDesc.graphicsQueue       = _graphicsQueue;
    nvrhiDesc.graphicsQueueIndex  = _graphicsFamily;
    nvrhiDesc.bufferDeviceAddressSupported = true;
    nvrhiDesc.deviceExtensions    = deviceExtensions;
    nvrhiDesc.numDeviceExtensions = std::size(deviceExtensions);

    _nvrhiDevice = nvrhi::vulkan::createDevice(nvrhiDesc);
    if (!_nvrhiDevice) {
        log.Error(log::PLATFORM, "VkDeviceManagerHeadless: nvrhi::vulkan::createDevice failed");
        return DeviceManagerCreateStatus::DeviceCreationFailed;
    }

    // Per §18.4 / pyxis::app::AovTextures the render target is now
    // caller-allocated; the device manager exposes only the device,
    // queues, and per-frame plumbing. `initialBackbuffer` is reduced to
    // a startup-log diagnostic for "what dims does the caller intend to
    // render at" — useful when scrubbing logs but not load-bearing.
    {
        std::string msg = "Vulkan headless device ready: ";
        msg.append(_adapter.NameView());
        msg += "  driver=";
        msg += std::to_string(_adapter.driverVersionRaw);
        msg += "  vram=";
        msg += std::to_string(_adapter.totalDeviceLocalBytes / (1024ull * 1024ull));
        msg += " MiB  framesInFlight=";
        msg += std::to_string(_framesInFlight);
        msg += "  intended-render-dims=";
        msg += std::to_string(initialBackbuffer.width) + "x" +
               std::to_string(initialBackbuffer.height);
        log.Info(log::PLATFORM, msg);
    }

    return DeviceManagerCreateStatus::Ok;
}

void VkDeviceManagerHeadless::Teardown() noexcept {
    if (_device != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(_device);
    }
    // The caller (HeadlessMode → AovTextures) owns the offscreen render
    // target now and must drop its handle before this manager goes
    // away — RAII in the caller's frame scope guarantees that. We just
    // drain NVRHI's deferred-destruction queue so anything the caller
    // already released gets cleaned up before the wrapped nvrhi::Device
    // dies, which must happen BEFORE vkDestroyDevice — NVRHI's deleters
    // walk internal refs that touch the VkDevice.
    if (_nvrhiDevice) {
        _nvrhiDevice->runGarbageCollection();
    }
    _nvrhiDevice = nullptr;
    if (_device != VK_NULL_HANDLE) {
        vkDestroyDevice(_device, nullptr);
        _device = VK_NULL_HANDLE;
    }
    if (_instance != VK_NULL_HANDLE) {
        vkDestroyInstance(_instance, nullptr);
        _instance = VK_NULL_HANDLE;
    }
}

nvrhi::IDevice*    VkDeviceManagerHeadless::GetDevice()         const noexcept { return _nvrhiDevice.Get(); }
const AdapterInfo& VkDeviceManagerHeadless::GetAdapterInfo()    const noexcept { return _adapter; }
uint32_t           VkDeviceManagerHeadless::GetFramesInFlight() const noexcept { return _framesInFlight; }

// Headless has no swapchain to acquire from / present to. The offscreen
// render target is created once at device-init and lives for the
// lifetime of the manager; readback + EXR write are the caller's job
// (HeadlessMode owns that path — see RunHeadless), so BeginFrame /
// EndFrame are intentional no-ops and stay no-ops at M3+ as long as
// the backbuffer remains a single persistent target.
void VkDeviceManagerHeadless::BeginFrame() {}
void VkDeviceManagerHeadless::EndFrame()   {}

void VkDeviceManagerHeadless::WaitIdle() {
    if (_device != VK_NULL_HANDLE) vkDeviceWaitIdle(_device);
}

VulkanContext VkDeviceManagerHeadless::GetVulkanContext() const noexcept {
    VulkanContext context{};
    context.instance       = static_cast<void*>(_instance);
    context.physicalDevice = static_cast<void*>(_physicalDevice);
    context.device         = static_cast<void*>(_device);
    context.graphicsQueue  = static_cast<void*>(_graphicsQueue);
    context.graphicsFamily = _graphicsFamily;
    return context;
}

IDeviceManager* CreateHeadlessDeviceManager(const DeviceCreationParams&  params,
                                            const Resolution&            initialBackbuffer,
                                            DeviceManagerCreateStatus*   status) noexcept {
    DeviceManagerCreateStatus localStatus = DeviceManagerCreateStatus::Unknown;
    auto* deviceManager = new VkDeviceManagerHeadless(params, initialBackbuffer, &localStatus);
    if (status) *status = localStatus;
    if (localStatus != DeviceManagerCreateStatus::Ok) {
        delete deviceManager;
        return nullptr;
    }
    return deviceManager;
}

}  // namespace pyxis
