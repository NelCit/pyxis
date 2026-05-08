// Pyxis platform — window-creation descriptor (POD). Plan §5 / §28.

#pragma once

#include <cstdint>
#include <string_view>

namespace pyxis {

struct WindowDesc {
    uint32_t         width  = 1920;
    uint32_t         height = 1080;
    std::string_view title  = "Pyxis";

    // Per-monitor DPI awareness is enabled application-wide via the app
    // manifest (§5). DIPs vs backbuffer pixels: width/height above are
    // backbuffer pixels; ImGui scales by monitor DPI.
    bool             resizable = true;

    // §28 frame-pacing knobs surface through RenderSettings later; the
    // window itself is just a surface owner.
};

}  // namespace pyxis
