#!/usr/bin/env python3
"""Measure PEG value at candidate-completion checkpoints.

Each position gets one uncapped 32-candidate 2-ply trace. The record-setting
checkpoint nominees from that trace are then judged together at direct 3 ply,
so every fixed candidate-count stopping policy uses a common deterministic
root-scenario sample. Execution is strictly sequential.
"""

from __future__ import annotations

import argparse
import datetime as dt
import json
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


DEFAULT_PANELS = (
    "tools/peg_time_calibration/heldout_positions.tsv",
    "tools/peg_time_calibration/heldout_positions_expansion.tsv",
)
DEFAULT_SENSITIVITY_RECORDS = (
    "obj/peg_time_calibration/run-20260729/records.jsonl",
    "obj/peg_time_calibration/run-20260729-expansion/records.jsonl",
)
TRACE_LABEL = "checkpoint"
STRIDE4_LABEL = "checkpoint_stride4"
STRIDE2_LABEL = "checkpoint_stride2"


def resolve(repo: pathlib.Path, path: pathlib.Path) -> pathlib.Path:
    return path.resolve() if path.is_absolute() else (repo / path).resolve()


def arm_events(
    records: list[dict[str, Any]], arm: dict[str, Any], stage: int
) -> list[dict[str, Any]]:
    sequence = int(arm["sequence"])
    return sorted(
        (
            record
            for record in records
            if record.get("kind") == "event"
            and int(record.get("sequence", -1)) == sequence
            and int(record.get("stage", -1)) == stage
        ),
        key=lambda record: int(record["rank"]),
    )


def build_checkpoint_map(
    records: list[dict[str, Any]],
    arm: dict[str, Any],
    position: dict[str, Any],
) -> dict[str, Any]:
    greedy_events = arm_events(records, arm, 0)
    events = arm_events(records, arm, 1)
    if not greedy_events or not events:
        raise RuntimeError(
            f"{position['position']}: trace is missing greedy or 2-ply events"
        )
    greedy_boundary = max(
        greedy_events, key=lambda event: float(event["completed_seconds"])
    )
    for expected_rank, event in enumerate(events):
        if int(event["rank"]) != expected_rank:
            raise RuntimeError(
                f"{position['position']}: non-contiguous candidate ranks"
            )

    best = events[0]
    frontier_moves = [str(best["move"])]
    checkpoints = []
    for completed in range(0, len(events) + 1):
        if completed > 0:
            event = events[completed - 1]
            event_key = float(event["win"]) + 1.0e-4 * float(event["spread"])
            best_key = float(best["win"]) + 1.0e-4 * float(best["spread"])
            if event_key > best_key:
                best = event
                move = str(best["move"])
                if move not in frontier_moves:
                    frontier_moves.append(move)
        boundary = greedy_boundary if completed == 0 else events[completed - 1]
        checkpoints.append(
            {
                "completed_candidates": completed,
                "move": str(best["move"]),
                "cumulative_scenarios": int(boundary["cumulative_scenarios"]),
                "nested_endgame_nodes": int(
                    boundary["nested_endgame_nodes"]
                ),
                "completed_seconds": float(boundary["completed_seconds"]),
                "process_cpu_seconds": float(
                    boundary["process_cpu_seconds"]
                ),
            }
        )

    returned_move = str(arm["move"])
    if returned_move != str(best["move"]):
        # A numerically exact self-utility tie may be ordered differently by
        # qsort. Include the actually published move in the common judge.
        if returned_move not in frontier_moves:
            frontier_moves.append(returned_move)
        checkpoints[-1]["move"] = returned_move
    return {
        "kind": "checkpoint_map",
        "position": position["position"],
        "bag": position["bag"],
        "arm_sequence": int(arm["sequence"]),
        "completed_candidates": len(events),
        "candidate_total": int(arm["refine_candidate_total"]),
        "full_stage": int(arm.get("status") == "completed"),
        "greedy_move": str(events[0]["move"]),
        "final_move": returned_move,
        "frontier_moves": frontier_moves,
        "checkpoints": checkpoints,
    }


def latest_checkpoint_map(
    records: list[dict[str, Any]], position: str
) -> dict[str, Any] | None:
    matches = [
        record
        for record in records
        if record.get("kind") == "checkpoint_map"
        and record.get("position") == position
    ]
    return matches[-1] if matches else None


