#!/usr/bin/env python3
"""Check VertexDB CTE benchmark *shape* from Google Benchmark JSON.

Compares median CPU times from one run (same binary, same process) so shared
noise mostly cancels. Absolute nanoseconds are printed for humans; CI gates
only on ratios:

  indexed CTE stays roughly flat as N grows (100k / 1k)
  non-indexed CTE at 100k is far slower than the indexed win path
  AS MATERIALIZED at 1k is far slower than inlined indexed CTE at 1k
  non-indexed CTE grows with table size (100k / 1k)

Usage:
  python3 scripts/check_benchmark_shape.py path/to/benchmark.json
  python3 scripts/check_benchmark_shape.py --self-test
"""

from __future__ import annotations

import argparse
import io
import json
import statistics
import sys
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path
from typing import Any

REPO_ROOT = Path(__file__).resolve().parent.parent
FIXTURE_DIR = REPO_ROOT / "tests" / "benchmark_shape"

INDEXED = "BM_CteIndexedWinSelect"
SCAN = "BM_CteNonIndexedSelect"
MATERIALIZED = "BM_CteMaterializedSelect"

# Conservative gates: local Release ratios are ~1, ~10^3–10^4, ~10^4, ~10^2.
# GHA noise should not collapse these; a planner/storage regression to scan or
# accidental inline-of-materialize will.
DEFAULT_MAX_INDEXED_GROWTH = 8.0
DEFAULT_MIN_SCAN_VS_WIN = 20.0
DEFAULT_MIN_MATERIALIZE_VS_INLINE = 50.0
DEFAULT_MIN_SCAN_GROWTH = 10.0

_AGG_SUFFIXES = ("_median", "_mean", "_stddev", "_cv")
_TIME_SCALE_TO_NS = {"ns": 1.0, "us": 1e3, "ms": 1e6, "s": 1e9}


class ShapeError(Exception):
    """Invalid or incomplete benchmark JSON."""


def cpu_time_ns(entry: dict[str, Any]) -> float:
    unit = str(entry.get("time_unit", "ns"))
    if unit not in _TIME_SCALE_TO_NS:
        raise ShapeError(f"unsupported time_unit {unit!r} in {entry.get('name')}")
    return float(entry["cpu_time"]) * _TIME_SCALE_TO_NS[unit]


def strip_aggregate_suffix(name: str) -> str:
    for suffix in _AGG_SUFFIXES:
        if name.endswith(suffix):
            return name[: -len(suffix)]
    return name


def run_name_of(entry: dict[str, Any]) -> str:
    if "run_name" in entry and entry["run_name"]:
        return str(entry["run_name"])
    return strip_aggregate_suffix(str(entry["name"]))


def parse_bench_arg(run_name: str) -> tuple[str, int | None]:
    base = strip_aggregate_suffix(run_name)
    parts = base.split("/")
    bench = parts[0]
    arg: int | None = None
    if len(parts) >= 2 and parts[1].isdigit():
        arg = int(parts[1])
    return bench, arg


def pick_cpu_ns(entries: list[dict[str, Any]]) -> float:
    medians = [e for e in entries if e.get("aggregate_name") == "median"]
    if medians:
        return cpu_time_ns(medians[0])
    iterations = [e for e in entries if e.get("run_type") != "aggregate"]
    if not iterations:
        iterations = entries
    times = sorted(cpu_time_ns(e) for e in iterations)
    if not times:
        raise ShapeError("no cpu_time samples")
    return float(statistics.median(times))


def load_cpu_times(payload: dict[str, Any]) -> dict[tuple[str, int], float]:
    grouped: dict[tuple[str, int], list[dict[str, Any]]] = {}
    for entry in payload.get("benchmarks", []):
        bench, arg = parse_bench_arg(run_name_of(entry))
        if arg is None:
            continue
        grouped.setdefault((bench, arg), []).append(entry)
    return {key: pick_cpu_ns(entries) for key, entries in grouped.items()}


def require_ns(times: dict[tuple[str, int], float], bench: str, arg: int) -> float:
    key = (bench, arg)
    if key not in times:
        raise ShapeError(f"missing {bench}/{arg} in benchmark JSON")
    return times[key]


def format_ns(ns: float) -> str:
    abs_ns = abs(ns)
    if abs_ns >= 1e9:
        return f"{ns / 1e9:.3f} s"
    if abs_ns >= 1e6:
        return f"{ns / 1e6:.3f} ms"
    if abs_ns >= 1e3:
        return f"{ns / 1e3:.3f} µs"
    return f"{ns:.3f} ns"


