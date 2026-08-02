#!/usr/bin/env python3
"""Export per-turn endgame and PEG value curves with source-game provenance.

The mode-curve harness intentionally accepts capped observations, so a
successful panel is not the same thing as a complete quality panel.  This
adapter emits only oracle-scored, fully completed decision boundaries as
training curves and writes separate root-audit and hard-root artifacts for
right-censored observations.

Native costs are endgame nodes for endgame and observed wall seconds for PEG.
They are useful for inspecting each mode separately, but must be passed
through ``convert_time_value_work_costs.py`` before modes are combined for a
hardware-independent rest-of-game model.  The portable work coordinates used
by that converter are retained in ``nodes``, ``scenarios``, and
``candidates``.
"""

from __future__ import annotations

import argparse
from collections import Counter, defaultdict
import csv
import json
import math
from pathlib import Path

try:
    from tools.analyze_endgame_value_curve import load_points
    from tools.analyze_peg_structural_curve import (
        load_arms,
        load_oracle_status,
        parse_fields,
    )
except ModuleNotFoundError:
    from analyze_endgame_value_curve import load_points
    from analyze_peg_structural_curve import (
        load_arms,
        load_oracle_status,
        parse_fields,
    )


CURVE_FIELDS = (
    "game",
    "turn",
    "player",
    "bag",
    "own_rack",
    "opp_rack",
    "predicted_future_turns",
    "trajectory_policy",
    "spread",
    "source_seed",
    "start",
    "cost",
    "nodes",
    "scenarios",
    "fixed_seconds",
    "greedy_scenarios",
    "greedy_endgame_nodes",
    "root_candidates",
    "added_scenarios",
    "added_endgame_nodes",
    "added_candidates",
    "observed_seconds",
    "regret",
    "option",
    "mode",
    "plies",
    "candidates",
    "source_position",
    "trajectory_scope",
)

ROOT_FIELDS = (
    "mode",
    "mode_index",
    "source_position",
    "game",
    "source_seed",
    "start",
    "turn",
    "player",
    "bag",
    "trajectory_policy",
    "usable",
    "reason",
    "terminal",
    "oracle_complete",
    "completed_boundaries",
    "maximum_completed_option",
    "protected",
    "scored",
    "target_depth",
)


def source_game(row: dict[str, str]) -> str:
    return f"{row['source_seed']}:{row['start']}"


def mapping_key(row: dict[str, str]) -> tuple[str, int, int]:
    mode = row["mode"]
    bag = int(row["bag"]) if mode == "peg" else 0
    return mode, bag, int(row["mode_index"])


def load_mapping(path: Path) -> dict[tuple[str, int, int], dict[str, str]]:
    rows: dict[tuple[str, int, int], dict[str, str]] = {}
    with path.open(newline="", encoding="utf-8") as stream:
        reader = csv.DictReader(stream)
        if reader.fieldnames is None:
            raise ValueError("position map has no header")
        for row in reader:
            key = mapping_key(row)
            if key in rows:
                raise ValueError(f"duplicate mapping key {key}")
            rows[key] = dict(row)
    if not rows:
        raise ValueError("position map is empty")
    return rows


def terminal_rows(
    path: Path,
) -> tuple[dict[int, tuple[str, dict[str, str]]], dict[tuple[int, int], str]]:
    endgame: dict[int, tuple[str, dict[str, str]]] = {}
    peg: dict[tuple[int, int], str] = {}
    with path.open(encoding="utf-8") as stream:
        for line in stream:
            if line.startswith("EGCURVEPOS "):
                row = parse_fields(line)
                endgame[int(row["pos"])] = ("complete", row)
            elif line.startswith("EGCURVETRUNC "):
                row = parse_fields(line)
                endgame[int(row["pos"])] = ("censored", row)
            elif line.startswith("PEGSTRUCT "):
                row = parse_fields(line.split(" s0=", 1)[0])
                peg[(int(row["bag"]), int(row["pos"]))] = "complete"
            elif line.startswith("PEGSTRUCTPARTIAL "):
                row = parse_fields(line.split(" s0=", 1)[0])
                peg[(int(row["bag"]), int(row["pos"]))] = "partial"
    return endgame, peg


