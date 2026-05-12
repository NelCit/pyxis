#!/usr/bin/env python3
"""Unit tests for usd_loading_audit.classify + audit_text.

Run directly: `python test_usd_loading_audit.py`. Returns exit 0 on
success, 1 on any test failure. Kept stdlib-only (no pytest needed
on dev machines).
"""

from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
import usd_loading_audit as ula  # noqa: E402  (path append before import)


def assert_eq(label: str, actual, expected) -> bool:
    if actual == expected:
        print(f"  OK   {label}")
        return True
    print(f"  FAIL {label}: expected {expected!r}, got {actual!r}")
    return False


def main() -> int:
    ok = True

    # classify() routes known types.
    ok &= assert_eq("classify Mesh",        ula.classify("Mesh"),        "LOADED")
    ok &= assert_eq("classify Sphere",      ula.classify("Sphere"),      "LOADED")
    ok &= assert_eq("classify Volume",      ula.classify("Volume"),      "STUB")
    ok &= assert_eq("classify NurbsPatch",  ula.classify("NurbsPatch"),  "STUB")
    ok &= assert_eq("classify SkelRoot",    ula.classify("SkelRoot"),    "STUB")
    ok &= assert_eq("classify Camera",      ula.classify("Camera"),      "LOADED")
    ok &= assert_eq("classify DistantLight",ula.classify("DistantLight"),"LOADED")
    ok &= assert_eq("classify RenderSettings", ula.classify("RenderSettings"), "STUB")

    # Unknown / future-USD types fall to UNHANDLED.
    ok &= assert_eq("classify Plane",       ula.classify("Plane"),       "UNHANDLED")
    ok &= assert_eq("classify MadeUpType",  ula.classify("MadeUpType"),  "UNHANDLED")

    # audit_text() counts `def <Type>` occurrences.
    sample = """#usda 1.0
def Xform "World"
{
    def Mesh "A" { }
    def Mesh "B" { }
    def Sphere "Ball" { }
    def Volume "Fog" { }
    def Plane "P" { }
}
"""
    counts = ula.audit_text(sample)
    ok &= assert_eq("count Xform",  counts["Xform"],  1)
    ok &= assert_eq("count Mesh",   counts["Mesh"],   2)
    ok &= assert_eq("count Sphere", counts["Sphere"], 1)
    ok &= assert_eq("count Volume", counts["Volume"], 1)
    ok &= assert_eq("count Plane",  counts["Plane"],  1)

    # `class` and `over` are NOT `def`; should not be counted.
    not_def = """#usda 1.0
class Mesh "Template" { }
over "Existing" { }
"""
    not_def_counts = ula.audit_text(not_def)
    ok &= assert_eq("class not counted", not_def_counts.get("Mesh", 0), 0)

    print("\nALL PASSED" if ok else "\nSOMETHING FAILED")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
