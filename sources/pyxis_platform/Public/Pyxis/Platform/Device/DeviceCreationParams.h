// Pyxis platform — device-creation parameters.
//
// Plain POD passed to VkDeviceManager / VkDeviceManagerHeadless ctors.
// Plan §5 / §5.b / §5.c.

#pragma once

#include <Pyxis/Platform/PlatformApi.h>

#include <cstdint>
#include <string_view>

namespace pyxis {

struct DeviceCreationParams {
    // -1 = pick the highest-VRAM ray-tracing-capable adapter (plan §5).
    int32_t adapterIndex = -1;

    // Vulkan validation layers; gated to Debug + --vk-validation per §5.
    bool enableValidation = false;

    // Aftermath crash diagnostics (Debug + Windows only — plan §33.9).
    bool enableAftermath = false;

    // Number of frames in flight. Compile-time cap = kMaxFramesInFlight (3).
    // Runtime default = 2 viewer / 3 headless for byte-identical EXR (§33.1).
    uint32_t framesInFlight = 2;

    // Application identity for the VkInstance / debug markers.
    std::string_view applicationName    = "Pyxis";
    uint32_t         applicationVersion = 0;
};

}  // namespace pyxis
