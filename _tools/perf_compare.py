#!/usr/bin/env python3
"""Pyxis perf-regression detector (M10 — plan §35 / §36.6).

Consumes the per-fixture KPI CSV emitted by ``_tools/run_regression.py``
and reports any metric that regressed by more than a configurable
threshold compared to a rolling-median baseline.

§36.6 baseline policy (default):
    * Rolling median over the 3 most-recent prior PASS runs.
    * Alert at ``> 10%`` regression on any tracked KPI.
    * Two consecutive nights past threshold = S2 incident (the CI
      job that aggregates this output is responsible for the
      "two-consecutive-runs" logic; this script reports per-run).

Usage:

    python perf_compare.py --csv tests/regression/<name>/kpis.csv

By default the script reads the latest row, builds a baseline from the
preceding rows, and writes one of:

    * ``ok    metric=value vs baseline=...``                  (green)
    * ``warn  metric=value vs baseline=... (+12.3%)``         (orange — past threshold)
    * ``info  metric=value (no baseline yet)``                (first few runs)

Exit codes:
    0  ok / info — no regressions past threshold (or insufficient history)
    1  warn      — at least one metric regressed past threshold
    2  invocation / IO failure

The KPIs compared today are: ``pathtrace_gpu_p50_ms``,
``pathtrace_gpu_p99_ms``, ``commit_resources_cpu_p50_ms``,
``frame_cpu_p50_ms``, ``wall_clock_ms``. Adding a metric: append to
``_TRACKED_KPIS`` below and the rolling-median pipeline picks it up.
"""

from __future__ import annotations

import argparse
import csv
import statistics
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Optional


# KPI columns we run regression checks on. Order = display order in
# the report. (column_name, human_label, lower_is_better).
_TRACKED_KPIS: list[tuple[str, str, bool]] = [
    ('pathtrace_gpu_p50_ms',         'pass.PathTrace p50',           True),
    ('pathtrace_gpu_p99_ms',         'pass.PathTrace p99',           True),
    ('commit_resources_cpu_p50_ms',  'commitResources p50',          True),
    ('commit_resources_cpu_p99_ms',  'commitResources p99',          True),
    ('frame_cpu_p50_ms',             'render.frame.cpu p50',         True),
    ('frame_cpu_p99_ms',             'render.frame.cpu p99',         True),
    ('wall_clock_ms',                'time-to-first-image (wall)',   True),
]


@dataclass(frozen=True)
class Report:
    """One per-KPI line in the output."""
    level: str        # 'ok' | 'warn' | 'info'
    metric: str
    current: float
    baseline: Optional[float]
    delta_pct: Optional[float]


def _parse_float(raw: str) -> Optional[float]:
    if raw is None or raw == '':
        return None
    try:
        return float(raw)
    except (TypeError, ValueError):
        return None


def _read_rows(csv_path: Path) -> list[dict[str, str]]:
    rows: list[dict[str, str]] = []
    with csv_path.open('r', newline='', encoding='utf-8') as fh:
        reader = csv.DictReader(fh)
        for row in reader:
            rows.append(row)
    return rows


def _baseline_for(rows: list[dict[str, str]], column: str, window: int) -> Optional[float]:
    """Rolling median of the ``window`` most-recent PASS rows preceding
    the latest one. Returns None when fewer than 2 prior PASS rows
    exist (a single sample is too noisy to gate against).
    """
    if not rows:
        return None
    # Walk in reverse — youngest first — skipping the latest row.
    prior_pass: list[float] = []
    for row in reversed(rows[:-1]):
        if row.get('status') != 'pass':
            continue
        value = _parse_float(row.get(column, ''))
        if value is None:
            continue
        prior_pass.append(value)
        if len(prior_pass) >= window:
            break
    if len(prior_pass) < 2:
        return None
    return statistics.median(prior_pass)


