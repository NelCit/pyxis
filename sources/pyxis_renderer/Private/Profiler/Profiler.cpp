// Pyxis renderer — Profiler implementation.
//
// Plan §18.7 / §34. CPU scopes use std::chrono::steady_clock; GPU
// scopes are bracketed with nvrhi::ITimerQuery (NVRHI's wrapper
// around a VkQueryPool slot). Per-frame scope records land in one of
// SLOT_COUNT = MAX_FRAMES_IN_FLIGHT + 1 ring slots; the slot is
// drained when the ring rotates back to it, by which point the GPU
// has retired the submission and ITimerQuery::pollTimerQuery
// resolves without blocking the CPU. The 240-frame rolling profile +
// JSON/CSV/Tracy backends + §29.3 Performance panel land at M11 —
// what's wired here is the per-frame timestamp resolution that
// feeds them.
//
// PIMPL: every NVRHI handle, every STL container, every per-frame
// ring slot lives behind `Impl` so the public Profiler.h header
// doesn't drag <vector>, <array>, or <nvrhi/nvrhi.h> into
// consumers' translation units. §18.9 ABI rule.

#include <Pyxis/Renderer/Profiler.h>

#include <Pyxis/Platform/Logging/Log.h>
#include <Pyxis/Platform/Logging/LogCategories.h>
#include <Pyxis/Renderer/Forward.h>

#include <nvrhi/nvrhi.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <utility>
#include <vector>

namespace pyxis {

namespace {

FrameProfile::ScopeName MakeScopeName(std::string_view source) noexcept {
  FrameProfile::ScopeName name{};
  const std::size_t copy = std::min(source.size(), FrameProfile::ScopeName::CAPACITY);
  std::memcpy(name.data.data(), source.data(), copy);
  name.size = static_cast<uint8_t>(copy);
  return name;
}

int64_t NowNs() noexcept {
  using namespace std::chrono;
  return duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count();
}

}  // namespace

struct Profiler::Impl {
  // One scope's record. queryIdx = -1 for CPU scopes (durationMs
  // filled immediately at scope close); GPU scopes carry an index
  // into queryPool with durationMs resolved when the ring slot is
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

  nvrhi::IDevice* device     = nullptr;
  std::int64_t    frameStart = 0;
  std::uint64_t   frameIndex = 0;
  std::uint32_t   depth      = 0;

  std::array<Slot, SLOT_COUNT> slots{};
  std::uint32_t                currentSlot = 0;

  // Lazily-grown query pool. ScopeRecord::queryIdx indexes here.
  std::vector<nvrhi::TimerQueryHandle> queryPool;
  std::vector<int>                     freeQueries;

  // Stable snapshot of the most recently drained slot (returned by
  // LastFrameProfile()). Held by value so the std::span we hand out
  // is valid until the next BeginFrame() drain rotation.
  std::vector<FrameProfile::PassTiming> lastFrame;
  double                                lastCpuFrameMs = 0.0;
  double                                lastGpuFrameMs = 0.0;
  std::uint64_t                         lastFrameIndex = 0;

  // ----- M11: 240-frame rolling history per named scope -----
  // Keyed on the scope's inline-owning name (raw bytes from the
  // FrameProfile::ScopeName buffer). 240 samples per pass × ~16
  // unique passes today = ~30 KB total; well under any sane budget
  // and bounded since the pass set is enumerated at startup.
  struct RollingEntry {
    FrameProfile::ScopeName name{};
    FrameProfile::ScopeKind kind = FrameProfile::ScopeKind::Cpu;
    // Fixed-size ring + size counter for O(1) push. When `count`
    // reaches ROLLING_WINDOW, push advances `head` and clobbers the
    // oldest sample — standard FIFO ring.
    std::array<double, 240> samples{};
    std::uint32_t           head  = 0;
    std::uint32_t           count = 0;

