// Pyxis renderer — Profiler facade.
//
// Plan §18.7. Public surface only — concrete impl in
// Private/Profiler/. M1 ships the skeleton: CpuScope + GpuScope +
// BeginFrame/EndFrame + LastFrameProfile. The 240-frame rolling ring,
// JSON/CSV/Tracy backends, and the §29.3 Performance panel land at M11.

#pragma once

#include <Pyxis/Renderer/Descs/FrameProfile.h>
#include <Pyxis/Renderer/Forward.h>
#include <Pyxis/Renderer/RendererApi.h>

#include <string_view>

namespace nvrhi {
class IDevice;
class ICommandList;
}  // namespace nvrhi

namespace pyxis {

class PYXIS_RENDERER_API Profiler final {
public:
    // GPU timing is enabled iff a non-null device is supplied. CPU-only
    // profiler (`Profiler{nullptr}`) is valid for unit tests that don't
    // touch the GPU. Default arguments are forbidden on public API
    // (§30.5), so callers must pass `nullptr` explicitly.
    explicit Profiler(nvrhi::IDevice* device);
    ~Profiler();

    Profiler(const Profiler&)            = delete;
    Profiler& operator=(const Profiler&) = delete;

    // ------------------------------------------------------------------
    // RAII CPU scope. Names follow the §34 dotted-lower-case convention.
    // ------------------------------------------------------------------
    class PYXIS_RENDERER_API CpuScope final {
    public:
        CpuScope(Profiler& profiler, std::string_view name);
        ~CpuScope();
        CpuScope(const CpuScope&)            = delete;
        CpuScope& operator=(const CpuScope&) = delete;
    private:
        Profiler*        _profiler = nullptr;
        std::int64_t     _startNs  = 0;
        FrameProfile::ScopeName _name{};
    };

    // ------------------------------------------------------------------
    // RAII GPU scope. Brackets a region on the supplied command list
    // with begin/end timestamp queries and an NVRHI debug marker. The
    // renderer uses this primitive internally for every render-graph
    // pass; the public type lets ingest / app code add their own GPU
    // regions that show up alongside engine passes in
    // FrameProfile::passes.
    // ------------------------------------------------------------------
    class PYXIS_RENDERER_API GpuScope final {
    public:
        GpuScope(Profiler& profiler, nvrhi::ICommandList* commandList, std::string_view name);
        ~GpuScope();
        GpuScope(const GpuScope&)            = delete;
        GpuScope& operator=(const GpuScope&) = delete;
    private:
        Profiler*            _profiler    = nullptr;
        nvrhi::ICommandList* _commandList = nullptr;
        FrameProfile::ScopeName _name{};
    };

    // Frame boundary — called by the Application once per frame.
    void BeginFrame();
    void EndFrame();

    // Read-only snapshot of the most recently resolved frame.
    [[nodiscard]] FrameProfile LastFrameProfile() const;

private:
    struct Impl;
    Impl* _impl = nullptr;
};

}  // namespace pyxis
