#!/usr/bin/env python3
"""Train MAGPIE's compact game-state spread forecast from FJ autoplay rows.

The model predicts the score swing from a pre-move position to the end of the
game, always from the player-on-turn perspective.  Its state key is deliberately
small and causal for the first version:

    (tiles in bag, tiles on-turn rack, tiles off-turn rack)

This directly represents end-of-bag timing.  Expected remaining turns for both
players are fitted and stored beside spread so that the proposed mechanism can
be inspected rather than inferred from an aggregate error metric.
"""

from __future__ import annotations

import argparse
import csv
import glob
import math
import os
import struct
from collections import defaultdict
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable

MAGIC = b"MAGSPRD\0"
VERSION = 1
RACK_SIZE = 7


@dataclass
class Row:
    seed: int
    turn: int
    player: int
    rack_count: int
    score_diff: float
    final_spread: float
    unseen_total: int
    bag_count: int = 0
    opponent_rack_count: int = 0
    future_self_turns: int = 0
    future_opp_turns: int = 0

    @property
    def spread_swing(self) -> float:
        return self.final_spread - self.score_diff

    @property
    def state(self) -> tuple[int, int, int]:
        return self.bag_count, self.rack_count, self.opponent_rack_count


@dataclass
class Sums:
    count: int = 0
    spread: float = 0.0
    self_turns: float = 0.0
    opp_turns: float = 0.0

    def add(self, row: Row) -> None:
        self.count += 1
        self.spread += row.spread_swing
        self.self_turns += row.future_self_turns
        self.opp_turns += row.future_opp_turns


@dataclass(frozen=True)
class Cell:
    spread: float
    self_turns: float
    opp_turns: float
    count: int


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--fj-glob", required=True)
    parser.add_argument("--output", required=True)
    parser.add_argument("--holdout-modulus", type=int, default=5)
    parser.add_argument("--holdout-remainder", type=int, default=0)
    parser.add_argument(
        "--prior-strength",
        type=float,
        default=32.0,
        help="bag-level pseudo-observations used to shrink sparse rack states",
    )
    return parser.parse_args()


def count_rack_tiles(text: str) -> int:
    # FJ uses one display codepoint per machine tile for the standard English
    # distribution. Parenthesized blanks are not used in rack strings.
    return len(text.strip())


def read_rows(pattern: str) -> list[Row]:
    paths = sorted(glob.glob(pattern))
    if not paths:
        raise SystemExit(f"no FJ files matched {pattern!r}")
    games: dict[int, list[Row]] = defaultdict(list)
    malformed = 0
    for filename in paths:
        with open(filename, newline="", encoding="utf-8") as stream:
            for fields in csv.reader(stream):
                if not fields:
                    continue
                try:
                    row = Row(
                        seed=int(fields[0]),
                        turn=int(fields[1]),
                        player=int(fields[2]),
                        rack_count=count_rack_tiles(fields[6]),
                        score_diff=float(fields[13]),
                        final_spread=float(fields[14]),
                        unseen_total=int(fields[15]),
                    )
                except (IndexError, ValueError):
                    malformed += 1
                    continue
                if row.unseen_total > RACK_SIZE:
                    row.bag_count = row.unseen_total - RACK_SIZE
                    row.opponent_rack_count = RACK_SIZE
                else:
                    # Once the bag is empty, "unseen" is exactly the
                    # opponent's current rack.
                    row.bag_count = 0
                    row.opponent_rack_count = row.unseen_total
                games[row.seed].append(row)

    rows: list[Row] = []
    for game_rows in games.values():
        game_rows.sort(key=lambda row: row.turn)
        remaining = [0, 0]
        for row in reversed(game_rows):
            remaining[row.player] += 1
            row.future_self_turns = remaining[row.player]
            row.future_opp_turns = remaining[1 - row.player]
        rows.extend(game_rows)
    if malformed:
        print(f"ignored {malformed} malformed or legacy FJ rows")
    return rows


def is_holdout(row: Row, modulus: int, remainder: int) -> bool:
    # Seeds are already deterministic 64-bit game identifiers. Modulo splitting
    # keeps every turn of a game on one side of the boundary.
    return row.seed % modulus == remainder


def nearest_bag_value(values: dict[int, tuple[float, float, float]], bag: int):
    if bag in values:
        return values[bag]
    nearest = min(values, key=lambda candidate: (abs(candidate - bag), candidate))
    return values[nearest]


