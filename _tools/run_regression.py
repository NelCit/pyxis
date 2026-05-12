#!/usr/bin/env python3
"""Pyxis image-regression harness (M10 — plan §35).

Spawns ``pyxis.exe --headless --config <fixture>``, reads the
produced EXR, diffs it pixel-wise against a baseline EXR, and
fails the run when RMSE / MAE / max-abs-delta exceed the
per-fixture tolerance.

Fixture layout (one per regression test)::

    tests/regression/<name>/
        regression.json     # this script's input — tolerances + metadata
        baseline.exr        # ground-truth render

``regression.json`` schema::

    {
        "name":           "M8a.WorldLobbyRegression",
        "description":    "Lobby hero camera, M9-fidelity stable",
        "config":         "tests/fixtures/m8_world_lobby.json",
        "scene":          "resources/scenes/world_lobby/World_Lobby.usd",
        "baseline":       "baseline.exr",
        "tolerance": {
            "rmse":           0.02,
            "mae":            0.01,
            "maxAbsoluteDelta": 0.30
        }
    }

Paths in ``config``/``scene``/``baseline`` may be absolute or
relative to the regression directory (resolved relative to the
fixture JSON's parent directory, falling back to the repo root).

Exit codes:
    0  pass — all metrics within tolerance
    1  fail — at least one metric exceeded tolerance (diff.exr
       written to output dir + summary printed)
    2  invocation / IO failure (pyxis.exe missing, EXR unreadable,
       baseline missing, fixture JSON malformed)

The harness is **plain-stdout-friendly**. No colour codes, no
spinners — it has to read well in CI logs and in
``ctest --output-on-failure``.
"""

from __future__ import annotations

import argparse
import csv
import datetime
import json
import shutil
import subprocess
import sys
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Optional

import numpy as np
import OpenEXR  # type: ignore[import-not-found]


# ---------------------------------------------------------------------
# EXR I/O
# ---------------------------------------------------------------------

def _read_exr_rgba(path: Path) -> np.ndarray:
    """Read an RGBA-or-RGB EXR into a float32 HxWx{3,4} numpy array.

    Pyxis writes RGBA32F EXRs via tinyexr (HeadlessMode); tinyexr
    bundles the four channels into a single packed ``RGBA`` channel
    whose ``.pixels`` is already an (H, W, 4) array. Canonical EXR
    writers (Nuke, oiiotool, OpenEXR's own) emit one channel per
    plane named ``R/G/B/A``; we support both so a developer-captured
    baseline can come from either tool. OpenEXR 3.x's Python
    binding handles both, but we have to branch on which key shape
    exists in ``channels``.
    """
    with OpenEXR.File(str(path)) as exr:
        # OpenEXR 3.x exposes a `parts` array; first part = main image.
        channels = exr.parts[0].channels

        # tinyexr's bundled-RGBA layout: one channel whose .pixels is
        # already (H, W, 4). Pass-through after a dtype check.
        if 'RGBA' in channels:
            packed = np.asarray(channels['RGBA'].pixels, dtype=np.float32)
            if packed.ndim != 3 or packed.shape[-1] < 3:
                raise ValueError(f"RGBA packed channel has unexpected shape {packed.shape}")
            return packed
        if 'RGB' in channels:
            packed = np.asarray(channels['RGB'].pixels, dtype=np.float32)
            if packed.ndim != 3 or packed.shape[-1] != 3:
                raise ValueError(f"RGB packed channel has unexpected shape {packed.shape}")
            return packed

        # Canonical per-plane layout: stack R/G/B[/A] into an HxWxC array.
        names = ['R', 'G', 'B']
        if 'A' in channels:
            names.append('A')
        planes = []
        for name in names:
            if name not in channels:
                raise ValueError(
                    f"channel '{name}' not found in EXR; available: {list(channels.keys())}")
            planes.append(np.asarray(channels[name].pixels, dtype=np.float32))
        return np.stack(planes, axis=-1)


