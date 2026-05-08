// Pyxis platform — device-manager interface.
//
// Common base for VkDeviceManager (viewer) and VkDeviceManagerHeadless
// (CI / regression). Plan §5.c. The renderer holds an IDeviceManager*
// and never branches on mode; the branch lives in pyxis_app::Main.cpp.

#pragma once

#include <Pyxis/Platform/Device/AdapterInfo.h>
#include <Pyxis/Platform/Device/Resolution.h>
#include <Pyxis/Platform/Device/VulkanContext.h>
#include <Pyxis/Platform/Forward.h>
#include <Pyxis/Platform/PlatformApi.h>

#include <cstdint>

namespace nvrhi {
class ITexture;
}

namespace pyxis {

class IWindow;

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
    // Swapchain accessors (viewer only — headless returns nullptr / 0).
    // The current backbuffer is the texture the renderer writes its final
    // present blit into; the index rotates per-frame after BeginFrame.
    // -------------------------------------------------------------------
    [[nodiscard]] virtual nvrhi::ITexture* GetCurrentBackbuffer() const noexcept = 0;
    [[nodiscard]] virtual uint32_t         GetBackbufferCount()   const noexcept = 0;
    [[nodiscard]] virtual uint32_t         GetCurrentBackbufferIndex() const noexcept = 0;
    [[nodiscard]] virtual nvrhi::ITexture* GetBackbuffer(uint32_t index) const noexcept = 0;

    // -------------------------------------------------------------------
    // Synchronous wait for the GPU. Used at shutdown and for readback in
    // headless mode.
    // -------------------------------------------------------------------
    virtual void WaitIdle() = 0;

    // -------------------------------------------------------------------
    // Raw Vulkan handles — opaque escape hatch for the ImGui Vulkan
    // backend (and, post-v1, anything else that needs Vulkan-shaped
    // interop). The IDeviceManager retains ownership; callers must not
    // destroy any handle in the returned struct.
    // -------------------------------------------------------------------
    [[nodiscard]] virtual VulkanContext GetVulkanContext() const noexcept = 0;

protected:
    IDeviceManager() = default;
};

// -----------------------------------------------------------------------
// Free factory functions. Concrete classes live in Private/.
// Caller owns the returned pointer; nullptr on failure (status set).
// -----------------------------------------------------------------------
[[nodiscard]] PYXIS_PLATFORM_API IDeviceManager* CreateWindowedDeviceManager(
    const DeviceCreationParams&  params,
    IWindow*                     window,        // borrowed; outlives the device manager
    const Resolution&            initialBackbuffer,
    DeviceManagerCreateStatus*   status) noexcept;

[[nodiscard]] PYXIS_PLATFORM_API IDeviceManager* CreateHeadlessDeviceManager(
    const DeviceCreationParams&  params,
    const Resolution&            initialBackbuffer,
    DeviceManagerCreateStatus*   status) noexcept;

}  // namespace pyxis
