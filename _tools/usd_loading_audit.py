#!/usr/bin/env python3
"""Pyxis USD loading audit (M28 / V2.A.28).

Walks a .usda or .usd text file, counts each authored `def <Type>`,
and classifies the type against Pyxis's M12..M27 coverage:

    LOADED   — Pyxis ingests this prim type today (renders or affects
               the scene as the USD spec requires).
    STUB     — Pyxis detects the prim type and logs a stub warning;
               the prim does not contribute to the render. Future
               milestones will close these gaps.
    UNHANDLED — Pyxis ignores this prim type silently. A non-empty
                UNHANDLED list is the audit's failure signal: it
                means production scenes can author content Pyxis
                doesn't even know about.

Output: one row per prim type with [classification | count | type]
plus a verdict. Exit 0 if every authored type is LOADED or STUB;
exit 1 if any UNHANDLED rows.

Caveats:
- Pure-text regex; no pxr Python bindings (Pyxis ships USD with
  Python disabled per plan §6). Therefore `def` lines inside a
  variant block or a comment are still counted. Good enough as a
  coverage signal; the day pxr Python bindings ship in a v2 plugin
  the script swaps to `pxr.Usd.Stage.Open(...)`.
- Only top-level `def <Type>` is parsed. Schema-class lines like
  `class <Type>` or `over <Type>` are excluded — they're authoring
  hints, not renderable prims.

Usage:
    python usd_loading_audit.py path/to/scene.usda [--quiet]
    python usd_loading_audit.py path/to/scene.usda --fail-on-stub
"""

from __future__ import annotations

import argparse
import re
import sys
from collections import Counter
from pathlib import Path
from typing import Iterable

# Prim types Pyxis ingests as renderable today (M12..M21).
LOADED_TYPES: set[str] = {
    # Geometry that produces MeshDesc:
    "Mesh",
    "Sphere",
    "Cube",
    "Cylinder",
    "Cone",
    "Capsule",
    "BasisCurves",
    "Points",
    "PointInstancer",
    # Materials + shaders (shaders are children of Materials but
    # surface as their own `def Shader`):
    "Material",
    "Shader",
    "NodeGraph",
    # Lights:
    "DistantLight",
    "DomeLight",
    "RectLight",
    "DiskLight",
    "SphereLight",
    "CylinderLight",
    "GeometryLight",
    "PortalLight",
    # Cameras + xforms + scopes (organisational):
    "Camera",
    "Xform",
    "Scope",
    # Subsets (face partitioning for material binding):
    "GeomSubset",
}

# Prim types Pyxis detects + warns + skips (M15, M20, M21 stubs).
STUB_TYPES: set[str] = {
    # M15: Volumes
    "Volume",
    "OpenVDBAsset",
    "Field3DAsset",
    # M20: NURBS + Skel
    "NurbsPatch",
    "NurbsCurves",
    "SkelRoot",
    "Skeleton",
    "SkelAnimation",
    "BlendShape",
    # M21: UsdRender
    "RenderSettings",
    "RenderProduct",
    "RenderVar",
    "RenderPass",
}

DEF_RE = re.compile(
    r'^\s*def\s+(?P<type>\w+)\s+"',
    re.MULTILINE,
)


def classify(prim_type: str) -> str:
    """Return one of LOADED / STUB / UNHANDLED."""
    if prim_type in LOADED_TYPES:
        return "LOADED"
    if prim_type in STUB_TYPES:
        return "STUB"
    return "UNHANDLED"


def audit_text(text: str) -> Counter[str]:
    """Return a Counter mapping prim-type-name → occurrence count."""
    return Counter(match.group("type") for match in DEF_RE.finditer(text))


def format_report(counts: Counter[str], stream) -> dict[str, list[str]]:
    """Print the report; return the per-classification type lists."""
    by_class: dict[str, list[str]] = {"LOADED": [], "STUB": [], "UNHANDLED": []}
    for prim_type in sorted(counts):
        klass = classify(prim_type)
        by_class[klass].append(prim_type)
    stream.write(f"{'CLASS':<10} {'COUNT':>6}  TYPE\n")
    stream.write(f"{'-' * 10} {'-' * 6}  {'-' * 32}\n")
    for klass in ("LOADED", "STUB", "UNHANDLED"):
        for prim_type in by_class[klass]:
            stream.write(f"{klass:<10} {counts[prim_type]:>6}  {prim_type}\n")
    return by_class


def main(argv: Iterable[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("scene", type=Path,
                        help="Path to a .usda / .usd file.")
    parser.add_argument("--fail-on-stub", action="store_true",
                        help="Exit 1 even when only STUB rows are present "
                             "(tighter coverage gate for nightly CI).")
    parser.add_argument("--quiet", action="store_true",
                        help="Only print the verdict line.")
    args = parser.parse_args(list(argv))

    if not args.scene.exists():
        sys.stderr.write(f"audit: scene not found: {args.scene}\n")
        return 2
    if args.scene.suffix.lower() not in (".usda", ".usd"):
        sys.stderr.write(
            f"audit: only .usda / .usd text-format scenes are audited "
            f"(got {args.scene.suffix}). Convert with usdcat first.\n"
        )
        return 2

    text = args.scene.read_text(encoding="utf-8", errors="replace")
    counts = audit_text(text)

    if not args.quiet:
        by_class = format_report(counts, sys.stdout)
    else:
        by_class = {"LOADED": [], "STUB": [], "UNHANDLED": []}
        for prim_type in counts:
            by_class[classify(prim_type)].append(prim_type)

    total = sum(counts.values())
    unhandled_count = sum(counts[name] for name in by_class["UNHANDLED"])
    stub_count = sum(counts[name] for name in by_class["STUB"])

    verdict = "OK"
    rc = 0
    if unhandled_count > 0:
        verdict = f"FAIL ({unhandled_count} unhandled prim(s))"
        rc = 1
    elif args.fail_on_stub and stub_count > 0:
        verdict = f"FAIL --fail-on-stub ({stub_count} stub prim(s))"
        rc = 1

    sys.stdout.write(
        f"\nVERDICT: {verdict}  "
        f"(total={total} loaded={total - unhandled_count - stub_count} "
        f"stub={stub_count} unhandled={unhandled_count})\n"
    )
    return rc


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
