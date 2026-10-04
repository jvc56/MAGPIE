#!/usr/bin/env python3
"""Analyze a POSITIONAL_SIM_SELECTOR confirmation log."""

from __future__ import annotations

import argparse
import math
import re
from collections import defaultdict
from pathlib import Path

import numpy as np


NUMERIC_FIELDS = (
    "position",
    "game",
    "turn",
    "bag",
    "overlap",
    "static_iterations",
    "positional_iterations",
    "positional_minus_static",
    "sem",
    "win_delta",
    "spread_delta",
)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", type=Path, nargs="+")
    return parser.parse_args()


def field(line: str, name: str) -> str:
    match = re.search(rf"(?:^| ){re.escape(name)}=([^ ]+)", line)
    if match is None:
        raise ValueError(f"missing {name} in {line.rstrip()}")
    return match.group(1)


def mean_ci_p(values: np.ndarray) -> tuple[float, float, float, float]:
    mean = float(values.mean())
    sem = float(values.std(ddof=1) / math.sqrt(len(values)))
    if sem == 0.0:
        return mean, mean, mean, 1.0
    p_value = math.erfc(abs(mean / sem) / math.sqrt(2.0))
    return mean, mean - 1.96 * sem, mean + 1.96 * sem, p_value


def exact_sign_p(positive: int, negative: int) -> float:
    trials = positive + negative
    extreme = max(positive, negative)
    tail = sum(math.comb(trials, count) for count in range(extreme, trials + 1))
    return min(1.0, 2.0 * tail / 2**trials)


def clustered_mean_ci_p(
    values_by_cluster: dict[int, list[float]],
) -> tuple[float, float, float, float]:
    values = np.asarray(
        [value for cluster in values_by_cluster.values() for value in cluster],
        dtype=np.float64,
    )
    mean = float(values.mean())
    clusters = len(values_by_cluster)
    scores = np.asarray(
        [sum(value - mean for value in cluster) for cluster in values_by_cluster.values()],
        dtype=np.float64,
    )
    sem = math.sqrt(
        clusters / (clusters - 1) * float(np.dot(scores, scores))
    ) / len(values)
    p_value = math.erfc(abs(mean / sem) / math.sqrt(2.0)) if sem else 1.0
    return mean, mean - 1.96 * sem, mean + 1.96 * sem, p_value


def main() -> None:
    args = parse_args()
    rows: list[dict[str, float | int]] = []
    done_lines: list[str] = []
    for path in args.log:
        for line in path.read_text(encoding="utf-8").splitlines():
            if line.startswith("POSITIONAL_SIM_SELECTOR_POSITION "):
                row: dict[str, float | int] = {}
                for name in NUMERIC_FIELDS:
                    value = field(line, name)
                    row[name] = (
                        float(value)
                        if name in {
                            "positional_minus_static",
                            "sem",
                            "win_delta",
                            "spread_delta",
                        }
                        else int(value)
                    )
                rows.append(row)
            elif line.startswith("POSITIONAL_SIM_SELECTOR_DONE "):
                done_lines.append(line)
    if not rows or len(done_lines) != len(args.log):
        raise ValueError("log is incomplete")

    if any(
        row["static_iterations"] != row["positional_iterations"] for row in rows
    ):
        raise ValueError("selectors did not receive equal iteration counts")

    source_positions = sum(
        int(field(done_line, "source_positions")) for done_line in done_lines
    )
    candidate_set_changes = sum(
        int(field(done_line, "candidate_set_changes"))
        for done_line in done_lines
    )
    utility = np.asarray(
        [row["positional_minus_static"] for row in rows], dtype=np.float64
    )
    win = np.asarray([row["win_delta"] for row in rows], dtype=np.float64)
    spread = np.asarray([row["spread_delta"] for row in rows], dtype=np.float64)
    positive = int(np.count_nonzero(utility > 0))
    negative = int(np.count_nonzero(utility < 0))

    game_values: dict[int, list[float]] = defaultdict(list)
    for row in rows:
        game_values[int(row["game"])].append(
            float(row["positional_minus_static"])
        )
    all_source = np.zeros(source_positions, dtype=np.float64)
    all_source[: len(utility)] = utility
    print(
        f"accounting disagreements={len(rows)} source_positions={source_positions} "
        f"candidate_set_changes={candidate_set_changes} "
        "logged_disagreement_iterations_per_selector="
        f"{sum(int(row['static_iterations']) for row in rows)}"
    )
    for label, values in (
        ("conditional_utility", utility),
        ("conditional_win", win),
        ("conditional_spread", spread),
        ("all_source_utility", all_source),
    ):
        mean, low, high, p_value = mean_ci_p(values)
        print(
            f"{label} mean={mean:+.9f} ci95=[{low:+.9f},{high:+.9f}] "
            f"p={p_value:.6g}"
        )
    mean, low, high, p_value = clustered_mean_ci_p(game_values)
    print(
        "conditional_utility_game_clustered "
        f"mean={mean:+.9f} ci95=[{low:+.9f},{high:+.9f}] "
        f"p={p_value:.6g} games={len(game_values)}"
    )
    print(
        f"oracle_root_wins positional={positive} static={negative} "
        f"exact_two_sided_p={exact_sign_p(positive, negative):.6g}"
    )

    for label, minimum, maximum in (
        ("bag_61_plus", 61, 100),
        ("bag_45_60", 45, 60),
        ("bag_29_44", 29, 44),
    ):
        values = np.asarray(
            [
                row["positional_minus_static"]
                for row in rows
                if minimum <= int(row["bag"]) <= maximum
            ],
            dtype=np.float64,
        )
        if len(values) > 1:
            mean, low, high, p_value = mean_ci_p(values)
            print(
                f"{label} n={len(values)} mean={mean:+.9f} "
                f"ci95=[{low:+.9f},{high:+.9f}] p={p_value:.6g}"
            )


if __name__ == "__main__":
    main()
