// Pyxis renderer — Profiler facade.
//
// Plan §18.7. Public surface only — implementation lives in
// Private/Profiler/. The CPU + GPU scope state, query pool ring, and
// per-frame timing snapshot are hidden behind a PIMPL pointer per
// §18.9: nothing about NVRHI's resource lifetimes leaks into
// consumer translation units.

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

  Profiler(const Profiler&) = delete;
  Profiler& operator=(const Profiler&) = delete;

  // ------------------------------------------------------------------
  // RAII CPU scope. Names follow the §34 dotted-lower-case convention.
  // ------------------------------------------------------------------
  class PYXIS_RENDERER_API CpuScope final {
  public:
    CpuScope(Profiler& profiler, std::string_view name);
    ~CpuScope();
    CpuScope(const CpuScope&) = delete;
    CpuScope& operator=(const CpuScope&) = delete;

  private:
    Profiler*     _profiler    = nullptr;
    std::int64_t  _startNs     = 0;
    std::uint32_t _slotIndex   = 0;
    std::size_t   _recordIndex = 0;
  };

  // ------------------------------------------------------------------
  // RAII GPU scope. Brackets a region on the supplied command list
  // with begin/end timestamp queries and an NVRHI debug marker.
  // ------------------------------------------------------------------
  class PYXIS_RENDERER_API GpuScope final {
  public:
    GpuScope(Profiler& profiler, nvrhi::ICommandList* commandList, std::string_view name);
    ~GpuScope();
    GpuScope(const GpuScope&) = delete;
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

  // ------------------------------------------------------------------
  // M11 — rolling per-pass percentile API (plan §34.2 / §34.3).
  //
  // The profiler keeps a 240-frame ring per *named* scope; this method
  // exposes the current p50 / p99 / max of that ring so consumers can
  // build sparklines, KPI gates, and regression reports without
  // touching internal state. Stats are recomputed from the latest
  // drained slot every BeginFrame — readers see at most one frame of
  // staleness.
  //
  // `stats` is filled with up to `capacity` entries; the actual count
  // is returned. Passing `out=nullptr` queries the count of scopes
  // currently in the ring (useful for sizing the destination buffer
  // before a second call).
  //
  // Output entries are owned by the caller after copy-out — they
  // hold `FrameProfile::ScopeName`'s inline 56-byte name buffer, not
  // a borrowed pointer, so they outlive the next BeginFrame.
  struct RollingStat {
    FrameProfile::ScopeName name{};
    FrameProfile::ScopeKind kind = FrameProfile::ScopeKind::Cpu;
    double p50Ms       = 0.0;
    double p99Ms       = 0.0;
    double maxMs       = 0.0;
    std::uint32_t sampleCount = 0;  // <= 240
  };
  static constexpr std::uint32_t ROLLING_WINDOW_FRAMES = 240;
  [[nodiscard]] std::uint32_t GetRollingStats(RollingStat* out,
                                              std::uint32_t capacity) const noexcept;

private:
  // PIMPL: the per-frame ring slots, NVRHI timer-query pool, and
  // ScopeRecord storage live behind this pointer so consumers don't
  // see `<vector>`, `<array>`, or `<nvrhi/nvrhi.h>` through this
  // header. §18.9 ABI rule.
  struct Impl;
  Impl* _impl = nullptr;
};

}  // namespace pyxis
