// Pyxis platform — input-event POD.
// M1 ships the minimum: window-close, resize, keyboard / mouse passthroughs.
// Per plan §29.2 the full keyboard-navigation contract lands later.

#pragma once

#include <cstdint>

namespace pyxis {

enum class InputEventKind : uint32_t {
    None         = 0,
    WindowClose  = 1,
    WindowResize = 2,
    KeyDown      = 3,
    KeyUp        = 4,
    MouseMove    = 5,
    MouseButton  = 6,
    MouseScroll  = 7,
};

struct InputEvent {
    InputEventKind kind = InputEventKind::None;

    // WindowResize: new client-area size (backbuffer pixels — §5).
    uint32_t width  = 0;
    uint32_t height = 0;

    // KeyDown/KeyUp: GLFW key code; mods is the modifier bitmask (Ctrl/Shift/Alt).
    int32_t  key     = 0;
    uint32_t mods    = 0;

    // MouseMove: cursor in window pixels.
    // MouseButton: button index, with `key` set to GLFW button.
    // MouseScroll: scroll dx/dy in dy = scroll-wheel notches.
    double   mouseX  = 0.0;
    double   mouseY  = 0.0;
    double   scrollDx = 0.0;
    double   scrollDy = 0.0;
};

}  // namespace pyxis
