// Pyxis platform — Vulkan feature-check helpers.
//
// Plan §5.b — device creation fails fast with FeatureMissing if any of the
// required extensions / features are absent. The renderer never branches on
// "missing fallback"; v1 has no fallback (§5.b last paragraph).

#pragma once

#include <Pyxis/Platform/Device/AdapterInfo.h>

#include <vulkan/vulkan.h>

#include <array>
#include <cstdint>

namespace pyxis {

// One row per required extension/feature. The list mirrors the §5.b table.
struct RequiredFeature {
  const char* name;  // Diagnostic name (extension or feature).
  bool isExtension;  // true → check enabled extension list.
  bool required;     // false → optional (memory budget, etc.).
};

inline constexpr std::array<RequiredFeature, 12> REQUIRED_FEATURES{{
    {VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME, true, true},
    {VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME, true, true},
    {VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME, true, true},
    {VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME, true, true},
    {VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME, true, true},
    {VK_EXT_SCALAR_BLOCK_LAYOUT_EXTENSION_NAME, true, false},
    {VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME, true, true},
    {VK_EXT_HOST_QUERY_RESET_EXTENSION_NAME, true, true},
    {VK_KHR_MAINTENANCE_4_EXTENSION_NAME, true, true},
    {VK_EXT_MEMORY_BUDGET_EXTENSION_NAME, true, true},
    {VK_EXT_PIPELINE_CREATION_CACHE_CONTROL_EXTENSION_NAME, true, false},
    {"shaderInt64", false, true},
}};

// Inspects a physical device and fills out the AdapterInfo flag fields.
// Returns true iff every `required` feature is supported.
bool QueryAdapterFeatures(VkPhysicalDevice device, AdapterInfo& outInfo) noexcept;

}  // namespace pyxis
