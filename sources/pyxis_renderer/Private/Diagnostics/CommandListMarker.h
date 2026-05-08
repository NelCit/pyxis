// Pyxis renderer — CommandListMarker.
//
// Plan §33.6 / §33.9: every pass calls cl->beginMarker / endMarker so
// RenderDoc / Aftermath captures localise the failure. Used both
// directly and as the foundation of Profiler::GpuScope.

#pragma once

#include <nvrhi/nvrhi.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <string_view>

namespace pyxis {

class CommandListMarker final {
public:
    CommandListMarker(nvrhi::ICommandList* cl, std::string_view name) noexcept
        : _cl(cl) {
        if (_cl && !name.empty()) {
            // beginMarker takes const char*; std::string_view::data() is
            // not guaranteed null-terminated, so we materialise a small
            // bounded copy.
            const std::size_t n = std::min(name.size(), _name.size() - 1);
            std::memcpy(_name.data(), name.data(), n);
            _name[n] = '\0';
            _cl->beginMarker(_name.data());
        }
    }
    ~CommandListMarker() noexcept {
        if (_cl) _cl->endMarker();
    }
    CommandListMarker(const CommandListMarker&)            = delete;
    CommandListMarker& operator=(const CommandListMarker&) = delete;

private:
    nvrhi::ICommandList*  _cl = nullptr;
    std::array<char, 64>  _name{};
};

}  // namespace pyxis
