// Pyxis renderer — CommandListMarker.
//
// Plan §33.6 / §33.9: every pass calls commandList->beginMarker / endMarker
// so RenderDoc / Aftermath captures localise the failure. Used both
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
    CommandListMarker(nvrhi::ICommandList* commandList, std::string_view name) noexcept
        : _commandList(commandList) {
        if (_commandList && !name.empty()) {
            // beginMarker takes const char*; std::string_view::data() is
            // not guaranteed null-terminated, so we materialise a small
            // bounded copy.
            const std::size_t copyLen = std::min(name.size(), _name.size() - 1);
            std::memcpy(_name.data(), name.data(), copyLen);
            _name[copyLen] = '\0';
            _commandList->beginMarker(_name.data());
        }
    }
    ~CommandListMarker() noexcept {
        if (_commandList) _commandList->endMarker();
    }
    CommandListMarker(const CommandListMarker&)            = delete;
    CommandListMarker& operator=(const CommandListMarker&) = delete;

private:
    nvrhi::ICommandList*  _commandList = nullptr;
    std::array<char, 64>  _name{};
};

}  // namespace pyxis
