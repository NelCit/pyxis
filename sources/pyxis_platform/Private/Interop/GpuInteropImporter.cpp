// Pyxis platform — GPU external-memory importer (Win32). RFC 0004 Stage 3 (C3).
// See Public/Pyxis/Platform/Interop/GpuInteropImporter.h for the contract.

#include <vulkan/vulkan.h>
#include <vulkan/vulkan_win32.h>

#include <Pyxis/Platform/Interop/GpuInteropImporter.h>
#include <Pyxis/Platform/Logging/Log.h>
#include <Pyxis/Platform/Logging/LogCategories.h>

#include "InteropVkCommon.h"

#include <vector>

namespace pyxis {

struct GpuInteropImporter::Impl {
  VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
  VkDevice device = VK_NULL_HANDLE;
  PFN_vkImportSemaphoreWin32HandleKHR importSemaphore = nullptr;

  std::vector<VkImage> images;
  std::vector<VkDeviceMemory> memories;
  std::vector<VkSemaphore> semaphores;

  ~Impl() {
    if (device == VK_NULL_HANDLE)
      return;
    vkDeviceWaitIdle(device);
    for (VkImage img : images)
      vkDestroyImage(device, img, nullptr);
    for (VkDeviceMemory mem : memories)
      vkFreeMemory(device, mem, nullptr);
    for (VkSemaphore sem : semaphores)
      vkDestroySemaphore(device, sem, nullptr);
  }
};

GpuInteropImporter::GpuInteropImporter() : _impl(std::make_unique<Impl>()) {}
GpuInteropImporter::~GpuInteropImporter() = default;

std::unique_ptr<GpuInteropImporter> GpuInteropImporter::Create(
    const VulkanContext& targetDevice) noexcept {
  auto& log = Logging::Get();
  if (targetDevice.device == nullptr) {
    log.Error(log::PLATFORM, "GpuInteropImporter::Create: null device");
    return nullptr;
  }
  auto importer = std::unique_ptr<GpuInteropImporter>(new GpuInteropImporter());
  Impl& impl = *importer->_impl;
  impl.physicalDevice = static_cast<VkPhysicalDevice>(targetDevice.physicalDevice);
  impl.device = static_cast<VkDevice>(targetDevice.device);
  impl.importSemaphore = reinterpret_cast<PFN_vkImportSemaphoreWin32HandleKHR>(
      vkGetDeviceProcAddr(impl.device, "vkImportSemaphoreWin32HandleKHR"));
  if (impl.importSemaphore == nullptr) {
    log.Error(log::PLATFORM,
              "GpuInteropImporter::Create: VK_KHR_external_semaphore_win32 not enabled");
    return nullptr;
  }
  return importer;
}

ImportedImage GpuInteropImporter::ImportImage(void* memoryHandle, uint64_t allocationSize,
                                              uint32_t width, uint32_t height, uint32_t vkFormat,
                                              bool isColorRenderTarget) noexcept {
  auto& log = Logging::Get();
  ImportedImage out{};
  Impl& impl = *_impl;

  // Shared create-info — MUST match the exporter's (or the import fails).
  VkExternalMemoryImageCreateInfo extImg{};
  VkImageCreateInfo imgInfo{};
  interop::FillSharedImageCreateInfo(width, height, vkFormat, isColorRenderTarget, extImg, imgInfo);

  VkImage image = VK_NULL_HANDLE;
  if (vkCreateImage(impl.device, &imgInfo, nullptr, &image) != VK_SUCCESS) {
    log.Error(log::PLATFORM, "GpuInteropImporter: vkCreateImage failed");
    return out;
  }

  VkMemoryRequirements req{};
  vkGetImageMemoryRequirements(impl.device, image, &req);
  const uint32_t typeIndex = interop::FindMemoryType(impl.physicalDevice, req.memoryTypeBits,
                                                     VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  if (typeIndex == UINT32_MAX) {
    log.Error(log::PLATFORM, "GpuInteropImporter: no DEVICE_LOCAL memory type");
    vkDestroyImage(impl.device, image, nullptr);
    return out;
  }

  // Dedicated import (the exporter used a dedicated allocation).
  VkMemoryDedicatedAllocateInfo dedicated{};
  dedicated.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
  dedicated.image = image;

  VkImportMemoryWin32HandleInfoKHR importMem{};
  importMem.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR;
  importMem.pNext = &dedicated;
  importMem.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
  importMem.handle = static_cast<HANDLE>(memoryHandle);

  VkMemoryAllocateInfo alloc{};
  alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  alloc.pNext = &importMem;
  alloc.allocationSize = allocationSize;
  alloc.memoryTypeIndex = typeIndex;

  VkDeviceMemory memory = VK_NULL_HANDLE;
  if (vkAllocateMemory(impl.device, &alloc, nullptr, &memory) != VK_SUCCESS) {
    log.Error(log::PLATFORM, "GpuInteropImporter: vkAllocateMemory (import) failed");
    vkDestroyImage(impl.device, image, nullptr);
    return out;
  }
  if (vkBindImageMemory(impl.device, image, memory, 0) != VK_SUCCESS) {
    log.Error(log::PLATFORM, "GpuInteropImporter: vkBindImageMemory failed");
    vkFreeMemory(impl.device, memory, nullptr);
    vkDestroyImage(impl.device, image, nullptr);
    return out;
  }

  impl.images.push_back(image);
  impl.memories.push_back(memory);
  out.image = image;
  out.memory = memory;
  out.width = width;
  out.height = height;
  out.vkFormat = vkFormat;
  return out;
}

void* GpuInteropImporter::ImportTimelineSemaphore(void* semaphoreHandle) noexcept {
  auto& log = Logging::Get();
  Impl& impl = *_impl;

  VkSemaphoreTypeCreateInfo typeInfo{};
  typeInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
  typeInfo.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
  typeInfo.initialValue = 0;
  VkSemaphoreCreateInfo semInfo{};
  semInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
  semInfo.pNext = &typeInfo;

  VkSemaphore semaphore = VK_NULL_HANDLE;
  if (vkCreateSemaphore(impl.device, &semInfo, nullptr, &semaphore) != VK_SUCCESS) {
    log.Error(log::PLATFORM, "GpuInteropImporter: vkCreateSemaphore failed");
    return nullptr;
  }

  VkImportSemaphoreWin32HandleInfoKHR isi{};
  isi.sType = VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_WIN32_HANDLE_INFO_KHR;
  isi.semaphore = semaphore;
  isi.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT;
  isi.handle = static_cast<HANDLE>(semaphoreHandle);
  if (impl.importSemaphore(impl.device, &isi) != VK_SUCCESS) {
    log.Error(log::PLATFORM, "GpuInteropImporter: vkImportSemaphoreWin32HandleKHR failed");
    vkDestroySemaphore(impl.device, semaphore, nullptr);
    return nullptr;
  }
  impl.semaphores.push_back(semaphore);
  return semaphore;
}

}  // namespace pyxis
