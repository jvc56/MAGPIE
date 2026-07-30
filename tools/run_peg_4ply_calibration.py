#!/usr/bin/env python3
"""Run a strictly sequential PEG 3-ply/4-ply depth-versus-width sweep."""

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

from run_peg_stage_cap_calibration import (
    arm_stage,
    best_event,
    endpoint_work,
    find_event,
    incremental_work,
    latest_judge,
    resolve,
)
from run_peg_time_calibration import (
    SequentialRunner,
    append_jsonl,
    has_summary,
    latest_arm,
    load_records,
    read_panel,
    sha256,
)


DEFAULT_PANEL = "tools/peg_time_calibration/heldout_positions.tsv"
DEFAULT_SENSITIVITY_RECORDS = (
    "obj/peg_time_calibration/run-20260729/records.jsonl",
    "obj/peg_time_calibration/run-20260729-expansion/records.jsonl",
)
SCHEDULES = {
    "narrow4": [16, 8, 4],
    "wide4": [24, 12, 6],
}
ARM_ORDER = ("greedy", *SCHEDULES)
STRIDE4_LABEL = "direct4_stride4"
STRIDE2_LABEL = "direct4_stride2"
STRIDE1_LABEL = "direct4_stride1"


def stage_complete(stage: dict[str, Any] | None) -> bool:
    return bool(
        stage
        and int(stage["completed_candidates"]) == int(stage["candidate_total"])
        and float(stage["end_seconds"]) > 0.0
    )


def build_depth_map(
    records: list[dict[str, Any]], position: dict[str, Any]
) -> dict[str, Any]:
    greedy_arm = latest_arm(records, position["position"], "greedy")
    if greedy_arm is None:
        raise RuntimeError(f"{position['position']}: missing greedy arm")
    endpoints = [
        {
            "label": "greedy",
            "arm_label": "greedy",
            "stage": 0,
            "fidelity": 0,
            "move": str(greedy_arm["move"]),
            "accepted": 1,
            "status": str(greedy_arm["status"]),
            "candidate_cap": None,
            "schedule": [],
            "cumulative_scenarios": int(
                greedy_arm["cumulative_scenarios"]
            ),
            "nested_endgame_nodes": int(
                greedy_arm["nested_endgame_nodes"]
            ),
            "completed_seconds": float(greedy_arm["wall_seconds"]),
            "process_cpu_seconds": float(
                greedy_arm["process_cpu_seconds"]
            ),
        }
    ]
    arm_sequences = {"greedy": int(greedy_arm["sequence"])}

    for arm_label, schedule in SCHEDULES.items():
        arm = latest_arm(records, position["position"], arm_label)
        if arm is None:
            continue
        arm_sequences[arm_label] = int(arm["sequence"])
        stage_rows: dict[int, dict[str, Any] | None] = {}
        stage_events: dict[int, list[dict[str, Any]]] = {}
        for stage_idx in range(4):
            stage_rows[stage_idx], stage_events[stage_idx] = arm_stage(
                records, arm, stage_idx
            )

        previous_work = (
            endpoint_work(stage_events[0]) if stage_events[0] else None
        )
        for stage_idx, cap in enumerate(schedule, start=1):
            events = stage_events[stage_idx]
            if not events:
                continue
            complete = stage_complete(stage_rows[stage_idx])
            next_events = stage_events.get(stage_idx + 1, [])
            if next_events:
                # The next stage receives this stage's actual qsort winner as
                # its rank-0 input, resolving exact self-value ties.
                move = str(next_events[0]["move"])
            elif complete and stage_idx == len(schedule):
                move = str(arm["move"])
            else:
                move = str(best_event(events)["move"])
            value = find_event(events, move)
            if value is None:
                value = best_event(events)
                move = str(value["move"])
            work = endpoint_work(events)
            endpoint = {
                "label": f"{arm_label}_{stage_idx + 1}ply",
                "arm_label": arm_label,
                "stage": stage_idx,
                "fidelity": stage_idx + 1,
                "move": move,
                "self_win": float(value["win"]),
                "self_spread": float(value["spread"]),
                "accepted": int(complete),
                "status": "completed" if complete else "partial",
                "candidate_cap": cap,
                "schedule": schedule,
                "completed_candidates": len(events),
                "candidate_total": (
                    int(stage_rows[stage_idx]["candidate_total"])
                    if stage_rows[stage_idx] is not None
                    else cap
                ),
                **work,
            }
            if previous_work is not None:
                endpoint.update(incremental_work(work, previous_work))
            endpoints.append(endpoint)
            previous_work = work

    return {
        "kind": "peg4_map",
        "schema_version": 1,
        "position": position["position"],
        "bag": int(position["bag"]),
        "arm_sequences": arm_sequences,
        "endpoints": endpoints,
    }


def latest_depth_map(
    records: list[dict[str, Any]], position: str
) -> dict[str, Any] | None:
    matches = [
        record
        for record in records
        if record.get("kind") == "peg4_map"
        and record.get("position") == position
    ]
    return matches[-1] if matches else None


