// Pyxis renderer — Error / PYXIS_ERROR implementation.

#include <Pyxis/Renderer/Error.h>

#include <cstdarg>
#include <cstdio>

namespace pyxis::detail {

namespace {

// Fill in `msg.size` from a vsnprintf return value. If the formatted
// length would exceed CAPACITY, set the visible end to "..." so the
// truncation is obvious in logs / crash reports rather than leaving a
// silently cut-off sentence.
void FinaliseMessage(ErrorMessage& msg, int written) noexcept {
    if (written < 0) {
        // vsnprintf returns negative on encoding error; emit nothing.
        msg.size = 0;
        msg.data[0] = '\0';
        return;
    }

    const auto want = static_cast<std::size_t>(written);
    if (want < ErrorMessage::CAPACITY) {
        msg.size = static_cast<uint16_t>(want);
        return;
    }

    // Truncated. vsnprintf already wrote CAPACITY-1 bytes + null. Stamp
    // the last three visible bytes with "..." so callers see truncation.
    static_assert(ErrorMessage::CAPACITY >= 4,
                  "ErrorMessage::CAPACITY must be >= 4 for the truncation marker.");
    msg.size = static_cast<uint16_t>(ErrorMessage::CAPACITY - 1);
    msg.data[msg.size - 3] = '.';
    msg.data[msg.size - 2] = '.';
    msg.data[msg.size - 1] = '.';
}

}  // namespace

Error MakeError(ErrorKind   kind,
                const char* file,
                int         line,
                const char* fmt,
                ...) noexcept {
    Error err;
    err.kind = kind;

    // Caller-supplied human-readable message.
    {
        va_list args;
        va_start(args, fmt);
        const int written = std::vsnprintf(err.message.data.data(),
                                           ErrorMessage::CAPACITY,
                                           fmt != nullptr ? fmt : "",
                                           args);
        va_end(args);
        FinaliseMessage(err.message, written);
    }

    // "file:line" source. `__FILE__` is mapped to a repo-relative path
    // by Compiler.cmake's `-fmacro-prefix-map=<repo>=.`, so this comes
    // out as e.g. "./sources/pyxis_renderer/Private/.../Foo.cpp:123".
    {
        const int written = std::snprintf(err.source.data.data(),
                                          ErrorMessage::CAPACITY,
                                          "%s:%d",
                                          file != nullptr ? file : "<unknown>",
                                          line);
        FinaliseMessage(err.source, written);
    }

    return err;
}

}  // namespace pyxis::detail
