// RFC 0004 — headless proof that USD's PlugRegistry can resolve + LOAD the Pyxis
// delegate from its staged plugInfo.json, exactly as omni.hydra.pxr does when it
// instantiates the viewport delegate (Plug::_Load -> ArchLibraryOpen). This
// catches plugInfo LibraryPath/Root resolution bugs + DLL-dependency issues
// WITHOUT needing a Kit viewport. Exit 0 = the library loaded.
//
//   PluginLoadTest <ext-bin-resources-dir>

#include <pxr/base/plug/plugin.h>
#include <pxr/base/plug/registry.h>
#include <pxr/base/tf/type.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdio>
#include <string>

PXR_NAMESPACE_USING_DIRECTIVE

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        std::printf("usage: PluginLoadTest <ext-bin-resources-dir>\n");
        return 2;
    }
    const std::string resDir = argv[1];
    const std::string binDir = resDir.substr(0, resDir.find_last_of("\\/"));

    // Resolve the delegate's runtime deps (pyxis_renderer/platform, ...) from bin/.
    const int wn = ::MultiByteToWideChar(CP_UTF8, 0, binDir.c_str(), -1, nullptr, 0);
    std::wstring binW(wn > 0 ? wn - 1 : 0, L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, binDir.c_str(), -1, binW.data(), wn);
    ::AddDllDirectory(binW.c_str());

    PlugRegistry::GetInstance().RegisterPlugins(resDir);

    const TfType type = TfType::FindByName("HdPyxisRendererPlugin");
    if (type.IsUnknown())
    {
        std::printf("FAIL: HdPyxisRendererPlugin type not found (plugInfo Info bad)\n");
        return 3;
    }
    const PlugPluginPtr plugin = PlugRegistry::GetInstance().GetPluginForType(type);
    if (!plugin)
    {
        std::printf("FAIL: no plugin owns the type\n");
        return 4;
    }
    std::printf("PluginLoadTest: resolved library path = %s\n", plugin->GetPath().c_str());
    const bool loaded = plugin->Load();
    std::printf("PluginLoadTest: Load() = %s\n", loaded ? "OK" : "FAILED");
    return loaded ? 0 : 5;
}
