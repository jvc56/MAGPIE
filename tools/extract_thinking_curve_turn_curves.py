#!/usr/bin/env python3
"""Extract SIM turn curves for rest-game label generation.

This adapter is intentionally strict about provenance. Existing July 29 data
are top-60 and cover only the bag>=50 prefix, so their output is marked as a
partial observed trajectory and must not be exported as a production value
model. A future matched full-game run can use the same schema with a different
trajectory_scope.
"""

from __future__ import annotations

import argparse
import csv
from pathlib import Path


def fields(line: str) -> dict[str, str]:
    result: dict[str, str] = {}
    for token in line.split()[1:]:
        if "=" in token:
            key, value = token.split("=", 1)
            result[key] = value
    return result


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("candidate_corpus", type=Path)
    parser.add_argument("thinking_curve_log", type=Path)
    parser.add_argument("--plies", type=int, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument(
        "--trajectory-scope",
        default="observed_partial_bag50_plus_top60",
    )
    args = parser.parse_args()

    metadata: dict[int, dict[str, str]] = {}
    with args.candidate_corpus.open(encoding="utf-8") as stream:
        for line in stream:
            if line.startswith("TIME_VALUE_POSITION "):
                row = fields(line)
                position = int(row["position"])
                bag = int(row["bag"])
                own_rack = int(row.get("own_rack", "7"))
                opp_rack = int(row.get("opp_rack", "7"))
                source_seed = row.get("source_seed", "")
                start = row.get("start", "")
                # The display-oriented integer `game` restarts in every
                # collected corpus.  Source seed plus starting seat is the
                # stable whole-game identity required for grouped splits.
                source_game = (
                    f"{source_seed}:{start}"
                    if source_seed != "" and start != ""
                    else row["game"]
                )
                predicted_future_turns = max(
                    0, max(1, (bag + own_rack + opp_rack + 7) // 8) - 1
                )
                metadata[position] = {
                    "game": source_game,
                    "turn": row["turn"],
                    "player": row["player"],
                    "bag": str(bag),
                    "own_rack": str(own_rack),
                    "opp_rack": str(opp_rack),
                    "predicted_future_turns": row.get(
                        "predicted_future_turns", str(predicted_future_turns)
                    ),
                    "trajectory_policy": row.get(
                        "trajectory_policy", "unknown"
                    ),
                    "spread": row.get("spread", ""),
                    "source_seed": source_seed,
                    "start": start,
                }
                continue
            if not line.startswith("POSITIONAL_CANDIDATE "):
                continue
            row = fields(line)
            if int(row["rank"]) != 0:
                continue
            position = int(row["position"])
            metadata[position] = {
                "game": row["game"],
                "turn": row["turn"],
                # Only same-player sequencing matters for suffix labels. The
                # starting seat is immaterial, so parity is a stable slot.
                "player": str(int(row["turn"]) % 2),
                "bag": row["bag"],
                "own_rack": "7",
                "opp_rack": "7",
                "predicted_future_turns": str(
                    max(0, max(1, (int(row["bag"]) + 21) // 8) - 1)
                ),
                "trajectory_policy": "unknown",
            }

    points: dict[tuple[int, int], dict[str, str]] = {}
    forced_positions: set[int] = set()
    with args.thinking_curve_log.open(encoding="utf-8") as stream:
        for line in stream:
            if line.startswith("THINKING_CURVE_FORCED_POSITION "):
                row = fields(line)
                if int(row["plies"]) == args.plies:
                    forced_positions.add(int(row["position"]))
                continue
            if not line.startswith("THINKING_CURVE_POINT "):
                continue
            row = fields(line)
            if (
                int(row["plies"]) != args.plies
                or int(row["final"]) != 0
                or int(row["target_nodes"]) <= 0
            ):
                continue
            position = int(row["position"])
            points[(position, int(row["target_nodes"]))] = row

    rows: list[dict[str, str | int]] = []
    for (position, target), point in sorted(points.items()):
        if position not in metadata:
            raise ValueError(f"position {position} missing from candidate corpus")
        meta = metadata[position]
        rows.append(
            {
                **meta,
                "cost": point["nodes"],
                "nodes": point["nodes"],
                "scenarios": 0,
                "fixed_seconds": 0,
                "greedy_scenarios": 0,
                "greedy_endgame_nodes": 0,
                "root_candidates": 0,
                "added_scenarios": 0,
                "added_endgame_nodes": 0,
                "added_candidates": 0,
                "observed_seconds": f"{int(point.get('elapsed_ns', '0')) / 1.0e9:.9f}",
                "regret": point["judge_regret"],
                "option": target,
                "mode": "sim",
                "plies": args.plies,
                "candidates": point.get("candidates", "60"),
                "source_position": position,
                "trajectory_scope": args.trajectory_scope,
            }
        )

    # Forced positions are true zero-regret options rather than missing data.
    # Their zero cost is a portable boundary; any fixed move-generation cost
    # belongs in the runtime safety/mandatory-work model, not search value.
    for position in sorted(forced_positions):
        if position not in metadata:
            raise ValueError(f"forced position {position} missing from corpus")
        meta = metadata[position]
        rows.append(
            {
                **meta,
                "cost": 0,
                "nodes": 0,
                "scenarios": 0,
                "fixed_seconds": 0,
                "greedy_scenarios": 0,
                "greedy_endgame_nodes": 0,
                "root_candidates": 0,
                "added_scenarios": 0,
                "added_endgame_nodes": 0,
                "added_candidates": 0,
                "observed_seconds": 0,
                "regret": 0,
                "option": "forced",
                "mode": "sim",
                "plies": args.plies,
                "candidates": 1,
                "source_position": position,
                "trajectory_scope": args.trajectory_scope,
            }
        )

    rows.sort(
        key=lambda row: (
            str(row["game"]),
            int(str(row["turn"])),
            float(str(row["cost"])),
        )
    )

    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open("w", newline="", encoding="utf-8") as stream:
        fieldnames = list(rows[0]) if rows else [
            "game",
            "turn",
            "player",
            "cost",
            "regret",
        ]
        writer = csv.DictWriter(stream, fieldnames=fieldnames)
        writer.writeheader()
        writer.writerows(rows)
    print(
        f"turn_options={len(rows)} positions={len({key[0] for key in points})} "
        f"plies={args.plies} scope={args.trajectory_scope} output={args.output}"
    )


if __name__ == "__main__":
    main()
