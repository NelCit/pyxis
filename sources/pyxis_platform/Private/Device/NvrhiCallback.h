// Pyxis platform — shared NVRHI message-callback adapter.
//
// Both VkDeviceManager and VkDeviceManagerHeadless feed
// nvrhi::DeviceDesc::errorCB; the singleton lives here so the two
// device managers share one stateless adapter and don't accidentally
// drift on what severities they forward (§33.10 cross-DLL logger).

#pragma once

#include <nvrhi/nvrhi.h>

namespace pyxis {

// Returns the process-wide NVRHI -> spdlog adapter. Stateless; safe to
// share between the windowed and headless device managers.
nvrhi::IMessageCallback& NvrhiCallback() noexcept;

}  // namespace pyxis