    void Push(double sampleMs) noexcept {
      samples[head] = sampleMs;
      head = (head + 1u) % static_cast<std::uint32_t>(samples.size());
      if (count < samples.size())
        ++count;
    }
  };
  std::vector<RollingEntry> rolling;

  // O(N) name lookup is fine here — N is ~16 unique passes; a hash
  // map would add allocations to the hot path with no measurable
  // benefit. Returns index into `rolling`, allocating a fresh slot
  // on first sight of a new name.
  std::size_t LookupOrAddRolling(const FrameProfile::ScopeName& name,
                                 FrameProfile::ScopeKind        kind) noexcept {
    const std::string_view nameView = name.View();
    for (std::size_t i = 0; i < rolling.size(); ++i)
    {
      if (rolling[i].name.View() == nameView)
        return i;
    }
    rolling.emplace_back();
    rolling.back().name = name;
    rolling.back().kind = kind;
    return rolling.size() - 1;
  }

  int AcquireQuery() noexcept {
    if (device == nullptr)
      return -1;
    if (!freeQueries.empty())
    {
      const int idx = freeQueries.back();
      freeQueries.pop_back();
      device->resetTimerQuery(queryPool[static_cast<std::size_t>(idx)].Get());
      return idx;
    }
    nvrhi::TimerQueryHandle queryHandle = device->createTimerQuery();
    if (!queryHandle)
      return -1;
    const int idx = static_cast<int>(queryPool.size());
    queryPool.push_back(std::move(queryHandle));
    return idx;
  }

  void ReleaseQuery(int idx) noexcept {
    if (idx < 0)
      return;
    freeQueries.push_back(idx);
  }

