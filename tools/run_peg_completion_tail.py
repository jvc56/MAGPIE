#!/usr/bin/env python3
"""Measure uncensored PEG candidate-completion tails, strictly sequentially.

Every position gets an explicit two-candidate 2-ply admission wave. Bag-2/3
positions also get a separate trace through 16 completed 2-ply candidates and
two completed 3-ply candidates. Keeping the traces separate matters because a
16-candidate stage may have concurrent work in flight at its first completion
and therefore cannot measure the cost of admitting only two candidates.
Identical eight-candidate sentinels are inserted throughout the stable
bag-interleaved order.
"""

from __future__ import annotations

import argparse
import datetime as dt
import json
import os
import pathlib
import platform
import shutil
import subprocess
import sys
from typing import Any

from run_peg_time_calibration import (
    SequentialRunner,
    append_jsonl,
    has_summary,
    latest_arm,
    load_records,
    read_panel,
    sha256,
)


FIRST2_LABEL = "first2_2ply"
DEEP_LABEL = "deep_16_2"
LEGACY_LABEL = "completion_tail"
SENTINEL_EVERY = 32
FIRST2_SCHEDULE = [2]
DEEP_SCHEDULE = [16, 2]


def resolve(repo: pathlib.Path, path: pathlib.Path) -> pathlib.Path:
    return path.resolve() if path.is_absolute() else (repo / path).resolve()


def interleave_by_bag(
    positions: list[dict[str, Any]],
) -> list[dict[str, Any]]:
    by_bag = {
        bag: [position for position in positions if position["bag"] == bag]
        for bag in range(1, 5)
    }
    per_bag = len(by_bag[1])
    if any(len(by_bag[bag]) != per_bag for bag in range(1, 5)):
        raise ValueError("completion-tail panel must be balanced by bag")
    ordered = []
    for index in range(per_bag):
        bag_order = (1, 2, 3, 4) if index % 2 == 0 else (4, 3, 2, 1)
        ordered.extend(by_bag[bag][index] for bag in bag_order)
    return ordered


def arm_is_complete(arm: dict[str, Any] | None, schedule: list[int]) -> bool:
    return bool(
        arm is not None
        and arm.get("status") == "completed"
        and int(arm.get("requested_stages", -1)) == len(schedule)
        and int(arm.get("deepest_candidate_total", -1)) == schedule[-1]
        and int(arm.get("deepest_completed_candidates", -1)) == schedule[-1]
    )


def completed_arm(
    records: list[dict[str, Any]],
    position: dict[str, Any],
    label: str,
    schedule: list[int],
) -> dict[str, Any] | None:
    arm = latest_arm(records, str(position["position"]), label)
    if arm_is_complete(arm, schedule):
        return arm
    # Reuse the first ten traces written before the explicit-wave distinction
    # was added. Their exact schedule is recoverable from the arm summary.
    legacy = latest_arm(records, str(position["position"]), LEGACY_LABEL)
    if arm_is_complete(legacy, schedule):
        return legacy
    return arm


def position_is_complete(
    records: list[dict[str, Any]], position: dict[str, Any]
) -> bool:
    first2 = completed_arm(
        records, position, FIRST2_LABEL, FIRST2_SCHEDULE
    )
    if not arm_is_complete(first2, FIRST2_SCHEDULE):
        return False
    if int(position["bag"]) not in {2, 3}:
        return True
    deep = completed_arm(records, position, DEEP_LABEL, DEEP_SCHEDULE)
    return arm_is_complete(deep, DEEP_SCHEDULE)