def metadata(row: dict[str, str]) -> dict[str, str]:
    return {
        "game": source_game(row),
        "turn": row["turn"],
        "player": row["player"],
        "bag": row["bag"],
        "own_rack": row["own_rack"],
        "opp_rack": row["opp_rack"],
        "predicted_future_turns": row["predicted_future_turns"],
        "trajectory_policy": row["trajectory_policy"],
        "spread": row["spread"],
        "source_seed": row["source_seed"],
        "start": row["start"],
    }


def write_csv(path: Path, fieldnames: tuple[str, ...], rows: list[dict]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=fieldnames)
        writer.writeheader()
        writer.writerows(rows)


def root_audit_row(
    mapping: dict[str, str],
    *,
    usable: bool,
    reason: str,
    terminal: str,
    oracle_complete: bool,
    completed_options: list[int],
    protected: int = 0,
    scored: int = 0,
    target_depth: int = 0,
) -> dict[str, str | int]:
    return {
        "mode": mapping["mode"],
        "mode_index": mapping["mode_index"],
        "source_position": mapping["source_position"],
        "game": source_game(mapping),
        "source_seed": mapping["source_seed"],
        "start": mapping["start"],
        "turn": mapping["turn"],
        "player": mapping["player"],
        "bag": mapping["bag"],
        "trajectory_policy": mapping["trajectory_policy"],
        "usable": int(usable),
        "reason": reason,
        "terminal": terminal,
        "oracle_complete": int(oracle_complete),
        "completed_boundaries": len(completed_options),
        "maximum_completed_option": (
            max(completed_options) if completed_options else -1
        ),
        "protected": protected,
        "scored": scored,
        "target_depth": target_depth,
    }


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mapping", type=Path)
    parser.add_argument("mode_log", type=Path)
    parser.add_argument("--output-dir", required=True, type=Path)
    parser.add_argument(
        "--trajectory-scope",
        default="complete_140_stratified_depth4_direct4",
    )
    args = parser.parse_args()

    mappings = load_mapping(args.mapping)
    endgame_terminal, peg_terminal = terminal_rows(args.mode_log)
    endgame_map = {
        key[2]: row for key, row in mappings.items() if key[0] == "endgame"
    }
    peg_map = {
        (key[1], key[2]): row
        for key, row in mappings.items()
        if key[0] == "peg"
    }
    if len(endgame_map) != 366 or len(peg_map) != 172:
        raise ValueError(
            f"unexpected mapping counts endgame={len(endgame_map)} "
            f"peg={len(peg_map)}"
        )
    if set(endgame_terminal) != set(endgame_map):
        raise ValueError("endgame terminal rows do not cover the mapping")
    if set(peg_terminal) != set(peg_map):
        raise ValueError("PEG terminal rows do not cover the mapping")

    endgame_points = [point for point in load_points(0, [args.mode_log]) if point.scored]
    endgame_by_root: dict[int, list] = defaultdict(list)
    endgame_rows: list[dict[str, str | int | float]] = []
    negative_endgame = 0
    for point in endgame_points:
        mapping = endgame_map.get(point.position)
        if mapping is None:
            raise ValueError(f"unmapped endgame position {point.position}")
        endgame_by_root[point.position].append(point)
        negative_endgame += int(point.utility_regret < 0.0)
        endgame_rows.append(
            {
                **metadata(mapping),
                "cost": point.nodes,
                "nodes": point.nodes,
                "scenarios": 0,
                "fixed_seconds": 0,
                "greedy_scenarios": 0,
                "greedy_endgame_nodes": 0,
                "root_candidates": 0,
                "added_scenarios": 0,
                "added_endgame_nodes": 0,
                "added_candidates": 0,
                "observed_seconds": f"{point.elapsed_seconds:.9f}",
                "regret": f"{max(0.0, point.utility_regret):.12f}",
                "option": point.depth,
                "mode": "endgame",
                "plies": point.depth,
                "candidates": point.root_total,
                "source_position": mapping["source_position"],
                "trajectory_scope": args.trajectory_scope,
            }
        )

    endgame_audit: list[dict] = []
    for position, mapping in sorted(endgame_map.items()):
        terminal, _ = endgame_terminal[position]
        options = sorted(point.depth for point in endgame_by_root[position])
        usable = bool(options)
        endgame_audit.append(
            root_audit_row(
                mapping,
                usable=usable,
                reason="" if usable else "arm_censored",
                terminal=terminal,
                oracle_complete=usable,
                completed_options=options,
                target_depth=4,
            )
        )

    statuses = load_oracle_status(args.mode_log)
    if set(statuses) != set(peg_map):
        raise ValueError("PEG oracle rows do not cover the mapping")
    all_peg_arms = load_arms([args.mode_log])
    complete_oracle_arms = load_arms(
        [args.mode_log], require_complete_oracle=True
    )
    usable_peg_arms = [arm for arm in complete_oracle_arms if arm.boundary_complete]
    peg_by_root: dict[tuple[int, int], list] = defaultdict(list)
    peg_rows: list[dict[str, str | int | float]] = []
    negative_peg = 0
    for arm in usable_peg_arms:
        key = (arm.bag, arm.position)
        mapping = peg_map.get(key)
        if mapping is None:
            raise ValueError(f"unmapped PEG position {key}")
        peg_by_root[key].append(arm)
        negative_peg += int(arm.utility_regret < 0.0)
        peg_rows.append(
            {
                **metadata(mapping),
                # PEG has no honest one-dimensional portable native cost.
                # Observed seconds make this file independently inspectable;
                # the work-coordinate converter replaces this field.
                "cost": f"{arm.elapsed_seconds:.9f}",
                "nodes": arm.endgame_nodes,
                "scenarios": arm.scenarios,
                "fixed_seconds": 0,
                "greedy_scenarios": arm.greedy_scenarios,
                "greedy_endgame_nodes": arm.greedy_endgame_nodes,
                "root_candidates": arm.root_candidates,
                "added_scenarios": max(
                    0, arm.scenarios - arm.greedy_scenarios
                ),
                "added_endgame_nodes": max(
                    0, arm.endgame_nodes - arm.greedy_endgame_nodes
                ),
                "added_candidates": max(
                    0, arm.candidate_evals - arm.root_candidates
                ),
                "observed_seconds": f"{arm.elapsed_seconds:.9f}",
                "regret": f"{max(0.0, arm.utility_regret):.12f}",
                "option": arm.requested_stage,
                "mode": "peg",
                "plies": 0,
                "candidates": arm.candidate_evals,
                "source_position": mapping["source_position"],
                "trajectory_scope": args.trajectory_scope,
            }
        )

    all_peg_by_root: dict[tuple[int, int], list] = defaultdict(list)
    for arm in all_peg_arms:
        all_peg_by_root[(arm.bag, arm.position)].append(arm)
    peg_audit: list[dict] = []
    for key, mapping in sorted(peg_map.items()):
        status = statuses[key]
        completed_options = sorted(
            arm.requested_stage for arm in peg_by_root.get(key, [])
        )
        reasons: list[str] = []
        if not status.complete:
            reasons.append("oracle_censored")
        if status.complete and not completed_options:
            reasons.append("no_completed_boundary")
        if peg_terminal[key] == "partial":
            reasons.append("arm_partial")
        peg_audit.append(
            root_audit_row(
                mapping,
                usable=status.complete and bool(completed_options),
                reason=";".join(reasons),
                terminal=peg_terminal[key],
                oracle_complete=status.complete,
                completed_options=completed_options,
                protected=status.protected,
                scored=status.scored,
                target_depth=status.target_depth,
            )
        )

    endgame_rows.sort(key=lambda row: (row["game"], int(row["turn"]), int(row["option"])))
    peg_rows.sort(key=lambda row: (row["game"], int(row["turn"]), int(row["option"])))
    write_csv(args.output_dir / "endgame-turn-curves.csv", CURVE_FIELDS, endgame_rows)
    write_csv(args.output_dir / "peg-turn-curves.csv", CURVE_FIELDS, peg_rows)
    write_csv(args.output_dir / "endgame-root-audit.csv", ROOT_FIELDS, endgame_audit)
    write_csv(args.output_dir / "peg-root-audit.csv", ROOT_FIELDS, peg_audit)
    write_csv(
        args.output_dir / "endgame-hard-roots.csv",
        ROOT_FIELDS,
        [row for row in endgame_audit if not int(row["usable"])],
    )
    write_csv(
        args.output_dir / "peg-hard-roots.csv",
        ROOT_FIELDS,
        [row for row in peg_audit if not int(row["usable"])],
    )

    all_games: dict[str, dict[str, object]] = {}
    hard_by_game: dict[str, list[dict]] = defaultdict(list)
    for row in mappings.values():
        identity = source_game(row)
        all_games.setdefault(
            identity,
            {
                "game": identity,
                "source_seed": row["source_seed"],
                "start": row["start"],
                "trajectory_policy": row["trajectory_policy"],
            },
        )
    for row in endgame_audit + peg_audit:
        if not int(row["usable"]):
            hard_by_game[str(row["game"])].append(row)
    game_rows: list[dict[str, object]] = []
    for identity, row in sorted(all_games.items()):
        hard = hard_by_game.get(identity, [])
        game_rows.append(
            {
                **row,
                "eligible": int(not hard),
                "hard_roots": len(hard),
                "hard_modes": ";".join(sorted({str(item["mode"]) for item in hard})),
            }
        )
    write_csv(
        args.output_dir / "complete-game-eligibility.csv",
        (
            "game",
            "source_seed",
            "start",
            "trajectory_policy",
            "eligible",
            "hard_roots",
            "hard_modes",
        ),
        game_rows,
    )

    peg_reason_counts = Counter(
        reason
        for row in peg_audit
        for reason in str(row["reason"]).split(";")
        if reason
    )
    audit = {
        "artifact_kind": "time_value_mode_curve_audit",
        "mapping": {
            "rows": len(mappings),
            "games": len(all_games),
            "policies": dict(
                sorted(Counter(row["trajectory_policy"] for row in mappings.values()).items())
            ),
        },
        "endgame": {
            "mapped_roots": len(endgame_map),
            "usable_roots": sum(int(row["usable"]) for row in endgame_audit),
            "hard_roots": sum(not int(row["usable"]) for row in endgame_audit),
            "curve_rows": len(endgame_rows),
            "rows_by_depth": dict(sorted(Counter(point.depth for point in endgame_points).items())),
            "negative_regrets_clamped": negative_endgame,
        },
        "peg": {
            "mapped_roots": len(peg_map),
            "oracle_complete": sum(status.complete for status in statuses.values()),
            "oracle_censored": sum(not status.complete for status in statuses.values()),
            "usable_roots": sum(int(row["usable"]) for row in peg_audit),
            "hard_roots": sum(not int(row["usable"]) for row in peg_audit),
            "terminal_complete": sum(value == "complete" for value in peg_terminal.values()),
            "terminal_partial": sum(value == "partial" for value in peg_terminal.values()),
            "curve_rows": len(peg_rows),
            "rows_by_stage": dict(sorted(Counter(arm.requested_stage for arm in usable_peg_arms).items())),
            "hard_reason_counts": dict(sorted(peg_reason_counts.items())),
            "negative_regrets_clamped": negative_peg,
        },
        "complete_games": {
            "eligible": sum(int(row["eligible"]) for row in game_rows),
            "ineligible": sum(not int(row["eligible"]) for row in game_rows),
            "eligible_by_policy": dict(
                sorted(
                    Counter(
                        str(row["trajectory_policy"])
                        for row in game_rows
                        if int(row["eligible"])
                    ).items()
                )
            ),
        },
        "notes": [
            "Curve regret is clamped at zero for nonnegative training labels; aggregate analyzers retain signed oracle noise.",
            "PEG native cost is observed wall time and is not portable; convert retained work coordinates before combining modes.",
            "A complete solver attempt may still be unusable for quality when its oracle or minimum decision boundary was censored.",
        ],
    }
    (args.output_dir / "final-audit.json").write_text(
        json.dumps(audit, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    print(json.dumps(audit, indent=2, sort_keys=True))


if __name__ == "__main__":
    main()
