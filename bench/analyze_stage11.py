#!/usr/bin/env python3
"""AI-CODE-BEGIN: S11-RESOURCE-ANALYZER

把资源 CSV 和一批压测 JSON 汇总成机器可判定的阶段11报告。
首尾使用中位数而不是单点，避免某一次采样抖动造成误判。
"""

from __future__ import annotations

import argparse
import csv
import json
import statistics
from pathlib import Path
from typing import Any


def load_samples(path: Path) -> list[dict[str, float]]:
    with path.open(encoding="utf-8", newline="") as source:
        rows = []
        for row in csv.DictReader(source):
            rows.append({key: float(value) for key, value in row.items()})
    if len(rows) < 4:
        raise ValueError("resource sampler produced fewer than four samples")
    return rows


def load_runs(path: Path) -> list[dict[str, Any]]:
    runs = []
    with path.open(encoding="utf-8") as source:
        for line_number, line in enumerate(source, start=1):
            if line.strip():
                try:
                    runs.append(json.loads(line))
                except json.JSONDecodeError as error:
                    raise ValueError(
                        f"invalid load JSON at line {line_number}: {error}"
                    ) from error
    if not runs:
        raise ValueError("no load-generator runs were recorded")
    return runs


def median_window(rows: list[dict[str, float]], field: str, head: bool) -> float:
    window_size = max(2, min(60, len(rows) // 10))
    window = rows[:window_size] if head else rows[-window_size:]
    return statistics.median(row[field] for row in window)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--resources", type=Path, required=True)
    parser.add_argument("--runs", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--minimum-duration-seconds", type=int, default=0)
    arguments = parser.parse_args()

    samples = load_samples(arguments.resources)
    runs = load_runs(arguments.runs)
    elapsed = samples[-1]["timestamp_epoch"] - samples[0]["timestamp_epoch"]
    successes = sum(run["results"]["successes"] for run in runs)
    failures = sum(run["results"]["failures"] for run in runs)

    rss_head = median_window(samples, "rss_kib", True)
    rss_tail = median_window(samples, "rss_kib", False)
    fd_head = median_window(samples, "fd_count", True)
    fd_tail = median_window(samples, "fd_count", False)
    thread_head = median_window(samples, "threads", True)
    thread_tail = median_window(samples, "threads", False)

    # 允许 allocator 缓存带来 16 MiB 或 10% 的稳定平台抬升；超过后才判为持续增长嫌疑。
    rss_allowance = max(16384.0, rss_head * 0.10)
    checks = {
        "duration_reached": elapsed >= arguments.minimum_duration_seconds,
        "no_request_failures": failures == 0,
        "rss_not_sustained_growth": rss_tail <= rss_head + rss_allowance,
        "fd_not_sustained_growth": fd_tail <= fd_head + 10,
        "threads_not_sustained_growth": thread_tail <= thread_head + 2,
    }
    report = {
        "schema_version": 1,
        "duration_seconds": elapsed,
        "load_runs": len(runs),
        "requests": {"successes": successes, "failures": failures},
        "resources": {
            "samples": len(samples),
            "cpu_percent_peak": max(row["cpu_percent"] for row in samples),
            "rss_kib": {
                "head_median": rss_head,
                "tail_median": rss_tail,
                "peak": max(row["rss_kib"] for row in samples),
            },
            "fd_count": {
                "head_median": fd_head,
                "tail_median": fd_tail,
                "peak": max(row["fd_count"] for row in samples),
            },
            "threads": {
                "head_median": thread_head,
                "tail_median": thread_tail,
                "peak": max(row["threads"] for row in samples),
            },
        },
        "checks": checks,
        "pass": all(checks.values()),
    }
    arguments.output.parent.mkdir(parents=True, exist_ok=True)
    arguments.output.write_text(
        json.dumps(report, indent=2, ensure_ascii=False) + "\n", encoding="utf-8"
    )
    print(json.dumps(report, indent=2, ensure_ascii=False))
    return 0 if report["pass"] else 1


if __name__ == "__main__":
    raise SystemExit(main())

# AI-CODE-END: S11-RESOURCE-ANALYZER
