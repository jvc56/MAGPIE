#!/usr/bin/env python3
"""Check throughput stability and CPU scheduling in analysis-trace TSVs.

Strength curves should be fitted against portable work counters. Wall time is
still needed to calibrate how quickly a particular machine can buy that work.
This tool separates the two and reports whether repeated calibration runs were
quiet enough to compare.
"""

from __future__ import annotations

import argparse
import csv
import math
import statistics
from dataclasses import dataclass
from pathlib import Path


@dataclass(frozen=True)
class RunRate:
    order: int
    source_index: int
    run_id: str
    ordinal: int
    mode: str
    workers: int
    work_kind: str
    work: int
    elapsed_seconds: float
    cpu_seconds: float

    @property
    def rate(self) -> float:
        return self.work / self.elapsed_seconds

    @property
    def scheduled_cores(self) -> float:
        return self.cpu_seconds / self.elapsed_seconds

    @property
    def core_fraction(self) -> float:
        if self.workers <= 0:
            return math.nan
        return self.scheduled_cores / self.workers


def robust_cv(values: list[float]) -> float:
    if not values:
        return math.nan
    median = statistics.median(values)
    if median == 0.0:
        return math.nan
    mad = statistics.median(abs(value - median) for value in values)
    return 1.4826 * mad / abs(median)


def half_drift(values: list[float]) -> float:
    half = len(values) // 2
    if half == 0:
        return math.nan
    early = statistics.median(values[:half])
    if early == 0.0:
        return math.nan
    late = statistics.median(values[-half:])
    return late / early - 1.0


def load_runs(paths: list[Path]) -> list[RunRate]:
    rates: list[RunRate] = []
    order = 0
    for source_index, path in enumerate(paths):
        by_run: dict[str, list[dict[str, str]]] = {}
        with path.open(newline="", encoding="utf-8") as stream:
            reader = csv.DictReader(stream, delimiter="\t")
            required = {"run_id", "mode", "elapsed_ns", "cpu_ns", "workers"}
            if reader.fieldnames is None or not required.issubset(
                reader.fieldnames
            ):
                missing = sorted(required - set(reader.fieldnames or []))
                raise ValueError(f"{path}: missing trace columns {missing}")
            for row in reader:
                by_run.setdefault(row["run_id"], []).append(row)

        ordered_runs = sorted(
            by_run.items(),
            key=lambda item: min(int(row["sequence"]) for row in item[1]),
        )
        for ordinal, (run_id, rows) in enumerate(ordered_runs):
            starts = [row for row in rows if row["event"] == "start"]
            metadata = starts[0] if starts else rows[0]
            mode = metadata["mode"]
            workers = int(metadata["workers"])
            finishes = [row for row in rows if row["event"] == "finish"]
            endpoint = (
                max(finishes, key=lambda row: int(row["elapsed_ns"]))
                if finishes
                else max(rows, key=lambda row: int(row["elapsed_ns"]))
            )
            elapsed = int(endpoint["elapsed_ns"]) / 1e9
            cpu = int(endpoint["cpu_ns"]) / 1e9
            if elapsed <= 0.0 or cpu < 0.0:
                continue
            work_fields: tuple[tuple[str, str], ...]
            if mode == "sim":
                work_fields = (("nodes", "nodes"),)
                if int(endpoint["nodes"]) == 0:
                    work_fields = (("iterations", "iterations"),)
            elif mode == "endgame":
                work_fields = (("nodes", "nodes"),)
            elif mode == "peg":
                # PEG has two non-interchangeable coordinates. Report both;
                # a future cost fit can use them jointly.
                work_fields = (("nodes", "endgame_nodes"), ("scenarios", "scenarios"))
            else:
                continue
            for field, label in work_fields:
                work = int(endpoint[field])
                if work <= 0:
                    continue
                rates.append(
                    RunRate(
                        order=order,
                        source_index=source_index,
                        run_id=run_id,
                        ordinal=ordinal,
                        mode=mode,
                        workers=workers,
                        work_kind=label,
                        work=work,
                        elapsed_seconds=elapsed,
                        cpu_seconds=cpu,
                    )
                )
            order += 1
    return rates


