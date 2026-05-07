// Pyxis platform — asset locator.
//
// Resolves bundled-asset paths relative to the running executable. Used at
// M3.5+ to find the default-scene USD (plan §29.4.a) and at M5+ to find
// shader SPIR-V output. Independent of OpenUSD's ArResolver, which lives
// inside the ingest adapters.

#pragma once

#include <Pyxis/Platform/FileSystem/Path.h>
#include <Pyxis/Platform/PlatformApi.h>

#include <string_view>

namespace pyxis {

class PYXIS_PLATFORM_API AssetLocator final {
public:
    AssetLocator();

    // Returns the directory containing the running executable. Cached on
    // first call. Stable for the lifetime of the process.
    [[nodiscard]] const Path& ExecutableDirectory() const noexcept;

    // Path to the bundled Resources/ tree. By v1 convention this is
    //   <exe-dir>/Resources/
    [[nodiscard]] Path Resources() const noexcept;

    // Resolves a `relative` path against the Resources/ tree. Returns
    // an empty Path on miss; the renderer logs once and falls back to
    // the appropriate magenta/grey default.
    //
    // Named LocateResource (not FindResource) because <windows.h> already
    // `#define`s the latter to FindResourceA / FindResourceW.
    [[nodiscard]] Path LocateResource(std::string_view relative) const noexcept;

    // %LOCALAPPDATA%/Pyxis/, the per-user state directory used by:
    //   - the rotating log sink (§47.2)
    //   - parameters.json overlay (§29.1)
    //   - recent_scenes.json (§29.4)
    //   - PipelineCache/ (§33.8)
    //   - Crashes/ (§47.1)
    [[nodiscard]] Path LocalAppData() const noexcept;

private:
    mutable Path _exeDir;
    mutable bool _exeDirCached = false;
};

}  // namespace pyxis
