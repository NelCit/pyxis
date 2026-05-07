// Pyxis platform — physical adapter description.
//
// Returned by IDeviceManager::GetAdapterInfo(). Plain POD so it crosses the
// DLL boundary safely (plan §18.9 ABI rule, applied to the platform surface).

#pragma once

#include <Pyxis/Platform/PlatformApi.h>

#include <array>
#include <cstdint>
#include <string_view>

namespace pyxis {

// Vendor IDs as published by Khronos / PCI-SIG. Listed for the vendors
// Pyxis actually targets (RTX 30/40 + recent AMD/Intel ray-tracing GPUs).
enum class AdapterVendor : uint32_t {
    Unknown   = 0,
    Nvidia    = 0x10DE,
    Amd       = 0x1002,
    Intel     = 0x8086,
    Qualcomm  = 0x5143,
};

struct AdapterInfo {
    static constexpr std::size_t kNameCapacity = 192;

    // ---- Identity --------------------------------------------------------
    AdapterVendor vendor       = AdapterVendor::Unknown;
    uint32_t      vendorId     = 0;
    uint32_t      deviceId     = 0;
    uint32_t      driverVersionRaw = 0;       // VkPhysicalDeviceProperties::driverVersion

    // ---- Inline-owning name buffer (no STL across DLL — §18.9-equivalent).
    std::array<char, kNameCapacity> name{};
    uint16_t                        nameSize = 0;

    [[nodiscard]] std::string_view NameView() const noexcept {
        return { name.data(), nameSize };
    }

    // ---- Memory ----------------------------------------------------------
    uint64_t totalDeviceLocalBytes = 0;        // VRAM (largest device-local heap).
    uint64_t totalHostVisibleBytes = 0;        // System RAM exposed to the GPU.

    // ---- Capabilities ----------------------------------------------------
    bool     supportsRayTracingPipeline = false;
    bool     supportsAccelerationStructure = false;
    bool     supportsBufferDeviceAddress = false;
    bool     supportsDescriptorIndexing = false;
    bool     supportsTimelineSemaphore = false;
    bool     supportsSync2 = false;
    bool     supportsHostQueryReset = false;
    bool     supportsMaintenance4 = false;       // > 4 GiB single allocation.
    bool     supportsShaderInt64 = false;        // RNG seed-mix (§12.1).
    bool     supportsMemoryBudget = false;       // VK_EXT_memory_budget.

    // pipelineCacheUUID — primary key for the persisted pipeline cache
    // (plan §33.8). Encodes device + driver + ABI atomically.
    std::array<uint8_t, 16> pipelineCacheUuid{};
};

}  // namespace pyxis
