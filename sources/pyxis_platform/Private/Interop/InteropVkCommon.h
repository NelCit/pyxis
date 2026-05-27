// Pyxis platform — shared Vulkan external-memory helpers (interop-private).
//
// The GPU-interop EXPORTER and IMPORTER must create their shared VkImage with
// IDENTICAL parameters (usage / tiling / sharing / external-handle type) or the
// import fails at runtime; and both needed the same memory-type search. Both were
// copy-pasted byte-for-byte. One definition here keeps the export/import image
// contract locked together.

#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>

namespace pyxis::interop {

// Memory-type index satisfying `typeBits` AND all `wanted` property flags.
// Returns UINT32_MAX if none match.
[[nodiscard]] inline uint32_t FindMemoryType(VkPhysicalDevice phys, uint32_t typeBits,
                                             VkMemoryPropertyFlags wanted) noexcept {
  VkPhysicalDeviceMemoryProperties props{};
  vkGetPhysicalDeviceMemoryProperties(phys, &props);
  for (uint32_t i = 0; i < props.memoryTypeCount; ++i) {
    const bool typeOk = (typeBits & (1u << i)) != 0u;
    const bool propsOk = (props.memoryTypes[i].propertyFlags & wanted) == wanted;
    if (typeOk && propsOk)
      return i;
  }
  return UINT32_MAX;
}

// Fill `imgInfo` (and its chained `extImg`) for a shared 2D image. The exporter
// and importer MUST agree on these exact params. The caller owns both structs'
// storage — `imgInfo.pNext` points at `extImg`, so both must outlive the
// vkCreateImage call (declare them as locals in the same scope).
inline void FillSharedImageCreateInfo(uint32_t width, uint32_t height, uint32_t vkFormat,
                                      bool isColorRenderTarget,
                                      VkExternalMemoryImageCreateInfo& extImg,
                                      VkImageCreateInfo& imgInfo) noexcept {
  extImg = {};
  extImg.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
  extImg.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;

  VkImageUsageFlags usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                            VK_IMAGE_USAGE_SAMPLED_BIT;
  if (isColorRenderTarget)
    usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_STORAGE_BIT;

  imgInfo = {};
  imgInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  imgInfo.pNext = &extImg;
  imgInfo.imageType = VK_IMAGE_TYPE_2D;
  imgInfo.format = static_cast<VkFormat>(vkFormat);
  imgInfo.extent = {width, height, 1};
  imgInfo.mipLevels = 1;
  imgInfo.arrayLayers = 1;
  imgInfo.samples = VK_SAMPLE_COUNT_1_BIT;
  imgInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
  imgInfo.usage = usage;
  imgInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  imgInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
}

}  // namespace pyxis::interop
