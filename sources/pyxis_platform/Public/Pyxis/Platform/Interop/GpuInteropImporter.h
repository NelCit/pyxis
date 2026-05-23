// Pyxis platform — GPU external-memory importer (Win32). RFC 0004 Stage 3 (C3).
//
// The Kit-side counterpart to GpuInteropExporter. Given a target Vulkan device
// (the Omniverse Kit viewport's device, or a test stand-in) and the wire
// contract an ExportedImage / ExportedSemaphore carries (Win32 handle, size,
// format), it recreates a matching VkImage backed by the *shared* memory and
// imports the timeline semaphore — so the target device reads exactly what the
// Pyxis device rendered, with no host copy.
//
// Same public-surface discipline as the exporter (§18.9): no <windows.h>, no
// Vulkan types in this header; Win32 HANDLEs and Vulkan objects cross as void*,
// reinterpreted on the side where Vulkan is linked. The importer owns every
// VkImage / VkDeviceMemory / VkSemaphore it creates and destroys them on
// teardown (it does NOT close the source Win32 handles — the exporter owns those).

#pragma once

#include <Pyxis/Platform/Device/VulkanContext.h>
#include <Pyxis/Platform/Forward.h>
#include <Pyxis/Platform/PlatformApi.h>

#include <cstdint>
#include <memory>

namespace pyxis {

// A VkImage on the target device backed by imported (shared) memory. `image`
// and `memory` are VkImage / VkDeviceMemory as void*. The caller uses the image
// (e.g. as the HdRenderBuffer backing in Kit, or copies from it in tests); the
// importer destroys both on teardown.
struct PYXIS_PLATFORM_API ImportedImage {
  void* image = nullptr;
  void* memory = nullptr;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t vkFormat = 0;
  [[nodiscard]] bool IsValid() const noexcept { return image != nullptr && memory != nullptr; }
};

class PYXIS_PLATFORM_API GpuInteropImporter {
 public:
  // `targetDevice` is the raw Vulkan handles of the importing device (the Kit
  // viewport device in production). Returns nullptr if external-memory_win32 /
  // external_semaphore_win32 entry points are unavailable on it.
  [[nodiscard]] static std::unique_ptr<GpuInteropImporter> Create(
      const VulkanContext& targetDevice) noexcept;

  ~GpuInteropImporter();
  GpuInteropImporter(const GpuInteropImporter&) = delete;
  GpuInteropImporter& operator=(const GpuInteropImporter&) = delete;

  // Import a shared image given the contract an ExportedImage carries. The
  // VkImage create-info (format/extent/usage/tiling) must match the exporter's;
  // these params encode that. Returns an invalid ImportedImage on failure.
  [[nodiscard]] ImportedImage ImportImage(void* memoryHandle, uint64_t allocationSize,
                                          uint32_t width, uint32_t height, uint32_t vkFormat,
                                          bool isColorRenderTarget) noexcept;

  // Import an exported timeline semaphore. Returns a VkSemaphore (as void*) the
  // caller waits on, or nullptr on failure. Owned + destroyed by the importer.
  [[nodiscard]] void* ImportTimelineSemaphore(void* semaphoreHandle) noexcept;

 private:
  GpuInteropImporter();
  struct Impl;
  std::unique_ptr<Impl> _impl;
};

}  // namespace pyxis
