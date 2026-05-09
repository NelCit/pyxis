// Pyxis platform — VulkanContext POD.
//
// Plan §5.c. Opaque-by-design escape hatch for tools that need the raw
// Vulkan handles the device manager already owns — currently the
// pyxis_app ImGui Vulkan backend, on the way to the §29 ImGui panels.
// All handles cross the DLL boundary as `void*` so this public header
// does not need to transitively pull `<vulkan/vulkan.h>`; the consumer
// reinterprets them on the renderer/app side where Vulkan is already
// linked.
//
// Headless device managers fill in real handles too — there is no
// surface/swapchain — so a future regression tool can drive raw Vulkan
// just as the viewer does.

#pragma once

#include <Pyxis/Platform/Forward.h>
#include <Pyxis/Platform/PlatformApi.h>

#include <cstdint>

namespace pyxis {

struct PYXIS_PLATFORM_API VulkanContext {
  // Reinterpret on consumer side as VkInstance / VkPhysicalDevice /
  // VkDevice / VkQueue. Borrowed — owned by the IDeviceManager.
  void* instance = nullptr;
  void* physicalDevice = nullptr;
  void* device = nullptr;
  void* graphicsQueue = nullptr;
  uint32_t graphicsFamily = 0;
  // VkFormat of the swapchain colour attachment. Reinterpret as
  // VkFormat on the consumer side. Zero in headless mode (no
  // swapchain — ImGui-style consumers don't run there anyway).
  uint32_t colorFormat = 0;
};

}  // namespace pyxis