def _write_exr_rgba(path: Path, image: np.ndarray) -> None:
    """Write a float32 HxWxC array as an EXR with a packed RGBA channel.

    Matches tinyexr's output convention (one bundled ``RGBA``
    channel) so a diff EXR opens correctly in the same viewer as
    the produced + baseline. Diff EXRs are dev-eyeball-only —
    consumers don't parse them.
    """
    image = np.ascontiguousarray(image, dtype=np.float32)
    name = 'RGBA' if image.shape[-1] >= 4 else 'RGB'
    channels = {name: image}
    header = {'compression': OpenEXR.ZIP_COMPRESSION, 'type': OpenEXR.scanlineimage}
    with OpenEXR.File(header, channels) as exr:
        exr.write(str(path))


# ---------------------------------------------------------------------
# Metrics
# ---------------------------------------------------------------------

@dataclass(frozen=True)
class DiffMetrics:
    rmse: float
    mae: float
    max_abs_delta: float
    pixel_count: int


def _compute_metrics(produced: np.ndarray, baseline: np.ndarray) -> DiffMetrics:
    if produced.shape != baseline.shape:
        raise ValueError(
            f"shape mismatch — produced={produced.shape}, baseline={baseline.shape}")
    delta = produced.astype(np.float32) - baseline.astype(np.float32)
    sq = delta * delta
    return DiffMetrics(
        rmse=float(np.sqrt(sq.mean())),
        mae=float(np.abs(delta).mean()),
        max_abs_delta=float(np.abs(delta).max()),
        pixel_count=int(produced.size),
    )


# ---------------------------------------------------------------------
# Fixture
# ---------------------------------------------------------------------

@dataclass
class Fixture:
    name: str
    description: str
    config_path: Path
    scene_path: Optional[Path]
    baseline_path: Path
    tolerance_rmse: float
    tolerance_mae: float
    tolerance_max_abs: float


def _resolve_path(raw: str, fixture_dir: Path, repo_root: Path) -> Path:
    """Resolve a path that may be absolute, relative to the fixture
    directory, or relative to the repo root."""
    candidate = Path(raw)
    if candidate.is_absolute() and candidate.exists():
        return candidate
    rel_to_fixture = (fixture_dir / candidate).resolve()
    if rel_to_fixture.exists():
        return rel_to_fixture
    rel_to_root = (repo_root / candidate).resolve()
    return rel_to_root


def _load_fixture(fixture_json: Path, repo_root: Path) -> Fixture:
    data = json.loads(fixture_json.read_text(encoding='utf-8'))
    fixture_dir = fixture_json.parent
    tol = data.get('tolerance', {})
    scene_raw = data.get('scene')
    return Fixture(
        name=data['name'],
        description=data.get('description', ''),
        config_path=_resolve_path(data['config'], fixture_dir, repo_root),
        scene_path=_resolve_path(scene_raw, fixture_dir, repo_root) if scene_raw else None,
        baseline_path=_resolve_path(data['baseline'], fixture_dir, repo_root),
        tolerance_rmse=float(tol.get('rmse', 0.0)),
        tolerance_mae=float(tol.get('mae', 0.0)),
        tolerance_max_abs=float(tol.get('maxAbsoluteDelta', 0.0)),
    )


# ---------------------------------------------------------------------
# Main driver
# ---------------------------------------------------------------------

@dataclass(frozen=True)
class RunOutcome:
    returncode: int
    wall_clock_ms: float


def _run_pyxis(pyxis_exe: Path, fixture: Fixture, output_exr: Path,
               profile_json: Optional[Path], bench_frames: int = 0) -> RunOutcome:
    args = [
        str(pyxis_exe),
        '--headless',
        '--config', str(fixture.config_path),
        '--output', str(output_exr),
    ]
    if fixture.scene_path is not None:
        args.extend(['--scene', str(fixture.scene_path)])
    if profile_json is not None:
        args.extend(['--profile', str(profile_json)])
    if bench_frames > 0:
        args.extend(['--bench-frames', str(bench_frames)])
    start = time.perf_counter()
    proc = subprocess.run(args, capture_output=True, text=True, encoding='utf-8',
                          errors='replace')
    wall_ms = (time.perf_counter() - start) * 1000.0
    sys.stdout.write(proc.stdout)
    sys.stderr.write(proc.stderr)
    return RunOutcome(returncode=proc.returncode, wall_clock_ms=wall_ms)


