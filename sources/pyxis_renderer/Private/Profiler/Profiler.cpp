// Pyxis renderer — Profiler implementation (M1).
//
// Plan §18.7 / §34. CPU scopes use std::chrono::high_resolution_clock;
// GPU scopes are bracketed with nvrhi::ITimerQuery (NVRHI's wrapper
// around a VkQueryPool slot). Per-frame scope records land in one of
// SLOT_COUNT = MAX_FRAMES_IN_FLIGHT + 1 ring slots; the slot is drained
// when the ring rotates back to it, by which point the GPU has retired
// the submission and ITimerQuery::pollTimerQuery resolves without
// blocking the CPU. The 240-frame rolling profile + JSON/CSV/Tracy
// backends + §29.3 Performance panel still land at M11 — what's wired
// here is the per-frame timestamp resolution that feeds them.

#include <Pyxis/Renderer/Profiler.h>

#include <Pyxis/Platform/Logging/Log.h>
#include <Pyxis/Platform/Logging/LogCategories.h>
#include <Pyxis/Renderer/Forward.h>

#include <nvrhi/nvrhi.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <vector>

namespace pyxis {

namespace {

FrameProfile::ScopeName MakeScopeName(std::string_view sv) noexcept {
    FrameProfile::ScopeName n{};
    const std::size_t copy = std::min(sv.size(), FrameProfile::ScopeName::CAPACITY);
    std::memcpy(n.data.data(), sv.data(), copy);
    n.size = static_cast<uint8_t>(copy);
    return n;
}

int64_t NowNs() noexcept {
    using namespace std::chrono;
    return duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count();
}

}  // namespace

struct Profiler::Impl {
    // One scope's record. queryIdx = -1 for CPU scopes (durationMs filled
    // immediately at scope close); GPU scopes carry an index into queryPool
    // and have their durationMs resolved when the ring slot is drained.
    struct ScopeRecord {
        FrameProfile::ScopeName name{};
        FrameProfile::ScopeKind kind = FrameProfile::ScopeKind::Cpu;
        uint32_t                depth = 0;
        double                  durationMs = 0.0;
        int                     queryIdx = -1;
    };

    // Per-frame ring slot. SLOT_COUNT = MAX_FRAMES_IN_FLIGHT + 1 so by the
    // time we rotate back to a slot, every command list that wrote into
    // its timer queries has been retired by the GPU.
    static constexpr uint32_t SLOT_COUNT = MAX_FRAMES_IN_FLIGHT + 1;
    struct Slot {
        std::vector<ScopeRecord> records;
        double                   cpuFrameMs = 0.0;
        bool                     inFlight   = false;
    };

    nvrhi::IDevice*                       device     = nullptr;
    int64_t                               frameStart = 0;
    uint64_t                              frameIndex = 0;
    uint32_t                              depth      = 0;

    std::array<Slot, SLOT_COUNT>          slots{};
    uint32_t                              currentSlot = 0;

    // Lazily-grown query pool. queryIdx in ScopeRecord indexes here.
    std::vector<nvrhi::TimerQueryHandle>  queryPool;
    std::vector<int>                      freeQueries;

    // Stable snapshot of the most recently drained slot (returned by
    // LastFrameProfile()). Held by value here so the std::span we hand
    // out is valid until the next BeginFrame() drain rotation.
    std::vector<FrameProfile::PassTiming> lastFrame;
    double                                lastCpuFrameMs = 0.0;
    double                                lastGpuFrameMs = 0.0;

    int AcquireQuery() noexcept {
        if (!device) return -1;
        if (!freeQueries.empty()) {
            const int idx = freeQueries.back();
            freeQueries.pop_back();
            device->resetTimerQuery(queryPool[static_cast<std::size_t>(idx)].Get());
            return idx;
        }
        nvrhi::TimerQueryHandle h = device->createTimerQuery();
        if (!h) return -1;
        const int idx = static_cast<int>(queryPool.size());
        queryPool.push_back(std::move(h));
        return idx;
    }

    void ReleaseQuery(int idx) noexcept {
        if (idx < 0) return;
        freeQueries.push_back(idx);
    }

