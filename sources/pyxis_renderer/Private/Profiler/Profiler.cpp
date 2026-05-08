// Pyxis renderer — Profiler implementation (M1 skeleton).
//
// Plan §18.7 / §34. CPU scopes use std::chrono::high_resolution_clock;
// GPU scopes wrap NVRHI's beginMarker/endMarker (real GPU timestamp
// queries land at M11 when the FrameProfile ring + Performance panel
// are wired). The frame-timing entries are kept on _passes; LastFrame-
// Profile returns a span into the just-completed frame's entries.

#include <Pyxis/Renderer/Profiler.h>

#include <Pyxis/Platform/Logging/Log.h>
#include <Pyxis/Platform/Logging/LogCategories.h>

#include <nvrhi/nvrhi.h>

#include <algorithm>
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
    nvrhi::IDevice*                       device     = nullptr;
    int64_t                               frameStart = 0;
    uint64_t                              frameIndex = 0;
    std::vector<FrameProfile::PassTiming> currentFrame;
    std::vector<FrameProfile::PassTiming> lastFrame;
    double                                cpuFrameMs = 0.0;
    double                                gpuFrameMs = 0.0;
    uint32_t                              depth      = 0;
};

Profiler::Profiler(nvrhi::IDevice* device) : _impl(new Impl()) {
    _impl->device = device;
}

Profiler::~Profiler() { delete _impl; }

void Profiler::BeginFrame() {
    _impl->frameStart = NowNs();
    _impl->currentFrame.clear();
    _impl->depth = 0;
}

void Profiler::EndFrame() {
    const int64_t now = NowNs();
    _impl->cpuFrameMs = static_cast<double>(now - _impl->frameStart) / 1.0e6;
    _impl->lastFrame  = _impl->currentFrame;
    ++_impl->frameIndex;
}

FrameProfile Profiler::LastFrameProfile() const {
    FrameProfile p{};
    p.passes     = std::span<const FrameProfile::PassTiming>(_impl->lastFrame);
    p.cpuFrameMs = _impl->cpuFrameMs;
    p.gpuFrameMs = _impl->gpuFrameMs;
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
    FrameProfile::PassTiming t{};
    t.name        = _name;
    t.kind        = FrameProfile::ScopeKind::Cpu;
    t.durationMs  = static_cast<double>(end - _startNs) / 1.0e6;
    t.depth       = _profiler->_impl->depth;
    _profiler->_impl->currentFrame.push_back(t);
}

// ---------------------------------------------------------------------------
// GpuScope — M1 wraps the NVRHI debug marker; GPU-timestamp resolution
// lands at M11.
// ---------------------------------------------------------------------------

Profiler::GpuScope::GpuScope(Profiler& profiler, nvrhi::ICommandList* cl, std::string_view name)
    : _profiler(&profiler), _commandList(cl), _name(MakeScopeName(name)) {
    if (_commandList) _commandList->beginMarker(_name.data.data());
}

Profiler::GpuScope::~GpuScope() {
    if (_commandList) _commandList->endMarker();
    if (!_profiler) return;
    FrameProfile::PassTiming t{};
    t.name = _name;
    t.kind = FrameProfile::ScopeKind::Gpu;
    t.durationMs = 0.0;
    t.depth      = _profiler->_impl->depth;
    _profiler->_impl->currentFrame.push_back(t);
}

}  // namespace pyxis
