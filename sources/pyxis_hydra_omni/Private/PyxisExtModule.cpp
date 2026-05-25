// RFC 0004 — native Carbonite IExt that registers the prebuilt Pyxis Hydra
// delegate with USD's plugin registry on extension startup, so "Pyxis" appears
// in Kit's Hydra-plugin enumeration. This REPLACES the former Python startup
// (omni/hydra/pyxis/extension.py): the omni.hydra.pyxis extension is now
// Python-free — a [[native.plugin]] (this DLL) plus the prebuilt delegate.
//
// Built as `omni.hydra.pyxis.plugin.dll` (the `*.plugin` filename Kit's
// `[[native.plugin]] path = "bin/*.plugin"` glob matches). Kit's extension
// system loads it, calls onStartup, and we point PlugRegistry at the staged
// plugInfo.json next to us — exactly what extension.py used to do.
//
// We deliberately keep the Hydra delegate (pyxis_hydra_omni.dll) a *pure* USD
// plugin with no Carbonite dependency; this thin module is the only carb-aware
// translation unit.

#define CARB_EXPORTS

#include <carb/PluginUtils.h>

#include <omni/ext/IExt.h>

#include <pxr/base/plug/registry.h>
#include <pxr/base/tf/type.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>  // NOMINMAX comes from the build (target_compile_definitions)

#include <cstdio>
#include <set>
#include <string>

namespace {

// Absolute directory holding THIS DLL (omni.hydra.pyxis.plugin.dll), which Kit
// stages at <ext>/bin/. plugInfo.json sits in <ext>/bin/resources/ (see
// build.ps1), and its Root=".." resolves the delegate LibraryPath to
// <ext>/bin/pyxis_hydra_omni.dll.
std::string ThisDllDir()
{
    HMODULE thisModule = nullptr;
    // GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS: resolve the module that contains
    // this function's address — i.e. this DLL, regardless of the host exe.
    if (!::GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&ThisDllDir), &thisModule))
    {
        return {};
    }
    wchar_t pathW[MAX_PATH] = {};
    const DWORD len = ::GetModuleFileNameW(thisModule, pathW, MAX_PATH);
    if (len == 0 || len == MAX_PATH)
    {
        return {};
    }
    std::wstring path(pathW, len);
    const size_t slash = path.find_last_of(L"\\/");
    if (slash != std::wstring::npos)
    {
        path.resize(slash);
    }
    // UTF-16 -> UTF-8 for PlugRegistry (ASCII paths in practice).
    const int n =
        ::WideCharToMultiByte(CP_UTF8, 0, path.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (n <= 0)
    {
        return {};
    }
    std::string out(static_cast<size_t>(n - 1), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, path.c_str(), -1, out.data(), n, nullptr, nullptr);
    return out;
}

} // namespace

const struct carb::PluginImplDesc kPluginImpl = {
    "omni.hydra.pyxis.plugin",                         // globally-unique plugin name
    "Registers the Pyxis Hydra render delegate (RFC 0004) with USD PlugRegistry.",
    "Pyxis",                                           // author
    carb::PluginHotReload::eDisabled,                  // a Hydra delegate cannot hot-reload
    "dev"                                              // build version
};

// No Carbonite interface dependencies: we only touch USD's PlugRegistry.
CARB_PLUGIN_IMPL_NO_DEPS()

class PyxisHydraExt : public omni::ext::IExt
{
public:
    void onStartup(const char* /*extId*/) override
    {
        const std::string binDir = ThisDllDir();
        if (binDir.empty())
        {
            std::printf("[omni.hydra.pyxis] could not locate plugin DLL directory; "
                        "delegate NOT registered.\n");
            return;
        }
        const std::string pluginDir = binDir + "\\resources";

        // When the pxr viewport engine instantiates our delegate, USD's
        // PlugRegistry LoadLibrary()s pyxis_hydra_omni.dll on the render thread.
        // That DLL's runtime deps (pyxis_renderer.dll, pyxis_platform.dll, nvrhi,
        // flecs, spdlog, ...) live here in bin/. Kit uses SECURE DLL search
        // (SetDefaultDllDirectories), so the legacy PATH and the loaded DLL's own
        // directory are NOT searched — hence "The specified module could not be
        // found." The fix: AddDllDirectory(bin) (adds it to the loader's user
        // dirs, which secure search DOES consult), then pre-load the delegate
        // with LOAD_LIBRARY_SEARCH_DEFAULT_DIRS so its ENTIRE dependency chain
        // (incl. transitive bin deps) resolves now. Pre-loading also means USD's
        // later load on the render thread is a no-op refcount bump.
        {
            const int wn =
                ::MultiByteToWideChar(CP_UTF8, 0, binDir.c_str(), -1, nullptr, 0);
            std::wstring binW(wn > 0 ? wn - 1 : 0, L'\0');
            ::MultiByteToWideChar(CP_UTF8, 0, binDir.c_str(), -1, binW.data(), wn);

            ::AddDllDirectory(binW.c_str());  // user dir, consulted by secure search
            const std::wstring delegateW = binW + L"\\pyxis_hydra_omni.dll";
            const HMODULE handle = ::LoadLibraryExW(delegateW.c_str(), nullptr,
                                                    LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
            std::printf("[omni.hydra.pyxis] pre-load delegate runtime (%s) from %s\n",
                        handle ? "ok" : "FAILED (dep still missing)", binDir.c_str());
            std::fflush(stdout);
        }

        PXR_NS::PlugRegistry& registry = PXR_NS::PlugRegistry::GetInstance();
        registry.RegisterPlugins(pluginDir);

        // Confirm the delegate type is now discoverable (proves plugInfo ingested),
        // mirroring the diagnostics the former Python shim emitted.
        const PXR_NS::TfType pyxisType =
            PXR_NS::TfType::FindByName("HdPyxisRendererPlugin");
        std::printf("[omni.hydra.pyxis] Registered Pyxis Hydra delegate from %s; "
                    "type discoverable = %d\n",
                    pluginDir.c_str(), static_cast<int>(!pyxisType.IsUnknown()));

        // Enumerate HdRendererPlugin types the way Kit's renderer menu does, so a
        // grep of stdout confirms Pyxis is enumerable alongside Storm.
        const PXR_NS::TfType base = PXR_NS::TfType::FindByName("HdRendererPlugin");
        std::set<PXR_NS::TfType> derived;
        if (!base.IsUnknown())
        {
            base.GetAllDerivedTypes(&derived);
        }
        bool pyxisSeen = false;
        std::string names;
        for (const PXR_NS::TfType& t : derived)
        {
            if (!names.empty())
            {
                names += ", ";
            }
            names += t.GetTypeName();
            if (t.GetTypeName().find("Pyxis") != std::string::npos)
            {
                pyxisSeen = true;
            }
        }
        std::printf("PYXIS_ENUM count=%zu pyxis=%d plugins=[%s]\n",
                    derived.size(), static_cast<int>(pyxisSeen), names.c_str());
        std::fflush(stdout);
    }

    void onShutdown() override
    {
        // USD has no unregister-plugins API; the delegate simply stops being
        // selectable when the process exits. Nothing to do here.
    }
};

CARB_PLUGIN_IMPL(kPluginImpl, PyxisHydraExt)

// One fillInterface per exported interface type (here: just IExt via PyxisHydraExt).
void fillInterface(PyxisHydraExt& /*iface*/)
{
}