def fit_cells(
    train_rows: Iterable[Row], max_bag: int, prior_strength: float
) -> dict[tuple[int, int, int], Cell]:
    state_sums: dict[tuple[int, int, int], Sums] = defaultdict(Sums)
    bag_sums: dict[int, Sums] = defaultdict(Sums)
    for row in train_rows:
        state_sums[row.state].add(row)
        bag_sums[row.bag_count].add(row)

    bag_means = {
        bag: (
            sums.spread / sums.count,
            sums.self_turns / sums.count,
            sums.opp_turns / sums.count,
        )
        for bag, sums in bag_sums.items()
        if sums.count
    }
    if not bag_means:
        raise SystemExit("training split has no rows")

    cells: dict[tuple[int, int, int], Cell] = {}
    for bag in range(max_bag + 1):
        prior = nearest_bag_value(bag_means, bag)
        for self_rack in range(RACK_SIZE + 1):
            for opp_rack in range(RACK_SIZE + 1):
                state = bag, self_rack, opp_rack
                sums = state_sums[state]
                denominator = sums.count + prior_strength
                cells[state] = Cell(
                    spread=(sums.spread + prior_strength * prior[0]) / denominator,
                    self_turns=(
                        sums.self_turns + prior_strength * prior[1]
                    )
                    / denominator,
                    opp_turns=(sums.opp_turns + prior_strength * prior[2])
                    / denominator,
                    count=sums.count,
                )
    return cells


def write_model(path: str, cells: dict[tuple[int, int, int], Cell], max_bag: int):
    output = Path(path)
    output.parent.mkdir(parents=True, exist_ok=True)
    cell_count = (max_bag + 1) * (RACK_SIZE + 1) * (RACK_SIZE + 1)
    with output.open("wb") as stream:
        stream.write(MAGIC)
        stream.write(struct.pack("<IIII", VERSION, max_bag, RACK_SIZE, cell_count))
        for bag in range(max_bag + 1):
            for self_rack in range(RACK_SIZE + 1):
                for opp_rack in range(RACK_SIZE + 1):
                    cell = cells[bag, self_rack, opp_rack]
                    stream.write(
                        struct.pack(
                            "<fffI",
                            cell.spread,
                            cell.self_turns,
                            cell.opp_turns,
                            cell.count,
                        )
                    )


def summarize_errors(
    rows: Iterable[Row],
    cells: dict[tuple[int, int, int], Cell],
    label: str,
) -> None:
    rows = list(rows)
    if not rows:
        print(f"{label}: no rows")
        return

    def metrics(
        selected: list[Row],
    ) -> tuple[int, float, float, float, float, float, float]:
        errors = [
            cells[row.state].spread - row.spread_swing for row in selected
        ]
        baseline_errors = [-row.spread_swing for row in selected]
        self_turn_errors = [
            cells[row.state].self_turns - row.future_self_turns
            for row in selected
        ]
        opp_turn_errors = [
            cells[row.state].opp_turns - row.future_opp_turns
            for row in selected
        ]
        return (
            len(selected),
            sum(abs(error) for error in errors) / len(errors),
            math.sqrt(sum(error * error for error in errors) / len(errors)),
            sum(abs(error) for error in baseline_errors) / len(errors),
            math.sqrt(
                sum(error * error for error in baseline_errors) / len(errors)
            ),
            sum(abs(error) for error in self_turn_errors) / len(selected),
            sum(abs(error) for error in opp_turn_errors) / len(selected),
        )

    strata = [
        ("all", lambda row: True),
        ("bag=0", lambda row: row.bag_count == 0),
        ("bag=1..7", lambda row: 1 <= row.bag_count <= 7),
        ("bag=8..20", lambda row: 8 <= row.bag_count <= 20),
        ("bag>=21", lambda row: row.bag_count >= 21),
    ]
    print(f"{label} spread forecast (model vs current-spread-only baseline)")
    print(
        "stratum,count,model_mae,model_rmse,baseline_mae,baseline_rmse,"
        "on_turn_turn_mae,off_turn_turn_mae"
    )
    for name, predicate in strata:
        selected = [row for row in rows if predicate(row)]
        if not selected:
            continue
        (
            count,
            mae,
            rmse,
            baseline_mae,
            baseline_rmse,
            self_turn_mae,
            opp_turn_mae,
        ) = metrics(selected)
        print(
            f"{name},{count},{mae:.6f},{rmse:.6f},"
            f"{baseline_mae:.6f},{baseline_rmse:.6f},"
            f"{self_turn_mae:.6f},{opp_turn_mae:.6f}"
        )


def main() -> None:
    args = parse_args()
    if args.holdout_modulus < 2:
        raise SystemExit("--holdout-modulus must be at least 2")
    if not 0 <= args.holdout_remainder < args.holdout_modulus:
        raise SystemExit("--holdout-remainder must be within the modulus")
    if args.prior_strength <= 0:
        raise SystemExit("--prior-strength must be positive")

    rows = read_rows(args.fj_glob)
    train_rows = [
        row
        for row in rows
        if not is_holdout(row, args.holdout_modulus, args.holdout_remainder)
    ]
    holdout_rows = [
        row
        for row in rows
        if is_holdout(row, args.holdout_modulus, args.holdout_remainder)
    ]
    max_bag = max(row.bag_count for row in rows)
    cells = fit_cells(train_rows, max_bag, args.prior_strength)
    write_model(args.output, cells, max_bag)
    print(
        f"rows={len(rows)} train={len(train_rows)} holdout={len(holdout_rows)} "
        f"max_bag={max_bag} output={args.output} "
        f"bytes={os.path.getsize(args.output)}"
    )
    summarize_errors(holdout_rows, cells, "held-out")


if __name__ == "__main__":
    main()
