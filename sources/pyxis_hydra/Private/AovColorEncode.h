// Pyxis Hydra — host color-AOV encode policy (the Omniverse-viewer ⇄ Pyxis-engine
// color-parity contract).
//
// PyxisEngine exports the ACES-tonemapped color in LINEAR display space (range
// [0,1], no OETF — raygen writes acesFilmic() with no sRGB). Every path that ends
// at a display must apply the sRGB OETF EXACTLY ONCE:
//   * pyxis.exe headless PNG  : WritePng applies sRGB.
//   * standalone viewer       : sRGB-format swapchain applies sRGB on present.
//   * WorldLobbyHeadless BMP   : WriteBmp applies sRGB (reads ReadbackColorHdr).
//   * Omniverse Kit pxr viewport / usdview : HdxColorCorrectionTask applies sRGB
//     to the color AOV on present (empirically the viewport's transfer function is
//     exactly the sRGB OETF — verified by per-pixel fit, commit bb94133).
//
// THE INVARIANT (§25.O.3): by default the Omniverse-viewer image must equal the
// Pyxis-engine (headless/viewer) image. Because the host AOV consumer already
// applies sRGB, the delegate must write the color AOV LINEAR (verbatim readback) —
// pre-encoding sRGB here would double-apply it (washed-out / too bright). Then:
//     viewer_display = sRGB(EncodeAovColorChannel(L, c, /*encodeSrgb=*/false))
//                    = sRGB(L)
//                    = headless_PNG = standalone_display.        ← equal by construction
//
// PYXIS_OMNI_AOV_SRGB=1 flips `encodeSrgb` on for a hypothetical host that presents
// the AOV WITHOUT any color correction (then the delegate must pre-encode). That is
// NOT the default and breaks parity with the engine on a color-correcting host.
//
// This policy lives in a header-only inline so pyxis_unit_tests can pin it directly
// (it is the single source of truth for HdPyxisRenderDelegate's WritePyxisColorToAov).

#pragma once

#include <Pyxis/Platform/Color/ColorEncoding.h>

namespace pyxis::hydra {

// Map one channel of the LINEAR readback into the value written to the host color
// AOV. Alpha (channel 3) is always passed through clamped-linear. RGB (0..2) is
// clamped-linear by default (the host applies the sRGB OETF), or sRGB-encoded when
// `encodeSrgb` (a verbatim-present host). Input/output are in [0,1] after clamping.
[[nodiscard]] inline float EncodeAovColorChannel(float linear, int channel,
                                                 bool encodeSrgb) noexcept {
  if (channel == 3 || !encodeSrgb)
    return linear < 0.0f ? 0.0f : (linear > 1.0f ? 1.0f : linear);
  return pyxis::color::LinearToSrgb(linear);
}

}  // namespace pyxis::hydra
