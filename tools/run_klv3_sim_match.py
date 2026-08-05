#!/usr/bin/env python3
"""Run a restartable KLV2-vs-KLV3 simulation match.

Each round is an autoplay game-pair block with fresh seeds.  KLV3 alternates
between player 1 and player 2 across rounds.  Autoplay runs one game worker at
a time and gives all requested threads to each simulation search.  The CSV is
appended and flushed after every completed round, so a long match can be
stopped and resumed.

The match intentionally uses legacy two-ply simulation rather than
PlayChooser.  This isolates the leave model's effect on simulation from PEG
and endgame policy.
"""

from __future__ import annotations

import argparse
import csv
import math
import os
import statistics
import subprocess
import sys
import time
from pathlib import Path


CSV_FIELDS = [
    "round",
    "seed",
    "klv3_player",
    "game_pairs",
    "games",
    "klv3_wins",
    "klv3_losses",
    "ties",
    "klv3_mean_score",
    "klv2_mean_score",
    "klv3_mean_spread",
    "wall_seconds",
]


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, default=Path("bin/magpie"))
    parser.add_argument("--klv2", default="CSW24")
    parser.add_argument("--klv3", default="CSW24_klv3_ctx400")
    parser.add_argument("--rounds", type=int, default=20)
    parser.add_argument("--pairs-per-round", type=int, default=250)
    parser.add_argument("--seconds-per-turn", type=float, default=0.1)
    parser.add_argument("--threads", type=int, default=os.cpu_count() or 1)
    parser.add_argument("--num-plays", type=int, default=15)
    parser.add_argument("--plies", type=int, default=2)
    parser.add_argument("--min-play-iterations", type=int, default=100_000)
    parser.add_argument("--base-seed", type=int, default=812_401)
    parser.add_argument(
        "--output",
        type=Path,
        default=Path("obj/klv3-sim-match-100ms.csv"),
    )
    return parser.parse_args()


def validate_args(args: argparse.Namespace) -> None:
    for name in (
        "rounds",
        "pairs_per_round",
        "threads",
        "num_plays",
        "plies",
        "min_play_iterations",
    ):
        if getattr(args, name) <= 0:
            raise ValueError(f"--{name.replace('_', '-')} must be positive")
    if args.seconds_per_turn <= 0.0:
        raise ValueError("--seconds-per-turn must be positive")
    if not args.binary.is_file():
        raise ValueError(f"binary does not exist: {args.binary}")


def read_completed_rows(path: Path) -> list[dict[str, str]]:
    if not path.exists():
        return []
    with path.open(newline="", encoding="utf-8") as stream:
        reader = csv.DictReader(stream)
        if reader.fieldnames != CSV_FIELDS:
            raise ValueError(
                f"{path} has incompatible columns: {reader.fieldnames}"
            )
        return list(reader)


def parse_autoplay_line(output: str) -> tuple[int, int, int, int, float, float]:
    lines = [line for line in output.splitlines() if line.startswith("autoplay games ")]
    if not lines:
        raise RuntimeError(f"no autoplay result in output:\n{output}")
    fields = lines[-1].split()
    if len(fields) < 11:
        raise RuntimeError(f"malformed autoplay result: {lines[-1]}")
    games = int(fields[2])
    p0_wins = int(fields[3])
    p0_losses = int(fields[4])
    ties = int(fields[5])
    p0_mean_score = float(fields[7])
    p1_mean_score = float(fields[9])
    return games, p0_wins, p0_losses, ties, p0_mean_score, p1_mean_score


def make_command(args: argparse.Namespace, round_index: int) -> tuple[list[str], int]:
    # Alternate model-to-player assignment.  Game pairs already alternate the
    # starting player; this additionally removes any player-index asymmetry.
    klv3_player = 1 if round_index % 2 == 0 else 0
    leaves = [args.klv2, args.klv2]
    leaves[klv3_player] = args.klv3
    seed = args.base_seed + round_index * 1_000_003
    command = [
        str(args.binary),
        "autoplay",
        "games",
        str(args.pairs_per_round),
        "-lex",
        "CSW24",
        "-k1",
        leaves[0],
        "-k2",
        leaves[1],
        "-wmp",
        "true",
        "-rit",
        "true",
        "-ritmmap",
        "true",
        "-wit",
        "true",
        "-s1",
        "equity",
        "-s2",
        "equity",
        "-r1",
        "all",
        "-r2",
        "all",
        "-numplays",
        str(args.num_plays),
        "-plies",
        str(args.plies),
        "-tlim",
        format(args.seconds_per_turn, ".17g"),
        # Keep BAI from declaring a winner before the clock.  The time limit
        # still interrupts this deliberately unreachable per-arm minimum, as
        # it does in simbench.
        "-minplayiterations",
        str(args.min_play_iterations),
        "-sinfer",
        "false",
        "-gp",
        "true",
        "-threads",
        str(args.threads),
        "-mtmode",
        "igp",
        "-pfrequency",
        "0",
        "-hr",
        "false",
        "-savesettings",
        "false",
        "-autosavegcg",
        "false",
        "-fgrequired",
        "false",
        "-seed",
        str(seed),
    ]
    return command, klv3_player


