#!/usr/bin/env python3
"""Extract complete source-game positions from detailed autoplay traces.

The output is the raw-position input understood by the ``thinkingcurve`` test.
It deliberately retains every turn, including PEG and endgame turns: later
mode-specific calibration can join those boundaries without losing the game
suffix that makes rest-of-game labels possible.

PCTURN rows must include ``seed`` (new traces do). A source game is identified
by (seed, start); the legacy ``game`` field is only the within-pair game number.
Complete source games are accepted only when their PCGAME row reconciles with
the turn count. This prevents a killed autoplay process from silently creating
truncated value-to-go labels.
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass
from pathlib import Path
import re


FIELD_RE = re.compile(r'(\w+)=("(?:[^"\\]|\\.)*"|\S+)')


def fields(line: str) -> dict[str, str]:
    parsed: dict[str, str] = {}
    for key, raw in FIELD_RE.findall(line):
        if raw.startswith('"') and raw.endswith('"'):
            parsed[key] = raw[1:-1]
        else:
            parsed[key] = raw
    return parsed


@dataclass(frozen=True)
class SourcePosition:
    seed: int
    start: int
    turn: int
    player: int
    bag: int
    own_rack: int
    opp_rack: int
    spread: int
    cgp: str


def read_complete_games(paths: list[Path]) -> list[list[SourcePosition]]:
    turns: dict[tuple[int, int], list[SourcePosition]] = {}
    terminal_turns: dict[tuple[int, int], int] = {}
    for path in paths:
        with path.open(encoding="utf-8", errors="replace") as stream:
            for raw_line in stream:
                if raw_line.startswith("PCTURN "):
                    row = fields(raw_line)
                    required = {
                        "seed",
                        "turn",
                        "start",
                        "player",
                        "bag",
                        "own_rack",
                        "opp_rack",
                        "spread_before",
                        "cgp",
                    }
                    missing = required - set(row)
                    if missing:
                        raise ValueError(
                            f"{path}: PCTURN is missing {','.join(sorted(missing))}"
                        )
                    position = SourcePosition(
                        seed=int(row["seed"]),
                        start=int(row["start"]),
                        turn=int(row["turn"]),
                        player=int(row["player"]),
                        bag=int(row["bag"]),
                        own_rack=int(row["own_rack"]),
                        opp_rack=int(row["opp_rack"]),
                        spread=int(row["spread_before"]),
                        cgp=row["cgp"],
                    )
                    turns.setdefault((position.seed, position.start), []).append(
                        position
                    )
                elif raw_line.startswith("PCGAME "):
                    row = fields(raw_line)
                    required = {"seed", "start", "turns"}
                    missing = required - set(row)
                    if missing:
                        raise ValueError(
                            f"{path}: PCGAME is missing {','.join(sorted(missing))}"
                        )
                    key = (int(row["seed"]), int(row["start"]))
                    count = int(row["turns"])
                    previous = terminal_turns.setdefault(key, count)
                    if previous != count:
                        raise ValueError(f"conflicting PCGAME rows for {key}")

    complete: list[list[SourcePosition]] = []
    for key, expected in sorted(terminal_turns.items()):
        game_turns = sorted(turns.get(key, []), key=lambda item: item.turn)
        actual_numbers = [item.turn for item in game_turns]
        if actual_numbers != list(range(1, expected + 1)):
            raise ValueError(
                f"source game {key} is incomplete: expected turns 1..{expected}, "
                f"got {actual_numbers[:3]}...{actual_numbers[-3:]}"
            )
        for previous, current in zip(game_turns, game_turns[1:]):
            if current.bag > previous.bag:
                raise ValueError(f"bag count increased in source game {key}")
            if current.player == previous.player:
                raise ValueError(f"player did not alternate in source game {key}")
        complete.append(game_turns)
    if not complete:
        raise ValueError("no complete PCTURN/PCGAME source games found")
    return complete


def write_corpus(
    games: list[list[SourcePosition]],
    output: Path,
    trajectory_policy: str = "unknown",
) -> int:
    if not trajectory_policy or any(char.isspace() for char in trajectory_policy):
        raise ValueError("trajectory_policy must be one nonempty token")
    output.parent.mkdir(parents=True, exist_ok=True)
    position_index = 0
    with output.open("w", encoding="utf-8") as stream:
        for game_index, game in enumerate(games):
            for position in game:
                estimated_turns_remaining = max(
                    1,
                    (position.bag + position.own_rack + position.opp_rack + 7)
                    // 8,
                )
                predicted_future_turns = max(0, estimated_turns_remaining - 1)
                stream.write(
                    "TIME_VALUE_POSITION "
                    f"position={position_index} game={game_index} "
                    f"source_seed={position.seed} start={position.start} "
                    f"turn={position.turn} player={position.player} "
                    f"bag={position.bag} own_rack={position.own_rack} "
                    f"opp_rack={position.opp_rack} spread={position.spread} "
                    f"predicted_future_turns={predicted_future_turns} "
                    f"trajectory_policy={trajectory_policy} "
                    f"cgp={position.cgp}\n"
                )
                position_index += 1
    return position_index


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("logs", nargs="+", type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument(
        "--one-start-per-seed",
        action="store_true",
        help="retain only the lower starting-seat game from mirrored pairs",
    )
    parser.add_argument("--trajectory-policy", default="unknown")
    args = parser.parse_args()

    games = read_complete_games(args.logs)
    if args.one_start_per_seed:
        retained: dict[int, list[SourcePosition]] = {}
        for game in games:
            retained.setdefault(game[0].seed, game)
        games = [retained[seed] for seed in sorted(retained)]

    position_index = write_corpus(
        games, args.output, trajectory_policy=args.trajectory_policy
    )
    print(
        f"games={len(games)} positions={position_index} "
        f"output={args.output} scope=complete_observed_source_games"
    )


if __name__ == "__main__":
    main()
