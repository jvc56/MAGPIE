#!/usr/bin/env python3
"""Select a deterministic stratified panel of complete source games.

Sampling positions independently would leak the same game's suffix across
training and evaluation.  This tool therefore samples only whole trajectories.
Within each requested trajectory policy it stratifies by game length, PEG-turn
frequency, and starting seat, then rewrites only the display-oriented game and
position indices.  Stable source seed/seat identities and every CGP are kept.
"""

from __future__ import annotations

import argparse
from collections import Counter, defaultdict
import hashlib
import json
from pathlib import Path
import re

try:
    from tools.extract_time_value_positions import fields
except ModuleNotFoundError:
    from extract_time_value_positions import fields


def length_band(turns: int) -> str:
    if turns <= 21:
        return "short_le21"
    if turns <= 23:
        return "medium_22_23"
    return "long_ge24"


def peg_band(peg_turns: int) -> str:
    if peg_turns == 0:
        return "none"
    if peg_turns == 1:
        return "one"
    return "two_plus"


def stable_rank(seed: int, *parts: str) -> str:
    payload = "\0".join((str(seed), *parts)).encode("utf-8")
    return hashlib.sha256(payload).hexdigest()


def parse_policy_targets(values: list[str]) -> dict[str, int]:
    targets: dict[str, int] = {}
    for value in values:
        if "=" not in value:
            raise ValueError("policy targets must use POLICY=GAMES")
        policy, raw_count = value.rsplit("=", 1)
        count = int(raw_count)
        if not policy or count <= 0 or policy in targets:
            raise ValueError("policy targets must be unique and positive")
        targets[policy] = count
    if not targets:
        raise ValueError("at least one policy target is required")
    return targets


def allocate_strata(counts: dict[tuple[str, str, str], int], target: int) -> dict[tuple[str, str, str], int]:
    available = sum(counts.values())
    if target > available:
        raise ValueError(f"requested {target} games but only {available} are available")
    keys = sorted(key for key, count in counts.items() if count > 0)
    allocation = {key: 0 for key in keys}
    # When affordable, guarantee at least one complete game from every observed
    # stratum.  The remaining seats track the source distribution by largest
    # proportional deficit.
    if target >= len(keys):
        for key in keys:
            allocation[key] = 1
    while sum(allocation.values()) < target:
        candidates = [key for key in keys if allocation[key] < counts[key]]
        key = max(
            candidates,
            key=lambda item: (
                target * counts[item] / available - allocation[item],
                counts[item] - allocation[item],
                tuple(reversed(item)),
            ),
        )
        allocation[key] += 1
    return allocation


def select_panel(
    source: Path,
    output: Path,
    manifest_path: Path,
    policy_targets: dict[str, int],
    seed: int,
) -> dict[str, object]:
    raw_lines = source.read_text(encoding="utf-8").splitlines(keepends=True)
    games: dict[tuple[str, str], list[tuple[str, dict[str, str]]]] = defaultdict(list)
    for line in raw_lines:
        if not line.startswith("TIME_VALUE_POSITION "):
            raise ValueError("source corpus contains a non-position row")
        row = fields(line)
        identity = (row["source_seed"], row["start"])
        games[identity].append((line, row))
    if not games:
        raise ValueError("source corpus is empty")

    records: list[dict[str, object]] = []
    for identity, rows in games.items():
        rows.sort(key=lambda item: int(item[1]["turn"]))
        turns = [int(item[1]["turn"]) for item in rows]
        if turns != list(range(1, len(rows) + 1)):
            raise ValueError(f"source game {identity} has nonconsecutive turns")
        policies = {item[1]["trajectory_policy"] for item in rows}
        if len(policies) != 1:
            raise ValueError(f"source game {identity} mixes policies")
        peg_turns = sum(1 <= int(item[1]["bag"]) <= 4 for item in rows)
        record = {
            "identity": identity,
            "rows": rows,
            "policy": next(iter(policies)),
            "turns": len(rows),
            "length_band": length_band(len(rows)),
            "peg_turns": peg_turns,
            "peg_band": peg_band(peg_turns),
            "endgame_turns": sum(int(item[1]["bag"]) == 0 for item in rows),
            "start": identity[1],
        }
        records.append(record)

    selected: list[dict[str, object]] = []
    strata_available: Counter[tuple[str, str, str, str]] = Counter()
    strata_selected: Counter[tuple[str, str, str, str]] = Counter()
    for policy, target in policy_targets.items():
        candidates = [record for record in records if record["policy"] == policy]
        by_stratum: dict[tuple[str, str, str], list[dict[str, object]]] = defaultdict(list)
        for record in candidates:
            key = (
                str(record["length_band"]),
                str(record["peg_band"]),
                str(record["start"]),
            )
            by_stratum[key].append(record)
            strata_available[(policy, *key)] += 1
        allocation = allocate_strata(
            {key: len(group) for key, group in by_stratum.items()}, target
        )
        for key, quota in allocation.items():
            ranked = sorted(
                by_stratum[key],
                key=lambda record: stable_rank(
                    seed,
                    policy,
                    *key,
                    *record["identity"],
                ),
            )
            chosen = ranked[:quota]
            selected.extend(chosen)
            strata_selected[(policy, *key)] += len(chosen)

    # Hash order interleaves policies/strata.  A partially completed oracle run
    # therefore remains broadly representative instead of exhausting one
    # policy or phase profile first.
    selected.sort(
        key=lambda record: stable_rank(seed, "panel", *record["identity"])
    )

    output.parent.mkdir(parents=True, exist_ok=True)
    position_index = 0
    with output.open("w", encoding="utf-8") as stream:
        for game_index, record in enumerate(selected):
            for line, _ in record["rows"]:
                rewritten = re.sub(r"\bposition=\d+", f"position={position_index}", line, count=1)
                rewritten = re.sub(r"\bgame=\d+", f"game={game_index}", rewritten, count=1)
                stream.write(rewritten)
                position_index += 1

    source_hash = hashlib.sha256(source.read_bytes()).hexdigest()
    manifest: dict[str, object] = {
        "artifact_kind": "stratified_complete_game_panel",
        "source": str(source),
        "source_sha256": source_hash,
        "seed": seed,
        "policy_targets": policy_targets,
        "games": len(selected),
        "positions": position_index,
        "strata_available": {
            "|".join(key): count for key, count in sorted(strata_available.items())
        },
        "strata_selected": {
            "|".join(key): count for key, count in sorted(strata_selected.items())
        },
        "selected_games": [
            {
                "game": game_index,
                "source_seed": record["identity"][0],
                "start": record["identity"][1],
                "trajectory_policy": record["policy"],
                "turns": record["turns"],
                "length_band": record["length_band"],
                "peg_turns": record["peg_turns"],
                "peg_band": record["peg_band"],
                "endgame_turns": record["endgame_turns"],
            }
            for game_index, record in enumerate(selected)
        ],
    }
    manifest_path.parent.mkdir(parents=True, exist_ok=True)
    manifest_path.write_text(
        json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    return manifest


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--manifest", required=True, type=Path)
    parser.add_argument("--policy-target", action="append", default=[])
    parser.add_argument("--seed", type=int, default=82401)
    args = parser.parse_args()
    targets = parse_policy_targets(args.policy_target)
    manifest = select_panel(
        args.source, args.output, args.manifest, targets, args.seed
    )
    print(
        f"games={manifest['games']} positions={manifest['positions']} "
        f"output={args.output} manifest={args.manifest}"
    )


if __name__ == "__main__":
    main()
