#!/usr/bin/env python3
"""Sweep paired 2-ply/3-ply PEG halving schedules, strictly sequentially."""

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


DEFAULT_PANELS = ("tools/peg_time_calibration/heldout_positions.tsv",)
DEFAULT_SENSITIVITY_RECORDS = (
    "obj/peg_time_calibration/run-20260729/records.jsonl",
    "obj/peg_time_calibration/run-20260729-expansion/records.jsonl",
)
SCHEDULES = {
    "cap16": [16, 8],
    "cap24": [24, 12],
    "cap48": [48, 24],
    "cap64": [64, 32],
}
ARM_ORDER = ("greedy", *SCHEDULES)
STRIDE4_LABEL = "stagecap_stride4"
STRIDE2_LABEL = "stagecap_stride2"


def resolve(repo: pathlib.Path, path: pathlib.Path) -> pathlib.Path:
    return path.resolve() if path.is_absolute() else (repo / path).resolve()


def records_for_sequence(
    records: list[dict[str, Any]], sequence: int, kind: str
) -> list[dict[str, Any]]:
    return [
        record
        for record in records
        if record.get("kind") == kind
        and int(record.get("sequence", -1)) == sequence
    ]


def arm_stage(
    records: list[dict[str, Any]], arm: dict[str, Any], stage: int
) -> tuple[dict[str, Any] | None, list[dict[str, Any]]]:
    sequence = int(arm["sequence"])
    stages = [
        record
        for record in records_for_sequence(records, sequence, "stage")
        if int(record.get("stage", -1)) == stage
    ]
    events = sorted(
        (
            record
            for record in records_for_sequence(records, sequence, "event")
            if int(record.get("stage", -1)) == stage
        ),
        key=lambda record: int(record["rank"]),
    )
    return (stages[-1] if stages else None), events


def endpoint_work(events: list[dict[str, Any]]) -> dict[str, Any]:
    boundary = max(events, key=lambda event: float(event["completed_seconds"]))
    return {
        "cumulative_scenarios": int(boundary["cumulative_scenarios"]),
        "nested_endgame_nodes": int(boundary["nested_endgame_nodes"]),
        "completed_seconds": float(boundary["completed_seconds"]),
        "process_cpu_seconds": float(boundary["process_cpu_seconds"]),
    }


def incremental_work(
    after: dict[str, Any], before: dict[str, Any]
) -> dict[str, Any]:
    return {
        "incremental_scenarios": int(after["cumulative_scenarios"])
        - int(before["cumulative_scenarios"]),
        "incremental_nested_endgame_nodes": int(
            after["nested_endgame_nodes"]
        )
        - int(before["nested_endgame_nodes"]),
        "incremental_seconds": float(after["completed_seconds"])
        - float(before["completed_seconds"]),
        "incremental_process_cpu_seconds": float(
            after["process_cpu_seconds"]
        )
        - float(before["process_cpu_seconds"]),
    }


def find_event(
    events: list[dict[str, Any]], move: str
) -> dict[str, Any] | None:
    return next(
        (event for event in events if str(event["move"]) == move), None
    )


def best_event(events: list[dict[str, Any]]) -> dict[str, Any]:
    return max(
        events,
        key=lambda event: (
            float(event["win"]) + 1.0e-4 * float(event["spread"])
        ),
    )


