// Pyxis renderer — Profiler facade.
//
// Plan §18.7. Public surface only — implementation lives in
// Private/Profiler/. M1 ships the skeleton: CpuScope + GpuScope +
// BeginFrame/EndFrame + LastFrameProfile. The 240-frame rolling
// ring, JSON/CSV/Tracy backends, and the §29.3 Performance panel
// land at M11.
//
// PIMPL was relaxed for Profiler in the same commit that dropped it
// for GpuScene — see plan §18.9 for the cross-DLL ABI rationale.
// The Profiler's inner ScopeRecord / Slot types and the per-frame
// query pool are now nested directly on the class, with the same
// trade-off: pyxis_renderer.dll consumers must compile with the
// matching vcpkg + clang-cl + EH/RTTI flags as the renderer DLL.
// The build presets enforce this.

#pragma once

#include <Pyxis/Renderer/Descs/FrameProfile.h>
#include <Pyxis/Renderer/Forward.h>
#include <Pyxis/Renderer/RendererApi.h>

#include <nvrhi/nvrhi.h>

#include <array>
#include <cstdint>
#include <string_view>
#include <vector>

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
        std::uint32_t    _slotIndex   = 0;
        std::size_t      _recordIndex = 0;
    };

    // ------------------------------------------------------------------
    // RAII GPU scope. Brackets a region on the supplied command list
    // with begin/end timestamp queries and an NVRHI debug marker.
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
        int                  _queryIdx    = -1;
        std::uint32_t        _slotIndex   = 0;
        std::size_t          _recordIndex = 0;
    };

    // Frame boundary — called by the Application once per frame.
    void BeginFrame();
    void EndFrame();

    // Read-only snapshot of the most recently resolved frame.
    [[nodiscard]] FrameProfile LastFrameProfile() const;

private:
    // ---- Inner scope-record types --------------------------------------
    // ScopeRecord is one entry in a slot's records vector. queryIdx = -1
    // for CPU scopes (durationMs filled at scope close); GPU scopes carry
    // an index into _queryPool with durationMs resolved when the slot is
    // drained.
    struct ScopeRecord {
        FrameProfile::ScopeName name{};
        FrameProfile::ScopeKind kind = FrameProfile::ScopeKind::Cpu;
        std::uint32_t           depth = 0;
        double                  durationMs = 0.0;
        int                     queryIdx = -1;
    };

    // SLOT_COUNT = MAX_FRAMES_IN_FLIGHT + 1 so by the time we rotate
    // back to a slot, every command list that wrote into its timer
    // queries has been retired by the GPU.
    static constexpr std::uint32_t SLOT_COUNT = MAX_FRAMES_IN_FLIGHT + 1;

    struct Slot {
        std::vector<ScopeRecord> records;
        std::uint64_t            frameIndex = 0;
        double                   cpuFrameMs = 0.0;
        bool                     inFlight   = false;
    };

    // ---- Internal helpers ----------------------------------------------
    [[nodiscard]] int  AcquireQuery() noexcept;
    void               ReleaseQuery(int idx) noexcept;
    void               DrainSlot(std::uint32_t slotIdx) noexcept;

    // ---- Data ----------------------------------------------------------
    nvrhi::IDevice* _device     = nullptr;
    std::int64_t    _frameStart = 0;
    std::uint64_t   _frameIndex = 0;
    std::uint32_t   _depth      = 0;

    std::array<Slot, SLOT_COUNT> _slots{};
    std::uint32_t                _currentSlot = 0;

    // Lazily-grown query pool. ScopeRecord::queryIdx indexes here.
    std::vector<nvrhi::TimerQueryHandle> _queryPool;
    std::vector<int>                     _freeQueries;

    // Stable snapshot of the most recently drained slot (returned by
    // LastFrameProfile()). Held by value so the std::span we hand out
    // is valid until the next BeginFrame() drain rotation.
    std::vector<FrameProfile::PassTiming> _lastFrame;
    double                                _lastCpuFrameMs = 0.0;
    double                                _lastGpuFrameMs = 0.0;
    std::uint64_t                         _lastFrameIndex = 0;

    // Friend the scope classes so they can reach private members
    // without going through accessors. They live on the Profiler
    // class so the friend declaration is just a pair of names.
    friend class CpuScope;
    friend class GpuScope;
};

}  // namespace pyxis
