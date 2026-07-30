#!/usr/bin/env python3
"""Run the locked representative PEG quality arms, one process at a time."""

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

from run_peg_checkpoint_calibration import build_checkpoint_map
from run_peg_time_calibration import (
    SequentialRunner,
    append_jsonl,
    has_summary,
    load_records,
    read_panel,
    sha256,
)


SCHEDULES = {
    "checkpoint32": [32],
    "deep16": [16, 8],
    "deep24": [24, 12],
}
ARM_ORDER = tuple(SCHEDULES)
SENTINEL_EVERY = 10


def resolve(repo: pathlib.Path, path: pathlib.Path) -> pathlib.Path:
    return path.resolve() if path.is_absolute() else (repo / path).resolve()


def interleave_by_bag(
    positions: list[dict[str, Any]],
) -> list[dict[str, Any]]:
    by_bag = {
        bag: [position for position in positions if position["bag"] == bag]
        for bag in range(1, 5)
    }
    if len({len(rows) for rows in by_bag.values()}) != 1:
        raise ValueError("fresh quality panel must be balanced by bag")
    ordered = []
    for index in range(len(by_bag[1])):
        bag_order = (1, 2, 3, 4) if index % 2 == 0 else (4, 3, 2, 1)
        ordered.extend(by_bag[bag][index] for bag in bag_order)
    return ordered


def arm_is_complete(
    arm: dict[str, Any] | None, schedule: list[int]
) -> bool:
    return bool(
        arm is not None
        and arm.get("status") == "completed"
        and int(arm.get("requested_stages", -1)) == len(schedule)
        and int(arm.get("deepest_candidate_total", -1)) == schedule[-1]
        and int(arm.get("deepest_completed_candidates", -1)) == schedule[-1]
    )


def patience_stop(
    checkpoints: list[dict[str, Any]],
    minimum_candidates: int,
    patience: int,
) -> int:
    last_change = 0
    for completed in range(1, len(checkpoints)):
        if str(checkpoints[completed]["move"]) != str(
            checkpoints[completed - 1]["move"]
        ):
            last_change = completed
        if (
            completed >= minimum_candidates
            and completed - last_change >= patience
        ):
            return completed
    return len(checkpoints) - 1


def endpoint_work(events: list[dict[str, Any]]) -> dict[str, Any]:
    boundary = max(events, key=lambda event: float(event["completed_seconds"]))
    return {
        "cumulative_scenarios": int(boundary["cumulative_scenarios"]),
        "nested_endgame_nodes": int(boundary["nested_endgame_nodes"]),
        "completed_seconds": float(boundary["completed_seconds"]),
        "process_cpu_seconds": float(boundary["process_cpu_seconds"]),
    }


def build_deep_endpoint(
    arm: dict[str, Any],
    events_by_stage: dict[int, list[dict[str, Any]]],
    schedule: list[int],
) -> dict[str, Any]:
    stage1 = events_by_stage.get(1, [])
    stage2 = events_by_stage.get(2, [])
    if len(stage1) != schedule[0] or len(stage2) != schedule[1]:
        raise RuntimeError(
            f"{arm['position']}/{arm['label']}: incomplete deep event trace"
        )
    stage1_work = endpoint_work(stage1)
    stage2_work = endpoint_work(stage2)
    return {
        "arm_label": str(arm["label"]),
        "schedule": schedule,
        "move_2ply": str(stage2[0]["move"]),
        "move_3ply": str(arm["move"]),
        "stage_2ply": {
            **stage1_work,
            "completed_candidates": schedule[0],
        },
        "stage_3ply": {
            **stage2_work,
            "completed_candidates": schedule[1],
            "incremental_scenarios": (
                stage2_work["cumulative_scenarios"]
                - stage1_work["cumulative_scenarios"]
            ),
            "incremental_nested_endgame_nodes": (
                stage2_work["nested_endgame_nodes"]
                - stage1_work["nested_endgame_nodes"]
            ),
            "incremental_seconds": (
                stage2_work["completed_seconds"]
                - stage1_work["completed_seconds"]
            ),
            "incremental_process_cpu_seconds": (
                stage2_work["process_cpu_seconds"]
                - stage1_work["process_cpu_seconds"]
            ),
        },
    }


