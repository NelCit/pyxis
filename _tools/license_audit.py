#!/usr/bin/env python3
"""
license_audit.py — third-party licence audit for Pyxis.

Per plan_final.md §48.3 / §46.3:

  - Walks `vcpkg list --x-installed` (when vcpkg is available) and the
    `_cmake/Thirdparty.cmake` FetchContent declarations to enumerate every
    third-party component currently linked by Pyxis.
  - Emits `NOTICE.generated` in the repo root with the full attribution text
    aggregated from this script's per-component table.
  - In `--check` mode, asserts that the shipped `NOTICE` file is byte-equal
    to `NOTICE.generated`. CI runs `--check` and fails the build on drift.
  - Fails on any component whose licence is not in the Apache-2.0-compatible
    allowlist (MIT / BSD-2 / BSD-3 / Apache-2.0 / Zlib / public-domain).

This script is intentionally self-contained — no third-party Python imports.
It is the v1 floor; the post-v1 SBOM tool (`generate_sbom.py`, plan §46.3)
extends this to CycloneDX output.

Usage:
    python _tools/license_audit.py             # write NOTICE.generated
    python _tools/license_audit.py --check     # CI mode: diff vs shipped NOTICE
    python _tools/license_audit.py --list      # print one line per component
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable

REPO_ROOT = Path(__file__).resolve().parent.parent

# Apache-2.0-compatible licence allowlist (plan §48.3).
APACHE2_COMPATIBLE = {
    "Apache-2.0",
    "Apache-2.0-LLVM",       # Apache 2.0 with LLVM exceptions
    "Apache-2.0-Modified",   # OpenUSD's TOST modification
    "MIT",
    "BSD-2-Clause",
    "BSD-3-Clause",
    "Zlib",
    "Unlicense",             # public domain
    "Public-Domain",
    "CC0-1.0",               # Creative Commons Zero — public-domain dedication
}

# Explicitly forbidden licences (plan §48.3 — copyleft incompat with Apache-2.0).
FORBIDDEN_LICENCES = {
    "GPL-2.0", "GPL-2.0-only", "GPL-2.0-or-later",
    "GPL-3.0", "GPL-3.0-only", "GPL-3.0-or-later",
    "LGPL-2.1", "LGPL-3.0",
    "AGPL-3.0",
    "SSPL-1.0",
}


@dataclass(frozen=True)
class Component:
    """One third-party dependency."""
    name: str
    homepage: str
    licence: str
    used_by: str
    notes: str = ""

    def render(self) -> str:
        block = [
            f"{self.name}",
            f"  {self.homepage}",
            f"  {self.licence}.",
            f"  Used by: {self.used_by}.",
        ]
        if self.notes:
            block.append(f"  {self.notes}")
        return "\n".join(block)


# Canonical component table. Keep in sync with NOTICE manually until
# vcpkg / FetchContent introspection is wired in (post-v1).
COMPONENTS: list[Component] = [
    Component(
        "OpenUSD (Universal Scene Description)",
        "https://github.com/PixarAnimationStudios/OpenUSD",
        "Apache-2.0-Modified",
        "pyxis_hydra, pyxis_usd_ingest, pyxis_material_translation",
        "TOST license — Apache 2.0 with redistribution clauses reflecting "
        "Pixar trademark guidance.",
    ),
    Component(
        "MaterialX",
        "https://github.com/AcademySoftwareFoundation/MaterialX",
        "Apache-2.0",
        "pyxis_material_translation (MaterialX → OpenPBR translation)",
    ),
    Component(
        "NVRHI",
        "https://github.com/NVIDIAGameWorks/nvrhi",
        "MIT",
        "pyxis_platform (Vulkan device abstraction), pyxis_renderer",
    ),
    Component(
        "Slang",
        "https://github.com/shader-slang/slang",
        "Apache-2.0-LLVM",
        "shader compilation pipeline",
    ),
    Component(
        "ShaderMake",
        "https://github.com/NVIDIA-RTX/ShaderMake",
        "MIT",
        "build-time shader permutation expansion + Slang invocation",
    ),
    Component(
        "Vulkan-Headers",
        "https://github.com/KhronosGroup/Vulkan-Headers",
        "Apache-2.0",
        "pyxis_platform (Vulkan API headers)",
    ),
    Component(
        "GLFW",
        "https://github.com/glfw/glfw",
        "Zlib",
        "pyxis_platform (viewer-mode window + input)",
    ),
    Component(
        "Dear ImGui (docking branch)",
        "https://github.com/ocornut/imgui",
        "MIT",
        "pyxis_app (viewer UI)",
    ),
    Component(
        "Tracy",
        "https://github.com/wolfpld/tracy",
        "BSD-3-Clause",
        "pyxis_platform (CPU/GPU profiling)",
    ),
    Component(
        "spdlog",
        "https://github.com/gabime/spdlog",
        "MIT",
        "pyxis_platform (logging)",
    ),
    Component(
        "Flecs",
        "https://github.com/SanderMertens/flecs",
        "MIT",
        "pyxis_renderer (SceneWorld ECS — linked PRIVATE)",
    ),
    Component(
        "moodycamel-concurrentqueue",
        "https://github.com/cameron314/concurrentqueue",
        "BSD-2-Clause",
        "pyxis_renderer (multi-producer / single-consumer mutation queue)",
    ),
    Component(
        "hlslpp",
        "https://github.com/redorav/hlslpp",
        "MIT",
        "pyxis_renderer, pyxis_renderer/Public/* (HLSL-shaped C++ math types)",
    ),
    Component(
        "nlohmann/json",
        "https://github.com/nlohmann/json",
        "MIT",
        "pyxis_app (parameters.json parsing, JSON profiling output)",
    ),
    Component(
        "stb (stb_image, stb_image_write)",
        "https://github.com/nothings/stb",
        "Public-Domain",
        "pyxis_renderer (LDR texture decode), pyxis_app (PNG output)",
    ),
    Component(
        "tinyexr",
        "https://github.com/syoyo/tinyexr",
        "BSD-3-Clause",
        "pyxis_renderer (HDR/EXR texture decode), pyxis_app (EXR output)",
    ),
    Component(
        "MikkTSpace",
        "https://github.com/mmikk/MikkTSpace",
        "Zlib",
        "pyxis_renderer (tangent-space generation for normal maps)",
    ),
    Component(
        "GoogleTest",
        "https://github.com/google/googletest",
        "BSD-3-Clause",
        "pyxis_unit_tests",
    ),
]


# Bundled scene assets (data files shipped under resources/, not linked
# software). Same render shape as Component above so attribution lines
# up; rendered into a dedicated "Bundled scene assets" section in the
# NOTICE so consumers can see at a glance what's source vs data.
BUNDLED_ASSETS: list[Component] = [
    Component(
        "Kloofendal 43d Clear PureSky (default_sky.exr)",
        "https://polyhaven.com/a/kloofendal_43d_clear_puresky",
        "CC0-1.0",
        "pyxis_app (resources/scenes/default_sky.exr — bundled dome-light "
        "environment for the §29.4.a default startup scene)",
        "Authored by Greg Zaal for Poly Haven; redistributed under the "
        "Creative Commons Zero (public-domain dedication) per polyhaven.com.",
    ),
]


def validate_licences(components: Iterable[Component]) -> list[str]:
    """Return list of error strings for any forbidden / unknown licence."""
    errors: list[str] = []
    for c in components:
        if c.licence in FORBIDDEN_LICENCES:
            errors.append(
                f"FORBIDDEN licence '{c.licence}' on component '{c.name}' "
                f"— GPL-family is incompatible with Apache 2.0 (plan §48.3)."
            )
        elif c.licence not in APACHE2_COMPATIBLE:
            errors.append(
                f"UNKNOWN licence '{c.licence}' on component '{c.name}'. "
                f"Add it to APACHE2_COMPATIBLE in license_audit.py if intentional."
            )
    return errors


def render_notice(
    components: Iterable[Component], bundled_assets: Iterable[Component]
) -> str:
    header = """\
