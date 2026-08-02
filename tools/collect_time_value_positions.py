#!/usr/bin/env python3
"""Generate complete, sequential source-game trajectories for TimeManager.

This runs one autoplay game at a time (``-mtmode igp`` inside ``pcbench``),
using the requested thread count only within each move analysis. By default
the benchmark runs every analysis but pins the played move to static equity,
making corpus generation cheap and reproducible. ``--play-analyzed-moves``
instead follows the actual PlayChooser policy, so a mixture of both corpora can
cover policy-dependent state distributions. Detailed PCTURN rows are preserved
as the provenance log and then strictly reconciled against PCGAME rows before
a raw-position corpus is written.
"""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import subprocess
import sys

from extract_time_value_positions import read_complete_games, write_corpus


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, default=Path("bin/magpie_test"))
    parser.add_argument("--games", type=int, required=True)
    parser.add_argument("--clock-ms", type=int, default=1)
    parser.add_argument("--threads", type=int, default=os.cpu_count() or 1)
    parser.add_argument("--seed", type=int, default=88001)
    parser.add_argument("--play-analyzed-moves", action="store_true")
    parser.add_argument("--log", type=Path, required=True)
    parser.add_argument("--corpus", type=Path, required=True)
    args = parser.parse_args()
    if args.games <= 0 or args.clock_ms <= 0 or args.threads <= 0:
        parser.error("games, clock-ms, and threads must be positive")
    if not args.binary.is_file():
        parser.error(f"binary not found: {args.binary}")

    args.log.parent.mkdir(parents=True, exist_ok=True)
    environment = os.environ.copy()
    environment.update(
        {
            "PCBENCH_GAMES": str(args.games),
            "PCBENCH_CLOCK_MS": str(args.clock_ms),
            "PCBENCH_THREADS": str(args.threads),
            "PCBENCH_WIT": "true",
            "PCBENCH_SEED": str(args.seed),
            "PCBENCH_GAME_PAIR": "false",
            "PCBENCH_COMMON_RNG": "true",
            "PCBENCH_TURN_ROWS": "true",
            "PCBENCH_GAME_ROWS": "true",
            "PCBENCH_DETAIL_EVENTS": "false",
            "PCBENCH_PLAY_MOVES": (
                "true" if args.play_analyzed_moves else "false"
            ),
        }
    )
    with args.log.open("w", encoding="utf-8") as output:
        completed = subprocess.run(
            [str(args.binary), "pcbench"],
            env=environment,
            stdout=output,
            stderr=subprocess.STDOUT,
            text=True,
            check=False,
        )
    if completed.returncode != 0:
        print(
            f"pcbench failed with status {completed.returncode}; see {args.log}",
            file=sys.stderr,
        )
        raise SystemExit(completed.returncode)

    games = read_complete_games([args.log])
    if len(games) != args.games:
        raise ValueError(
            f"requested {args.games} games but reconciled {len(games)}"
        )
    trajectory_policy = (
        f"playchooser-g{args.clock_ms}ms"
        if args.play_analyzed_moves
        else "static"
    )
    positions = write_corpus(
        games, args.corpus, trajectory_policy=trajectory_policy
    )
    print(
        f"games={len(games)} positions={positions} clock_ms={args.clock_ms} "
        f"threads={args.threads} trajectory_policy={trajectory_policy} "
        f"log={args.log} corpus={args.corpus}"
    )


if __name__ == "__main__":
    main()
