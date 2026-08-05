#!/usr/bin/env python3
"""Collect prospective whole-stage PEG completion traces, one at a time.

The model artifact must be frozen before this runner is started.  This tool
does not make admission decisions; it observes what the ordinary complete
stage actually costs under a fixed physical deadline.  The companion analyzer
replays the frozen decision against these append-only records.
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
    latest_arm,
    load_records,
    read_panel,
    sha256,
)


def resolve(repo: pathlib.Path, path: pathlib.Path) -> pathlib.Path:
    return path.resolve() if path.is_absolute() else (repo / path).resolve()


def interleave_by_bag(
    positions: list[dict[str, Any]],
) -> list[dict[str, Any]]:
    by_bag = {
        bag: [position for position in positions if position["bag"] == bag]
        for bag in range(1, 5)
    }
    ordered: list[dict[str, Any]] = []
    for index in range(len(by_bag[1])):
        bag_order = (1, 2, 3, 4) if index % 2 == 0 else (4, 3, 2, 1)
        ordered.extend(by_bag[bag][index] for bag in bag_order)
    return ordered


def write_manifest(
    *,
    repo: pathlib.Path,
    panel: pathlib.Path,
    artifact: pathlib.Path,
    binary: pathlib.Path,
    output_dir: pathlib.Path,
    positions: list[dict[str, Any]],
    threads: int,
    budget: float,
    schedule: list[int],
) -> None:
    manifest = {
        "schema_version": 1,
        "created_utc": dt.datetime.now(dt.timezone.utc).isoformat(),
        "command": [sys.executable, *sys.argv],
        "platform": platform.platform(),
        "git_head": subprocess.check_output(
            ["git", "rev-parse", "HEAD"], cwd=repo, text=True
        ).strip(),
        "panel": str(panel),
        "panel_sha256": sha256(panel),
        "model_artifact": str(artifact),
        "model_artifact_sha256": sha256(artifact),
        "binary": str(binary),
        "binary_sha256": sha256(binary),
        "positions": len(positions),
        "positions_per_bag": len(positions) // 4,
        "threads": threads,
        "detected_hardware_concurrency": os.cpu_count(),
        "budget_seconds": budget,
        "schedule": schedule,
        "execution": "one process at a time; append-only; resumable",
        "output_dir": str(output_dir),
    }
    manifest_path = output_dir / "manifest.json"
    if manifest_path.exists():
        existing = json.loads(manifest_path.read_text())
        immutable_keys = (
            "panel_sha256",
            "model_artifact_sha256",
            "binary_sha256",
            "positions",
            "threads",
            "budget_seconds",
            "schedule",
        )
        mismatches = [
            key
            for key in immutable_keys
            if existing.get(key) != manifest.get(key)
        ]
        if mismatches:
            raise RuntimeError(
                "refusing to resume with changed frozen protocol: "
                + ", ".join(mismatches)
            )
        return
    manifest_path.write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--panel",
        type=pathlib.Path,
        default=pathlib.Path(
            "tools/peg_time_calibration/heldout_positions_expansion.tsv"
        ),
    )
    parser.add_argument(
        "--artifact",
        type=pathlib.Path,
        default=pathlib.Path(
            "tools/peg_time_calibration/adaptive_complete_stage_admission_v6_20260801.json"
        ),
    )
    parser.add_argument(
        "--binary", type=pathlib.Path, default=pathlib.Path("bin/magpie_test")
    )
    parser.add_argument("--output-dir", type=pathlib.Path, required=True)
    parser.add_argument("--threads", type=int, default=os.cpu_count())
    parser.add_argument("--budget", type=float, default=180.0)
    parser.add_argument("--schedule", default="32")
    parser.add_argument("--max-positions", type=int)
    parser.add_argument("--skip-positions", type=int, default=0)
    args = parser.parse_args()

    if args.threads is None or args.threads < 1:
        raise ValueError("--threads must be positive")
    if args.budget <= 0.0:
        raise ValueError("--budget must be positive")
    schedule = [int(value) for value in args.schedule.split(",")]
    if not schedule or any(value < 2 for value in schedule):
        raise ValueError("--schedule must contain candidate counts >= 2")

    repo = pathlib.Path(__file__).resolve().parent.parent
    panel = resolve(repo, args.panel)
    artifact = resolve(repo, args.artifact)
    binary = resolve(repo, args.binary)
    output_dir = resolve(repo, args.output_dir)
    if not artifact.exists() or not binary.exists():
        raise FileNotFoundError("model artifact and binary must both exist")
    output_dir.mkdir(parents=True, exist_ok=True)
    if shutil.disk_usage(output_dir).free < 5 * 1024**3:
        raise RuntimeError("less than 5 GiB free")

    all_positions = read_panel(panel)
    positions = interleave_by_bag(all_positions)
    if args.skip_positions < 0:
        raise ValueError("--skip-positions must be nonnegative")
    positions = positions[args.skip_positions :]
    if args.max_positions is not None:
        if args.max_positions < 1:
            raise ValueError("--max-positions must be positive")
        positions = positions[: args.max_positions]
    write_manifest(
        repo=repo,
        panel=panel,
        artifact=artifact,
        binary=binary,
        output_dir=output_dir,
        positions=positions,
        threads=args.threads,
        budget=args.budget,
        schedule=schedule,
    )

    records_path = output_dir / "records.jsonl"
    raw_path = output_dir / "raw.log"
    records = load_records(records_path)
    runner = SequentialRunner(
        binary,
        output_dir,
        records_path,
        raw_path,
        records,
        threads=args.threads,
    )
    artifact_id = str(json.loads(artifact.read_text())["artifact_id"])
    label = artifact_id + "-" + "-".join(str(value) for value in schedule)
    for position in positions:
        if latest_arm(runner.records, str(position["position"]), label):
            continue
        runner.run(
            position=position,
            mode="arm",
            label=label,
            budget=args.budget,
            block="prospective-heldout",
            schedule=schedule,
        )

    print(f"VALIDATION_DONE artifacts={output_dir}", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