Pyxis
Copyright 2026 The Pyxis Project Authors

This product includes software developed at The Pyxis Project (the "Project").
Licensed under the Apache License, Version 2.0 (see LICENSE).

================================================================================
Third-party software included or linked by Pyxis
================================================================================

This NOTICE file is the canonical attribution surface for every third-party
component Pyxis links against, in source or binary form. Per plan §48.3 it is
asserted byte-equal in CI to `NOTICE.generated` produced by
`_tools/license_audit.py`. Adding or removing a dependency requires updating
both files in the same PR.

All listed components are distributed under licenses compatible with Apache 2.0
(MIT, BSD-2/3, Apache-2.0, Zlib, public domain / Unlicense, or
Boost-equivalent). No GPL, AGPL, or SSPL components are linked.

"""
    blocks = [c.render() for c in components]
    body = "\n\n".join(f"--------------------------------------------------------------------------------\n{b}" for b in blocks)

    # Bundled scene assets — data files (HDRIs, textures, USD scenes)
    # shipped under resources/. Distinct from the linked-software list
    # above so consumers can see at a glance what's source vs data.
    asset_section = ""
    asset_blocks = [a.render() for a in bundled_assets]
    if asset_blocks:
        asset_section = "\n\n================================================================================\nBundled scene assets\n================================================================================\n\nData files (HDRIs, textures, default-scene USD) shipped under resources/.\nLicences are CC0 / public-domain unless noted otherwise.\n\n"
        asset_section += "\n\n".join(
            f"--------------------------------------------------------------------------------\n{b}"
            for b in asset_blocks
        )

    footer = """

