#!/usr/bin/env python3

import argparse
import math
import os
from pathlib import Path
import re
import subprocess
import sys


ITERS_PER_SECOND_RE = re.compile(r"\(([0-9]+) iters/sec\)")
AUTOPLAY_RESULT_RE = re.compile(r"^autoplay games .+$", re.MULTILINE)


def run_simbench(binary, seed, rit):
    env = os.environ.copy()
    env["SIMBENCH_RIT"] = rit
    env["SIMBENCH_SEED"] = str(seed)
    env["SIMBENCH_WMP"] = "true"
    command = [str(binary), "simbench"]
    print(f"Running {' '.join(command)} with seed {seed}", flush=True)
    result = subprocess.run(
        command,
        check=False,
        cwd=Path(__file__).resolve().parent,
        env=env,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )
    print(result.stdout, end="", flush=True)
    if result.returncode != 0:
        raise RuntimeError(f"simbench exited with status {result.returncode}")

    rate_matches = ITERS_PER_SECOND_RE.findall(result.stdout)
    result_matches = AUTOPLAY_RESULT_RE.findall(result.stdout)
    if len(rate_matches) != 1 or len(result_matches) != 1:
        raise RuntimeError("could not parse a unique simbench result")
    return int(rate_matches[0]), result_matches[0].strip()


def markdown_table(rows, geometric_improvement, pairs_won):
    lines = [
        "## PGO simbench comparison",
        "",
        "| Seed | Release iters/sec | PGO iters/sec | Improvement |",
        "|---:|---:|---:|---:|",
    ]
    for seed, release_rate, pgo_rate, improvement in rows:
        lines.append(
            f"| {seed} | {release_rate:,} | {pgo_rate:,} | {improvement:+.2f}% |"
        )
    lines.extend(
        [
            "",
            f"Paired geometric-mean improvement: **{geometric_improvement:+.2f}%**  ",
            f"Pairs won: **{pairs_won}/{len(rows)}**",
        ]
    )
    return "\n".join(lines)


def parse_args():
    parser = argparse.ArgumentParser(
        description="Compare release and PGO Magpie binaries on paired simbench runs."
    )
    parser.add_argument("--release", type=Path, required=True)
    parser.add_argument("--pgo", type=Path, required=True)
    parser.add_argument("--seeds", type=int, nargs="+", required=True)
    parser.add_argument("--rit", choices=("true", "false"), default="false")
    parser.add_argument("--min-improvement", type=float, default=2.0)
    return parser.parse_args()


def main():
    args = parse_args()
    binaries = {
        "release": args.release.resolve(),
        "pgo": args.pgo.resolve(),
    }
    for binary in binaries.values():
        if not binary.is_file():
            raise RuntimeError(f"binary does not exist: {binary}")

    results = {}
    for seed_idx, seed in enumerate(args.seeds):
        # Alternate order to reduce systematic thermal and runner drift.
        variants = ("release", "pgo") if seed_idx % 2 == 0 else ("pgo", "release")
        results[seed] = {}
        for variant in variants:
            results[seed][variant] = run_simbench(
                binaries[variant], seed, args.rit
            )

    rows = []
    ratios = []
    pairs_won = 0
    for seed in args.seeds:
        release_rate, release_result = results[seed]["release"]
        pgo_rate, pgo_result = results[seed]["pgo"]
        if release_result != pgo_result:
            raise RuntimeError(f"release and PGO trajectories differ for seed {seed}")
        ratio = pgo_rate / release_rate
        improvement = (ratio - 1.0) * 100.0
        rows.append((seed, release_rate, pgo_rate, improvement))
        ratios.append(ratio)
        if pgo_rate > release_rate:
            pairs_won += 1

    geometric_ratio = math.exp(sum(math.log(ratio) for ratio in ratios) / len(ratios))
    geometric_improvement = (geometric_ratio - 1.0) * 100.0
    summary = markdown_table(rows, geometric_improvement, pairs_won)
    print(summary)
    github_summary = os.getenv("GITHUB_STEP_SUMMARY")
    if github_summary is not None:
        with open(github_summary, "a", encoding="utf-8") as summary_file:
            summary_file.write(summary + "\n")

    required_wins = len(args.seeds) // 2 + 1
    if geometric_improvement < args.min_improvement:
        print(
            f"PGO improvement {geometric_improvement:.2f}% is below the "
            f"required {args.min_improvement:.2f}%",
            file=sys.stderr,
        )
        return 1
    if pairs_won < required_wins:
        print(
            f"PGO won {pairs_won} pairs; at least {required_wins} are required",
            file=sys.stderr,
        )
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