# ---------------------------------------------------------------------
# KPI CSV
# ---------------------------------------------------------------------

# Columns the rolling KPI CSV stores. Stable across runs so
# _tools/perf_compare.py + perf_dashboard.py (M10 Phase D) can index
# into them without a schema migration each time we add a metric.
# When adding a column: APPEND to keep the existing CSV header valid
# under csv.DictWriter's extrasaction='ignore' default.
_KPI_COLUMNS = [
    'timestamp_iso',
    'fixture',
    'status',
    'pyxis_version',
    'pyxis_git_sha',
    'gpu_name',
    'gpu_driver_version_raw',
    'gpu_vendor_id',
    'gpu_device_id',
    'wall_clock_ms',
    'rmse',
    'mae',
    'max_abs_delta',
    'pixel_count',
    'bench_frames',
    'pathtrace_gpu_p50_ms',
    'pathtrace_gpu_p99_ms',
    'pathtrace_gpu_max_ms',
    'commit_resources_cpu_p50_ms',
    'commit_resources_cpu_p99_ms',
    'frame_cpu_p50_ms',
    'frame_cpu_p99_ms',
]


def _extract_pass_metric(passes: list[dict[str, Any]], name: str, kind: str,
                         percentile: str) -> Optional[float]:
    """Look up a per-pass percentile (returns None if not present in
    this run — e.g. when bench was disabled or the scope was inactive).
    Percentile key is one of ``p50_ms``/``p99_ms``/``max_ms``.
    """
    for entry in passes:
        if entry.get('name') == name and entry.get('kind') == kind:
            value = entry.get(percentile)
            return float(value) if value is not None else None
    return None


def _append_kpi_csv(csv_path: Path, fixture: Fixture, outcome: RunOutcome,
                    metrics: Optional[DiffMetrics], status: str,
                    profile_path: Optional[Path]) -> None:
    """Append one row to ``csv_path``; writes the header first if the
    file didn't exist. Designed for ``perf_compare.py``'s rolling-
    median consumption (M10 Phase D).
    """
    profile: dict[str, Any] = {}
    if profile_path is not None and profile_path.exists():
        try:
            profile = json.loads(profile_path.read_text(encoding='utf-8'))
        except (OSError, json.JSONDecodeError):
            profile = {}

    gpu = profile.get('gpu', {})
    bench = profile.get('bench', {})
    passes = bench.get('passes', []) if isinstance(bench, dict) else []

    row: dict[str, Any] = {
        'timestamp_iso': datetime.datetime.now(datetime.timezone.utc)
                                 .replace(microsecond=0).isoformat(),
        'fixture': fixture.name,
        'status': status,
        'pyxis_version': profile.get('pyxis_version', ''),
        'pyxis_git_sha': profile.get('pyxis_git_sha', ''),
        'gpu_name': gpu.get('name', ''),
        'gpu_driver_version_raw': gpu.get('driver_version_raw', ''),
        'gpu_vendor_id': gpu.get('vendor_id', ''),
        'gpu_device_id': gpu.get('device_id', ''),
        'wall_clock_ms': f'{outcome.wall_clock_ms:.3f}',
        'rmse': f'{metrics.rmse:.6f}' if metrics else '',
        'mae': f'{metrics.mae:.6f}' if metrics else '',
        'max_abs_delta': f'{metrics.max_abs_delta:.6f}' if metrics else '',
        'pixel_count': metrics.pixel_count if metrics else '',
        'bench_frames': bench.get('frames', 0) if isinstance(bench, dict) else 0,
        'pathtrace_gpu_p50_ms': _extract_pass_metric(passes, 'pass.PathTrace', 'Gpu', 'p50_ms'),
        'pathtrace_gpu_p99_ms': _extract_pass_metric(passes, 'pass.PathTrace', 'Gpu', 'p99_ms'),
        'pathtrace_gpu_max_ms': _extract_pass_metric(passes, 'pass.PathTrace', 'Gpu', 'max_ms'),
        'commit_resources_cpu_p50_ms':
            _extract_pass_metric(passes, 'render.commitResources', 'Cpu', 'p50_ms'),
        'commit_resources_cpu_p99_ms':
            _extract_pass_metric(passes, 'render.commitResources', 'Cpu', 'p99_ms'),
        'frame_cpu_p50_ms': _extract_pass_metric(passes, 'render.frame.cpu', 'Cpu', 'p50_ms'),
        'frame_cpu_p99_ms': _extract_pass_metric(passes, 'render.frame.cpu', 'Cpu', 'p99_ms'),
    }

    needs_header = not csv_path.exists()
    csv_path.parent.mkdir(parents=True, exist_ok=True)
    with csv_path.open('a', newline='', encoding='utf-8') as fh:
        writer = csv.DictWriter(fh, fieldnames=_KPI_COLUMNS, extrasaction='ignore')
        if needs_header:
            writer.writeheader()
        writer.writerow(row)