def build_stage_cap_map(
    records: list[dict[str, Any]],
    position: dict[str, Any],
) -> dict[str, Any]:
    greedy_arm = latest_arm(records, position["position"], "greedy")
    if greedy_arm is None:
        raise RuntimeError(f"{position['position']}: missing greedy arm")
    endpoints = [
        {
            "label": "greedy",
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

    for label, schedule in SCHEDULES.items():
        arm = latest_arm(records, position["position"], label)
        if arm is None:
            continue
        arm_sequences[label] = int(arm["sequence"])
        _, events0 = arm_stage(records, arm, 0)
        stage1, events1 = arm_stage(records, arm, 1)
        stage2, events2 = arm_stage(records, arm, 2)
        root_work = endpoint_work(events0) if events0 else None
        stage1_complete = bool(
            stage1
            and int(stage1["completed_candidates"])
            == int(stage1["candidate_total"])
            and float(stage1["end_seconds"]) >= 0.0
        )
        stage2_complete = bool(
            stage2
            and int(stage2["completed_candidates"])
            == int(stage2["candidate_total"])
            and float(stage2["end_seconds"]) >= 0.0
            and int(arm.get("last_completed_stage", -1)) == 2
            and not int(arm.get("partial", 0))
        )

        if events1:
            # A completed stage 2 receives stage 1's actual qsort winner as
            # its rank-0 input, resolving exact stage-1 self-value ties.
            stage1_move = (
                str(events2[0]["move"])
                if stage2_complete and events2
                else str(best_event(events1)["move"])
            )
            stage1_value = find_event(events1, stage1_move)
            assert stage1_value is not None
            stage1_work = endpoint_work(events1)
            endpoint = {
                "label": f"{label}_2ply",
                "arm_label": label,
                "stage": 1,
                "fidelity": 2,
                "move": stage1_move,
                "self_win": float(stage1_value["win"]),
                "self_spread": float(stage1_value["spread"]),
                "accepted": int(stage1_complete),
                "status": (
                    "completed" if stage1_complete else "partial"
                ),
                "candidate_cap": schedule[0],
                "schedule": schedule,
                "completed_candidates": len(events1),
                "candidate_total": (
                    int(stage1["candidate_total"])
                    if stage1 is not None
                    else schedule[0]
                ),
                **stage1_work,
            }
            if root_work is not None:
                endpoint.update(incremental_work(stage1_work, root_work))
            endpoints.append(endpoint)

        if events2:
            stage2_move = (
                str(arm["move"])
                if stage2_complete
                else str(best_event(events2)["move"])
            )
            stage2_value = find_event(events2, stage2_move)
            if stage2_value is None:
                stage2_value = best_event(events2)
                stage2_move = str(stage2_value["move"])
            stage2_work = endpoint_work(events2)
            endpoint = {
                "label": f"{label}_3ply",
                "arm_label": label,
                "stage": 2,
                "fidelity": 3,
                "move": stage2_move,
                "self_win": float(stage2_value["win"]),
                "self_spread": float(stage2_value["spread"]),
                "accepted": int(stage2_complete),
                "status": (
                    "completed" if stage2_complete else "partial"
                ),
                "candidate_cap": schedule[1],
                "schedule": schedule,
                "completed_candidates": len(events2),
                "candidate_total": (
                    int(stage2["candidate_total"])
                    if stage2 is not None
                    else schedule[1]
                ),
                **stage2_work,
            }
            if events1:
                endpoint.update(
                    incremental_work(stage2_work, endpoint_work(events1))
                )
            endpoints.append(endpoint)

    return {
        "kind": "stage_cap_map",
        "schema_version": 2,
        "position": position["position"],
        "bag": int(position["bag"]),
        "arm_sequences": arm_sequences,
        "endpoints": endpoints,
    }


def latest_stage_cap_map(
    records: list[dict[str, Any]], position: str
) -> dict[str, Any] | None:
    matches = [
        record
        for record in records
        if record.get("kind") == "stage_cap_map"
        and record.get("position") == position
    ]
    return matches[-1] if matches else None


def latest_judge(
    records: list[dict[str, Any]], position: str, label: str
) -> dict[str, Any] | None:
    matches = [
        record
        for record in records
        if record.get("kind") == "judge"
        and record.get("position") == position
        and record.get("label") == label
    ]
    return matches[-1] if matches else None


def create_manifest(
    *,
    repo: pathlib.Path,
    panels: list[pathlib.Path],
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
        "panels": [
            {"path": str(panel), "sha256": sha256(panel)} for panel in panels
        ],
        "positions": [
            {"position": position["position"], "bag": position["bag"]}
            for position in positions
        ],
        "arms": {
            label: {
                "schedule": schedule,
                "fidelities": ["2-ply", "3-ply"],
                "budget_seconds": arm_budget,
            }
            for label, schedule in SCHEDULES.items()
        },
        "arm_order": "forward/reverse by position",
        "threads": 10,
        "build": "no_pgo_release",
        "rit": True,
        "oracle": {
            "fidelity": "direct 3-ply",
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
        },
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
    parser.add_argument("--arm-budget", type=float, default=900.0)
    parser.add_argument("--oracle-budget", type=float, default=900.0)
    parser.add_argument("--position")
    parser.add_argument(
        "--schedule-label",
        action="append",
        choices=tuple(SCHEDULES),
        help="Run only selected schedules (repeatable); default is all",
    )
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
    ids = [str(position["position"]) for position in positions]
    if len(ids) != len(set(ids)):
        raise ValueError("stage-cap panels contain duplicate position IDs")
    bag_counts = {
        bag: sum(int(position["bag"]) == bag for position in positions)
        for bag in range(1, 5)
    }
    if len(set(bag_counts.values())) != 1:
        raise ValueError(f"combined panels are not balanced: {bag_counts}")
    run_positions = [
        position
        for position in positions
        if args.position is None or position["position"] == args.position
    ]
    if args.position is not None and not run_positions:
        raise ValueError(f"unknown stage-cap position {args.position}")
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

    records_path = output_dir / "records.jsonl"
    raw_path = output_dir / "raw.log"
    records = load_records(records_path)
    create_manifest(
        repo=repo,
        panels=panels,
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
    sentinel["position"] = "stagecap-sentinel"

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
        order = active_arm_order if position_index % 2 == 0 else tuple(
            reversed(active_arm_order)
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

        stage_map = latest_stage_cap_map(
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
            stage_map is None
            or int(stage_map.get("schema_version", 1)) < 2
            or stage_map.get("arm_sequences") != current_sequences
        ):
            stage_map = build_stage_cap_map(runner.records, position)
            stage_map["block"] = block
            append_jsonl(records_path, stage_map)
            runner.records.append(stage_map)

        accepted_endpoints = [
            endpoint
            for endpoint in stage_map["endpoints"]
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
                "stagecap_exact",
            ):
                agreement = {
                    "kind": "agreement",
                    "position": position["position"],
                    "bag": position["bag"],
                    "label": "stagecap_exact",
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
            prior = latest_judge(
                runner.records, position["position"], label
            )
            if (
                prior is not None
                and int(prior.get("accepted", 0)) == 1
                and int(prior.get("nominee_count", -1)) == len(moves)
            ):
                continue
            runner.run(
                position=position,
                mode="judge",
                label=label,
                budget=args.oracle_budget,
                block=block,
                moves=moves,
                stride=2 if label == STRIDE2_LABEL else 4,
            )

    run_sentinel("after")
    print(f"complete artifacts={output_dir}", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