def run_round(args: argparse.Namespace, round_index: int) -> dict[str, object]:
    command, klv3_player = make_command(args, round_index)
    seed = args.base_seed + round_index * 1_000_003
    started = time.monotonic()
    completed = subprocess.run(
        command,
        check=True,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
    )
    wall_seconds = time.monotonic() - started
    games, p0_wins, p0_losses, ties, p0_score, p1_score = parse_autoplay_line(
        completed.stdout
    )
    if games != 2 * args.pairs_per_round:
        raise RuntimeError(
            f"round {round_index} returned {games} games, expected "
            f"{2 * args.pairs_per_round}"
        )
    if klv3_player == 0:
        klv3_wins = p0_wins
        klv3_losses = p0_losses
        klv3_score = p0_score
        klv2_score = p1_score
    else:
        klv3_wins = p0_losses
        klv3_losses = p0_wins
        klv3_score = p1_score
        klv2_score = p0_score
    return {
        "round": round_index,
        "seed": seed,
        "klv3_player": klv3_player + 1,
        "game_pairs": args.pairs_per_round,
        "games": games,
        "klv3_wins": klv3_wins,
        "klv3_losses": klv3_losses,
        "ties": ties,
        "klv3_mean_score": f"{klv3_score:.6f}",
        "klv2_mean_score": f"{klv2_score:.6f}",
        "klv3_mean_spread": f"{klv3_score - klv2_score:.6f}",
        "wall_seconds": f"{wall_seconds:.3f}",
    }


def normal_two_sided_p(z_score: float) -> float:
    return math.erfc(abs(z_score) / math.sqrt(2.0))


def print_summary(rows: list[dict[str, str]]) -> None:
    if not rows:
        return
    games = sum(int(row["games"]) for row in rows)
    wins = sum(int(row["klv3_wins"]) for row in rows)
    losses = sum(int(row["klv3_losses"]) for row in rows)
    ties = sum(int(row["ties"]) for row in rows)
    decisive = wins + losses
    win_rate = wins / decisive if decisive else 0.5
    win_z = (
        (wins - decisive / 2.0) / math.sqrt(decisive / 4.0)
        if decisive
        else 0.0
    )
    round_spreads = [float(row["klv3_mean_spread"]) for row in rows]
    mean_spread = statistics.fmean(round_spreads)
    if len(round_spreads) >= 2:
        spread_se = statistics.stdev(round_spreads) / math.sqrt(len(round_spreads))
        spread_ci = 1.96 * spread_se
        spread_p = (
            normal_two_sided_p(mean_spread / spread_se)
            if spread_se > 0.0
            else 0.0
        )
    else:
        spread_ci = float("nan")
        spread_p = float("nan")
    elapsed = sum(float(row["wall_seconds"]) for row in rows)
    print(
        f"cumulative: rounds={len(rows)} games={games} "
        f"KLV3 W-L-T={wins}-{losses}-{ties} "
        f"decisive_win_rate={100.0 * win_rate:.3f}% "
        f"nominal_win_p={normal_two_sided_p(win_z):.4g} "
        f"spread={mean_spread:+.4f} "
        f"round-clustered_95%CI=±{spread_ci:.4f} "
        f"spread_p≈{spread_p:.4g} wall={elapsed / 60.0:.1f}m",
        flush=True,
    )


def main() -> int:
    args = parse_args()
    try:
        validate_args(args)
        rows = read_completed_rows(args.output)
    except ValueError as error:
        print(f"error: {error}", file=sys.stderr)
        return 2
    completed_rounds = {int(row["round"]) for row in rows}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    append = args.output.exists()
    with args.output.open("a", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=CSV_FIELDS)
        if not append:
            writer.writeheader()
            stream.flush()
        print_summary(rows)
        for round_index in range(args.rounds):
            if round_index in completed_rounds:
                continue
            print(
                f"round {round_index + 1}/{args.rounds}: "
                f"{args.pairs_per_round} game pairs",
                flush=True,
            )
            try:
                row = run_round(args, round_index)
            except (OSError, subprocess.CalledProcessError, RuntimeError) as error:
                print(f"round {round_index} failed: {error}", file=sys.stderr)
                return 1
            writer.writerow(row)
            stream.flush()
            rows.append({key: str(value) for key, value in row.items()})
            print_summary(rows)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
