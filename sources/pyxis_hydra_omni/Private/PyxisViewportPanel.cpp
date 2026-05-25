// RFC 0004 Option A — in-Kit viewport panel (native Carbonite IExt, C++17).
//
// Drives the Pyxis Hydra delegate over the live edited stage every app update
// and (Stage 2 proof) writes the rendered frame to disk next to the DLL. Stage 3
// adds the on-screen omni.ui Image; Stage 4 makes the display zero-copy.
//
// This TU is C++17 (Carbonite headers don't build under strict clang-cl C++23 —
// see RFC). It talks to the C++23 render core ONLY through the POD
// PyxisViewportBridge boundary (no pxr / renderer / nvrhi here).

#define CARB_EXPORTS

#include <carb/PluginUtils.h>
#include <carb/eventdispatcher/IEventDispatcher.h>

#include <omni/ext/IExt.h>
#include <omni/kit/IApp.h>  // omni::kit::kGlobalEventUpdate

#include <carb/Numeric.h>         // CARB_UINT32_MAX (used by IRenderer.h)
#include <carb/RenderingTypes.h>  // carb::Format
#include <carb/Types.h>           // carb::Uint2

#include <omni/ui/Frame.h>
#include <omni/ui/ImageProvider/DynamicTextureProvider.h>
#include <omni/ui/ImageWithProvider.h>
#include <omni/ui/Window.h>

#include "PyxisViewportBridge.h"

#include <memory>

#ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

std::string ThisDllDir()
{
    HMODULE thisModule = nullptr;
    if (!::GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&ThisDllDir), &thisModule))
    {
        return ".";
    }
    wchar_t pathW[MAX_PATH] = {};
    const DWORD len = ::GetModuleFileNameW(thisModule, pathW, MAX_PATH);
    if (len == 0 || len == MAX_PATH)
    {
        return ".";
    }
    std::wstring path(pathW, len);
    const size_t slash = path.find_last_of(L"\\/");
    if (slash != std::wstring::npos)
    {
        path.resize(slash);
    }
    const int n = ::WideCharToMultiByte(CP_UTF8, 0, path.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (n <= 0)
    {
        return ".";
    }
    std::string out(static_cast<size_t>(n - 1), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, path.c_str(), -1, out.data(), n, nullptr, nullptr);
    return out;
}

// RGBA8 top-down -> 24-bit BMP (bottom-up BGR). Proof artifact for Stage 2.
void WriteBmpRgba8(const char* path, const uint8_t* rgba, uint32_t w, uint32_t h)
{
    if (!rgba || w == 0 || h == 0)
    {
        return;
    }
    const uint32_t rowBytes = ((w * 3u) + 3u) & ~3u;  // 4-byte aligned rows.
    const uint32_t dataSize = rowBytes * h;
    const uint32_t fileSize = 54u + dataSize;
    std::FILE* file = std::fopen(path, "wb");
    if (!file)
    {
        return;
    }
    uint8_t hdr[54] = {};
    hdr[0] = 'B';
    hdr[1] = 'M';
    std::memcpy(hdr + 2, &fileSize, 4);
    const uint32_t dataOff = 54;
    std::memcpy(hdr + 10, &dataOff, 4);
    const uint32_t infoSize = 40;
    std::memcpy(hdr + 14, &infoSize, 4);
    std::memcpy(hdr + 18, &w, 4);
    std::memcpy(hdr + 22, &h, 4);
    const uint16_t planes = 1;
    std::memcpy(hdr + 26, &planes, 2);
    const uint16_t bpp = 24;
    std::memcpy(hdr + 28, &bpp, 2);
    std::memcpy(hdr + 34, &dataSize, 4);
    std::fwrite(hdr, 1, 54, file);
    std::vector<uint8_t> row(rowBytes, 0);
    for (int y = int(h) - 1; y >= 0; --y)  // BMP rows bottom-up.
    {
        const uint8_t* s = rgba + size_t(y) * w * 4;
        for (uint32_t x = 0; x < w; ++x)
        {
            row[x * 3 + 0] = s[x * 4 + 2];  // B
            row[x * 3 + 1] = s[x * 4 + 1];  // G
            row[x * 3 + 2] = s[x * 4 + 0];  // R
        }
        std::fwrite(row.data(), 1, rowBytes, file);
    }
    std::fclose(file);
}

} // namespace

