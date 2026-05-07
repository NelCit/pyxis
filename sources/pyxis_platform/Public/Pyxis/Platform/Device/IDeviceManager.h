// Pyxis platform — device-manager interface.
//
// Common base for VkDeviceManager (viewer) and VkDeviceManagerHeadless
// (CI / regression). Plan §5.c. The renderer holds an IDeviceManager*
// and never branches on mode; the branch lives in pyxis_app::Main.cpp.

#pragma once

#include <Pyxis/Platform/Device/AdapterInfo.h>
#include <Pyxis/Platform/Device/Resolution.h>
#include <Pyxis/Platform/Forward.h>
#include <Pyxis/Platform/PlatformApi.h>

#include <cstdint>

namespace pyxis {

// Result of a device-manager creation attempt. We do not throw across the
// DLL boundary (§30.6) — failure is reported as a value with a kind. Full
// error catalogue lives in pyxis_renderer/Public; the platform layer only
// has to surface device-creation failures.
enum class DeviceManagerCreateStatus : uint8_t {
    Ok = 0,
    NoVulkanDriver,
    NoCompatibleAdapter,
    FeatureMissing,             // §5.b mandatory feature not supported.
    InstanceCreationFailed,
    DeviceCreationFailed,
    SurfaceCreationFailed,      // viewer mode only.
    SwapchainCreationFailed,    // viewer mode only.
    OutOfMemory,
    Unknown,
};

class PYXIS_PLATFORM_API IDeviceManager {
public:
    virtual ~IDeviceManager();

    IDeviceManager(const IDeviceManager&)            = delete;
    IDeviceManager& operator=(const IDeviceManager&) = delete;

    // -------------------------------------------------------------------
    // Identity & access (borrowed pointers; the device-manager owns).
    // -------------------------------------------------------------------
    [[nodiscard]] virtual nvrhi::IDevice*    GetDevice()      const noexcept = 0;
    [[nodiscard]] virtual const AdapterInfo& GetAdapterInfo() const noexcept = 0;
    [[nodiscard]] virtual uint32_t           GetFramesInFlight() const noexcept = 0;
    [[nodiscard]] virtual bool               IsHeadless()     const noexcept = 0;

    // -------------------------------------------------------------------
    // Per-frame plumbing.
    //   BeginFrame: viewer = vkAcquireNextImageKHR, headless = no-op.
    //   EndFrame:   viewer = vkQueuePresentKHR,    headless = optional readback.
    // -------------------------------------------------------------------
    virtual void BeginFrame() = 0;
    virtual void EndFrame()   = 0;

    // -------------------------------------------------------------------
    // Synchronous wait for the GPU. Used at shutdown and for readback in
    // headless mode.
    // -------------------------------------------------------------------
    virtual void WaitIdle() = 0;

protected:
    IDeviceManager() = default;
};

// -----------------------------------------------------------------------
// Free factory functions. Concrete classes live in Private/.
// Caller owns the returned pointer; nullptr on failure (status set).
// -----------------------------------------------------------------------
[[nodiscard]] PYXIS_PLATFORM_API IDeviceManager* CreateWindowedDeviceManager(
    const DeviceCreationParams&  params,
    const Resolution&            initialBackbuffer,
    DeviceManagerCreateStatus*   status) noexcept;

[[nodiscard]] PYXIS_PLATFORM_API IDeviceManager* CreateHeadlessDeviceManager(
    const DeviceCreationParams&  params,
    const Resolution&            initialBackbuffer,
    DeviceManagerCreateStatus*   status) noexcept;

}  // namespace pyxis