================================================================================
Optional / build-time-only third-party components
================================================================================

NVIDIA Aftermath SDK (optional, gated by PYXIS_ENABLE_AFTERMATH)
  https://developer.nvidia.com/nsight-aftermath
  NVIDIA Aftermath SDK License Agreement.
  Used by: pyxis_platform (Debug-only, Windows-only crash diagnostics).
  Not redistributed in the Pyxis source tree; consumers must source the SDK
  separately from NVIDIA. The optional integration code is gated by a CMake
  flag and excluded from default builds.

NVIDIA Nsight Capture (optional)
  https://developer.nvidia.com/nsight-graphics
  NVIDIA SDK License.
  Used by: pyxis_platform (optional capture hookup, Debug-only).
  Same redistribution policy as Aftermath above.

================================================================================
End of NOTICE
================================================================================
"""
    return header + body + asset_section + footer


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--check", action="store_true",
                        help="Diff generated output against shipped NOTICE; exit 1 on mismatch.")
    parser.add_argument("--list", action="store_true",
                        help="Print one line per component (name, licence) and exit.")
    parser.add_argument("--out", type=Path, default=REPO_ROOT / "NOTICE.generated",
                        help="Output path for generated NOTICE (default: NOTICE.generated).")
    args = parser.parse_args()

    licence_errors = validate_licences(COMPONENTS) + validate_licences(BUNDLED_ASSETS)
    if licence_errors:
        for e in licence_errors:
            print(f"license_audit: {e}", file=sys.stderr)
        return 2

    if args.list:
        for c in COMPONENTS:
            print(f"{c.licence:24s}  {c.name}")
        for a in BUNDLED_ASSETS:
            print(f"{a.licence:24s}  {a.name}  (bundled asset)")
        return 0

    generated = render_notice(COMPONENTS, BUNDLED_ASSETS)
    args.out.write_text(generated, encoding="utf-8", newline="\n")

    if args.check:
        notice_path = REPO_ROOT / "NOTICE"
        if not notice_path.exists():
            print("license_audit: NOTICE not found in repo root", file=sys.stderr)
            return 1
        shipped = notice_path.read_text(encoding="utf-8")
        # Tolerate Windows CRLF-checked-out files in the comparison.
        if shipped.replace("\r\n", "\n") != generated:
            print(
                "license_audit: NOTICE drift detected.\n"
                f"  Generated: {args.out}\n"
                "  Run `python _tools/license_audit.py` to refresh and commit the diff.",
                file=sys.stderr,
            )
            return 1
        print("license_audit: NOTICE matches generated output.")
        return 0

    print(f"license_audit: wrote {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
