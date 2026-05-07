// Pyxis platform — windowed VkDeviceManager.
//
// Plan §5.c. M0 ships the constructor + adapter pick + feature query +
// VkInstance/VkDevice creation. Swapchain + GLFW window are wired in M1
// (per §41) — both are stubbed to no-ops for now so the M0 exit criterion
// "pyxis.exe runs and prints adapter on the lab machine" holds.
//
// A NVRHI device is created in M0 only if NVRHI is present in the build
// (PYXIS_HAVE_NVRHI). The skeleton commit pins NVRHI's GIT_TAG to a TODO
// placeholder (§49) so the platform target builds without NVRHI by default.

#pragma once

#include <Pyxis/Platform/Device/DeviceCreationParams.h>
#include <Pyxis/Platform/Device/IDeviceManager.h>

#include <vulkan/vulkan.h>

#include <atomic>

namespace pyxis {

class VkDeviceManager final : public IDeviceManager {
public:
    VkDeviceManager(const DeviceCreationParams& params,
                    const Resolution&           initialBackbuffer,
                    DeviceManagerCreateStatus*  outStatus) noexcept;
    ~VkDeviceManager() override;

    // IDeviceManager.
    [[nodiscard]] nvrhi::IDevice*    GetDevice()         const noexcept override;
    [[nodiscard]] const AdapterInfo& GetAdapterInfo()    const noexcept override;
    [[nodiscard]] uint32_t           GetFramesInFlight() const noexcept override;
    [[nodiscard]] bool               IsHeadless()        const noexcept override { return false; }

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
    uint32_t          _framesInFlight = 2;
    Resolution        _backbuffer{};

    nvrhi::IDevice*   _nvrhiDevice = nullptr;
    bool              _ready       = false;
};

}  // namespace pyxis
