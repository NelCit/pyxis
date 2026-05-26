#!/usr/bin/env python3
# Pyxis display-parity checker -- the "Omniverse viewer == Pyxis engine viewer"
# invariant.
#
# Asserts that the SAME World Lobby (same camera/res) renders identically across
# the THREE Pyxis display paths:
#   * headless : pyxis.exe --headless --output PNG   (sRGB in the PNG writer)
#   * viewer   : pyxis.exe --screenshot PNG          (sRGB in SsaaResolvePass present)
#   * kit      : Omniverse Kit pxr viewport capture  (sRGB in HdxColorCorrectionTask)
#
# Each path applies the sRGB OETF to the ACES-tonemapped LINEAR color exactly once,
# so the displayed pixels must match. This is the regression guard for the color
# double-encode bug (Kit was 2x sRGB; the viewer was 0x). See AovColorEncodeFixture
# for the CPU-level proof of the same invariant.
#
#   python display_parity_check.py <headless.png> <viewer.png> <kit.png>
#
# Hard gate: pairwise mean-RGB within tol (PYXIS_DISPLAY_PARITY_TOL, default 4.0).
# Soft gate: coarse luma correlation > 0.9 (orientation-aware -- the Kit delegate
# presents the color AOV vertically flipped vs the standalone). Exit 0 = parity.

import math
import os
import sys


def load_rgb(path):
    from PIL import Image
    img = Image.open(path).convert("RGB")
    return img


def _flat(img):
    # Pillow >=11 renamed getdata() -> get_flattened_data(); support both.
    fn = getattr(img, "get_flattened_data", None)
    return list(fn()) if fn else list(img.getdata())


def pixels(img):
    return _flat(img)


def mean_rgb(px):
    n = len(px)
    return tuple(sum(p[c] for p in px) / n for c in range(3))


def coarse_luma(img, fac=16):
    w, h = max(1, img.size[0] // fac), max(1, img.size[1] // fac)
    small = img.resize((w, h))
    return [0.299 * p[0] + 0.587 * p[1] + 0.114 * p[2] for p in _flat(small)], w, h


def correlation(la, lb):
    n = min(len(la), len(lb))
    if n == 0:
        return 0.0
    la, lb = la[:n], lb[:n]
    ma, mb = sum(la) / n, sum(lb) / n
    cov = sum((la[i] - ma) * (lb[i] - mb) for i in range(n)) / n
    sa = math.sqrt(sum((x - ma) ** 2 for x in la) / n)
    sb = math.sqrt(sum((x - mb) ** 2 for x in lb) / n)
    return cov / (sa * sb) if sa * sb > 0 else 0.0


def vflip_luma(luma, w, h):
    out = [0.0] * len(luma)
    for row in range(h):
        for col in range(w):
            out[(h - 1 - row) * w + col] = luma[row * w + col]
    return out


def best_corr(img_a, img_b):
    """Coarse luma correlation, best of direct vs vertical-flip (the Kit delegate
    presents the AOV bottom-up vs the standalone top-down)."""
    la, wa, ha = coarse_luma(img_a)
    lb, wb, hb = coarse_luma(img_b)
    if (wa, ha) != (wb, hb):
        return correlation(la, lb)  # size mismatch -- direct only
    direct = correlation(la, lb)
    flipped = correlation(la, vflip_luma(lb, wb, hb))
    return max(direct, flipped)


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

    imgs, means = {}, {}
    for label, path in zip(labels, paths):
        if not os.path.isfile(path):
            print(f"[display-parity] FAIL: missing {label} image: {path}")
            return 1
        img = load_rgb(path)
        imgs[label] = img
        means[label] = mean_rgb(pixels(img))
        print(f"[display-parity] {label:9s} {img.size[0]}x{img.size[1]} "
              f"meanRGB=({means[label][0]:.1f}, {means[label][1]:.1f}, {means[label][2]:.1f})")

    tol = float(os.environ.get("PYXIS_DISPLAY_PARITY_TOL", "4.0"))
    corr_tol = float(os.environ.get("PYXIS_DISPLAY_PARITY_CORR", "0.9"))
    ok = True
    pairs = [("headless", "viewer"), ("headless", "kit"), ("viewer", "kit")]
    for left, right in pairs:
        dmean = max(abs(means[left][c] - means[right][c]) for c in range(3))
        corr = best_corr(imgs[left], imgs[right])
        status = "OK" if (dmean <= tol and corr >= corr_tol) else "FAIL"
        print(f"[display-parity] {left} vs {right}: max-abs-mean-diff={dmean:.2f} (tol {tol})  "
              f"coarse-luma-corr={corr:.4f} (tol {corr_tol})  {status}")
        if dmean > tol:
            print(f"[display-parity]   FAIL: {left} vs {right} mean diverged "
                  f"({dmean:.2f} > {tol}) -- a display path applies the wrong number "
                  f"of sRGB encodes (color double/zero-encode regression).")
            ok = False
        if corr < corr_tol:
            print(f"[display-parity]   FAIL: {left} vs {right} structure diverged "
                  f"(corr {corr:.3f} < {corr_tol}) -- not the same scene/camera.")
            ok = False

    print("\n==== DISPLAY-PARITY SUMMARY ====")
    if ok:
        print("PASS: viewer == headless == Kit/Omniverse (one sRGB encode on each path).")
    else:
        print("FAIL: the three Pyxis display paths diverged.")
    print("================================")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
