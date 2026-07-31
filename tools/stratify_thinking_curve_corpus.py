#!/usr/bin/env python3
"""Select one bag-stratified position per game from a candidate corpus."""

from __future__ import annotations

import argparse
from collections import OrderedDict, defaultdict
from dataclasses import dataclass
from pathlib import Path


@dataclass
class Position:
    game: int
    bag: int
    lines: list[str]


def parse_fields(line: str) -> dict[str, str]:
    return {
        key: value
        for token in line.split()
        if "=" in token
        for key, value in (token.split("=", 1),)
    }


def evenly_spaced(values: list[int], count: int) -> list[int]:
    if count >= len(values):
        return values
    return [
        values[((2 * index + 1) * len(values)) // (2 * count)]
        for index in range(count)
    ]


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("input", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--games", type=int, default=40)
    parser.add_argument(
        "--exclude-corpus",
        type=Path,
        help="exclude every game represented in this previously selected corpus",
    )
    parser.add_argument(
        "--bag-targets",
        default="82,72,62,52",
        help="comma-separated target bag sizes, cycled across selected games",
    )
    args = parser.parse_args()
    if args.games < 1:
        parser.error("--games must be positive")
    targets = [int(value) for value in args.bag_targets.split(",")]
    if not targets:
        parser.error("--bag-targets must not be empty")

    positions: OrderedDict[int, Position] = OrderedDict()
    with args.input.open(encoding="utf-8") as stream:
        for line in stream:
            if not line.startswith("POSITIONAL_CANDIDATE "):
                continue
            fields = parse_fields(line)
            position_id = int(fields["position"])
            position = positions.setdefault(
                position_id,
                Position(game=int(fields["game"]), bag=int(fields["bag"]), lines=[]),
            )
            if (
                position.game != int(fields["game"])
                or position.bag != int(fields["bag"])
            ):
                raise ValueError(f"inconsistent metadata for position {position_id}")
            position.lines.append(line)

    by_game: dict[int, list[tuple[int, Position]]] = defaultdict(list)
    for position_id, position in positions.items():
        by_game[position.game].append((position_id, position))
    excluded_games: set[int] = set()
    if args.exclude_corpus:
        with args.exclude_corpus.open(encoding="utf-8") as stream:
            for line in stream:
                if line.startswith("POSITIONAL_CANDIDATE "):
                    excluded_games.add(int(parse_fields(line)["game"]))
    # Avoid assigning a late-bag target to an unusually short source game (or
    # vice versa). Three tiles of slack retains nearly every ordinary game.
    eligible_games = [
        game
        for game, game_positions in by_game.items()
        if game not in excluded_games
        and min(position.bag for _, position in game_positions) <= min(targets) + 3
        and max(position.bag for _, position in game_positions)
        >= max(targets) - 3
    ]
    if len(eligible_games) < args.games:
        raise ValueError(
            f"only {len(eligible_games)} games span the requested bag targets"
        )
    selected_games = evenly_spaced(sorted(eligible_games), args.games)

    selected: list[tuple[int, Position]] = []
    for index, game in enumerate(selected_games):
        target = targets[index % len(targets)]
        selected.append(
            min(
                by_game[game],
                key=lambda item: (abs(item[1].bag - target), item[0]),
            )
        )

    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open("w", encoding="utf-8") as stream:
        for _, position in selected:
            stream.writelines(position.lines)
    print(
        f"selected={len(selected)} games={len(selected_games)} "
        f"positions={','.join(str(position_id) for position_id, _ in selected)} "
        f"bags={','.join(str(position.bag) for _, position in selected)}"
    )


if __name__ == "__main__":
    main()
