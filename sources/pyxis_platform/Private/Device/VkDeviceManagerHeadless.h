// Pyxis platform — headless VkDeviceManager.
//
// Plan §5.c. No GLFW, no swapchain, no surface. M1 pins framesInFlight
// to 1 like the windowed manager; M2's --config EXR path raises it back
// to 3 per §33.7 once the multi-frame queueing is actually exercised.
// Same VkInstance/VkDevice/nvrhi::IDevice machinery as the windowed
// variant — kept as a separate class so the no-display invariant is
// *physically* true (no GLFW symbol is reachable from --headless).

#pragma once

#include <Pyxis/Platform/Device/DeviceCreationParams.h>
#include <Pyxis/Platform/Device/IDeviceManager.h>

#include <nvrhi/nvrhi.h>
#include <nvrhi/vulkan.h>

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

    // Headless: a single offscreen colour render target stands in for
    // the swapchain. Same nvrhi::ITexture surface as viewer mode so the
    // renderer doesn't branch on headless vs windowed.
    [[nodiscard]] nvrhi::ITexture* GetCurrentBackbuffer() const noexcept override { return _renderTarget.Get(); }
    [[nodiscard]] uint32_t         GetBackbufferCount()   const noexcept override { return _renderTarget ? 1u : 0u; }
    [[nodiscard]] uint32_t         GetCurrentBackbufferIndex() const noexcept override { return 0; }
    [[nodiscard]] nvrhi::ITexture* GetBackbuffer(uint32_t index) const noexcept override {
        return index == 0 ? _renderTarget.Get() : nullptr;
    }
    // Headless never rebuilds the target — generation stays at 0.
    [[nodiscard]] uint32_t         GetSwapchainGeneration() const noexcept override { return 0; }

    void BeginFrame() override;
    void EndFrame() override;
    void WaitIdle() override;

    [[nodiscard]] VulkanContext GetVulkanContext() const noexcept override;

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
    // M1 pins both viewer and headless to 1 frame in flight; M2's EXR
    // path will raise this back to 3 per §33.7 once it actually exercises
    // the multi-frame queueing.
    uint32_t          _framesInFlight = 1;
    Resolution        _backbuffer{};

    // RefCountPtr so the wrapped nvrhi::Device follows the same lifetime
    // discipline as the windowed manager (drops before vkDestroyDevice).
    nvrhi::DeviceHandle _nvrhiDevice;

    // Offscreen colour render target — the headless equivalent of a
    // swapchain image. Sized to backbuffer dims at Bringup time. Format
    // is SBGRA8_UNORM to match the viewer swapchain so the renderer's
    // pass code paths don't branch on headless vs windowed; M3+ may
    // raise this to RGBA16F when the path tracer wants HDR fidelity.
    nvrhi::TextureHandle _renderTarget;
};

}  // namespace pyxis
