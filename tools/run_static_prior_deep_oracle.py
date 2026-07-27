#!/usr/bin/env python3
"""Re-oracle recorded static-prior disagreements at a larger sample count."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import subprocess
import sys
import time


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def git_output(*args: str) -> str:
    return subprocess.run(
        ["git", *args],
        check=True,
        capture_output=True,
        text=True,
    ).stdout.strip()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--corpus", type=Path, action="append", required=True)
    parser.add_argument("--tag", required=True)
    parser.add_argument("--outer-samples", type=int, default=10000)
    parser.add_argument("--outer-plies", type=int, default=4)
    parser.add_argument("--nested-samples", type=int, default=100)
    parser.add_argument(
        "--spread-forecast",
        default="CSW24_spread_v1",
        help="terminal-spread forecast used by the nested oracle",
    )
    parser.add_argument("--threads", type=int, default=os.cpu_count() or 1)
    parser.add_argument("--wall-seconds", type=float, default=0)
    parser.add_argument("--max-disagreements", type=int, default=0)
    parser.add_argument("--start-disagreement", type=int, default=0)
    parser.add_argument("--binary", type=Path, default=Path("bin/magpie_test"))
    parser.add_argument("--output-dir", type=Path, default=Path("obj"))
    args = parser.parse_args()
    if args.outer_samples <= 0 or args.outer_plies <= 0:
        parser.error("outer samples and plies must be positive")
    if not args.spread_forecast:
        parser.error("--spread-forecast must not be empty")
    if args.nested_samples <= 0 or args.nested_samples % 2:
        parser.error("nested samples must be positive and even")
    if args.threads <= 0 or args.wall_seconds < 0:
        parser.error("threads must be positive and wall time nonnegative")
    if args.max_disagreements < 0 or args.start_disagreement < 0:
        parser.error("disagreement bounds must be nonnegative")
    missing = [str(path) for path in args.corpus if not path.is_file()]
    if missing:
        parser.error(f"missing corpus files: {', '.join(missing)}")

    args.output_dir.mkdir(parents=True, exist_ok=True)
    binary = args.binary.resolve()
    if not binary.is_file():
        parser.error(f"{binary} is not a file")
    manifest_path = (
        args.output_dir / f"static-prior-{args.tag}-manifest.jsonl"
    )
    log_path = args.output_dir / f"static-prior-{args.tag}.log"
    err_path = args.output_dir / f"static-prior-{args.tag}.err"
    for path in (manifest_path, log_path, err_path):
        if path.exists():
            parser.error(f"{path} already exists")

    corpus = ",".join(str(path.resolve()) for path in args.corpus)
    environment = {
        "KLV3_ORACLE_CORPUS_FILES": corpus,
        "KLV3_ORACLE_NESTED": "true",
        "KLV3_ORACLE_OUTER_SAMPLES": str(args.outer_samples),
        "KLV3_ORACLE_OUTER_PLIES": str(args.outer_plies),
        "KLV3_ORACLE_NESTED_SAMPLES": str(args.nested_samples),
        "KLV3_ORACLE_JUDGE_SPREAD_FORECAST": args.spread_forecast,
        "KLV3_ORACLE_THREADS": str(args.threads),
        "KLV3_ORACLE_WALL_SECONDS": f"{args.wall_seconds:g}",
        "KLV3_ORACLE_CORPUS_MAX_DISAGREEMENTS": str(args.max_disagreements),
        "KLV3_ORACLE_START_DISAGREEMENTS": str(args.start_disagreement),
    }
    started = time.time()
    provenance = {
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
        "environment": environment,
        "corpus_sha256": {
            str(path): sha256(path) for path in args.corpus
        },
    }
    with manifest_path.open("x", encoding="utf-8") as manifest:
        manifest.write(json.dumps(provenance, sort_keys=True) + "\n")
        manifest.flush()
        env = os.environ.copy()
        env.update(environment)
        with log_path.open("x", encoding="utf-8") as stdout, (
            err_path.open("x", encoding="utf-8")
        ) as stderr:
            completed = subprocess.run(
                [str(binary), "klv3oracle"],
                env=env,
                stdout=stdout,
                stderr=stderr,
                check=False,
            )
        manifest.write(
            json.dumps(
                {
                    "event": "done",
                    "time": time.time(),
                    "elapsed_seconds": time.time() - started,
                    "returncode": completed.returncode,
                },
                sort_keys=True,
            )
            + "\n"
        )
    return completed.returncode


if __name__ == "__main__":
    raise SystemExit(main())
