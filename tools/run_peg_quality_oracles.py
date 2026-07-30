#!/usr/bin/env python3
"""Judge fresh-quality nominees on common direct-3/direct-4 scenarios."""

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

from run_peg_fresh_quality import interleave_by_bag
from run_peg_time_calibration import (
    SequentialRunner,
    append_jsonl,
    load_records,
    read_panel,
    sha256,
)


SENTINEL_EVERY = 4


def resolve(repo: pathlib.Path, path: pathlib.Path) -> pathlib.Path:
    return path.resolve() if path.is_absolute() else (repo / path).resolve()


def judge_label(ply: int, stride: int) -> str:
    return f"fresh_direct{ply}_stride{stride}"


def judge_is_accepted(
    judge: dict[str, Any] | None, ply: int, nominees: int
) -> bool:
    return bool(
        judge is not None
        and int(judge.get("accepted", 0)) == 1
        and int(judge.get("judge_ply", -1)) == ply
        and int(judge.get("nominee_count", -1)) == nominees
    )


def create_manifest(
    repo: pathlib.Path,
    panel: pathlib.Path,
    records_path: pathlib.Path,
    selection: pathlib.Path | None,
    binary: pathlib.Path,
    output_dir: pathlib.Path,
    threads: int,
    plies: list[int],
    stride: int,
    budget: float,
    position_ids: list[str],
) -> None:
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
        "arm_records": str(records_path),
        "selection": (
            {"path": str(selection), "sha256": sha256(selection)}
            if selection is not None
            else "all policy disagreements"
        ),
        "positions": position_ids,
        "judges": [
            {
                "label": judge_label(ply, stride),
                "fidelity": f"direct {ply}-ply",
                "stride": stride,
                "budget_seconds": budget,
                "nominees": (
                    "all distinct policy moves together under identical "
                    "root scenarios and seeds"
                ),
                "common_sample": "deterministic weight-strata boundaries",
                "continuation": "CSW24 KLV2 horizon residual",
            }
            for ply in plies
        ],
        "threads": threads,
        "detected_hardware_concurrency": os.cpu_count(),
        "build": "no_pgo_release",
        "lexicon": "CSW24",
        "rit": True,
        "binary": str(binary),
        "binary_sha256": sha256(binary),
        "agreement_policy": "exact zero regret; no oracle process",
        "partial_policy": "never substitute a shallower or arm value",
    }
    selection_label = selection.stem if selection is not None else "all"
    ply_label = "-".join(str(ply) for ply in plies)
    manifest_path = (
        output_dir
        / f"oracle-run-manifest-{selection_label}-ply{ply_label}-s{stride}.json"
    )
    manifest_path.write_text(
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
        "--records",
        type=pathlib.Path,
        default=pathlib.Path("obj/peg_quality_20260730/records.jsonl"),
    )
    parser.add_argument(
        "--binary", type=pathlib.Path, default=pathlib.Path("bin/magpie_test")
    )
    parser.add_argument("--output-dir", type=pathlib.Path, required=True)
    parser.add_argument("--selection", type=pathlib.Path)
    parser.add_argument(
        "--judge-ply",
        type=int,
        choices=(3, 4),
        action="append",
        required=True,
    )
    parser.add_argument("--stride", type=int, default=4)
    parser.add_argument("--budget", type=float, default=0.0)
    parser.add_argument("--threads", type=int, default=os.cpu_count())
    parser.add_argument("--max-positions", type=int)
    args = parser.parse_args()
    if args.threads is None or args.threads < 1:
        raise ValueError("--threads must be positive")
    if args.stride < 1:
        raise ValueError("--stride must be positive")

    repo = pathlib.Path(__file__).resolve().parent.parent
    panel = resolve(repo, args.panel)
    records_path = resolve(repo, args.records)
    binary = resolve(repo, args.binary)
    output_dir = resolve(repo, args.output_dir)
    selection = (
        resolve(repo, args.selection) if args.selection is not None else None
    )
    if not binary.exists():
        raise FileNotFoundError(binary)
    output_dir.mkdir(parents=True, exist_ok=True)
    if shutil.disk_usage(output_dir).free < 5 * 1024**3:
        raise RuntimeError("less than 5 GiB free")

    positions = read_panel(panel)
    position_by_id = {
        str(position["position"]): position for position in positions
    }
    records = load_records(records_path)
    maps = {
        str(record["position"]): record
        for record in records
        if record.get("kind") == "fresh_quality_map"
    }
    if len(maps) != len(positions):
        raise RuntimeError(
            f"quality arms incomplete: {len(maps)}/{len(positions)} maps"
        )
    if selection is not None:
        selection_data = json.loads(selection.read_text())
        selected_ids = [
            str(row["position"]) for row in selection_data["selected"]
        ]
    else:
        selected_ids = [
            str(position["position"])
            for position in interleave_by_bag(positions)
            if int(maps[str(position["position"])]["distinct_nominees"]) > 1
        ]
    if args.max_positions is not None:
        selected_ids = selected_ids[: args.max_positions]
    if any(position_id not in position_by_id for position_id in selected_ids):
        raise ValueError("selection contains an unknown panel position")

    create_manifest(
        repo,
        panel,
        records_path,
        selection,
        binary,
        output_dir,
        args.threads,
        args.judge_ply,
        args.stride,
        args.budget,
        selected_ids,
    )
    runner = SequentialRunner(
        binary,
        output_dir,
        records_path,
        output_dir / "raw.log",
        records,
        threads=args.threads,
    )
    judge_index = {
        (str(record["position"]), str(record["label"])): record
        for record in records
        if record.get("kind") == "judge"
    }
    agreement_keys = {
        (str(record["position"]), str(record["label"]))
        for record in records
        if record.get("kind") == "agreement"
    }
    sentinel_labels = {
        str(record["label"])
        for record in records
        if record.get("kind") == "arm"
        and record.get("mode") == "sentinel"
    }
    sentinel = dict(
        next(position for position in positions if position["bag"] == 2)
    )
    sentinel["position"] = "fresh-quality-sentinel"

    def run_sentinel(ordinal: int) -> None:
        label = f"oracle-sentinel-{ordinal:04d}"
        if label in sentinel_labels:
            return
        parsed = runner.run(
            position=sentinel,
            mode="sentinel",
            label=label,
            budget=0.0,
            block=f"oracle-after-{ordinal}",
        )
        if any(record.get("kind") == "arm" for record in parsed):
            sentinel_labels.add(label)

    run_sentinel(0)
    for ordinal, position_id in enumerate(selected_ids, 1):
        position = position_by_id[position_id]
        quality_map = maps[position_id]
        moves = [str(move) for move in quality_map["nominees"]]
        if len(moves) == 1:
            key = (position_id, "fresh_quality_exact")
            if key not in agreement_keys:
                agreement = {
                    "kind": "agreement",
                    "position": position_id,
                    "bag": int(position["bag"]),
                    "label": "fresh_quality_exact",
                    "move": moves[0],
                    "accepted": 1,
                    "pair_regret": 0.0,
                }
                append_jsonl(records_path, agreement)
                runner.records.append(agreement)
                agreement_keys.add(key)
            continue
        if len(moves) > 16:
            raise RuntimeError(
                f"{position_id}: {len(moves)} nominees exceeds judge limit"
            )
        plies = (
            args.judge_ply
            if ordinal % 2 == 1
            else list(reversed(args.judge_ply))
        )
        for ply in plies:
            label = judge_label(ply, args.stride)
            prior = judge_index.get((position_id, label))
            if judge_is_accepted(prior, ply, len(moves)):
                continue
            parsed = runner.run(
                position=position,
                mode="judge",
                label=label,
                budget=args.budget,
                block=f"oracle-{ordinal:03d}",
                moves=moves,
                stride=args.stride,
                judge_ply=ply,
            )
            for record in parsed:
                if record.get("kind") == "judge":
                    judge_index[(position_id, label)] = record
        if ordinal % SENTINEL_EVERY == 0:
            run_sentinel(ordinal)
    run_sentinel(len(selected_ids))
    accepted = sum(
        judge_is_accepted(
            judge_index.get((position_id, judge_label(ply, args.stride))),
            ply,
            len(maps[position_id]["nominees"]),
        )
        for position_id in selected_ids
        for ply in args.judge_ply
    )
    print(
        f"oracle accepted={accepted}/"
        f"{len(selected_ids) * len(args.judge_ply)} artifacts={output_dir}",
        flush=True,
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
