// Pyxis platform — NVRHI message-callback adapter.

#include "Device/NvrhiCallback.h"

#include <Pyxis/Platform/Logging/Log.h>
#include <Pyxis/Platform/Logging/LogCategories.h>

#include <string_view>

namespace pyxis {

namespace {

class NvrhiMessageCallback final : public nvrhi::IMessageCallback {
public:
    void message(nvrhi::MessageSeverity severity, const char* messageText) override {
        auto& log = Logging::Get();
        const std::string_view msg{ messageText ? messageText : "" };
        switch (severity) {
            case nvrhi::MessageSeverity::Info:    log.Info    (log::PLATFORM, msg); break;
            case nvrhi::MessageSeverity::Warning: log.Warn    (log::PLATFORM, msg); break;
            case nvrhi::MessageSeverity::Error:   log.Error   (log::PLATFORM, msg); break;
            case nvrhi::MessageSeverity::Fatal:   log.Critical(log::PLATFORM, msg); break;
        }
    }
};

}  // namespace

nvrhi::IMessageCallback& NvrhiCallback() noexcept {
    static NvrhiMessageCallback callback;
    return callback;
}

}  // namespace pyxis
