// RFC 0004 Option A — plain-C++ boundary between the Pyxis/USD render core and
// the Kit panel.
//
// The render core (PyxisHydraHost + PyxisEngine + nv-usd) compiles at C++23
// (Pyxis public headers use std::expected). The Kit panel (carb IExt + omni.ui)
// must compile at C++17 (Carbonite headers don't build under strict clang-cl
// C++23 — see RFC). They cannot share a translation unit, so they meet here:
// this header contains ONLY plain C++ / POD — no pxr, no renderer, no carb
// types. The C++23 side implements it; the C++17 panel consumes it.

#pragma once

#include <cstdint>

// Create() is the only symbol the C++17 panel DLL links; everything else is
// virtual dispatch through the vtable built in pyxis_hydra_omni.dll. The MSVC
// ABI is stable across /std versions, so the C++17 caller and C++23 impl agree.
#if defined(PYXIS_VIEWPORT_BRIDGE_EXPORTS)
#    define PYXIS_VIEWPORT_API __declspec(dllexport)
#else
#    define PYXIS_VIEWPORT_API __declspec(dllimport)
#endif

namespace pyxis_omni {

// The exportable color texture Kit imports for zero-copy display (Stage 4). All
// Win32 handles cross as void*; the importer (Kit's RTX device) must report the
// same deviceUuid or the shared handle is invalid.
struct ViewportSharedTexture
{
    void* memoryHandle = nullptr;     // Win32 NT HANDLE (OPAQUE_WIN32) of the VkImage memory.
    void* semaphoreHandle = nullptr;  // Win32 NT HANDLE of the export timeline semaphore.
    uint64_t allocationSize = 0;      // bytes of backing device memory.
    uint64_t lastSignaledValue = 0;   // timeline value to wait on before sampling.
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t vkFormat = 0;            // VkFormat value; importer must match.
    uint8_t deviceUuid[16] = {};      // exporting Pyxis device UUID.
};

// Drives the Pyxis Hydra delegate over the live Kit stage. Owns a separate Pyxis
// Vulkan device (per §32) + the render stack. Created/destroyed on the render
// thread; RenderCurrentStage is called from the app update tick.
class PyxisViewportBridge
{
public:
    // Stand up the render core at the given size. Returns nullptr if no GPU or
    // external-memory interop. Caller owns the result (delete via the dtor).
    static PYXIS_VIEWPORT_API PyxisViewportBridge* Create(uint32_t width, uint32_t height);

    virtual ~PyxisViewportBridge() = default;

    // Locate the live edited stage (USD global stage cache) and render one frame
    // through the Pyxis delegate, framing through `cameraPath` (UTF-8 SdfPath;
    // null/empty = first UsdGeomCamera found, else engine default). Returns true
    // if a frame was produced (a stage was found and rendered).
    virtual bool RenderCurrentStage(const char* cameraPath) = 0;

    // CPU display path (Stage 3): RGBA8, top-down, Width()*Height()*4 bytes.
    // Valid after a successful RenderCurrentStage; null before the first frame.
    virtual const uint8_t* PixelsRgba8() const = 0;

    // Zero-copy display path (Stage 4): the exportable color texture handles.
    virtual ViewportSharedTexture SharedTexture() const = 0;

    virtual uint32_t Width() const = 0;
    virtual uint32_t Height() const = 0;

    // Diagnostics: instances committed in the last frame (0 = nothing rendered).
    virtual uint64_t LastInstanceCount() const = 0;
};

} // namespace pyxis_omni
