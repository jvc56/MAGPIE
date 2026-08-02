#!/usr/bin/env python3
"""Derive clustered population SIM-tail priors from a deep curve panel.

The broad complete-game panel is intentionally shallow.  This adapter uses an
independent deep panel to estimate how much expected regret remains beyond a
chosen baseline node count.  It emits two monotone sensitivity scenarios:

* ``central`` uses the observed population mean retention; and
* ``lower95`` credits only the lower end of a two-sided 95% confidence
  interval for the paired regret improvement.

The unit of inference is the source game, not the position.  These priors are
for sensitivity analysis when panel topology differs; they are not direct
per-position oracle observations and cannot by themselves enable live policy.
"""

from __future__ import annotations

import argparse
from collections import defaultdict
import hashlib
import json
import math
from pathlib import Path


def fields(line: str) -> dict[str, str]:
    result: dict[str, str] = {}
    for token in line.split()[1:]:
        if "=" in token:
            key, value = token.split("=", 1)
            result[key] = value
    return result


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def parse_targets(value: str) -> tuple[int, ...]:
    targets = tuple(int(item) for item in value.split(","))
    if not targets or targets != tuple(sorted(set(targets))):
        raise ValueError("targets must be unique and increasing")
    return targets


def load_points(path: Path) -> tuple[
    dict[tuple[int, int, int], float], dict[tuple[int, int], str]
]:
    points: dict[tuple[int, int, int], float] = {}
    games: dict[tuple[int, int], str] = {}
    completed: set[tuple[int, int]] = set()
    with path.open(encoding="utf-8") as stream:
        for line in stream:
            if line.startswith("THINKING_CURVE_POSITION_DONE "):
                row = fields(line)
                if int(row.get("events", "-1")) != 16:
                    raise ValueError("completed deep root does not have 16 events")
                if int(row.get("dropped", "-1")) != 0:
                    raise ValueError("completed deep root reports dropped events")
                completed.add((int(row["plies"]), int(row["position"])))
                continue
            if not line.startswith("THINKING_CURVE_POINT "):
                continue
            row = fields(line)
            if int(row["final"]) or int(row["target_nodes"]) <= 0:
                continue
            plies = int(row["plies"])
            position = int(row["position"])
            nodes = int(row["target_nodes"])
            regret = float(row["judge_regret"])
            if not math.isfinite(regret) or regret < 0.0:
                raise ValueError("judge regret must be finite and nonnegative")
            key = (plies, position, nodes)
            previous = points.setdefault(key, regret)
            if previous != regret:
                raise ValueError(f"conflicting duplicate curve point {key}")
            identity = (plies, position)
            previous_game = games.setdefault(identity, row["game"])
            if previous_game != row["game"]:
                raise ValueError(f"position changes source game {identity}")
    points = {
        key: regret
        for key, regret in points.items()
        if (key[0], key[1]) in completed
    }
    games = {
        key: game for key, game in games.items() if key in completed
    }
    if not completed or not points:
        raise ValueError("deep curve log contains no usable points")
    return points, games


def mean(values: list[float]) -> float:
    return sum(values) / len(values)


def summarize(
    points: dict[tuple[int, int, int], float],
    games: dict[tuple[int, int], str],
    plies: int,
    baseline: int,
    targets: tuple[int, ...],
) -> dict[str, object]:
    positions = {
        position
        for point_plies, position, nodes in points
        if point_plies == plies
        and nodes == baseline
        and all((plies, position, target) in points for target in targets)
    }
    if not positions:
        raise ValueError(f"plies={plies} has no complete matched tail")
    by_game: dict[str, list[int]] = defaultdict(list)
    for position in sorted(positions):
        by_game[games[(plies, position)]].append(position)

    baseline_by_game = [
        mean([points[(plies, position, baseline)] for position in group])
        for group in by_game.values()
    ]
    baseline_mean = mean(baseline_by_game)
    if baseline_mean <= 0.0:
        raise ValueError(f"plies={plies} has nonpositive baseline mean")

    rows: list[dict[str, object]] = []
    previous_central = 1.0
    previous_lower95 = 1.0
    for target in targets:
        target_by_game = [
            mean([points[(plies, position, target)] for position in group])
            for group in by_game.values()
        ]
        gains = [
            before - after
            for before, after in zip(baseline_by_game, target_by_game)
        ]
        gain = mean(gains)
        if len(gains) >= 2:
            variance = sum((value - gain) ** 2 for value in gains) / (
                len(gains) - 1
            )
            standard_error = math.sqrt(variance / len(gains))
        else:
            standard_error = math.nan
        half_width = 1.96 * standard_error
        ci_low = gain - half_width
        ci_high = gain + half_width
        p_value = (
            math.erfc(abs(gain / standard_error) / math.sqrt(2.0))
            if standard_error > 0.0
            else 0.0 if gain != 0.0 else 1.0
        )
        target_mean = mean(target_by_game)
        central = min(
            previous_central,
            max(0.0, min(1.0, target_mean / baseline_mean)),
        )
        credited_gain = max(0.0, ci_low)
        lower95 = min(
            previous_lower95,
            max(0.0, min(1.0, 1.0 - credited_gain / baseline_mean)),
        )
        previous_central = central
        previous_lower95 = lower95
        rows.append(
            {
                "target_nodes": target,
                "target_mean_regret": target_mean,
                "paired_gain": gain,
                "paired_gain_ci95": [ci_low, ci_high],
                "p_value": p_value,
                "central_retention": central,
                "lower95_retention": lower95,
            }
        )
    return {
        "plies": plies,
        "positions": len(positions),
        "source_games": len(by_game),
        "baseline_mean_regret": baseline_mean,
        "targets": rows,
    }


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("deep_log", type=Path)
    parser.add_argument("--baseline-nodes", type=int, default=300000)
    parser.add_argument(
        "--targets", default="500000,1000000,3000000,10000000"
    )
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    targets = parse_targets(args.targets)
    if args.baseline_nodes <= 0 or targets[0] <= args.baseline_nodes:
        parser.error("targets must all exceed the positive baseline")
    points, games = load_points(args.deep_log)
    plies_values = sorted({key[0] for key in points})
    document = {
        "artifact_kind": "sim_population_tail_prior",
        "artifact_version": 1,
        "source_log": str(args.deep_log),
        "source_sha256": sha256(args.deep_log),
        "baseline_nodes": args.baseline_nodes,
        "target_nodes": list(targets),
        "priors": [
            summarize(
                points, games, plies, args.baseline_nodes, targets
            )
            for plies in plies_values
        ],
        "scenarios": {
            "central": "monotone population mean regret retention",
            "lower95": (
                "monotone retention after crediting only the lower 95% "
                "confidence bound of paired game-clustered improvement"
            ),
        },
        "live_eligible": False,
        "caveats": [
            "The source panel used top-60 nomination while the complete-game panel uses top 15.",
            "Population retention is imputed onto roots and is not a per-root oracle measurement.",
            "Use this artifact for long-clock sensitivity analysis, not as a live allocation gate.",
        ],
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(
        json.dumps(document, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    print(json.dumps(document, indent=2, sort_keys=True))


if __name__ == "__main__":
    main()