def main(argv: Optional[list[str]] = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--pyxis', type=Path, required=True,
                        help='Path to pyxis.exe (the Release / Debug binary to test).')
    parser.add_argument('--fixture', type=Path, required=True,
                        help='Path to the regression.json or its parent directory.')
    parser.add_argument('--output-dir', type=Path, required=True,
                        help='Workdir for produced EXR + diff EXR + logs.')
    parser.add_argument('--repo-root', type=Path,
                        default=Path(__file__).resolve().parent.parent,
                        help='Repo root for resolving relative fixture paths.')
    parser.add_argument('--kpi-csv', type=Path, default=None,
                        help='Optional path to append per-run KPIs as CSV. '
                             'Default: <fixture-dir>/kpis.csv (created if missing). '
                             'Pass "" to disable.')
    parser.add_argument('--bench-frames', type=int, default=0,
                        help='Forward to pyxis as --bench-frames so the profile '
                             'JSON includes per-pass percentiles. 0 disables.')
    args = parser.parse_args(argv)

    fixture_json = args.fixture
    if fixture_json.is_dir():
        fixture_json = fixture_json / 'regression.json'
    if not fixture_json.exists():
        print(f'ERROR: fixture JSON not found at {fixture_json}', file=sys.stderr)
        return 2

    try:
        fixture = _load_fixture(fixture_json, args.repo_root)
    except (KeyError, ValueError, json.JSONDecodeError) as exc:
        print(f'ERROR: malformed fixture JSON: {exc}', file=sys.stderr)
        return 2

    if not args.pyxis.exists():
        print(f'ERROR: pyxis.exe not found at {args.pyxis}', file=sys.stderr)
        return 2
    if not fixture.config_path.exists():
        print(f'ERROR: pyxis config not found at {fixture.config_path}', file=sys.stderr)
        return 2
    if not fixture.baseline_path.exists():
        print(f'ERROR: baseline EXR not found at {fixture.baseline_path}', file=sys.stderr)
        return 2

    args.output_dir.mkdir(parents=True, exist_ok=True)
    produced_exr = args.output_dir / 'produced.exr'
    if produced_exr.exists():
        produced_exr.unlink()
    profile_json = args.output_dir / 'profile.json'
    if profile_json.exists():
        profile_json.unlink()

    # KPI CSV defaults to <fixture-dir>/kpis.csv so the rolling history
    # ships next to its fixture; --kpi-csv "" disables, --kpi-csv <path>
    # overrides (CI uploads to a shared location).
    kpi_csv: Optional[Path]
    if args.kpi_csv is None:
        kpi_csv = fixture_json.parent / 'kpis.csv'
    elif str(args.kpi_csv) == '':
        kpi_csv = None
    else:
        kpi_csv = args.kpi_csv

    print(f'== Pyxis regression: {fixture.name} ==')
    print(f'   {fixture.description}')
    print(f'   config:   {fixture.config_path}')
    if fixture.scene_path:
        print(f'   scene:    {fixture.scene_path}')
    print(f'   baseline: {fixture.baseline_path}')
    print(f'   output:   {produced_exr}')
    if kpi_csv is not None:
        print(f'   kpi csv:  {kpi_csv}')

    # --bench-frames forwards into pyxis so the profile JSON carries
    # per-pass percentiles. Default 0 (single-frame run) keeps the
    # regression cheap; CI's nightly job sets a larger value.
    outcome = _run_pyxis(args.pyxis, fixture, produced_exr, profile_json,
                         bench_frames=args.bench_frames)
    if outcome.returncode != 0:
        if kpi_csv is not None:
            _append_kpi_csv(kpi_csv, fixture, outcome, None, 'pyxis_nonzero_exit',
                            profile_json)
        print(f'FAIL: pyxis.exe exited rc={outcome.returncode}', file=sys.stderr)
        return 1
    if not produced_exr.exists():
        if kpi_csv is not None:
            _append_kpi_csv(kpi_csv, fixture, outcome, None, 'missing_output_exr',
                            profile_json)
        print(f'FAIL: pyxis exited 0 but produced no EXR at {produced_exr}', file=sys.stderr)
        return 1

    try:
        produced = _read_exr_rgba(produced_exr)
        baseline = _read_exr_rgba(fixture.baseline_path)
    except (OSError, ValueError, RuntimeError) as exc:
        if kpi_csv is not None:
            _append_kpi_csv(kpi_csv, fixture, outcome, None, 'exr_read_error',
                            profile_json)
        print(f'ERROR: failed to read EXR: {exc}', file=sys.stderr)
        return 2

    try:
        metrics = _compute_metrics(produced, baseline)
    except ValueError as exc:
        if kpi_csv is not None:
            _append_kpi_csv(kpi_csv, fixture, outcome, None, 'shape_mismatch',
                            profile_json)
        print(f'FAIL: {exc}', file=sys.stderr)
        return 1

    print('   rmse:        {:.6f} (tolerance {:.6f})'.format(
        metrics.rmse, fixture.tolerance_rmse))
    print('   mae:         {:.6f} (tolerance {:.6f})'.format(
        metrics.mae, fixture.tolerance_mae))
    print('   max |delta|: {:.6f} (tolerance {:.6f})'.format(
        metrics.max_abs_delta, fixture.tolerance_max_abs))
    print(f'   pixels:      {metrics.pixel_count}')

    failed_metrics = []
    if metrics.rmse > fixture.tolerance_rmse:
        failed_metrics.append(f'rmse {metrics.rmse:.6f} > {fixture.tolerance_rmse:.6f}')
    if metrics.mae > fixture.tolerance_mae:
        failed_metrics.append(f'mae {metrics.mae:.6f} > {fixture.tolerance_mae:.6f}')
    if metrics.max_abs_delta > fixture.tolerance_max_abs:
        failed_metrics.append(
            f'max_abs_delta {metrics.max_abs_delta:.6f} > {fixture.tolerance_max_abs:.6f}')

    if failed_metrics:
        # Dump diff EXR for the developer to eyeball: |produced - baseline|
        # on RGB, alpha forced to 1.0 so it shows in viewers without
        # premultiplication weirdness.
        delta = produced.astype(np.float32) - baseline.astype(np.float32)
        diff = np.abs(delta)
        if diff.shape[-1] >= 4:
            diff[..., 3] = 1.0
        diff_path = args.output_dir / 'diff.exr'
        try:
            _write_exr_rgba(diff_path, diff)
            shutil.copy2(produced_exr, args.output_dir / 'produced.exr')
            print(f'   diff EXR:    {diff_path}')
        except (OSError, RuntimeError) as exc:
            print(f'   diff EXR write failed: {exc}', file=sys.stderr)
        if kpi_csv is not None:
            _append_kpi_csv(kpi_csv, fixture, outcome, metrics, 'fail', profile_json)
        print('FAIL: ' + '; '.join(failed_metrics), file=sys.stderr)
        return 1

    if kpi_csv is not None:
        _append_kpi_csv(kpi_csv, fixture, outcome, metrics, 'pass', profile_json)
    print('PASS')
    return 0


if __name__ == '__main__':
    sys.exit(main())
