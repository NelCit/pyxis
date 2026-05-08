// Pyxis platform — Vulkan-Hpp dynamic dispatch loader storage + init.
//
// NVRHI's Vulkan backend uses vulkan.hpp with the dynamic loader, which:
//   1. Requires exactly one TU in the link to define the loader's static
//      storage via VULKAN_HPP_DEFAULT_DISPATCH_LOADER_DYNAMIC_STORAGE.
//   2. Requires the loader to be `.init(...)`-ed in three stages —
//      first with the C-side vkGetInstanceProcAddr, then again after
//      VkInstance creation, then again after VkDevice creation. Without
//      these calls every vk:: helper assertion-fails with
//      "Function <vkXxx> requires <VK_VERSION_1_0>".
//
// VkDeviceManager exposes only C Vulkan, so we keep vulkan.hpp out of
// its TU and provide three plain-C-friendly init shims here.

// NOLINTNEXTLINE(readability-identifier-naming) -- macro name dictated by Vulkan-Hpp.
#define VULKAN_HPP_DISPATCH_LOADER_DYNAMIC 1
#include <vulkan/vulkan.hpp>

VULKAN_HPP_DEFAULT_DISPATCH_LOADER_DYNAMIC_STORAGE

namespace pyxis {

void VulkanHppInitFromLoader(PFN_vkGetInstanceProcAddr getInstanceProcAddr) noexcept {
    VULKAN_HPP_DEFAULT_DISPATCHER.init(getInstanceProcAddr);
}

void VulkanHppInitFromInstance(VkInstance instance) noexcept {
    // Wrap the C handle into vk::Instance explicitly so the templated
    // init() overload doesn't try to deduce against the opaque C pointer
    // (which would require VkInstance_T's definition — it's opaque).
    VULKAN_HPP_DEFAULT_DISPATCHER.init(vk::Instance(instance));
}

void VulkanHppInitFromDevice(VkDevice device) noexcept {
    VULKAN_HPP_DEFAULT_DISPATCHER.init(vk::Device(device));
}

}  // namespace pyxis
