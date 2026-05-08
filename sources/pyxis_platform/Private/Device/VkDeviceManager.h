// Pyxis platform — windowed VkDeviceManager.
//
// Plan §5.c. M1 wires the full path: VkInstance + VkDevice + nvrhi::IDevice
// wrap + VkSurfaceKHR (from the IWindow) + VkSwapchainKHR + per-image
// nvrhi::ITexture handles for the renderer to write into.

#pragma once

#include <Pyxis/Platform/Device/DeviceCreationParams.h>
#include <Pyxis/Platform/Device/IDeviceManager.h>

#include <nvrhi/nvrhi.h>
#include <nvrhi/vulkan.h>

#include <vulkan/vulkan.h>

#include <vector>

namespace pyxis {

class VkDeviceManager final : public IDeviceManager {
public:
    VkDeviceManager(const DeviceCreationParams& params,
                    IWindow*                    window,
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

    [[nodiscard]] nvrhi::ITexture* GetCurrentBackbuffer() const noexcept override;
    [[nodiscard]] uint32_t         GetBackbufferCount()   const noexcept override { return static_cast<uint32_t>(_swapchainTextures.size()); }
    [[nodiscard]] uint32_t         GetCurrentBackbufferIndex() const noexcept override { return _currentImage; }
    [[nodiscard]] nvrhi::ITexture* GetBackbuffer(uint32_t index) const noexcept override;
    [[nodiscard]] uint32_t         GetSwapchainGeneration() const noexcept override { return _swapchainGeneration; }
    [[nodiscard]] VulkanContext    GetVulkanContext()    const noexcept override;

private:
    DeviceManagerCreateStatus Bringup(const DeviceCreationParams& params,
                                      IWindow*                    window,
                                      const Resolution&           initialBackbuffer) noexcept;
    bool                      CreateSwapchain(uint32_t width, uint32_t height) noexcept;
    void                      DestroySwapchain() noexcept;
    void                      Teardown() noexcept;

    // ---- Vulkan state ----------------------------------------------------
    VkInstance        _instance         = VK_NULL_HANDLE;
    VkPhysicalDevice  _physicalDevice   = VK_NULL_HANDLE;
    VkDevice          _device           = VK_NULL_HANDLE;
    VkQueue           _graphicsQueue    = VK_NULL_HANDLE;
    uint32_t          _graphicsFamily   = 0;

    IWindow*          _window           = nullptr;          // borrowed
    VkSurfaceKHR      _surface          = VK_NULL_HANDLE;
    VkSwapchainKHR    _swapchain        = VK_NULL_HANDLE;
    VkFormat          _swapchainFormat  = VK_FORMAT_UNDEFINED;
    VkExtent2D        _swapchainExtent  = { 0, 0 };

    std::vector<VkImage>                _swapchainImages;
    std::vector<nvrhi::TextureHandle>   _swapchainTextures;

    // Swapchain sync — M1 ships with framesInFlight = 1, so the canonical
    // "acquire keyed by frame slot" pattern collapses to a single binary
    // semaphore: only one frame is ever in flight, and the timeline below
    // forces the CPU to wait for that frame's GPU work before we reuse
    // it. When M2+ raises framesInFlight, this grows back to a per-slot
    // ring (acquire) + per-image ring (present).
    //
    // - _acquireSem: signalled by vkAcquireNextImageKHR, waited on by the
    //   renderer's submit. Reused each frame after the timeline wait
    //   below retires the previous use.
    // - _presentSem: signalled by the renderer's submit, waited on by
    //   vkQueuePresentKHR. Same single-instance reasoning as acquire.
    // - _frameTimeline + _frameValue: per-frame CPU throttle. We signal
    //   _frameTimeline alongside the swapchain submit and wait on
    //   (_frameValue - 1) at the start of the next BeginFrame so the
    //   prior use of _acquireSem is GPU-retired before we hand it back
    //   to vkAcquireNextImageKHR (VUID-vkAcquireNextImageKHR-semaphore-
    //   01779).
    VkSemaphore                         _acquireSem    = VK_NULL_HANDLE;
    VkSemaphore                         _presentSem    = VK_NULL_HANDLE;
    VkSemaphore                         _frameTimeline = VK_NULL_HANDLE;
    uint64_t                            _frameValue    = 0;

    uint32_t          _currentImage     = 0;     // last vkAcquireNextImageKHR result
    bool              _resizePending    = false;

    // Monotonic counter — bumped on every successful CreateSwapchain
    // (initial bringup + every resize). Consumers (ImGuiHost, future
    // pass-local framebuffer caches) compare against a stored value to
    // detect they need to re-init.
    uint32_t          _swapchainGeneration = 0;

    // ---- NVRHI -----------------------------------------------------------
    nvrhi::DeviceHandle      _nvrhiDevice;
    nvrhi::vulkan::IDevice*  _nvrhiVulkan = nullptr;

    // ---- Misc state ------------------------------------------------------
    AdapterInfo       _adapter{};
    // Active frames-in-flight. Pinned to 1 in M1 — the multi-frame
    // pipelining lives behind the §33.1 cap (MAX_FRAMES_IN_FLIGHT = 3)
    // and grows when M2+ needs it (e.g. headless EXR per §33.7).
    uint32_t          _framesInFlight   = 1;
    Resolution        _backbuffer{};
    bool              _ready            = false;
};

}  // namespace pyxis
