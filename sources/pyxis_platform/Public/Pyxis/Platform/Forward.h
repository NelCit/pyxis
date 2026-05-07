// Pyxis platform — forward declarations.
//
// Cross-translation-unit forward decls for every type the platform layer
// exposes through Public/. Following plan §30.3, this header pulls only
// stdlib + Vulkan forwards; concrete public PODs and interfaces live in
// their own headers.

#pragma once

#include <cstdint>

// Vulkan / NVRHI forwards. Both are required by IDeviceManager's signature.
namespace nvrhi {
class IDevice;
class ICommandList;
}  // namespace nvrhi

namespace pyxis {

// Public PODs and interfaces (full definitions in their own headers).
struct AdapterInfo;
struct DeviceCreationParams;
struct Resolution;
class IDeviceManager;
class Logger;
class Path;
class AssetLocator;

}  // namespace pyxis
