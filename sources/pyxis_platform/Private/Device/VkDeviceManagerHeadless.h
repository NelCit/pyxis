// Pyxis platform — headless VkDeviceManager.
//
// Plan §5.c. No GLFW, no swapchain, no surface. Pinned to framesInFlight = 3
// for byte-identical EXR output (§33.7). Same VkInstance/VkDevice
// machinery as the windowed variant — kept as a separate class so the
// no-display invariant is *physically* true (no GLFW symbol is reachable
// from --headless).

#pragma once

#include <Pyxis/Platform/Device/DeviceCreationParams.h>
#include <Pyxis/Platform/Device/IDeviceManager.h>

#include <vulkan/vulkan.h>

namespace pyxis {

class VkDeviceManagerHeadless final : public IDeviceManager {
public:
    VkDeviceManagerHeadless(const DeviceCreationParams& params,
                            const Resolution&           initialBackbuffer,
                            DeviceManagerCreateStatus*  outStatus) noexcept;
    ~VkDeviceManagerHeadless() override;

    [[nodiscard]] nvrhi::IDevice*    GetDevice()         const noexcept override;
    [[nodiscard]] const AdapterInfo& GetAdapterInfo()    const noexcept override;
    [[nodiscard]] uint32_t           GetFramesInFlight() const noexcept override;
    [[nodiscard]] bool               IsHeadless()        const noexcept override { return true; }

    void BeginFrame() override;
    void EndFrame() override;
    void WaitIdle() override;

private:
    DeviceManagerCreateStatus Bringup(const DeviceCreationParams& params,
                                      const Resolution&           initialBackbuffer) noexcept;
    void                      Teardown() noexcept;

    VkInstance        _instance       = VK_NULL_HANDLE;
    VkPhysicalDevice  _physicalDevice = VK_NULL_HANDLE;
    VkDevice          _device         = VK_NULL_HANDLE;
    VkQueue           _graphicsQueue  = VK_NULL_HANDLE;
    uint32_t          _graphicsFamily = 0;

    AdapterInfo       _adapter{};
    uint32_t          _framesInFlight = 3;   // pinned per §33.7.
    Resolution        _backbuffer{};

    nvrhi::IDevice*   _nvrhiDevice = nullptr;
};

}  // namespace pyxis