const struct carb::PluginImplDesc kPluginImpl = {
    "omni.hydra.pyxis.panel.plugin",
    "Pyxis viewport panel: drives the Pyxis Hydra delegate over the live stage (RFC 0004).",
    "Pyxis",
    carb::PluginHotReload::eDisabled,
    "dev"
};

// Depend on omni::kit::IApp so the update event stream is available before we
// observe it; the eventdispatcher itself is a core carb plugin.
CARB_PLUGIN_IMPL_DEPS(omni::kit::IApp)

class PyxisViewportPanel : public omni::ext::IExt
{
public:
    void onStartup(const char* /*extId*/) override
    {
        _bridge = pyxis_omni::PyxisViewportBridge::Create(1280, 720);
        if (!_bridge)
        {
            std::printf("[omni.hydra.pyxis.panel] bridge create failed "
                        "(no GPU / external-memory interop); panel inactive.\n");
            std::fflush(stdout);
            return;
        }
        // omni.ui panel: a Window holding an Image backed by a dynamic texture
        // we refill from the rendered RGBA8 each frame (Stage 3, CPU upload;
        // Stage 4 makes this zero-copy). Built on the main/UI thread (onStartup).
        _provider = std::make_shared<omni::ui::DynamicTextureProvider>("pyxis_view");
        _window = omni::ui::Window::create(std::string("Pyxis Renderer"));
        _window->setWidth(static_cast<float>(_bridge->Width()) + 16.0f);
        _window->setHeight(static_cast<float>(_bridge->Height()) + 40.0f);
        if (auto frame = _window->getFrame())
        {
            // ImageWithProvider's ctor is protected; the OMNIUI_OBJECT create()
            // factory builds it (and is friended to the ctor).
            frame->addChild(omni::ui::ImageWithProvider::create(_provider));
        }

        auto* ed = carb::getCachedInterface<carb::eventdispatcher::IEventDispatcher>();
        if (ed)
        {
            _sub = ed->observeEvent(carb::RStringKey("omni.hydra.pyxis.panel.update"), 0,
                                    omni::kit::kGlobalEventUpdate,
                                    [this](const carb::eventdispatcher::Event&) { OnUpdate(); });
        }
        std::printf("[omni.hydra.pyxis.panel] started; 'Pyxis Renderer' window driving the live stage.\n");
        std::fflush(stdout);
    }

    void onShutdown() override
    {
        _sub.reset();
        if (_window)
        {
            _window->destroy();
            _window.reset();
        }
        _provider.reset();
        delete _bridge;
        _bridge = nullptr;
    }

private:
    void OnUpdate()
    {
        if (!_bridge || !_bridge->RenderCurrentStage(nullptr))
        {
            return;  // no live stage yet, or render failed.
        }
        ++_frames;
        const uint8_t* pixels = _bridge->PixelsRgba8();
        if (!pixels)
        {
            return;
        }

        // Push the rendered RGBA8 into the dynamic texture the Image displays.
        const uint32_t w = _bridge->Width(), h = _bridge->Height();
        if (_provider)
        {
            _provider->setBytesData(pixels, carb::Uint2{ w, h }, size_t(w) * 4,
                                    carb::Format::eRGBA8_UNORM);
        }

        // Headless proof: also dump the first frame + every ~120th to disk so a
        // no-window run leaves a viewable artifact + a log line.
        if (_frames == 1 || (_frames % 120) == 0)
        {
            const std::string bmp = ThisDllDir() + "\\pyxis_viewport.bmp";
            WriteBmpRgba8(bmp.c_str(), pixels, w, h);
            std::printf("[omni.hydra.pyxis.panel] frame=%llu instances=%llu (wrote %s)\n",
                        static_cast<unsigned long long>(_frames),
                        static_cast<unsigned long long>(_bridge->LastInstanceCount()), bmp.c_str());
            std::fflush(stdout);
        }
    }

    pyxis_omni::PyxisViewportBridge* _bridge = nullptr;
    carb::eventdispatcher::ObserverGuard _sub;
    std::shared_ptr<omni::ui::Window> _window;
    std::shared_ptr<omni::ui::DynamicTextureProvider> _provider;
    uint64_t _frames = 0;
};

CARB_PLUGIN_IMPL(kPluginImpl, PyxisViewportPanel)

void fillInterface(PyxisViewportPanel& /*iface*/)
{
}
