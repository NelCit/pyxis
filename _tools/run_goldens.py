#!/usr/bin/env python3
"""Pyxis golden-test orchestrator.

Walks `tests/golden-tests-data/` and runs every subdirectory as a
pixel-equal regression against the matching `tests/golden-tests-
expected/<name>/baseline.exr`. Two modes:

    python _tools/run_goldens.py
        Default: render each fixture, byte-compare against the
        checked-in baseline.exr. Exits 1 if any test mismatches.

    python _tools/run_goldens.py --rebake
        Render each fixture and overwrite the matching expected
        baseline.exr in-place. Use this after a deliberate visual
        change ships.

Each subdir under `tests/golden-tests-data/` (except `_shared`) must
contain `fixture.usda` + `regression.json`; the matching expected
subdir must exist and is where the baseline lives.

regression.json minimal schema:
    {
        "$comment": "...",
        "frame":     -1   // optional --frame argument; -1 = default time
    }

Shared 256x256 render config: `tests/golden-tests-data/_shared/config.json`.
Per-test override: drop a `config.json` next to the fixture.
"""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
from pathlib import Path
from typing import List, Optional

REPO_ROOT = Path(__file__).resolve().parent.parent
DATA_DIR     = REPO_ROOT / "tests" / "golden-tests-data"
EXPECTED_DIR = REPO_ROOT / "tests" / "golden-tests-expected"
SHARED_CONFIG = DATA_DIR / "_shared" / "config.json"


def discover_tests() -> List[Path]:
    """Return every test subdirectory under DATA_DIR (sorted)."""
    return sorted(
        d for d in DATA_DIR.iterdir()
        if d.is_dir() and d.name not in {"_shared"}
        and (d / "fixture.usda").exists()
    )


def run_pyxis(pyxis_exe: Path, config: Path, scene: Path,
              output: Path, frame: Optional[int], cwd: Path) -> int:
    """Invoke pyxis --headless. Returns the exit code."""
    args = [
        str(pyxis_exe), "--headless",
        "--config", str(config),
        "--scene",  str(scene),
        "--output", str(output),
    ]
    if frame is not None and frame >= 0:
        args.extend(["--frame", str(frame)])
    proc = subprocess.run(args, cwd=str(cwd), capture_output=True, text=True)
    if proc.returncode != 0:
        sys.stderr.write(proc.stdout)
        sys.stderr.write(proc.stderr)
    return proc.returncode


def compare_bytes(produced: Path, baseline: Path) -> Optional[str]:
    """Return None on byte-equal, else a short error string."""
    if not baseline.exists():
        return f"baseline missing at {baseline}"
    produced_bytes = produced.read_bytes()
    baseline_bytes = baseline.read_bytes()
    if produced_bytes == baseline_bytes:
        return None
    return (f"pixels differ — produced {len(produced_bytes)} bytes, "
            f"baseline {len(baseline_bytes)} bytes")


def run_test(pyxis_exe: Path, test_dir: Path, rebake: bool,
             output_root: Path) -> bool:
    """Returns True on pass, False on fail (or on rebake success)."""
    name = test_dir.name
    fixture = test_dir / "fixture.usda"
    baseline_dir = EXPECTED_DIR / name
    baseline = baseline_dir / "baseline.exr"
    config = test_dir / "config.json"
    if not config.exists():
        config = SHARED_CONFIG
    if rebake:
        baseline_dir.mkdir(parents=True, exist_ok=True)

    regression_path = test_dir / "regression.json"
    frame = None
    if regression_path.exists():
        regression = json.loads(regression_path.read_text(encoding="utf-8"))
        if "frame" in regression and regression["frame"] >= 0:
            frame = regression["frame"]

    work_dir = output_root / name
    work_dir.mkdir(parents=True, exist_ok=True)
    produced = work_dir / "produced.exr"

    print(f"[{name}] rendering ...")
    rc = run_pyxis(pyxis_exe, config, fixture, produced, frame, work_dir)
    if rc != 0:
        print(f"[{name}] FAIL (pyxis exited rc={rc})")
        return False
    if not produced.exists():
        print(f"[{name}] FAIL (no EXR produced at {produced})")
        return False

    if rebake:
        baseline.write_bytes(produced.read_bytes())
        print(f"[{name}] REBAKED ({len(produced.read_bytes())} bytes)")
        return True

    err = compare_bytes(produced, baseline)
    if err is not None:
        print(f"[{name}] FAIL — {err}")
        return False
    print(f"[{name}] PASS")
    return True


def main(argv: List[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--pyxis", type=Path,
                        help="Path to pyxis.exe. Default: build/dev/bin/Debug/pyxis.exe")
    parser.add_argument("--rebake", action="store_true",
                        help="Overwrite baseline.exr instead of comparing.")
    parser.add_argument("--output-root", type=Path,
                        default=REPO_ROOT / "build" / "goldens",
                        help="Where to write the per-test produced EXRs.")
    parser.add_argument("--filter", type=str, default="",
                        help="Substring filter — only run tests whose name "
                             "contains this string.")
    args = parser.parse_args(list(argv))

    pyxis = args.pyxis or (REPO_ROOT / "build" / "dev" / "bin" / "Debug" / "pyxis.exe")
    if not pyxis.exists():
        sys.stderr.write(f"pyxis not found at {pyxis}\n")
        return 2

    tests = discover_tests()
    if args.filter:
        tests = [t for t in tests if args.filter in t.name]
    if not tests:
        sys.stderr.write("no goldens discovered\n")
        return 2

    args.output_root.mkdir(parents=True, exist_ok=True)
    failures = 0
    for test_dir in tests:
        if not run_test(pyxis, test_dir, args.rebake, args.output_root):
            failures += 1

    print()
    print(f"{len(tests)} tests, {failures} failure(s)")
    return 0 if failures == 0 else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
