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
        nvrhi::TimerQueryHandle queryHandle = device->createTimerQuery();
        if (!queryHandle) return -1;
        const int idx = static_cast<int>(queryPool.size());
        queryPool.push_back(std::move(queryHandle));
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

        // gpuFrameMs sums GPU scopes at the *top of the GPU hierarchy*.
        // The Profiler shares one depth counter across CPU + GPU scopes,
        // so a GpuScope nested inside a CpuScope (e.g. RenderGraph passes
        // sit inside "render.frame.cpu") doesn't have CPU depth 0. We
        // track minGpuDepth in the slot and sum only the records that
        // match it — that's the GPU-side root for this frame.
        uint32_t minGpuDepth = UINT32_MAX;
        for (const ScopeRecord& record : slot.records) {
            if (record.kind == FrameProfile::ScopeKind::Gpu && record.depth < minGpuDepth) {
                minGpuDepth = record.depth;
            }
        }

        for (ScopeRecord& record : slot.records) {
            if (record.queryIdx >= 0 && device) {
                nvrhi::ITimerQuery* query = queryPool[static_cast<std::size_t>(record.queryIdx)].Get();
                // SLOT_COUNT = MAX_FRAMES_IN_FLIGHT + 1, so we're past the
                // last frame the GPU could still be touching this query.
                // getTimerQueryTime busy-polls if not ready — should be a
                // no-op here.
                const float seconds = device->getTimerQueryTime(query);
                record.durationMs = static_cast<double>(seconds) * 1000.0;
                if (record.kind == FrameProfile::ScopeKind::Gpu && record.depth == minGpuDepth) {
                    gpuSumMs += record.durationMs;
                }
                ReleaseQuery(record.queryIdx);
                record.queryIdx = -1;
            }

            FrameProfile::PassTiming timing{};
            timing.name       = record.name;
            timing.kind       = record.kind;
            timing.durationMs = record.durationMs;
            timing.depth      = record.depth;
            resolved.push_back(timing);
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
    FrameProfile profile{};
    profile.passes     = std::span<const FrameProfile::PassTiming>(_impl->lastFrame);
    profile.cpuFrameMs = _impl->lastCpuFrameMs;
    profile.gpuFrameMs = _impl->lastGpuFrameMs;
    profile.frameIndex = _impl->frameIndex;
    return profile;
}

// ---------------------------------------------------------------------------
// CpuScope — pushes the record at ctor (pre-order), back-fills durationMs
// at dtor. This produces a parent-before-children record list that's
// natural to print as an indented tree.
// ---------------------------------------------------------------------------

Profiler::CpuScope::CpuScope(Profiler& profiler, std::string_view name)
    : _profiler(&profiler), _startNs(NowNs()) {
    auto& records = _profiler->_impl->slots[_profiler->_impl->currentSlot].records;
    Impl::ScopeRecord record{};
    record.name     = MakeScopeName(name);
    record.kind     = FrameProfile::ScopeKind::Cpu;
    record.depth    = _profiler->_impl->depth;
    record.queryIdx = -1;
    _recordIndex = records.size();
    records.push_back(record);
    ++_profiler->_impl->depth;
}

Profiler::CpuScope::~CpuScope() {
    if (!_profiler) return;
    const int64_t end = NowNs();
    --_profiler->_impl->depth;
    auto& records = _profiler->_impl->slots[_profiler->_impl->currentSlot].records;
    if (_recordIndex < records.size()) {
        records[_recordIndex].durationMs = static_cast<double>(end - _startNs) / 1.0e6;
    }
}

// ---------------------------------------------------------------------------
// GpuScope — same pre-order push + late-fill pattern. The CPU-side
// durationMs stays 0 here; the GPU duration is back-filled when the slot
// is drained (Impl::DrainSlot) and the timer query has resolved.
// ---------------------------------------------------------------------------

Profiler::GpuScope::GpuScope(Profiler& profiler, nvrhi::ICommandList* commandList, std::string_view name)
    : _profiler(&profiler), _commandList(commandList) {
    const FrameProfile::ScopeName scopeName = MakeScopeName(name);
    if (_commandList) _commandList->beginMarker(scopeName.data.data());

    if (_commandList && _profiler->_impl->device) {
        _queryIdx = _profiler->_impl->AcquireQuery();
        if (_queryIdx >= 0) {
            _commandList->beginTimerQuery(
                _profiler->_impl->queryPool[static_cast<std::size_t>(_queryIdx)].Get());
        }
    }

    auto& records = _profiler->_impl->slots[_profiler->_impl->currentSlot].records;
    Impl::ScopeRecord record{};
    record.name     = scopeName;
    record.kind     = FrameProfile::ScopeKind::Gpu;
    record.depth    = _profiler->_impl->depth;
    record.queryIdx = _queryIdx;
    _recordIndex = records.size();
    records.push_back(record);
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
    // GPU durationMs is filled in DrainSlot once the timer query resolves
    // — nothing to back-fill from CPU here.
}

}  // namespace pyxis
