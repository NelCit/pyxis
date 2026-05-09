#!/usr/bin/env python3
"""Pyxis — bootstrap generator for resources/scenes/default_sky.exr.

Plan §29.4.a: a tiny 64×32 lat-long EXR shipped alongside default.usd
that the bundled `UsdLuxDomeLight` references for indirect lighting +
a recognisable horizon. Procedural overcast-sky gradient — desaturated
warm horizon, cool blue-grey zenith, dark grey-green nadir hemisphere.

Run once from the repo root (the script is committed for
reproducibility, the output binary is committed too):

    py _tools/gen_default_sky.py

Output: resources/scenes/default_sky.exr (RGB half-float, ZIP scanline).
At M3.5 this file is just bytes that ship; M5+/M7+ when DomeLight
ingest goes online, the values feed the importance-sampling tables.
"""

from __future__ import annotations

import math
import sys
from pathlib import Path

import numpy as np
import OpenEXR
import Imath


WIDTH = 64
HEIGHT = 32
OUTPUT = Path(__file__).resolve().parent.parent / "resources" / "scenes" / "default_sky.exr"


def lerp(a: float, b: float, t: float) -> float:
    return a + (b - a) * t


def smoothstep(edge0: float, edge1: float, x: float) -> float:
    t = max(0.0, min(1.0, (x - edge0) / (edge1 - edge0)))
    return t * t * (3.0 - 2.0 * t)


def overcast_sky(latitude_radians: float) -> tuple[float, float, float]:
    # latitude: 0 at north pole / zenith, pi/2 at horizon, pi at nadir.
    # Top hemisphere — sky. Bottom hemisphere — ground reflection
    # (a low-intensity grey-green so the dome contributes some ambient
    # bounce from below for grounded subjects).
    if latitude_radians < math.pi / 2:
        # Sky: zenith (0) -> horizon (pi/2). Smoothstep so the horizon
        # band reads as a soft transition rather than a hard edge.
        t = smoothstep(0.0, 1.0, latitude_radians / (math.pi / 2))
        zenith = (0.62, 0.70, 0.82)   # cool overcast blue-grey
        horizon = (0.55, 0.50, 0.43)  # warm desaturated grey
        return (
            lerp(zenith[0], horizon[0], t),
            lerp(zenith[1], horizon[1], t),
            lerp(zenith[2], horizon[2], t),
        )
    else:
        # Ground reflection: horizon (pi/2) -> nadir (pi).
        t = (latitude_radians - math.pi / 2) / (math.pi / 2)
        horizon_low = (0.30, 0.28, 0.22)
        nadir = (0.10, 0.11, 0.09)
        return (
            lerp(horizon_low[0], nadir[0], t),
            lerp(horizon_low[1], nadir[1], t),
            lerp(horizon_low[2], nadir[2], t),
        )


def build_pixels() -> np.ndarray:
    pixels = np.empty((HEIGHT, WIDTH, 3), dtype=np.float16)
    for y in range(HEIGHT):
        # +0.5 to sample pixel centres, not edges.
        latitude = (y + 0.5) / HEIGHT * math.pi
        for x in range(WIDTH):
            r, g, b = overcast_sky(latitude)
            pixels[y, x, 0] = r
            pixels[y, x, 1] = g
            pixels[y, x, 2] = b
    return pixels


def write_exr(path: Path, pixels: np.ndarray) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)

    # OpenEXR 3.x Python binding accepts a numpy array directly; we use
    # the legacy header/channel API so the output matches what M5+'s
    # tinyexr-driven loader expects (RGB half scanline + ZIP
    # compression).
    header = OpenEXR.Header(WIDTH, HEIGHT)
    half = Imath.Channel(Imath.PixelType(Imath.PixelType.HALF))
    header["channels"] = {"R": half, "G": half, "B": half}
    header["compression"] = Imath.Compression(Imath.Compression.ZIP_COMPRESSION)

    out = OpenEXR.OutputFile(str(path), header)
    out.writePixels({
        "R": pixels[:, :, 0].tobytes(),
        "G": pixels[:, :, 1].tobytes(),
        "B": pixels[:, :, 2].tobytes(),
    })
    out.close()


def main() -> int:
    pixels = build_pixels()
    write_exr(OUTPUT, pixels)
    size = OUTPUT.stat().st_size
    print(f"Wrote {OUTPUT.relative_to(OUTPUT.parents[2])} ({size} bytes, {WIDTH}x{HEIGHT} RGB half ZIP)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
