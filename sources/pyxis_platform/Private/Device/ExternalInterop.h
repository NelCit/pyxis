// Pyxis platform — shared external-memory extension enablement. RFC 0004.
//
// Both VkDeviceManager (viewer) and VkDeviceManagerHeadless opt the device into
// Win32 external-memory + external-semaphore so GpuInteropExporter can hand AOV
// images / a timeline semaphore to another Vulkan device (the Omniverse Kit
// viewport) with zero host copy. Factored here so the two managers cannot drift
// (the same maintenance hazard the single-extension-list comment warns about).

#pragma once

#include <vulkan/vulkan.h>

#include <algorithm>
#include <cstring>
#include <vector>

namespace pyxis {

// Spelled as string literals so callers don't have to pull
// <vulkan/vulkan_win32.h>; the Win32 export *functions* live only in
// GpuInteropExporter.cpp.
inline constexpr const char* EXTERNAL_INTEROP_EXTENSIONS[] = {
    "VK_KHR_external_memory",
    "VK_KHR_external_memory_win32",
    "VK_KHR_external_semaphore",
    "VK_KHR_external_semaphore_win32",
};

// Appends the four interop extensions to `exts` iff the adapter advertises all
// of them; returns whether interop is available. A stripped-down stack (§5.c)
// without them still creates a device — just with interop disabled.
[[nodiscard]] inline bool AppendExternalInteropExtensionsIfAvailable(
    VkPhysicalDevice phys, std::vector<const char*>& exts) noexcept {
  uint32_t count = 0;
  vkEnumerateDeviceExtensionProperties(phys, nullptr, &count, nullptr);
  std::vector<VkExtensionProperties> avail(count);
  vkEnumerateDeviceExtensionProperties(phys, nullptr, &count, avail.data());
  const auto has = [&](const char* name) {
    return std::any_of(avail.begin(), avail.end(), [&](const VkExtensionProperties& ext) {
      return std::strcmp(ext.extensionName, name) == 0;
    });
  };
  for (const char* name : EXTERNAL_INTEROP_EXTENSIONS)
    if (!has(name))
      return false;
  for (const char* name : EXTERNAL_INTEROP_EXTENSIONS)
    exts.push_back(name);
  return true;
}

}  // namespace pyxis