  // Drain a slot whose GPU work has retired: resolve every query
  // into milliseconds, build the public PassTiming list, recycle
  // the queries.
  void DrainSlot(std::uint32_t slotIdx) noexcept {
    Slot& slot = slots[slotIdx];
    if (!slot.inFlight)
      return;

    std::vector<FrameProfile::PassTiming> resolved;
    resolved.reserve(slot.records.size());
    double gpuSumMs = 0.0;

    // gpuFrameMs sums GPU scopes at the *top of the GPU hierarchy*.
    // The Profiler shares one depth counter across CPU + GPU scopes,
    // so a GpuScope nested inside a CpuScope (e.g. RenderGraph
    // passes sit inside "render.frame.cpu") doesn't have CPU depth
    // 0. We track minGpuDepth in the slot and sum only the records
    // that match it — that's the GPU-side root for this frame.
    std::uint32_t minGpuDepth = UINT32_MAX;
    for (const ScopeRecord& record : slot.records)
    {
      if (record.kind == FrameProfile::ScopeKind::Gpu && record.depth < minGpuDepth)
      {
        minGpuDepth = record.depth;
      }
    }

    for (ScopeRecord& record : slot.records)
    {
      if (record.queryIdx >= 0 && device != nullptr)
      {
        nvrhi::ITimerQuery* query = queryPool[static_cast<std::size_t>(record.queryIdx)].Get();
        // SLOT_COUNT = MAX_FRAMES_IN_FLIGHT + 1, so we're past the
        // last frame the GPU could still be touching this query.
        // getTimerQueryTime busy-polls if not ready — should be a
        // no-op here.
        const float seconds = device->getTimerQueryTime(query);
        record.durationMs = static_cast<double>(seconds) * 1000.0;
        if (record.kind == FrameProfile::ScopeKind::Gpu && record.depth == minGpuDepth)
        {
          gpuSumMs += record.durationMs;
        }
        ReleaseQuery(record.queryIdx);
        record.queryIdx = -1;
      }

      FrameProfile::PassTiming timing{};
      timing.name = record.name;
      timing.kind = record.kind;
      timing.durationMs = record.durationMs;
      timing.depth = record.depth;
      resolved.push_back(timing);
    }

    lastFrame = std::move(resolved);
    lastCpuFrameMs = slot.cpuFrameMs;
    lastGpuFrameMs = gpuSumMs;
    lastFrameIndex = slot.frameIndex;

    // M11 — feed the rolling history. Skip unnamed records (length-0
    // ScopeName) — those don't have a stable identity to aggregate
    // against. The rolling entry is created lazily on first sight of
    // each name so the set grows naturally as new render passes
    // register.
    for (const FrameProfile::PassTiming& timing : lastFrame)
    {
      if (timing.name.size == 0)
        continue;
      const std::size_t idx = LookupOrAddRolling(timing.name, timing.kind);
      rolling[idx].Push(timing.durationMs);
    }

    slot.records.clear();
    slot.cpuFrameMs = 0.0;
    slot.inFlight = false;
  }
};

Profiler::Profiler(nvrhi::IDevice* device) : _impl(new Impl()) {
  _impl->device = device;
}

Profiler::~Profiler() {
  delete _impl;
}

void Profiler::BeginFrame() {
  // Rotate the ring and drain the slot we're about to reuse — by
  // now that slot's submission is GPU-retired (SLOT_COUNT >
  // MAX_FRAMES_IN_FLIGHT).
  _impl->currentSlot = (_impl->currentSlot + 1) % Impl::SLOT_COUNT;
  _impl->DrainSlot(_impl->currentSlot);

  _impl->frameStart = NowNs();
  _impl->depth = 0;
}

void Profiler::EndFrame() {
  Impl::Slot& slot = _impl->slots[_impl->currentSlot];
  slot.cpuFrameMs = static_cast<double>(NowNs() - _impl->frameStart) / 1.0e6;
  slot.frameIndex = _impl->frameIndex;
  slot.inFlight = true;
  ++_impl->frameIndex;
}

FrameProfile Profiler::LastFrameProfile() const {
  FrameProfile profile{};
  profile.passes = std::span<const FrameProfile::PassTiming>(_impl->lastFrame);
  profile.cpuFrameMs = _impl->lastCpuFrameMs;
  profile.gpuFrameMs = _impl->lastGpuFrameMs;
  // Index of the frame that produced these passes — see the comment
  // on FrameProfile::frameIndex. Defaults to 0 before the ring has
  // drained its first slot.
  profile.frameIndex = _impl->lastFrameIndex;
  return profile;
}

std::uint32_t Profiler::GetRollingStats(RollingStat* out,
                                        std::uint32_t capacity) const noexcept {
  // Caller may pass (nullptr, 0) to query the count without copying.
  if (out == nullptr)
    return static_cast<std::uint32_t>(_impl->rolling.size());

  // Percentile helper: copies the ring into a scratch buffer + sorts.
  // Per-pass N ≤ 240 so O(N log N) per pass per call is trivial; this
  // is a UI-cadence query (~30 Hz), not a hot path.
  auto percentile = [](std::vector<double>& sorted, double pct) noexcept -> double {
    if (sorted.empty())
      return 0.0;
    std::sort(sorted.begin(), sorted.end());
    const auto idx = static_cast<std::size_t>(
        (sorted.size() - 1) * std::clamp(pct, 0.0, 1.0));
    return sorted[idx];
  };

  std::vector<double> scratch;
  scratch.reserve(Impl::SLOT_COUNT > 0 ? 240u : 0u);  // bounded reserve

  const std::uint32_t emitCount =
      std::min(capacity, static_cast<std::uint32_t>(_impl->rolling.size()));
  for (std::uint32_t i = 0; i < emitCount; ++i)
  {
    const Impl::RollingEntry& entry = _impl->rolling[i];
    scratch.clear();
    scratch.insert(scratch.end(), entry.samples.begin(),
                   entry.samples.begin() + entry.count);
    RollingStat& dst = out[i];
    dst.name        = entry.name;
    dst.kind        = entry.kind;
    dst.sampleCount = entry.count;
    if (entry.count == 0)
    {
      dst.p50Ms = 0.0;
      dst.p99Ms = 0.0;
      dst.maxMs = 0.0;
      continue;
    }
    dst.p50Ms = percentile(scratch, 0.50);
    dst.p99Ms = percentile(scratch, 0.99);
    dst.maxMs = percentile(scratch, 1.00);
  }
  return emitCount;
}

// ---------------------------------------------------------------------------
// CpuScope — pushes the record at ctor (pre-order), back-fills
// durationMs at dtor. This produces a parent-before-children record
// list that's natural to print as an indented tree.
// ---------------------------------------------------------------------------

Profiler::CpuScope::CpuScope(Profiler& profiler, std::string_view name)
    : _profiler(&profiler), _startNs(NowNs()), _slotIndex(_profiler->_impl->currentSlot) {
  auto& records = _profiler->_impl->slots[_slotIndex].records;
  Impl::ScopeRecord record{};
  record.name = MakeScopeName(name);
  record.kind = FrameProfile::ScopeKind::Cpu;
  record.depth = _profiler->_impl->depth;
  record.queryIdx = -1;
  _recordIndex = records.size();
  records.push_back(record);
  ++_profiler->_impl->depth;
}

Profiler::CpuScope::~CpuScope() {
  if (_profiler == nullptr)
    return;
  const int64_t end = NowNs();
  --_profiler->_impl->depth;
  // Use the captured _slotIndex, not currentSlot — robust against
  // misuse that opens the scope across a Profiler::BeginFrame()
  // call.
  auto& records = _profiler->_impl->slots[_slotIndex].records;
  if (_recordIndex < records.size())
  {
    records[_recordIndex].durationMs = static_cast<double>(end - _startNs) / 1.0e6;
  }
}

// ---------------------------------------------------------------------------
// GpuScope — same pre-order push + late-fill pattern. The CPU-side
// durationMs stays 0 here; the GPU duration is back-filled when the
// slot is drained (Impl::DrainSlot) and the timer query has resolved.
// ---------------------------------------------------------------------------

Profiler::GpuScope::GpuScope(Profiler& profiler, nvrhi::ICommandList* commandList,
                             std::string_view name)
    : _profiler(&profiler),
      _commandList(commandList),
      _slotIndex(_profiler->_impl->currentSlot) {
  const FrameProfile::ScopeName scopeName = MakeScopeName(name);
  if (_commandList != nullptr)
    _commandList->beginMarker(scopeName.data.data());

  if (_commandList != nullptr && _profiler->_impl->device != nullptr)
  {
    _queryIdx = _profiler->_impl->AcquireQuery();
    if (_queryIdx >= 0)
    {
      _commandList->beginTimerQuery(
          _profiler->_impl->queryPool[static_cast<std::size_t>(_queryIdx)].Get());
    }
  }

  // Records pushed against the captured slot, not currentSlot —
  // same robustness rationale as CpuScope.
  auto& records = _profiler->_impl->slots[_slotIndex].records;
  Impl::ScopeRecord record{};
  record.name = scopeName;
  record.kind = FrameProfile::ScopeKind::Gpu;
  record.depth = _profiler->_impl->depth;
  record.queryIdx = _queryIdx;
  _recordIndex = records.size();
  records.push_back(record);
  ++_profiler->_impl->depth;
}

Profiler::GpuScope::~GpuScope() {
  if (_commandList != nullptr && _queryIdx >= 0
      && _profiler != nullptr && _profiler->_impl->device != nullptr)
  {
    _commandList->endTimerQuery(
        _profiler->_impl->queryPool[static_cast<std::size_t>(_queryIdx)].Get());
  }
  if (_commandList != nullptr)
    _commandList->endMarker();

  if (_profiler == nullptr)
    return;
  --_profiler->_impl->depth;
  // GPU durationMs is filled in DrainSlot once the timer query
  // resolves — nothing to back-fill from CPU here.
}

}  // namespace pyxis
