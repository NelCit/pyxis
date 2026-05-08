// Pyxis renderer — RenderSettings POD (M1 subset).
//
// Plan §18.4 / §21. M1 ships the bare-minimum knobs the TrianglePass and
// the eventual app FPS-panel need; the full path-trace knob set
// (samplesPerFrame, maxBounces, RR, firefly clamp, low-discrepancy
// sampling, tone-map operator, debug view, AOV mask) is wired
// progressively starting at M3.

#pragma once

#include <cstdint>

namespace pyxis {

struct RenderSettings {
    uint32_t width  = 1920;
    uint32_t height = 1080;

    // Backbuffer clear before any pass. Linear sRGB (§25.I.2 color
    // pipeline). M1 uses this to verify the swapchain present path
    // before any draw lands; a known non-black colour also makes
    // "did the swapchain even resize?" obvious in the viewer.
    float clearColor[4] = { 0.05f, 0.05f, 0.06f, 1.0f };

    // §29.5 feature mask seed. M1 only honours `triangle`; the rest
    // of the §29.5 toggle table fills in at M3+.
    struct Features {
        bool triangle      = true;     // M1 hard-coded TrianglePass
        bool imguiOverlay  = true;     // PYXIS_DEBUG_TOOLS-gated
    } features;
};

}  // namespace pyxis
