#!/usr/bin/env python3
"""Prospectively lock a bag-stratified random direct-4 pilot."""

from __future__ import annotations

import argparse
import hashlib
import json
import pathlib
from typing import Any

from run_peg_time_calibration import load_records, sha256


def select_pilot(
    maps: list[dict[str, Any]], per_bag: int, seed: str
) -> list[dict[str, Any]]:
    selected = []
    for bag in range(1, 5):
        eligible = [
            row
            for row in maps
            if int(row["bag"]) == bag
            and int(row["distinct_nominees"]) > 1
        ]
        ranked = sorted(
            eligible,
            key=lambda row: hashlib.sha256(
                f"{seed}|{row['position']}".encode()
            ).hexdigest(),
        )
        if len(ranked) < per_bag:
            raise ValueError(
                f"bag {bag}: only {len(ranked)} disagreement positions, "
                f"need {per_bag}"
            )
        selected.extend(ranked[:per_bag])
    return selected


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--records",
        type=pathlib.Path,
        default=pathlib.Path("obj/peg_quality_20260730/records.jsonl"),
    )
    parser.add_argument(
        "--panel",
        type=pathlib.Path,
        default=pathlib.Path(
            "tools/peg_time_calibration/quality_positions_20260730.tsv"
        ),
    )
    parser.add_argument("--output", type=pathlib.Path, required=True)
    parser.add_argument("--per-bag", type=int, default=6)
    parser.add_argument("--seed", default="quality-direct4-pilot-20260730-v1")
    args = parser.parse_args()

    repo = pathlib.Path(__file__).resolve().parent.parent
    records_path = (
        args.records if args.records.is_absolute() else repo / args.records
    )
    panel = args.panel if args.panel.is_absolute() else repo / args.panel
    output = args.output if args.output.is_absolute() else repo / args.output
    records = load_records(records_path)
    if any(record.get("kind") == "judge" for record in records):
        raise RuntimeError(
            "refusing to lock pilot after oracle judgments exist"
        )
    maps_by_position = {
        str(record["position"]): record
        for record in records
        if record.get("kind") == "fresh_quality_map"
    }
    if len(maps_by_position) != 200:
        raise RuntimeError(
            f"quality arms incomplete: {len(maps_by_position)}/200 maps"
        )
    selected = select_pilot(
        list(maps_by_position.values()), args.per_bag, args.seed
    )
    manifest = {
        "schema_version": 1,
        "locked_before_oracle": True,
        "selection": (
            "distinct-policy disagreements, then deterministic SHA-256 "
            "random rank within bag"
        ),
        "selection_excludes": [
            "oracle_values",
            "oracle_runtime",
            "direct_3_results",
            "direct_4_results",
        ],
        "seed": args.seed,
        "per_bag": args.per_bag,
        "positions": len(selected),
        "panel": {"path": str(panel), "sha256": sha256(panel)},
        "arm_records": {
            "path": str(records_path),
            "sha256_at_lock": sha256(records_path),
        },
        "selected": [
            {
                "position": row["position"],
                "bag": row["bag"],
                "nominees": row["nominees"],
                "policy_moves": {
                    label: policy["move"]
                    for label, policy in row["policies"].items()
                },
                "arm_sequences": row["arm_sequences"],
            }
            for row in selected
        ],
    }
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(
        json.dumps(manifest, indent=2, sort_keys=True) + "\n"
    )
    print(f"locked {len(selected)} disagreement positions at {output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