    // Drain a slot whose GPU work has retired: resolve every query into
    // milliseconds, build the public PassTiming list, recycle the queries.
    void DrainSlot(uint32_t slotIdx) noexcept {
        Slot& slot = slots[slotIdx];
        if (!slot.inFlight) return;

        std::vector<FrameProfile::PassTiming> resolved;
        resolved.reserve(slot.records.size());
        double gpuSumMs = 0.0;

        for (ScopeRecord& r : slot.records) {
            if (r.queryIdx >= 0 && device) {
                nvrhi::ITimerQuery* q = queryPool[static_cast<std::size_t>(r.queryIdx)].Get();
                // SLOT_COUNT = MAX_FRAMES_IN_FLIGHT + 1, so we're past the
                // last frame the GPU could still be touching this query.
                // getTimerQueryTime busy-polls if not ready — should be a
                // no-op here.
                const float seconds = device->getTimerQueryTime(q);
                r.durationMs = static_cast<double>(seconds) * 1000.0;
                if (r.depth == 0) gpuSumMs += r.durationMs;
                ReleaseQuery(r.queryIdx);
                r.queryIdx = -1;
            }

            FrameProfile::PassTiming t{};
            t.name       = r.name;
            t.kind       = r.kind;
            t.durationMs = r.durationMs;
            t.depth      = r.depth;
            resolved.push_back(t);
        }

        lastFrame      = std::move(resolved);
        lastCpuFrameMs = slot.cpuFrameMs;
        lastGpuFrameMs = gpuSumMs;

        slot.records.clear();
        slot.cpuFrameMs = 0.0;
        slot.inFlight   = false;
    }
};

Profiler::Profiler(nvrhi::IDevice* device) : _impl(new Impl()) {
    _impl->device = device;
}

Profiler::~Profiler() { delete _impl; }

void Profiler::BeginFrame() {
    // Rotate the ring and drain the slot we're about to reuse — by now
    // that slot's submission is GPU-retired (SLOT_COUNT > MAX_FRAMES_IN_FLIGHT).
    _impl->currentSlot = (_impl->currentSlot + 1) % Impl::SLOT_COUNT;
    _impl->DrainSlot(_impl->currentSlot);

    _impl->frameStart = NowNs();
    _impl->depth      = 0;
}

void Profiler::EndFrame() {
    Impl::Slot& slot = _impl->slots[_impl->currentSlot];
    slot.cpuFrameMs  = static_cast<double>(NowNs() - _impl->frameStart) / 1.0e6;
    slot.inFlight    = true;
    ++_impl->frameIndex;
}

FrameProfile Profiler::LastFrameProfile() const {
    FrameProfile p{};
    p.passes     = std::span<const FrameProfile::PassTiming>(_impl->lastFrame);
    p.cpuFrameMs = _impl->lastCpuFrameMs;
    p.gpuFrameMs = _impl->lastGpuFrameMs;
    p.frameIndex = _impl->frameIndex;
    return p;
}

// ---------------------------------------------------------------------------
// CpuScope
// ---------------------------------------------------------------------------

Profiler::CpuScope::CpuScope(Profiler& profiler, std::string_view name)
    : _profiler(&profiler), _startNs(NowNs()), _name(MakeScopeName(name)) {
    ++_profiler->_impl->depth;
}

Profiler::CpuScope::~CpuScope() {
    if (!_profiler) return;
    const int64_t end = NowNs();
    --_profiler->_impl->depth;

    Impl::ScopeRecord r{};
    r.name       = _name;
    r.kind       = FrameProfile::ScopeKind::Cpu;
    r.durationMs = static_cast<double>(end - _startNs) / 1.0e6;
    r.depth      = _profiler->_impl->depth;
    r.queryIdx   = -1;
    _profiler->_impl->slots[_profiler->_impl->currentSlot].records.push_back(r);
}

// ---------------------------------------------------------------------------
// GpuScope — bracket the command list with NVRHI's timer query and the
// debug marker. Resolution is deferred to the slot drain in BeginFrame().
// ---------------------------------------------------------------------------

Profiler::GpuScope::GpuScope(Profiler& profiler, nvrhi::ICommandList* commandList, std::string_view name)
    : _profiler(&profiler), _commandList(commandList), _name(MakeScopeName(name)) {
    if (_commandList) _commandList->beginMarker(_name.data.data());

    if (_commandList && _profiler->_impl->device) {
        _queryIdx = _profiler->_impl->AcquireQuery();
        if (_queryIdx >= 0) {
            _commandList->beginTimerQuery(
                _profiler->_impl->queryPool[static_cast<std::size_t>(_queryIdx)].Get());
        }
    }
    ++_profiler->_impl->depth;
}

Profiler::GpuScope::~GpuScope() {
    if (_commandList && _queryIdx >= 0 && _profiler && _profiler->_impl->device) {
        _commandList->endTimerQuery(
            _profiler->_impl->queryPool[static_cast<std::size_t>(_queryIdx)].Get());
    }
    if (_commandList) _commandList->endMarker();

    if (!_profiler) return;
    --_profiler->_impl->depth;

    Impl::ScopeRecord r{};
    r.name       = _name;
    r.kind       = FrameProfile::ScopeKind::Gpu;
    r.durationMs = 0.0;       // resolved when this slot is drained.
    r.depth      = _profiler->_impl->depth;
    r.queryIdx   = _queryIdx;
    _profiler->_impl->slots[_profiler->_impl->currentSlot].records.push_back(r);
}

}  // namespace pyxis