def create_manifest(
    repo: pathlib.Path,
    panel: pathlib.Path,
    positions: list[dict[str, Any]],
    binary: pathlib.Path,
    output_dir: pathlib.Path,
    threads: int,
) -> None:
    assets = {}
    for relative in (
        "data/lexica/CSW24.kwg",
        "data/lexica/CSW24.klv2",
        "data/lexica/CSW24.wmp",
        "data/lexica/CSW24.rit",
    ):
        assets[relative] = sha256(repo / relative)
    git_head = subprocess.check_output(
        ["git", "rev-parse", "HEAD"], cwd=repo, text=True
    ).strip()
    manifest = {
        "schema_version": 2,
        "created_utc": dt.datetime.now(dt.timezone.utc).isoformat(),
        "git_head": git_head,
        "command": [sys.executable, *sys.argv],
        "platform": platform.platform(),
        "python": sys.version,
        "panel": str(panel),
        "panel_sha256": sha256(panel),
        "binary": str(binary),
        "binary_sha256": sha256(binary),
        "positions": len(positions),
        "per_bag": len(positions) // 4,
        "order": "bag-interleaved; alternating 1,2,3,4 and 4,3,2,1",
        "traces": {
            FIRST2_LABEL: {
                "bags": [1, 2, 3, 4],
                "schedule": FIRST2_SCHEDULE,
                "purpose": "cost of admitting an initial wave of exactly two",
            },
            DEEP_LABEL: {
                "bags": [2, 3],
                "schedule": DEEP_SCHEDULE,
                "purpose": "full 2-ply {16} boundary then first two at 3 ply",
            },
        },
        "budget_seconds": 0,
        "censoring_policy": "unbounded; rerun any incomplete trace to completion",
        "portable_boundary": "completed candidate",
        "sentinel": {
            "source_position": next(
                position["position"]
                for position in positions
                if position["bag"] == 2
            ),
            "schedule": [8],
            "interval_positions": SENTINEL_EVERY,
        },
        "threads": threads,
        "detected_hardware_concurrency": os.cpu_count(),
        "build": "no_pgo_release",
        "lexicon": "CSW24",
        "rit": True,
        "asset_sha256": assets,
    }
    (output_dir / "manifest.json").write_text(
        json.dumps(manifest, indent=2, sort_keys=True) + "\n"
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--panel",
        type=pathlib.Path,
        default=pathlib.Path(
            "tools/peg_time_calibration/admission_positions_20260730.tsv"
        ),
    )
    parser.add_argument(
        "--binary", type=pathlib.Path, default=pathlib.Path("bin/magpie_test")
    )
    parser.add_argument("--output-dir", type=pathlib.Path, required=True)
    parser.add_argument("--threads", type=int, default=os.cpu_count())
    parser.add_argument(
        "--max-positions",
        type=int,
        help="run only this many positions in stable order; resume without it",
    )
    parser.add_argument("--position", help="run or resume one position")
    args = parser.parse_args()
    if args.threads is None or args.threads < 1:
        raise ValueError("--threads must be positive")
    if args.max_positions is not None and args.max_positions < 1:
        raise ValueError("--max-positions must be positive")
    if args.position is not None and args.max_positions is not None:
        raise ValueError("--position and --max-positions are mutually exclusive")

    repo = pathlib.Path(__file__).resolve().parent.parent
    panel = resolve(repo, args.panel)
    binary = resolve(repo, args.binary)
    output_dir = resolve(repo, args.output_dir)
    if not binary.exists():
        raise FileNotFoundError(
            f"{binary} is missing; build with "
            "`make -j10 magpie_test BUILD=no_pgo_release`"
        )
    output_dir.mkdir(parents=True, exist_ok=True)
    free_bytes = shutil.disk_usage(output_dir).free
    if free_bytes < 5 * 1024**3:
        raise RuntimeError(f"only {free_bytes / 1024**3:.1f} GiB free")

    positions = read_panel(panel)
    ordered = interleave_by_bag(positions)
    by_id = {str(position["position"]): position for position in positions}
    if args.position is not None:
        if args.position not in by_id:
            raise ValueError(f"unknown completion-tail position {args.position}")
        selected = [by_id[args.position]]
    elif args.max_positions is not None:
        selected = ordered[: args.max_positions]
    else:
        selected = ordered

    records_path = output_dir / "records.jsonl"
    raw_path = output_dir / "raw.log"
    records = load_records(records_path)
    create_manifest(
        repo, panel, positions, binary, output_dir, args.threads
    )
    runner = SequentialRunner(
        binary,
        output_dir,
        records_path,
        raw_path,
        records,
        threads=args.threads,
    )
    sentinel = dict(
        next(position for position in positions if position["bag"] == 2)
    )
    sentinel["position"] = "completion-tail-sentinel"

    def run_sentinel(ordinal: int) -> None:
        label = f"sentinel-{ordinal:04d}"
        if has_summary(runner.records, "arm", sentinel["position"], label):
            return
        runner.run(
            position=sentinel,
            mode="sentinel",
            label=label,
            budget=0.0,
            block=f"after-{ordinal}",
        )

    if args.position is None:
        run_sentinel(0)
    for position in selected:
        ordinal = ordered.index(position) + 1
        trace_specs = [(FIRST2_LABEL, FIRST2_SCHEDULE)]
        if int(position["bag"]) in {2, 3}:
            trace_specs.append((DEEP_LABEL, DEEP_SCHEDULE))
        for label, schedule in trace_specs:
            prior = completed_arm(
                runner.records, position, label, schedule
            )
            if arm_is_complete(prior, schedule):
                continue
            if prior is not None:
                censor = {
                    "kind": "censored_attempt",
                    "position": position["position"],
                    "bag": position["bag"],
                    "label": label,
                    "prior_sequence": prior["sequence"],
                    "reason": "incomplete_trace_resumed_unbounded",
                }
                append_jsonl(records_path, censor)
                runner.records.append(censor)
            runner.run(
                position=position,
                mode="arm",
                label=label,
                budget=0.0,
                block=f"quartile-{min(4, (ordinal - 1) * 4 // len(ordered) + 1)}",
                schedule=schedule,
            )
        if args.position is None and ordinal % SENTINEL_EVERY == 0:
            run_sentinel(ordinal)
    if args.position is None and len(selected) == len(ordered):
        run_sentinel(len(ordered))
    print(
        f"completion-tail progress="
        f"{sum(position_is_complete(runner.records, p) for p in ordered)}/"
        f"{len(ordered)} "
        f"artifacts={output_dir}",
        flush=True,
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
