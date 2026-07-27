#!/usr/bin/env python3
"""Run sequential, fixed-compute static-prior oracle comparisons.

Each weight is compared with weight zero on the same deterministic positions,
nomination seeds, iteration cap, and neutral four-ply oracle.  The test binary
flushes every disagreement, so the per-weight logs retain enough information
for independent reanalysis.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import platform
import subprocess
import sys
import time


def parse_weights(value: str) -> list[float]:
    weights = [float(item) for item in value.split(",")]
    if not weights or any(
        not math.isfinite(weight) or weight < 0 for weight in weights
    ):
        raise argparse.ArgumentTypeError(
            "weights must be comma-separated nonnegative values"
        )
    if len(weights) != len(set(weights)):
        raise argparse.ArgumentTypeError("weights must not contain duplicates")
    return weights


def weight_label(weight: float) -> str:
    return f"{weight:g}".replace(".", "p")


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def git_output(*args: str) -> str:
    completed = subprocess.run(
        ["git", *args],
        check=True,
        capture_output=True,
        text=True,
    )
    return completed.stdout.strip()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--weights", type=parse_weights, required=True)
    parser.add_argument("--tag", required=True)
    parser.add_argument("--wall-seconds", type=float, required=True)
    parser.add_argument("--outer-samples", type=int, default=1000)
    parser.add_argument("--outer-plies", type=int, default=4)
    parser.add_argument("--nested-samples", type=int, default=100)
    parser.add_argument("--samples-per-arm", type=int, default=1500)
    parser.add_argument("--min-samples-per-arm", type=int, default=500)
    parser.add_argument("--threads", type=int, default=os.cpu_count() or 1)
    parser.add_argument("--start-game", type=int, default=0)
    parser.add_argument("--start-position", type=int, default=0)
    parser.add_argument("--collect-from-position", type=int)
    parser.add_argument("--start-disagreements", type=int, default=0)
    parser.add_argument("--max-positions", type=int, default=100000)
    parser.add_argument("--target-disagreements", type=int, default=10000)
    parser.add_argument("--binary", type=Path, default=Path("bin/magpie_test"))
    parser.add_argument("--output-dir", type=Path, default=Path("obj"))
    parser.add_argument("--overwrite", action="store_true")
    args = parser.parse_args()

    if args.samples_per_arm <= 0 or not (
        1 <= args.min_samples_per_arm <= args.samples_per_arm
    ):
        parser.error(
            "samples per arm must be positive and min samples must be in "
            "[1, samples per arm]"
        )
    if args.outer_samples <= 0 or args.outer_plies <= 0:
        parser.error("outer samples and plies must be positive")
    if args.nested_samples <= 0 or args.nested_samples % 2:
        parser.error("--nested-samples must be positive and even")
    if args.threads <= 0:
        parser.error("--threads must be positive")
    if not math.isfinite(args.wall_seconds) or args.wall_seconds <= 0:
        parser.error("--wall-seconds must be positive")
    collect_from_position = (
        args.start_position
        if args.collect_from_position is None
        else args.collect_from_position
    )
    if not 0 <= args.start_position <= collect_from_position < args.max_positions:
        parser.error(
            "positions must satisfy 0 <= start-position <= "
            "collect-from-position < max-positions"
        )
    if not 0 <= args.start_disagreements < args.target_disagreements:
        parser.error(
            "start-disagreements must be nonnegative and below "
            "target-disagreements"
        )

    args.output_dir.mkdir(parents=True, exist_ok=True)
    manifest_path = args.output_dir / f"static-prior-{args.tag}-manifest.jsonl"
    manifest_mode = "w" if args.overwrite else "x"
    try:
        manifest = manifest_path.open(manifest_mode, encoding="utf-8")
    except FileExistsError:
        parser.error(f"{manifest_path} already exists; use --overwrite intentionally")

    binary = args.binary.resolve()
    if not binary.is_file():
        manifest.close()
        parser.error(f"{binary} is not a file")

    common_env = {
        "KLV3_ORACLE_STATIC_PRIOR_ONLINE": "true",
        "KLV3_ORACLE_GAMES": "10000",
        "KLV3_ORACLE_START_GAME": str(args.start_game),
        "KLV3_ORACLE_START_POSITION": str(args.start_position),
        "KLV3_ORACLE_COLLECT_FROM_POSITION": str(collect_from_position),
        "KLV3_ORACLE_START_DISAGREEMENTS": str(args.start_disagreements),
        "KLV3_ORACLE_MAX_POSITIONS": str(args.max_positions),
        "KLV3_ORACLE_TARGET_DISAGREEMENTS": str(args.target_disagreements),
        "KLV3_ORACLE_SAMPLES_PER_ARM": str(args.samples_per_arm),
        "KLV3_ORACLE_MIN_SAMPLES_PER_ARM": str(args.min_samples_per_arm),
        "KLV3_ORACLE_OUTER_SAMPLES": str(args.outer_samples),
        "KLV3_ORACLE_OUTER_PLIES": str(args.outer_plies),
        "KLV3_ORACLE_NESTED_SAMPLES": str(args.nested_samples),
        "KLV3_ORACLE_THREADS": str(args.threads),
        "KLV3_ORACLE_WALL_SECONDS": f"{args.wall_seconds:g}",
    }
    started = time.time()
    manifest.write(
        json.dumps(
            {
                "event": "provenance",
                "time": started,
                "argv": sys.argv,
                "cwd": str(Path.cwd()),
                "platform": platform.platform(),
                "logical_cpus": os.cpu_count(),
                "git_head": git_output("rev-parse", "HEAD"),
                "git_status": git_output("status", "--short"),
                "binary": str(binary),
                "binary_sha256": sha256(binary),
            },
            sort_keys=True,
        )
        + "\n"
    )
    manifest.flush()
    for weight in args.weights:
        weight_started = time.time()
        label = weight_label(weight)
        log_path = args.output_dir / f"static-prior-{args.tag}-w{label}.log"
        err_path = args.output_dir / f"static-prior-{args.tag}-w{label}.err"
        for path in (log_path, err_path):
            if path.exists() and not args.overwrite:
                manifest.close()
                parser.error(f"{path} already exists; use --overwrite intentionally")

        env = os.environ.copy()
        env.update(common_env)
        env["KLV3_ORACLE_STATIC_PRIOR"] = f"{weight:g}"
        record = {
            "event": "start",
            "weight": weight,
            "time": weight_started,
            "log": str(log_path),
            "err": str(err_path),
            "environment": common_env,
        }
        manifest.write(json.dumps(record, sort_keys=True) + "\n")
        manifest.flush()
        with log_path.open("w", encoding="utf-8") as stdout, err_path.open(
            "w", encoding="utf-8"
        ) as stderr:
            completed = subprocess.run(
                [str(args.binary), "klv3oracle"],
                env=env,
                stdout=stdout,
                stderr=stderr,
                check=False,
            )
        record = {
            "event": "done",
            "weight": weight,
            "time": time.time(),
            "weight_elapsed_seconds": time.time() - weight_started,
            "sweep_elapsed_seconds": time.time() - started,
            "returncode": completed.returncode,
        }
        manifest.write(json.dumps(record, sort_keys=True) + "\n")
        manifest.flush()
        if completed.returncode != 0:
            manifest.close()
            return completed.returncode

    manifest.write(
        json.dumps(
            {
                "event": "sweep_done",
                "time": time.time(),
                "elapsed_seconds": time.time() - started,
            },
            sort_keys=True,
        )
        + "\n"
    )
    manifest.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
