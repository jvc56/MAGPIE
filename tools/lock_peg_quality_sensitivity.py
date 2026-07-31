#!/usr/bin/env python3
"""Lock an oracle-independent bag-balanced stride sensitivity subset."""

from __future__ import annotations

import argparse
import hashlib
import json
import pathlib
from typing import Any

from run_peg_time_calibration import load_records, sha256


def select_sensitivity(
    selected: list[dict[str, Any]], per_bag: int, seed: str
) -> list[dict[str, Any]]:
    result = []
    for bag in range(1, 5):
        eligible = [
            row for row in selected if int(row["bag"]) == bag
        ]
        ranked = sorted(
            eligible,
            key=lambda row: hashlib.sha256(
                f"{seed}|{row['position']}".encode()
            ).hexdigest(),
        )
        if len(ranked) < per_bag:
            raise ValueError(
                f"bag {bag}: only {len(ranked)} positions, need {per_bag}"
            )
        result.extend(ranked[:per_bag])
    return result


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source-selection", type=pathlib.Path, required=True)
    parser.add_argument(
        "--records",
        type=pathlib.Path,
        default=pathlib.Path("obj/peg_quality_20260730/records.jsonl"),
    )
    parser.add_argument("--output", type=pathlib.Path, required=True)
    parser.add_argument("--per-bag", type=int, default=2)
    parser.add_argument(
        "--seed", default="quality-stride2-sensitivity-20260730-v1"
    )
    args = parser.parse_args()

    repo = pathlib.Path(__file__).resolve().parent.parent

    def resolve(path: pathlib.Path) -> pathlib.Path:
        return path if path.is_absolute() else repo / path

    source = resolve(args.source_selection)
    records_path = resolve(args.records)
    output = resolve(args.output)
    records = load_records(records_path)
    if any(
        record.get("kind") == "judge"
        and int(record.get("stride") or 0) == 2
        for record in records
    ):
        raise RuntimeError(
            "refusing to lock sensitivity subset after stride-2 judgments"
        )
    source_data = json.loads(source.read_text())
    selected = select_sensitivity(
        source_data["selected"], args.per_bag, args.seed
    )
    manifest = {
        "schema_version": 1,
        "locked_before_stride2": True,
        "selection": (
            "deterministic SHA-256 random rank within bag from the complete "
            "policy-disagreement set"
        ),
        "selection_excludes": [
            "direct_3_values",
            "direct_4_values",
            "oracle_runtime",
        ],
        "seed": args.seed,
        "per_bag": args.per_bag,
        "positions": len(selected),
        "source_selection": {
            "path": str(source),
            "sha256": sha256(source),
        },
        "records": {
            "path": str(records_path),
            "sha256_at_lock": sha256(records_path),
        },
        "selected": selected,
    }
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(
        json.dumps(manifest, indent=2, sort_keys=True) + "\n"
    )
    print(f"locked {len(selected)} sensitivity positions at {output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
