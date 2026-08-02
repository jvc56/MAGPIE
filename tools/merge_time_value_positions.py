#!/usr/bin/env python3
"""Merge complete trajectory corpora with globally unique integer indexes."""

from __future__ import annotations

import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path
import re

try:
    from tools.extract_time_value_positions import fields
except ModuleNotFoundError:  # Direct execution with tools/ as sys.path[0].
    from extract_time_value_positions import fields


POSITION_RE = re.compile(r"(?<=\bposition=)\d+")
GAME_RE = re.compile(r"(?<=\bgame=)\d+")


def bag_band(bag: int) -> str:
    if bag == 0:
        return "endgame"
    if bag <= 4:
        return "peg"
    if bag <= 15:
        return "sim_05_15"
    if bag <= 35:
        return "sim_16_35"
    if bag <= 60:
        return "sim_36_60"
    return "sim_61_100"


def merge(inputs: list[Path], output: Path, manifest: Path) -> dict[str, object]:
    output.parent.mkdir(parents=True, exist_ok=True)
    game_indexes: dict[tuple[str, str], int] = {}
    game_owner: dict[tuple[str, str], Path] = {}
    positions = 0
    policies: Counter[str] = Counter()
    bands: Counter[str] = Counter()
    input_rows: dict[str, int] = {}

    with output.open("w", encoding="utf-8") as destination:
        for path in inputs:
            rows = 0
            games_in_input: set[tuple[str, str]] = set()
            with path.open(encoding="utf-8") as source:
                for raw_line in source:
                    if not raw_line.startswith("TIME_VALUE_POSITION "):
                        raise ValueError(f"{path}: unexpected non-position row")
                    row = fields(raw_line)
                    required = {
                        "position",
                        "game",
                        "source_seed",
                        "start",
                        "bag",
                        "trajectory_policy",
                    }
                    missing = required - set(row)
                    if missing:
                        raise ValueError(
                            f"{path}: missing {','.join(sorted(missing))}"
                        )
                    identity = (row["source_seed"], row["start"])
                    owner = game_owner.get(identity)
                    if owner is not None and owner != path:
                        raise ValueError(
                            f"source game {identity} occurs in both {owner} and {path}"
                        )
                    game_owner[identity] = path
                    games_in_input.add(identity)
                    game_index = game_indexes.setdefault(identity, len(game_indexes))
                    rewritten = POSITION_RE.sub(str(positions), raw_line, count=1)
                    rewritten = GAME_RE.sub(str(game_index), rewritten, count=1)
                    destination.write(rewritten)
                    positions += 1
                    rows += 1
                    policies[row["trajectory_policy"]] += 1
                    bands[bag_band(int(row["bag"]))] += 1
            if not games_in_input:
                raise ValueError(f"{path}: empty position corpus")
            input_rows[str(path)] = rows

    document: dict[str, object] = {
        "artifact_kind": "time_value_complete_trajectory_corpus",
        "games": len(game_indexes),
        "positions": positions,
        "positions_by_trajectory_policy": dict(sorted(policies.items())),
        "positions_by_bag_band": dict(sorted(bands.items())),
        "inputs": [
            {
                "path": str(path),
                "rows": input_rows[str(path)],
                "sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
            }
            for path in inputs
        ],
    }
    manifest.write_text(
        json.dumps(document, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    return document


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("inputs", nargs="+", type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--manifest", required=True, type=Path)
    args = parser.parse_args()
    document = merge(args.inputs, args.output, args.manifest)
    print(
        f"games={document['games']} positions={document['positions']} "
        f"output={args.output} manifest={args.manifest}"
    )


if __name__ == "__main__":
    main()