def best_judged_move(
    records: list[dict[str, Any]], position: str, label: str
) -> str | None:
    judge = latest_judge(records, position, label)
    if judge is None or int(judge.get("accepted", 0)) != 1:
        return None
    sequence = int(judge["sequence"])
    values = [
        record
        for record in records
        if record.get("kind") == "value"
        and record.get("position") == position
        and record.get("label") == label
        and int(record.get("sequence", -1)) == sequence
    ]
    if len(values) != int(judge.get("nominee_count", -1)):
        return None
    return str(
        max(
            values,
            key=lambda value: (
                float(value["utility"]),
                float(value["win"]),
                float(value["spread"]),
                str(value["move"]),
            ),
        )["move"]
    )


def create_manifest(
    *,
    repo: pathlib.Path,
    panel: pathlib.Path,
    positions: list[dict[str, Any]],
    sensitivity_records: list[pathlib.Path],
    sensitivity_positions: set[str],
    output_dir: pathlib.Path,
    arm_budget: float,
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
        "panel": {"path": str(panel), "sha256": sha256(panel)},
        "positions": [
            {"position": position["position"], "bag": position["bag"]}
            for position in positions
        ],
        "arms": {
            label: {
                "schedule": schedule,
                "budget_seconds": arm_budget,
                "scenario_stride": 1,
            }
            for label, schedule in SCHEDULES.items()
        },
        "arm_order": "forward/reverse by position",
        "threads": 10,
        "build": "no_pgo_release",
        "rit": True,
        "oracle": {
            "fidelity": "direct 4-ply",
            "stride": 4,
            "budget_seconds": oracle_budget,
            "common_sample": "deterministic weight-strata boundaries",
            "nominees": "distinct accepted stage endpoints",
        },
        "sensitivity": {
            "stride": 2,
            "source_records": [
                {"path": str(path), "sha256": sha256(path)}
                for path in sensitivity_records
            ],
            "positions": sorted(sensitivity_positions),
            "mandatory": (
                "every observed completed depth or width move change"
            ),
            "bag1_full_enumeration": (
                "every observed completed bag-1 3-ply to 4-ply move change"
            ),
        },
        "stage_admission": {
            "minimum_useful_completed_candidates": 2,
            "partial_stage_quality_credit": False,
            "note": (
                "First-two 4-ply work is measured by this run before a "
                "portable admission threshold is proposed."
            ),
        },
        "asset_sha256": assets,
    }
    (output_dir / "manifest.json").write_text(
        json.dumps(manifest, indent=2, sort_keys=True) + "\n"
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--panel", type=pathlib.Path, default=pathlib.Path(DEFAULT_PANEL)
    )
    parser.add_argument(
        "--sensitivity-records", action="append", type=pathlib.Path
    )
    parser.add_argument(
        "--binary", type=pathlib.Path, default=pathlib.Path("bin/magpie_test")
    )
    parser.add_argument("--output-dir", type=pathlib.Path, required=True)
    parser.add_argument("--arm-budget", type=float, default=600.0)
    parser.add_argument("--oracle-budget", type=float, default=1800.0)
    parser.add_argument("--position")
    parser.add_argument(
        "--schedule-label",
        action="append",
        choices=tuple(SCHEDULES),
        help="Run only selected schedules (repeatable); default is all",
    )
    parser.add_argument(
        "--skip-sensitivity", action="store_true", help="Skip stride-2 judges"
    )
    args = parser.parse_args()

    repo = pathlib.Path(__file__).resolve().parent.parent
    panel = resolve(repo, args.panel)
    binary = resolve(repo, args.binary)
    output_dir = resolve(repo, args.output_dir)
    sensitivity_records = [
        resolve(repo, path)
        for path in (
            args.sensitivity_records
            if args.sensitivity_records is not None
            else [
                pathlib.Path(path)
                for path in DEFAULT_SENSITIVITY_RECORDS
            ]
        )
    ]
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
    bag_counts = {
        bag: sum(int(position["bag"]) == bag for position in positions)
        for bag in range(1, 5)
    }
    if len(set(bag_counts.values())) != 1:
        raise ValueError(f"panel is not balanced: {bag_counts}")
    run_positions = [
        position
        for position in positions
        if args.position is None or position["position"] == args.position
    ]
    if args.position is not None and not run_positions:
        raise ValueError(f"unknown PEG position {args.position}")
    active_schedule_labels = (
        tuple(args.schedule_label)
        if args.schedule_label is not None
        else tuple(SCHEDULES)
    )
    active_arm_order = ("greedy", *active_schedule_labels)

    source_records = [
        record
        for path in sensitivity_records
        for record in load_records(path)
    ]
    sensitivity_positions = {
        str(record["position"])
        for record in source_records
        if record.get("kind") == "judge"
        and record.get("label") == "stride2"
        and int(record.get("accepted", 0)) == 1
    }
    if args.skip_sensitivity:
        sensitivity_positions.clear()

    records_path = output_dir / "records.jsonl"
    raw_path = output_dir / "raw.log"
    records = load_records(records_path)
    create_manifest(
        repo=repo,
        panel=panel,
        positions=positions,
        sensitivity_records=sensitivity_records,
        sensitivity_positions=sensitivity_positions,
        output_dir=output_dir,
        arm_budget=args.arm_budget,
        oracle_budget=args.oracle_budget,
    )
    runner = SequentialRunner(binary, output_dir, records_path, raw_path, records)

    sentinel_source = next(
        (position for position in positions if position["bag"] == 2),
        positions[0],
    )
    sentinel = dict(sentinel_source)
    sentinel["position"] = "peg4-sentinel"

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
    split = len(run_positions) // 2
    for position_index, position in enumerate(run_positions):
        block = "early" if position_index < split else "late"
        if position_index == split:
            run_sentinel("between")
        order = (
            active_arm_order
            if position_index % 2 == 0
            else tuple(reversed(active_arm_order))
        )
        for label in order:
            if latest_arm(runner.records, position["position"], label):
                continue
            runner.run(
                position=position,
                mode="arm",
                label=label,
                budget=0.0 if label == "greedy" else args.arm_budget,
                block=block,
                schedule=SCHEDULES.get(label),
            )

        depth_map = latest_depth_map(
            runner.records, position["position"]
        )
        current_sequences = {
            label: int(arm["sequence"])
            for label in active_arm_order
            if (
                arm := latest_arm(
                    runner.records, position["position"], label
                )
            )
            is not None
        }
        if (
            depth_map is None
            or depth_map.get("arm_sequences") != current_sequences
        ):
            depth_map = build_depth_map(runner.records, position)
            depth_map["block"] = block
            append_jsonl(records_path, depth_map)
            runner.records.append(depth_map)

        accepted_endpoints = [
            endpoint
            for endpoint in depth_map["endpoints"]
            if int(endpoint["accepted"]) == 1
        ]
        moves = list(
            dict.fromkeys(
                str(endpoint["move"]) for endpoint in accepted_endpoints
            )
        )
        if len(moves) == 1:
            if not has_summary(
                runner.records,
                "agreement",
                position["position"],
                "peg4_exact",
            ):
                agreement = {
                    "kind": "agreement",
                    "position": position["position"],
                    "bag": position["bag"],
                    "label": "peg4_exact",
                    "move": moves[0],
                    "accepted": 1,
                    "pair_regret": 0.0,
                    "block": block,
                }
                append_jsonl(records_path, agreement)
                runner.records.append(agreement)
            continue

        endpoint_by_label = {
            str(endpoint["label"]): endpoint
            for endpoint in accepted_endpoints
        }
        has_4ply_change = any(
            f"{arm_label}_3ply" in endpoint_by_label
            and f"{arm_label}_4ply" in endpoint_by_label
            and endpoint_by_label[f"{arm_label}_3ply"]["move"]
            != endpoint_by_label[f"{arm_label}_4ply"]["move"]
            for arm_label in active_schedule_labels
        )
        has_width_change = any(
            f"narrow4_{ply}ply" in endpoint_by_label
            and f"wide4_{ply}ply" in endpoint_by_label
            and endpoint_by_label[f"narrow4_{ply}ply"]["move"]
            != endpoint_by_label[f"wide4_{ply}ply"]["move"]
            for ply in (2, 3, 4)
        )
        labels = [STRIDE4_LABEL]
        if (
            position["position"] in sensitivity_positions
            or has_4ply_change
            or has_width_change
        ):
            labels.append(STRIDE2_LABEL)
        if int(position["bag"]) == 1 and has_4ply_change:
            labels.append(STRIDE1_LABEL)
        if position_index % 2 == 1:
            labels.reverse()
        for label in labels:
            prior = latest_judge(
                runner.records, position["position"], label
            )
            if (
                prior is not None
                and int(prior.get("accepted", 0)) == 1
                and int(prior.get("nominee_count", -1)) == len(moves)
                and int(prior.get("judge_ply", -1)) == 4
            ):
                continue
            runner.run(
                position=position,
                mode="judge",
                label=label,
                budget=args.oracle_budget,
                block=block,
                moves=moves,
                stride=(
                    1
                    if label == STRIDE1_LABEL
                    else (2 if label == STRIDE2_LABEL else 4)
                ),
                judge_ply=4,
            )

        best4 = best_judged_move(
            runner.records, position["position"], STRIDE4_LABEL
        )
        best2 = best_judged_move(
            runner.records, position["position"], STRIDE2_LABEL
        )
        prior_full = latest_judge(
            runner.records, position["position"], STRIDE1_LABEL
        )
        if (
            best4 is not None
            and best2 is not None
            and best4 != best2
            and (
                prior_full is None
                or int(prior_full.get("accepted", 0)) != 1
                or int(prior_full.get("nominee_count", -1)) != len(moves)
            )
        ):
            runner.run(
                position=position,
                mode="judge",
                label=STRIDE1_LABEL,
                budget=args.oracle_budget,
                block=block,
                moves=moves,
                stride=1,
                judge_ply=4,
            )

    run_sentinel("after")
    print(f"complete artifacts={output_dir}", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
