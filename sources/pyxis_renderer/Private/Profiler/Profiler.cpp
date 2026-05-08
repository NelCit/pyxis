// Pyxis renderer — Profiler implementation.
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
//
// PIMPL was relaxed for Profiler in the same commit that dropped it
// for GpuScene; the inner ScopeRecord / Slot types live as private
// nested types of Profiler in the public header, with the same
// cross-DLL ABI trade-off documented in plan §18.9.

#include <Pyxis/Renderer/Profiler.h>

#include <Pyxis/Platform/Logging/Log.h>
#include <Pyxis/Platform/Logging/LogCategories.h>
#include <Pyxis/Renderer/Forward.h>

#include <nvrhi/nvrhi.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <utility>

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

Profiler::Profiler(nvrhi::IDevice* device) {
    _device = device;
}

Profiler::~Profiler() = default;

int Profiler::AcquireQuery() noexcept {
    if (_device == nullptr) return -1;
    if (!_freeQueries.empty()) {
        const int idx = _freeQueries.back();
        _freeQueries.pop_back();
        _device->resetTimerQuery(_queryPool[static_cast<std::size_t>(idx)].Get());
        return idx;
    }
    nvrhi::TimerQueryHandle queryHandle = _device->createTimerQuery();
    if (!queryHandle) return -1;
    const int idx = static_cast<int>(_queryPool.size());
    _queryPool.push_back(std::move(queryHandle));
    return idx;
}

void Profiler::ReleaseQuery(int idx) noexcept {
    if (idx < 0) return;
    _freeQueries.push_back(idx);
}

// Drain a slot whose GPU work has retired: resolve every query into
// milliseconds, build the public PassTiming list, recycle the queries.
void Profiler::DrainSlot(std::uint32_t slotIdx) noexcept {
    Slot& slot = _slots[slotIdx];
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
    std::uint32_t minGpuDepth = UINT32_MAX;
    for (const ScopeRecord& record : slot.records) {
        if (record.kind == FrameProfile::ScopeKind::Gpu && record.depth < minGpuDepth) {
            minGpuDepth = record.depth;
        }
    }

    for (ScopeRecord& record : slot.records) {
        if (record.queryIdx >= 0 && _device != nullptr) {
            nvrhi::ITimerQuery* query = _queryPool[static_cast<std::size_t>(record.queryIdx)].Get();
            // SLOT_COUNT = MAX_FRAMES_IN_FLIGHT + 1, so we're past the
            // last frame the GPU could still be touching this query.
            // getTimerQueryTime busy-polls if not ready — should be a
            // no-op here.
            const float seconds = _device->getTimerQueryTime(query);
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

    _lastFrame      = std::move(resolved);
    _lastCpuFrameMs = slot.cpuFrameMs;
    _lastGpuFrameMs = gpuSumMs;
    _lastFrameIndex = slot.frameIndex;

    slot.records.clear();
    slot.cpuFrameMs = 0.0;
    slot.inFlight   = false;
}

void Profiler::BeginFrame() {
    // Rotate the ring and drain the slot we're about to reuse — by now
    // that slot's submission is GPU-retired (SLOT_COUNT > MAX_FRAMES_IN_FLIGHT).
    _currentSlot = (_currentSlot + 1) % SLOT_COUNT;
    DrainSlot(_currentSlot);

    _frameStart = NowNs();
    _depth      = 0;
}

void Profiler::EndFrame() {
    Slot& slot     = _slots[_currentSlot];
    slot.cpuFrameMs = static_cast<double>(NowNs() - _frameStart) / 1.0e6;
    slot.frameIndex = _frameIndex;
    slot.inFlight   = true;
    ++_frameIndex;
}

FrameProfile Profiler::LastFrameProfile() const {
    FrameProfile profile{};
    profile.passes     = std::span<const FrameProfile::PassTiming>(_lastFrame);
    profile.cpuFrameMs = _lastCpuFrameMs;
    profile.gpuFrameMs = _lastGpuFrameMs;
    // Index of the frame that produced these passes — see the comment on
    // FrameProfile::frameIndex. Defaults to 0 before the ring has drained
    // its first slot.
    profile.frameIndex = _lastFrameIndex;
    return profile;
}

// ---------------------------------------------------------------------------
// CpuScope — pushes the record at ctor (pre-order), back-fills durationMs
// at dtor. This produces a parent-before-children record list that's
// natural to print as an indented tree.
// ---------------------------------------------------------------------------

Profiler::CpuScope::CpuScope(Profiler& profiler, std::string_view name)
    : _profiler(&profiler), _startNs(NowNs()),
      _slotIndex(_profiler->_currentSlot) {
    auto& records = _profiler->_slots[_slotIndex].records;
    ScopeRecord record{};
    record.name     = MakeScopeName(name);
    record.kind     = FrameProfile::ScopeKind::Cpu;
    record.depth    = _profiler->_depth;
    record.queryIdx = -1;
    _recordIndex = records.size();
    records.push_back(record);
    ++_profiler->_depth;
}

Profiler::CpuScope::~CpuScope() {
    if (_profiler == nullptr) return;
    const int64_t end = NowNs();
    --_profiler->_depth;
    // Use the captured _slotIndex, not currentSlot — robust against
    // misuse that opens the scope across a Profiler::BeginFrame() call.
    auto& records = _profiler->_slots[_slotIndex].records;
    if (_recordIndex < records.size()) {
        records[_recordIndex].durationMs = static_cast<double>(end - _startNs) / 1.0e6;
    }
}

// ---------------------------------------------------------------------------
// GpuScope — same pre-order push + late-fill pattern. The CPU-side
// durationMs stays 0 here; the GPU duration is back-filled when the slot
// is drained (Profiler::DrainSlot) and the timer query has resolved.
// ---------------------------------------------------------------------------

Profiler::GpuScope::GpuScope(Profiler& profiler, nvrhi::ICommandList* commandList, std::string_view name)
    : _profiler(&profiler), _commandList(commandList),
      _slotIndex(_profiler->_currentSlot) {
    const FrameProfile::ScopeName scopeName = MakeScopeName(name);
    if (_commandList != nullptr) _commandList->beginMarker(scopeName.data.data());

    if (_commandList != nullptr && _profiler->_device != nullptr) {
        _queryIdx = _profiler->AcquireQuery();
        if (_queryIdx >= 0) {
            _commandList->beginTimerQuery(
                _profiler->_queryPool[static_cast<std::size_t>(_queryIdx)].Get());
        }
    }

    // Records pushed against the captured slot, not currentSlot — same
    // robustness rationale as CpuScope.
    auto& records = _profiler->_slots[_slotIndex].records;
    ScopeRecord record{};
    record.name     = scopeName;
    record.kind     = FrameProfile::ScopeKind::Gpu;
    record.depth    = _profiler->_depth;
    record.queryIdx = _queryIdx;
    _recordIndex = records.size();
    records.push_back(record);
    ++_profiler->_depth;
}

Profiler::GpuScope::~GpuScope() {
    if (_commandList != nullptr && _queryIdx >= 0
        && _profiler != nullptr && _profiler->_device != nullptr) {
        _commandList->endTimerQuery(
            _profiler->_queryPool[static_cast<std::size_t>(_queryIdx)].Get());
    }
    if (_commandList != nullptr) _commandList->endMarker();

    if (_profiler == nullptr) return;
    --_profiler->_depth;
    // GPU durationMs is filled in DrainSlot once the timer query resolves
    // — nothing to back-fill from CPU here.
}

}  // namespace pyxis
