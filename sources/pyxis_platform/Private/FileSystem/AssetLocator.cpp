// Pyxis platform — AssetLocator implementation.
//
// Windows-only v1 (plan §5). Uses GetModuleFileNameW for the executable path
// and SHGetKnownFolderPath for %LOCALAPPDATA%.

#include <Pyxis/Platform/FileSystem/AssetLocator.h>

#include <Shlobj.h>
#include <windows.h>

#include <algorithm>
#include <array>
#include <string>

namespace pyxis {

namespace {

std::string Wide2Utf8(const wchar_t* w) {
    const int needed = ::WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (needed <= 1) return {};
    std::string out;
    out.resize(static_cast<std::size_t>(needed - 1));
    ::WideCharToMultiByte(CP_UTF8, 0, w, -1, out.data(), needed, nullptr, nullptr);
    // Normalise to forward slashes for cross-API friendliness.
    std::replace(out.begin(), out.end(), '\\', '/');
    return out;
}

}  // namespace

AssetLocator::AssetLocator() = default;

const Path& AssetLocator::ExecutableDirectory() const noexcept {
    if (!_exeDirCached) {
        std::array<wchar_t, MAX_PATH> wbuf{};
        const DWORD len = ::GetModuleFileNameW(nullptr, wbuf.data(),
                                               static_cast<DWORD>(wbuf.size()));
        if (len > 0 && len < wbuf.size()) {
            std::string utf8 = Wide2Utf8(wbuf.data());
            const auto pos = utf8.find_last_of("/\\");
            if (pos != std::string::npos) utf8.resize(pos);
            _exeDir.Assign(utf8);
        }
        _exeDirCached = true;
    }
    return _exeDir;
}

Path AssetLocator::Resources() const noexcept {
    return ExecutableDirectory().Join("Resources");
}

// NOTE: the public method is named `LocateResource` — `<windows.h>` already
// `#define`s `FindResource` to FindResourceA / FindResourceW, so the original
// `FindResource` member name collided with the macro.  See AssetLocator.h
// where the macro-safe spelling lives.
Path AssetLocator::LocateResource(std::string_view relative) const noexcept {
    Path p = Resources().Join(relative);
    if (!p.Exists()) return {};
    return p;
}

Path AssetLocator::LocalAppData() const noexcept {
    PWSTR  raw = nullptr;
    Path   out;
    if (SUCCEEDED(::SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &raw))) {
        const std::string base = Wide2Utf8(raw);
        ::CoTaskMemFree(raw);
        out.Assign(base);
        out = out.Join("Pyxis");
        (void)out.EnsureDirectoryExists();
    }
    return out;
}

}  // namespace pyxis
