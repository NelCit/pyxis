#!/usr/bin/env python3
# Pyxis display-parity checker -- the "Omniverse viewer == Pyxis engine viewer"
# invariant.
#
# Asserts that the SAME World Lobby (same camera/res) renders consistently across
# the THREE Pyxis display paths:
#   * headless : pyxis.exe --headless --output PNG   (sRGB in the PNG writer)
#   * viewer   : pyxis.exe --screenshot PNG          (sRGB in the BlitToSrgb present)
#   * kit      : Omniverse Kit pxr viewport capture  (sRGB in HdxColorCorrectionTask)
#
# The gate is PER PIXEL, not mean/correlation -- a loose mean+correlation gate hides
# real bugs (a vertically-flipped Kit AOV has an identical mean and ~1.0 coarse
# correlation yet is visibly upside-down). Two regimes, because the paths are not
# equally controllable:
#
# The gate uses two ROBUST statistics per pair -- the MEDIAN and the MEAN of the
# per-channel absolute differences -- not a per-pixel max or a within-tolerance
# percentage. The median says "does the typical pixel match"; the mean catches a
# broad systematic shift. (A max/percentile gate is dominated by a thin tail of
# genuinely-different pixels -- see the Kit note below -- so it is the wrong tool.)
#
#   STANDALONE pair (headless vs viewer): same deterministic engine + our own sRGB
#   encoders, so they agree TIGHTLY -- median <= STRICT_MED (0) AND mean <= STRICT_MEAN
#   (1). MEASURED: median 0, mean 0.18 (a few --screenshot-vs-headless edge pixels lift
#   the mean but not the median).
#
#   KIT pairs (vs the live Omniverse viewport): MEASURED FACTS (RFC 0008 bring-up) --
#   Kit's color management is the standard sRGB OETF, matching ours to ~0.5 LSB
#   (no-render linear-ramp capture), and the geometry is pixel-exact (a shift search
#   bottoms out at 0,0). The TYPICAL Kit pixel matches headless within 1 LSB
#   (median = 1, p95 = 5); a thin ~1% tail in the bright windows spikes to 50-250
#   because those high-variance highlights are TWO INDEPENDENT renders (fireflies),
#   not a Pyxis bug. So the Kit gate is median <= KIT_MED (2) AND mean <= KIT_MEAN (6),
#   which the correct capture passes (median 1, mean 2.8) and every real regression
#   fails: a vertical flip -> median/mean ~70; a double sRGB encode -> midtones shift
#   tens of LSB; a 1-pixel misalignment -> mean ~17. No loose per-pixel tolerance.
#
#   python display_parity_check.py <headless.png> <viewer.png> <kit.png>
#
# Env overrides: PYXIS_DISPLAY_PARITY_{STRICT_MED,STRICT_MEAN,KIT_MED,KIT_MEAN}.
# Exit 0 = parity.

import os
import sys


def load_rgb(path):
    from PIL import Image
    return Image.open(path).convert("RGB")


def _flat(img):
    # Pillow >=11 renamed getdata() -> get_flattened_data(); support both.
    fn = getattr(img, "get_flattened_data", None)
    return list(fn()) if fn else list(img.getdata())


def mean_rgb(px):
    n = len(px)
    return tuple(sum(p[c] for p in px) / n for c in range(3))


def diff_stats(px_a, px_b):
    """Per-channel abs-diff over two equal-length pixel lists. Returns
    (median, mean, p95, max) of the per-channel absolute differences."""
    n = min(len(px_a), len(px_b))
    hist = [0] * 256
    total = 0
    max_diff = 0
    for i in range(n):
        pa, pb = px_a[i], px_b[i]
        for c in range(3):
            d = abs(pa[c] - pb[c])
            hist[d] += 1
            total += d
            if d > max_diff:
                max_diff = d
    channels = n * 3
    if channels == 0:
        return 0, 0.0, 0, 0

    def percentile(p):
        target = p * channels
        cum = 0
        for value, count in enumerate(hist):
            cum += count
            if cum >= target:
                return value
        return 255

    return percentile(0.50), total / channels, percentile(0.95), max_diff


def _envf(name, default):
    return float(os.environ.get("PYXIS_DISPLAY_PARITY_" + name, default))


def main():
    if len(sys.argv) != 4:
        print(__doc__)
        return 2
    labels = ["headless", "viewer", "kit"]
    paths = sys.argv[1:4]
    try:
        from PIL import Image  # noqa: F401
    except ImportError:
        print("[display-parity] PIL not available -- cannot compare; install Pillow.")
        return 3

    strict_med = _envf("STRICT_MED", 0)
    strict_mean = _envf("STRICT_MEAN", 1.0)
    kit_med = _envf("KIT_MED", 2)
    kit_mean = _envf("KIT_MEAN", 6.0)

    imgs, px, sizes = {}, {}, {}
    for label, path in zip(labels, paths):
        if not os.path.isfile(path):
            print(f"[display-parity] FAIL: missing {label} image: {path}")
            return 1
        img = load_rgb(path)
        imgs[label] = img
        px[label] = _flat(img)
        sizes[label] = img.size
        mr = mean_rgb(px[label])
        print(f"[display-parity] {label:9s} {img.size[0]}x{img.size[1]} "
              f"meanRGB=({mr[0]:.1f}, {mr[1]:.1f}, {mr[2]:.1f})")

    ok = True
    pairs = [("headless", "viewer"), ("headless", "kit"), ("viewer", "kit")]
    for left, right in pairs:
        if sizes[left] != sizes[right]:
            print(f"[display-parity]   FAIL: {left} {sizes[left]} vs {right} {sizes[right]} "
                  f"-- dimensions differ; cannot compare per pixel.")
            ok = False
            continue
        is_kit = "kit" in (left, right)
        med_req = kit_med if is_kit else strict_med
        mean_req = kit_mean if is_kit else strict_mean

        median, mean_diff, p95, max_diff = diff_stats(px[left], px[right])
        passed = (median <= med_req) and (mean_diff <= mean_req)
        status = "OK" if passed else "FAIL"
        regime = "kit" if is_kit else "strict"
        print(f"[display-parity] {left} vs {right} [{regime}]: median={median} (<= {med_req:g})  "
              f"mean={mean_diff:.3f} (<= {mean_req:g})  p95={p95}  max={max_diff}  {status}")
        if not passed:
            print(f"[display-parity]   FAIL: {left} vs {right} diverged -- a display path "
                  f"mis-encodes sRGB (double/zero), renders a different image, or presents "
                  f"the AOV flipped/misaligned.")
            ok = False

    print("\n==== DISPLAY-PARITY SUMMARY ====")
    if ok:
        print("PASS: headless == viewer (strict) and Kit within capture tolerance "
              "(one sRGB encode on each path, no flip).")
    else:
        print("FAIL: the Pyxis display paths diverged.")
    print("================================")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