def normalized_matched_rates(
    runs: list[RunRate], match_by: str
) -> tuple[list[float], int]:
    if match_by == "none":
        return [run.rate for run in runs], len(runs)
    matched: dict[str | int, list[RunRate]] = {}
    for run in runs:
        key: str | int = run.run_id if match_by == "run-id" else run.ordinal
        matched.setdefault(key, []).append(run)
    normalized: list[tuple[int, float]] = []
    workload_count = 0
    for matches in matched.values():
        # A repeated workload must occur in at least two input traces. Multiple
        # records from one trace are not independent quietness sentinels.
        if len({run.source_index for run in matches}) < 2:
            continue
        workload_count += 1
        center = statistics.median(run.rate for run in matches)
        if center <= 0.0:
            continue
        normalized.extend((run.order, run.rate / center) for run in matches)
    normalized.sort()
    return [value for _, value in normalized], workload_count


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("traces", nargs="+", type=Path)
    parser.add_argument("--max-rate-cv", type=float, default=0.10)
    parser.add_argument("--max-rate-drift", type=float, default=0.10)
    parser.add_argument("--max-core-cv", type=float, default=0.10)
    parser.add_argument(
        "--match-by",
        choices=("run-id", "ordinal", "none"),
        default="run-id",
        help=(
            "normalize only repeated identical workloads across input traces; "
            "'none' is valid only when every run is intentionally homogeneous"
        ),
    )
    parser.add_argument(
        "--min-core-fraction",
        type=float,
        default=0.0,
        help="optional minimum process-CPU/wall/workers ratio",
    )
    args = parser.parse_args()

    grouped: dict[tuple[str, int, str], list[RunRate]] = {}
    for run in load_runs(args.traces):
        grouped.setdefault((run.mode, run.workers, run.work_kind), []).append(
            run
        )

    if not grouped:
        raise ValueError("no completed solver runs with usable work")

    failed = False
    for (mode, workers, work_kind), runs in sorted(grouped.items()):
        runs.sort(key=lambda run: run.order)
        rates = [run.rate for run in runs]
        normalized_rates, matched_workloads = normalized_matched_rates(
            runs, args.match_by
        )
        cores = [run.scheduled_cores for run in runs]
        core_fractions = [
            run.core_fraction for run in runs if math.isfinite(run.core_fraction)
        ]
        raw_rate_cv = robust_cv(rates)
        rate_cv = robust_cv(normalized_rates)
        core_cv = robust_cv(cores)
        drift = half_drift(normalized_rates)
        enough = len(normalized_rates) >= 3 and (
            args.match_by == "none" or matched_workloads > 0
        )
        quiet = (
            enough
            and rate_cv <= args.max_rate_cv
            and abs(drift) <= args.max_rate_drift
            and core_cv <= args.max_core_cv
            and (
                args.min_core_fraction <= 0.0
                or (
                    core_fractions
                    and statistics.median(core_fractions)
                    >= args.min_core_fraction
                )
            )
        )
        if enough and not quiet:
            failed = True
        status = "yes" if quiet else ("unknown" if not enough else "no")
        median_core_fraction = (
            statistics.median(core_fractions) if core_fractions else math.nan
        )
        print(
            "WORK_RATE "
            f"mode={mode} workers={workers} unit={work_kind} runs={len(runs)} "
            f"median_per_second={statistics.median(rates):.3f} "
            f"raw_rate_cv={raw_rate_cv:.4f} "
            f"rate_basis={args.match_by} "
            f"matched_workloads={matched_workloads} "
            f"robust_rate_cv={rate_cv:.4f} "
            f"late_vs_early={drift:+.4f} "
            f"median_scheduled_cores={statistics.median(cores):.3f} "
            f"median_core_fraction={median_core_fraction:.4f} "
            f"robust_core_cv={core_cv:.4f} quiet={status}"
        )

    if failed:
        raise SystemExit(2)


if __name__ == "__main__":
    main()