def check_shape(
    times: dict[tuple[str, int], float],
    *,
    max_indexed_growth: float,
    min_scan_vs_win: float,
    min_materialize_vs_inline: float,
    min_scan_growth: float,
) -> tuple[list[str], list[str]]:
    indexed_1k = require_ns(times, INDEXED, 1000)
    indexed_100k = require_ns(times, INDEXED, 100000)
    scan_1k = require_ns(times, SCAN, 1000)
    scan_100k = require_ns(times, SCAN, 100000)
    materialized_1k = require_ns(times, MATERIALIZED, 1000)

    indexed_growth = indexed_100k / indexed_1k
    scan_vs_win = scan_100k / indexed_100k
    materialize_vs_inline = materialized_1k / indexed_1k
    scan_growth = scan_100k / scan_1k

    lines = [
        "CTE cost shape (CPU time, median when repetitions produced aggregates)",
        f"  indexed 1k:           {format_ns(indexed_1k)}",
        f"  indexed 100k:         {format_ns(indexed_100k)}",
        f"  scan 1k:              {format_ns(scan_1k)}",
        f"  scan 100k:            {format_ns(scan_100k)}",
        f"  materialized 1k:      {format_ns(materialized_1k)}",
        "",
        "| Ratio | Value | Gate | Result |",
        "| --- | ---: | --- | --- |",
    ]

    checks = [
        (
            "indexed 100k / indexed 1k",
            indexed_growth,
            f"<= {max_indexed_growth:g}",
            indexed_growth <= max_indexed_growth,
        ),
        (
            "scan 100k / indexed 100k",
            scan_vs_win,
            f">= {min_scan_vs_win:g}",
            scan_vs_win >= min_scan_vs_win,
        ),
        (
            "materialized 1k / indexed 1k",
            materialize_vs_inline,
            f">= {min_materialize_vs_inline:g}",
            materialize_vs_inline >= min_materialize_vs_inline,
        ),
        (
            "scan 100k / scan 1k",
            scan_growth,
            f">= {min_scan_growth:g}",
            scan_growth >= min_scan_growth,
        ),
    ]

    failures: list[str] = []
    for label, value, gate, ok in checks:
        status = "ok" if ok else "FAIL"
        lines.append(f"| {label} | {value:.2f}× | {gate} | {status} |")
        if not ok:
            failures.append(f"{label}: {value:.2f}× does not satisfy {gate}")
    return lines, failures


def load_json(path: Path) -> dict[str, Any]:
    try:
        payload = json.loads(path.read_text(encoding="utf-8"))
    except FileNotFoundError as exc:
        raise ShapeError(f"benchmark JSON not found: {path}") from exc
    except json.JSONDecodeError as exc:
        raise ShapeError(f"invalid JSON in {path}: {exc}") from exc
    if not isinstance(payload, dict):
        raise ShapeError(f"{path} is not a JSON object")
    return payload


def run_check(
    path: Path,
    *,
    max_indexed_growth: float,
    min_scan_vs_win: float,
    min_materialize_vs_inline: float,
    min_scan_growth: float,
) -> int:
    try:
        times = load_cpu_times(load_json(path))
        lines, failures = check_shape(
            times,
            max_indexed_growth=max_indexed_growth,
            min_scan_vs_win=min_scan_vs_win,
            min_materialize_vs_inline=min_materialize_vs_inline,
            min_scan_growth=min_scan_growth,
        )
    except ShapeError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1

    print("\n".join(lines))
    if failures:
        print("", file=sys.stderr)
        print("CTE cost-shape check failed:", file=sys.stderr)
        for failure in failures:
            print(f"  - {failure}", file=sys.stderr)
        return 1
    return 0


SELF_TEST_CASES = (
    ("good_median.json", 0),
    ("good_iteration_only.json", 0),
    ("good_repetitions_no_aggregate.json", 0),
    ("regress_to_scan.json", 1),
    ("materialize_collapsed.json", 1),
    ("indexed_grows_like_scan.json", 1),
    ("missing_cte.json", 1),
)


def run_self_test(
    *,
    max_indexed_growth: float,
    min_scan_vs_win: float,
    min_materialize_vs_inline: float,
    min_scan_growth: float,
) -> int:
    failed = 0
    for name, expected in SELF_TEST_CASES:
        path = FIXTURE_DIR / name
        if not path.is_file():
            print(f"FAIL {name}: fixture missing at {path}", file=sys.stderr)
            failed += 1
            continue
        stdout = io.StringIO()
        stderr = io.StringIO()
        with redirect_stdout(stdout), redirect_stderr(stderr):
            code = run_check(
                path,
                max_indexed_growth=max_indexed_growth,
                min_scan_vs_win=min_scan_vs_win,
                min_materialize_vs_inline=min_materialize_vs_inline,
                min_scan_growth=min_scan_growth,
            )
        if code == expected:
            print(f"ok   {name} (exit {code})")
        else:
            print(f"FAIL {name}: expected exit {expected}, got {code}", file=sys.stderr)
            captured_out = stdout.getvalue().strip()
            captured_err = stderr.getvalue().strip()
            if captured_out:
                print(captured_out, file=sys.stderr)
            if captured_err:
                print(captured_err, file=sys.stderr)
            failed += 1
    if failed:
        print(f"{failed} fixture case(s) failed", file=sys.stderr)
        return 1
    print("all benchmark-shape fixtures passed")
    return 0


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "json_path",
        nargs="?",
        type=Path,
        help="Google Benchmark --benchmark_out JSON file",
    )
    parser.add_argument(
        "--self-test",
        action="store_true",
        help="run fixture cases under tests/benchmark_shape/",
    )
    parser.add_argument("--max-indexed-growth", type=float, default=DEFAULT_MAX_INDEXED_GROWTH)
    parser.add_argument("--min-scan-vs-win", type=float, default=DEFAULT_MIN_SCAN_VS_WIN)
    parser.add_argument(
        "--min-materialize-vs-inline",
        type=float,
        default=DEFAULT_MIN_MATERIALIZE_VS_INLINE,
    )
    parser.add_argument("--min-scan-growth", type=float, default=DEFAULT_MIN_SCAN_GROWTH)
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    kwargs = dict(
        max_indexed_growth=args.max_indexed_growth,
        min_scan_vs_win=args.min_scan_vs_win,
        min_materialize_vs_inline=args.min_materialize_vs_inline,
        min_scan_growth=args.min_scan_growth,
    )
    if args.self_test:
        return run_self_test(**kwargs)
    if args.json_path is None:
        print("error: json_path is required unless --self-test", file=sys.stderr)
        return 2
    return run_check(args.json_path, **kwargs)


if __name__ == "__main__":
    sys.exit(main())