def build_policy_map(
    position: dict[str, Any],
    checkpoint_map: dict[str, Any],
    deep16: dict[str, Any],
    deep24: dict[str, Any],
) -> dict[str, Any]:
    checkpoints = checkpoint_map["checkpoints"]
    policies = {}
    for count in (2, 4, 8, 12, 32):
        policies[f"fixed_{count}"] = {
            "move": str(checkpoints[count]["move"]),
            "completed_2ply_candidates": count,
            "work": checkpoints[count],
        }
    for label, minimum, patience in (
        ("min8_patience4", 8, 4),
        ("min8_patience8", 8, 8),
        ("min12_patience8", 12, 8),
    ):
        stop = patience_stop(checkpoints, minimum, patience)
        policies[label] = {
            "move": str(checkpoints[stop]["move"]),
            "completed_2ply_candidates": stop,
            "minimum": minimum,
            "patience": patience,
            "work": checkpoints[stop],
        }
    policies["deep_16_8"] = {
        "move": str(deep16["move_3ply"]),
        "schedule": [16, 8],
        "work": deep16["stage_3ply"],
    }
    policies["deep_24_12"] = {
        "move": str(deep24["move_3ply"]),
        "schedule": [24, 12],
        "work": deep24["stage_3ply"],
    }
    nominees = list(
        dict.fromkeys(str(policy["move"]) for policy in policies.values())
    )
    return {
        "kind": "fresh_quality_map",
        "schema_version": 1,
        "position": position["position"],
        "bag": int(position["bag"]),
        "policies": policies,
        "nominees": nominees,
        "distinct_nominees": len(nominees),
        "all_policy_agreement": int(len(nominees) == 1),
        "arm_sequences": {
            "checkpoint32": int(checkpoint_map["arm_sequence"]),
            "deep16": int(deep16["arm_sequence"]),
            "deep24": int(deep24["arm_sequence"]),
        },
    }


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
    manifest = {
        "schema_version": 1,
        "created_utc": dt.datetime.now(dt.timezone.utc).isoformat(),
        "git_head": subprocess.check_output(
            ["git", "rev-parse", "HEAD"], cwd=repo, text=True
        ).strip(),
        "command": [sys.executable, *sys.argv],
        "platform": platform.platform(),
        "python": sys.version,
        "panel": {"path": str(panel), "sha256": sha256(panel)},
        "positions": len(positions),
        "per_bag": len(positions) // 4,
        "binary": str(binary),
        "binary_sha256": sha256(binary),
        "arms": {
            label: {"schedule": schedule, "budget_seconds": 0}
            for label, schedule in SCHEDULES.items()
        },
        "arm_order": "forward/reverse across the bag-interleaved panel",
        "candidate_admission_topology_for_analysis": (
            "initial wave of two, then one candidate and replan"
        ),
        "policies": [
            "fixed 2/4/8/12/32 completed 2-ply candidates",
            "min 8 / patience 4",
            "min 8 / patience 8",
            "min 12 / patience 8",
            "completed 3-ply {16,8}",
            "completed 3-ply {24,12}",
        ],
        "threads": threads,
        "detected_hardware_concurrency": os.cpu_count(),
        "build": "no_pgo_release",
        "lexicon": "CSW24",
        "rit": True,
        "sentinel_interval_positions": SENTINEL_EVERY,
        "planned_oracle": {
            "primary_fidelity": "direct 4-ply",
            "pilot_disagreements": "24, stratified by bag and locked before judging",
            "stride": 4,
            "budget_seconds": 0,
            "common_sample": "deterministic weight-strata boundaries",
            "continuation": "CSW24 KLV2 horizon residual",
            "fallback": "direct 3-ply all disagreements plus direct-4 correction subset",
        },
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
            "tools/peg_time_calibration/quality_positions_20260730.tsv"
        ),
    )
    parser.add_argument(
        "--binary", type=pathlib.Path, default=pathlib.Path("bin/magpie_test")
    )
    parser.add_argument("--output-dir", type=pathlib.Path, required=True)
    parser.add_argument("--threads", type=int, default=os.cpu_count())
    parser.add_argument("--max-positions", type=int)
    parser.add_argument("--position")
    args = parser.parse_args()
    if args.threads is None or args.threads < 1:
        raise ValueError("--threads must be positive")
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
    if shutil.disk_usage(output_dir).free < 5 * 1024**3:
        raise RuntimeError("less than 5 GiB free")

    positions = read_panel(panel)
    ordered = interleave_by_bag(positions)
    by_id = {str(position["position"]): position for position in positions}
    if args.position is not None:
        if args.position not in by_id:
            raise ValueError(f"unknown fresh-quality position {args.position}")
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
    arm_index: dict[tuple[str, str], dict[str, Any]] = {}
    event_index: dict[tuple[int, int], list[dict[str, Any]]] = {}
    map_index: dict[str, dict[str, Any]] = {}
    for record in records:
        if record.get("kind") == "arm" and record.get("mode") == "arm":
            arm_index[(str(record["position"]), str(record["label"]))] = record
        elif record.get("kind") == "event":
            event_index.setdefault(
                (int(record["sequence"]), int(record["stage"])), []
            ).append(record)
        elif record.get("kind") == "fresh_quality_map":
            map_index[str(record["position"])] = record
    for events in event_index.values():
        events.sort(key=lambda event: float(event["completed_seconds"]))

    sentinel = dict(
        next(position for position in positions if position["bag"] == 2)
    )
    sentinel["position"] = "fresh-quality-sentinel"

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
        labels = ARM_ORDER if ordinal % 2 == 1 else tuple(reversed(ARM_ORDER))
        for label in labels:
            schedule = SCHEDULES[label]
            arm = arm_index.get((str(position["position"]), label))
            if arm_is_complete(arm, schedule):
                continue
            parsed = runner.run(
                position=position,
                mode="arm",
                label=label,
                budget=0.0,
                block=f"quartile-{min(4, (ordinal - 1) * 4 // len(ordered) + 1)}",
                schedule=schedule,
            )
            for record in parsed:
                if record.get("kind") == "arm":
                    arm_index[
                        (str(record["position"]), str(record["label"]))
                    ] = record
                elif record.get("kind") == "event":
                    event_index.setdefault(
                        (int(record["sequence"]), int(record["stage"])), []
                    ).append(record)

        arms = {
            label: arm_index.get((str(position["position"]), label))
            for label in ARM_ORDER
        }
        if not all(
            arm_is_complete(arms[label], SCHEDULES[label])
            for label in ARM_ORDER
        ):
            continue
        prior_map = map_index.get(str(position["position"]))
        sequences = {
            label: int(arms[label]["sequence"]) for label in ARM_ORDER
        }
        if (
            prior_map is not None
            and prior_map.get("arm_sequences") == sequences
        ):
            continue

        checkpoint_arm = arms["checkpoint32"]
        checkpoint_records = [
            *event_index[(int(checkpoint_arm["sequence"]), 0)],
            *event_index[(int(checkpoint_arm["sequence"]), 1)],
        ]
        checkpoint_map = build_checkpoint_map(
            checkpoint_records, checkpoint_arm, position
        )
        deep_endpoints = {}
        for label in ("deep16", "deep24"):
            arm = arms[label]
            sequence = int(arm["sequence"])
            endpoint = build_deep_endpoint(
                arm,
                {
                    stage: event_index.get((sequence, stage), [])
                    for stage in range(3)
                },
                SCHEDULES[label],
            )
            endpoint["arm_sequence"] = sequence
            deep_endpoints[label] = endpoint
        policy_map = build_policy_map(
            position,
            checkpoint_map,
            deep_endpoints["deep16"],
            deep_endpoints["deep24"],
        )
        append_jsonl(records_path, policy_map)
        runner.records.append(policy_map)
        map_index[str(position["position"])] = policy_map
        print(
            f"mapped {position['position']} nominees="
            f"{policy_map['distinct_nominees']}",
            flush=True,
        )
        if args.position is None and ordinal % SENTINEL_EVERY == 0:
            run_sentinel(ordinal)
    if args.position is None and len(selected) == len(ordered):
        run_sentinel(len(ordered))
    print(
        f"fresh-quality maps={len(map_index)}/{len(positions)} "
        f"artifacts={output_dir}",
        flush=True,
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
