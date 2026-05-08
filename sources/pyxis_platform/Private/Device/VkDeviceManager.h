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

    // Swapchain sync: the canonical "acquire keyed by frame slot, present
    // keyed by image index" pattern.
    //
    // - _acquireSems[framesInFlight]: signalled by vkAcquireNextImageKHR;
    //   indexed by _frameSlot. Frame slot rotates 0..framesInFlight-1.
    // - _presentSems[imageCount]: waited on by vkQueuePresentKHR; indexed
    //   by _currentImage (the swapchain image index returned by acquire).
    //   One per swapchain image so a present on image N is fully retired
    //   before we'd next reuse the same sem.
    // - _frameTimeline + _frameValue: timeline semaphore used as a
    //   per-frame CPU throttle. We signal _frameTimeline with the current
    //   frame value alongside the swapchain submit; a frame later we
    //   wait on (_frameValue - framesInFlight) before we reuse the slot.
    //   This keeps validation's VUID-vkAcquireNextImageKHR-semaphore-01779
    //   clean — the prior use of _acquireSems[_frameSlot] is GPU-retired
    //   before we hand it back to vkAcquireNextImageKHR.
    std::vector<VkSemaphore>            _acquireSems;
    std::vector<VkSemaphore>            _presentSems;
    VkSemaphore                         _frameTimeline = VK_NULL_HANDLE;
    uint64_t                            _frameValue    = 0;

    uint32_t          _currentImage     = 0;     // last vkAcquireNextImageKHR result
    uint32_t          _frameSlot        = 0;     // 0..framesInFlight - 1
    bool              _resizePending    = false;

    // ---- NVRHI -----------------------------------------------------------
    nvrhi::DeviceHandle      _nvrhiDevice;
    nvrhi::vulkan::IDevice*  _nvrhiVulkan = nullptr;

    // ---- Misc state ------------------------------------------------------
    AdapterInfo       _adapter{};
    uint32_t          _framesInFlight   = 2;
    Resolution        _backbuffer{};
    bool              _ready            = false;
};

}  // namespace pyxis