def create_manifest(
    repo: pathlib.Path,
    panels: list[pathlib.Path],
    positions: list[dict[str, Any]],
    sensitivity_records: list[pathlib.Path],
    sensitivity_positions: set[str],
    output_dir: pathlib.Path,
    oracle_budget: float,
) -> None:
    assets = {}
    for relative in (
        "data/lexica/CSW24.kwg",
        "data/lexica/CSW24.klv2",
        "data/lexica/CSW24.wmp",
        "data/lexica/CSW24.rit",
    ):
        assets[relative] = sha256(repo / relative)
    git_head = subprocess.run(
        ["git", "rev-parse", "HEAD"],
        cwd=repo,
        check=True,
        capture_output=True,
        text=True,
    ).stdout.strip()
    manifest = {
        "created_utc": dt.datetime.now(dt.timezone.utc).isoformat(),
        "git_head": git_head,
        "platform": platform.platform(),
        "python": sys.version,
        "panels": [
            {"path": str(panel), "sha256": sha256(panel)} for panel in panels
        ],
        "source_sensitivity_records": [
            {"path": str(path), "sha256": sha256(path)}
            for path in sensitivity_records
        ],
        "positions": [
            {"position": position["position"], "bag": position["bag"]}
            for position in positions
        ],
        "trace": {
            "fidelity": "2-ply",
            "candidate_total": 32,
            "budget_seconds": 0,
            "portable_boundary": "completed candidate",
        },
        "oracle": {
            "fidelity": "direct 3-ply",
            "stride": 4,
            "budget_seconds": oracle_budget,
            "common_sample": "deterministic weight-strata boundaries",
            "nominees": "every distinct checkpoint frontier move",
        },
        "sensitivity": {
            "stride": 2,
            "selection": "positions accepted in the original stride-2 subset",
            "positions": sorted(sensitivity_positions),
        },
        "threads": 10,
        "build": "no_pgo_release",
        "rit": True,
        "asset_sha256": assets,
    }
    (output_dir / "manifest.json").write_text(
        json.dumps(manifest, indent=2, sort_keys=True) + "\n"
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--panel", action="append", type=pathlib.Path)
    parser.add_argument(
        "--sensitivity-records", action="append", type=pathlib.Path
    )
    parser.add_argument(
        "--binary", type=pathlib.Path, default=pathlib.Path("bin/magpie_test")
    )
    parser.add_argument("--output-dir", type=pathlib.Path, required=True)
    parser.add_argument("--oracle-budget", type=float, default=600.0)
    args = parser.parse_args()

    repo = pathlib.Path(__file__).resolve().parent.parent
    panels = [
        resolve(repo, path)
        for path in (
            args.panel
            if args.panel is not None
            else [pathlib.Path(path) for path in DEFAULT_PANELS]
        )
    ]
    sensitivity_records = [
        resolve(repo, path)
        for path in (
            args.sensitivity_records
            if args.sensitivity_records is not None
            else [pathlib.Path(path) for path in DEFAULT_SENSITIVITY_RECORDS]
        )
    ]
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

    positions = [
        position for panel in panels for position in read_panel(panel)
    ]
    position_ids = [str(position["position"]) for position in positions]
    if len(position_ids) != len(set(position_ids)):
        raise ValueError("checkpoint panels contain duplicate position IDs")
    bag_counts = {
        bag: sum(int(position["bag"]) == bag for position in positions)
        for bag in range(1, 5)
    }
    if len(set(bag_counts.values())) != 1:
        raise ValueError(f"combined panels are not balanced: {bag_counts}")

    original_records = [
        record
        for path in sensitivity_records
        for record in load_records(path)
    ]
    sensitivity_positions = {
        str(record["position"])
        for record in original_records
        if record.get("kind") == "judge"
        and record.get("label") == "stride2"
        and int(record.get("accepted", 0)) == 1
    }

    records_path = output_dir / "records.jsonl"
    raw_path = output_dir / "raw.log"
    records = load_records(records_path)
    create_manifest(
        repo,
        panels,
        positions,
        sensitivity_records,
        sensitivity_positions,
        output_dir,
        args.oracle_budget,
    )
    runner = SequentialRunner(binary, output_dir, records_path, raw_path, records)

    sentinel_source = next(
        (position for position in positions if position["bag"] == 2),
        positions[0],
    )
    sentinel = dict(sentinel_source)
    sentinel["position"] = "checkpoint-sentinel"

    def run_sentinel(label: str) -> None:
        if has_summary(runner.records, "arm", sentinel["position"], label):
            return
        runner.run(
            position=sentinel,
            mode="sentinel",
            label=label,
            budget=0.0,
            block=label,
        )

    run_sentinel("before")
    split = len(positions) // 2
    for position_index, position in enumerate(positions):
        block = "early" if position_index < split else "late"
        if position_index == split:
            run_sentinel("between")

        arm = latest_arm(runner.records, position["position"], TRACE_LABEL)
        if arm is None:
            runner.run(
                position=position,
                mode="arm",
                label=TRACE_LABEL,
                budget=0.0,
                block=block,
            )
            arm = latest_arm(runner.records, position["position"], TRACE_LABEL)
        if arm is None:
            append_jsonl(
                records_path,
                {
                    "kind": "censored_position",
                    "position": position["position"],
                    "bag": position["bag"],
                    "reason": "missing_checkpoint_trace",
                    "block": block,
                },
            )
            continue

        checkpoint_map = latest_checkpoint_map(
            runner.records, position["position"]
        )
        if checkpoint_map is None or int(checkpoint_map["arm_sequence"]) != int(
            arm["sequence"]
        ):
            checkpoint_map = build_checkpoint_map(
                runner.records, arm, position
            )
            checkpoint_map["block"] = block
            append_jsonl(records_path, checkpoint_map)
            runner.records.append(checkpoint_map)

        moves = [str(move) for move in checkpoint_map["frontier_moves"]]
        if len(moves) == 1:
            if not has_summary(
                runner.records,
                "agreement",
                position["position"],
                "checkpoint_exact",
            ):
                agreement = {
                    "kind": "agreement",
                    "position": position["position"],
                    "bag": position["bag"],
                    "label": "checkpoint_exact",
                    "move": moves[0],
                    "accepted": 1,
                    "pair_regret": 0.0,
                    "block": block,
                }
                append_jsonl(records_path, agreement)
                runner.records.append(agreement)
            continue

        labels = [STRIDE4_LABEL]
        if position["position"] in sensitivity_positions:
            labels.append(STRIDE2_LABEL)
        if position_index % 2 == 1:
            labels.reverse()
        for label in labels:
            if has_summary(
                runner.records, "judge", position["position"], label
            ):
                continue
            stride = 2 if label == STRIDE2_LABEL else 4
            runner.run(
                position=position,
                mode="judge",
                label=label,
                budget=args.oracle_budget,
                block=block,
                moves=moves,
                stride=stride,
            )

    run_sentinel("after")
    print(f"complete artifacts={output_dir}", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
