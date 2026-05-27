// Pyxis platform — GPU external-memory exporter (Win32).
//
// RFC 0004 (Omniverse editor integration). The in-viewport Kit handoff is
// zero-copy: Pyxis renders AOVs into VkImages whose backing VkDeviceMemory is
// allocated *exportable* (VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32). The
// exported Win32 HANDLE is imported by the Kit Vulkan device as the storage of
// an HdRenderBuffer-backing image, so Kit composites Pyxis pixels with no host
// readback. Cross-device ordering uses an exported timeline semaphore.
//
// NVRHI allocates texture memory via VMA and cannot allocate it exportable, so
// this class creates the VkImage + exportable VkDeviceMemory itself (raw
// Vulkan) and adopts the image into NVRHI via createHandleForNativeTexture —
// the renderer writes into the returned nvrhi::ITexture exactly as it would any
// render target, but the backing memory is shareable.
//
// Public-surface discipline (§18.9): no <windows.h>, no Vulkan types in this
// header. Win32 HANDLEs cross as void*; VkImage / VkDeviceMemory / VkSemaphore
// live in the PIMPL. Consumers reinterpret on the side where Vulkan is linked.

#pragma once

#include <Pyxis/Platform/Device/VulkanContext.h>
#include <Pyxis/Platform/Forward.h>
#include <Pyxis/Platform/PlatformApi.h>

#include <cstddef>
#include <cstdint>
#include <memory>

namespace nvrhi {
class IDevice;
class ITexture;
}  // namespace nvrhi

namespace pyxis {

// 16-byte Vulkan device UUID (VkPhysicalDeviceIDProperties::deviceUUID). The
// importing (Kit) device must report the *same* UUID or the shared handle is
// meaningless — a mismatch (multi-GPU / Optimus) is a hard error, never a
// silent CPU fallback (RFC 0004 §4).
struct PYXIS_PLATFORM_API DeviceUuid {
  uint8_t bytes[16] = {};
  bool valid = false;
};

// One exported image. `texture` is borrowed (owned by the exporter); render
// into it. `memoryHandle` + `allocationSize` + `vkFormat` + the dedicated flag
// are the wire contract the importer needs to recreate a matching VkImage and
// import the same memory.
struct PYXIS_PLATFORM_API ExportedImage {
  nvrhi::ITexture* texture = nullptr;  // adopt-wrapped VkImage; render target.
  void* memoryHandle = nullptr;        // Win32 HANDLE, OPAQUE_WIN32. Owned by exporter.
  uint64_t allocationSize = 0;         // bytes of backing VkDeviceMemory.
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t vkFormat = 0;               // VkFormat value; importer must match.
  bool dedicatedAllocation = false;    // importer sets VkMemoryDedicatedAllocateInfo iff true.
  [[nodiscard]] bool IsValid() const noexcept { return texture != nullptr && memoryHandle != nullptr; }
};

// One exported timeline semaphore (initial value 0). The exporter signals
// monotonically increasing values as frames complete; the importer waits.
struct PYXIS_PLATFORM_API ExportedSemaphore {
  void* handle = nullptr;  // Win32 HANDLE, OPAQUE_WIN32. Owned by exporter.
  [[nodiscard]] bool IsValid() const noexcept { return handle != nullptr; }
};

// Owns every exportable resource it hands out; destroys them (and closes the
// Win32 handles) on teardown. One exporter per Pyxis device. Not thread-safe;
// all calls happen on the render thread (§31).
class PYXIS_PLATFORM_API GpuInteropExporter {
 public:
  // ctx: raw Vulkan handles from IDeviceManager::GetVulkanContext().
  // nvrhiDevice: the same VkDevice wrapped by NVRHI, so exported VkImages can
  //   be adopted as nvrhi::ITexture. Borrowed; must outlive the exporter.
  // Returns nullptr if the device did not enable VK_KHR_external_memory_win32 /
  // external_semaphore_win32 (the PFNs resolve null) — callers treat that as
  // "interop unsupported" and report it, per RFC 0004 §4.
  [[nodiscard]] static std::unique_ptr<GpuInteropExporter> Create(
      const VulkanContext& ctx, nvrhi::IDevice* nvrhiDevice) noexcept;

  ~GpuInteropExporter();
  GpuInteropExporter(const GpuInteropExporter&) = delete;
  GpuInteropExporter& operator=(const GpuInteropExporter&) = delete;

  // VkPhysicalDeviceIDProperties::deviceUUID of the exporting device. The Kit
  // side compares this against its own before importing.
  [[nodiscard]] DeviceUuid GetDeviceUuid() const noexcept;

  // Create an exportable 2D image. `vkFormat` is a VkFormat value (e.g.
  // RGBA16F for color per §25.I.1). `isColorRenderTarget` selects usage flags
  // (color attachment + storage + transfer-src for color; transfer/sampled for
  // others). Returns an invalid ExportedImage on failure (logged).
  [[nodiscard]] ExportedImage CreateExportableImage(uint32_t width, uint32_t height,
                                                    uint32_t vkFormat,
                                                    bool isColorRenderTarget) noexcept;

  // Release an image previously returned by CreateExportableImage: waits for the
  // device to idle, drops the NVRHI wrapper, and destroys the VkImage +
  // VkDeviceMemory + Win32 HANDLE. MUST be called before re-creating an image at a
  // new size (e.g. on viewport resize) — otherwise the old image + memory + handle
  // leak until the exporter is destroyed. No-op for an invalid/unknown image.
  // CAUTION: only call once no importer still references the image's memory handle
  // (the exporter owns and closes that handle here).
  void ReleaseImage(const ExportedImage& image) noexcept;

  // Diagnostic: number of live images the exporter currently owns. For tests
  // (assert resize releases the old image) and leak triage.
  [[nodiscard]] std::size_t DebugLiveImageCount() const noexcept;

  // Create an exportable timeline semaphore (initial value 0).
  [[nodiscard]] ExportedSemaphore CreateExportableTimelineSemaphore() noexcept;

  // Submit an empty signal of the exported timeline semaphore to `value` on the
  // graphics queue. Used to publish "frame N is ready" to the importer; in the
  // round-trip test it fences the clear before the import-side wait.
  void SignalTimeline(const ExportedSemaphore& sem, uint64_t value) noexcept;

 private:
  GpuInteropExporter();
  struct Impl;
  std::unique_ptr<Impl> _impl;
};

}  // namespace pyxis