def _check_one(rows: list[dict[str, str]], column: str, label: str,
               threshold_pct: float, window: int) -> Optional[Report]:
    latest = rows[-1]
    current = _parse_float(latest.get(column, ''))
    if current is None:
        return None  # metric not present in this run (e.g. no --bench-frames)

    baseline = _baseline_for(rows, column, window)
    if baseline is None or baseline <= 0.0:
        return Report(level='info', metric=label, current=current,
                      baseline=None, delta_pct=None)

    delta_pct = (current - baseline) / baseline * 100.0
    level = 'warn' if delta_pct > threshold_pct else 'ok'
    return Report(level=level, metric=label, current=current,
                  baseline=baseline, delta_pct=delta_pct)


def _format_report(report: Report) -> str:
    if report.baseline is None:
        return f'  info   {report.metric:30s}  current={report.current:.3f}  (no baseline yet)'
    direction = '+' if report.delta_pct is not None and report.delta_pct >= 0 else ''
    pct = f'{direction}{report.delta_pct:.1f}%' if report.delta_pct is not None else 'n/a'
    badge = {'ok': '  ok    ', 'warn': '  WARN  '}.get(report.level, '  ?     ')
    return (f'{badge}{report.metric:30s}  current={report.current:.3f}  '
            f'baseline={report.baseline:.3f}  delta={pct}')


def main(argv: Optional[list[str]] = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--csv', type=Path, required=True,
                        help='Per-fixture KPI CSV (e.g. tests/regression/<name>/kpis.csv).')
    parser.add_argument('--threshold-pct', type=float, default=10.0,
                        help='Per-KPI regression threshold (percent). Default 10 (§36.6).')
    parser.add_argument('--window', type=int, default=3,
                        help='Number of prior PASS rows to median for the baseline. Default 3.')
    parser.add_argument('--markdown', action='store_true',
                        help='Emit a markdown report (CI artifact); otherwise plain text.')
    args = parser.parse_args(argv)

    if not args.csv.exists():
        print(f'ERROR: KPI CSV not found at {args.csv}', file=sys.stderr)
        return 2

    rows = _read_rows(args.csv)
    if not rows:
        print(f'INFO: KPI CSV is empty ({args.csv}); nothing to compare', file=sys.stderr)
        return 0

    reports: list[Report] = []
    for column, label, _lower_is_better in _TRACKED_KPIS:
        report = _check_one(rows, column, label, args.threshold_pct, args.window)
        if report is not None:
            reports.append(report)

    any_warn = any(r.level == 'warn' for r in reports)
    latest = rows[-1]

    if args.markdown:
        print(f'# Pyxis perf-regression report - {latest.get("fixture", "?")}')
        print()
        print(f'**Status:** {"REGRESSION" if any_warn else "OK"}  ')
        print(f'**Timestamp:** {latest.get("timestamp_iso", "?")}  ')
        print(f'**GPU:** {latest.get("gpu_name", "?")} ')
        print(f'(driver {latest.get("gpu_driver_version_raw", "?")})  ')
        print(f'**pyxis:** {latest.get("pyxis_version", "?")}')
        print()
        print('| metric | current | baseline | delta | level |')
        print('|---|---|---|---|---|')
        for report in reports:
            if report.baseline is None:
                base_cell = '_(no history)_'
                delta_cell = '—'
            else:
                base_cell = f'{report.baseline:.3f}'
                delta_cell = f'{"+" if (report.delta_pct or 0) >= 0 else ""}{report.delta_pct:.1f}%'
            level_cell = {'ok': 'ok', 'warn': '**WARN**', 'info': '_info_'}[report.level]
            print(f'| {report.metric} | {report.current:.3f} | {base_cell} | '
                  f'{delta_cell} | {level_cell} |')
    else:
        print(f'== Pyxis perf-regression report — {latest.get("fixture", "?")} ==')
        print(f'   timestamp: {latest.get("timestamp_iso", "?")}')
        print(f'   gpu:       {latest.get("gpu_name", "?")}')
        print(f'   pyxis:     {latest.get("pyxis_version", "?")}')
        print(f'   threshold: > {args.threshold_pct:.1f}% (rolling window = {args.window})')
        for report in reports:
            print(_format_report(report))
        print(f'overall: {"WARN" if any_warn else "ok"}')

    return 1 if any_warn else 0


if __name__ == '__main__':
    sys.exit(main())
