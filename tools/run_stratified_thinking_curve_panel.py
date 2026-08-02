#!/usr/bin/env python3
"""Run a resumable, game-grouped thinking-curve panel.

Each subprocess writes a private chunk.  A chunk is appended to the canonical
log only after it exits successfully and every completed position passes the
structural accounting checks.  If the machine or process dies mid-position,
the partial chunk is retained for diagnosis but cannot contaminate labels.
"""

from __future__ import annotations

import argparse
from collections import defaultdict
import hashlib
import json
import math
import os
from pathlib import Path
import subprocess
import time

try:
    from tools.extract_time_value_positions import fields
except ModuleNotFoundError:
    from extract_time_value_positions import fields


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def parse_csv_ints(value: str) -> tuple[int, ...]:
    values = tuple(int(item) for item in value.split(","))
    if not values or any(item <= 0 for item in values):
        raise ValueError("integer lists must be nonempty and positive")
    if tuple(sorted(set(values))) != values:
        raise ValueError("integer lists must be strictly increasing")
    return values


def eligible_indices(corpus: Path, minimum_bag: int, maximum_bag: int) -> list[int]:
    eligible: list[int] = []
    expected_position = 0
    with corpus.open(encoding="utf-8") as stream:
        for source_index, line in enumerate(stream):
            if not line.startswith("TIME_VALUE_POSITION "):
                raise ValueError("corpus contains a non-position row")
            row = fields(line)
            position = int(row["position"])
            if position != expected_position or source_index != position:
                raise ValueError("panel positions must be globally consecutive")
            expected_position += 1
            bag = int(row["bag"])
            if minimum_bag <= bag <= maximum_bag:
                eligible.append(source_index)
    if not eligible:
        raise ValueError("panel has no positions in the requested bag range")
    return eligible


def completed_by_plies(log: Path) -> dict[int, set[int]]:
    completed: dict[int, set[int]] = defaultdict(set)
    if not log.exists():
        return completed
    with log.open(encoding="utf-8") as stream:
        for line in stream:
            if line.startswith("THINKING_CURVE_POSITION_DONE "):
                row = fields(line)
                if int(row.get("dropped", "-1")) != 0:
                    raise ValueError("canonical log contains a dropped event")
                completed[int(row["plies"])].add(int(row["source_index"]))
            elif line.startswith("THINKING_CURVE_FORCED_POSITION "):
                row = fields(line)
                completed[int(row["plies"])].add(int(row["source_index"]))
    return completed


def validate_prefix(eligible: list[int], completed: set[int]) -> int:
    unknown = completed - set(eligible)
    if unknown:
        raise ValueError(f"canonical log completed ineligible positions: {sorted(unknown)[:3]}")
    first_missing = len(eligible)
    for index, source_index in enumerate(eligible):
        if source_index not in completed:
            first_missing = index
            break
    if any(source_index in completed for source_index in eligible[first_missing + 1 :]):
        raise ValueError("canonical log completion is not a contiguous eligible prefix")
    return first_missing


def write_state(path: Path, document: dict[str, object]) -> None:
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(
        json.dumps(document, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    temporary.replace(path)


def chunk_completed(path: Path, plies: int) -> list[int]:
    completed = completed_by_plies(path).get(plies, set())
    if "THINKING_CURVE_DONE " not in path.read_text(encoding="utf-8"):
        raise ValueError("successful chunk lacks THINKING_CURVE_DONE")
    if not completed:
        raise ValueError("successful chunk completed no positions")
    return sorted(completed)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--corpus", required=True, type=Path)
    parser.add_argument("--binary", required=True, type=Path)
    parser.add_argument("--run-dir", required=True, type=Path)
    parser.add_argument("--plies", default="2,4,6")
    parser.add_argument("--targets", default="50000,100000,200000,300000")
    parser.add_argument("--max-nodes", type=int, default=300000)
    parser.add_argument("--num-plays", type=int, default=15)
    parser.add_argument("--threads", type=int, default=os.cpu_count() or 1)
    parser.add_argument("--minimum-bag", type=int, default=5)
    parser.add_argument("--maximum-bag", type=int, default=100)
    parser.add_argument("--judge-plies", type=int, default=10)
    parser.add_argument("--judge-samples", type=int, default=100000)
    parser.add_argument("--judge-risk-plays", type=int, default=0)
    parser.add_argument("--uniform-floor-per-mille", type=int, default=100)
    parser.add_argument("--chunk-positions", type=int, default=8)
    parser.add_argument("--wall-seconds", type=float, default=0.0)
    args = parser.parse_args()

    args.corpus = args.corpus.resolve()
    args.binary = args.binary.resolve()
    args.run_dir = args.run_dir.resolve()
    # MAGPIE's default data paths are repository-relative. launchd does not
    # guarantee a useful working directory, so make this invariant explicit.
    os.chdir(Path(__file__).resolve().parent.parent)
    if not args.corpus.is_file() or not args.binary.is_file():
        parser.error("corpus and binary must exist")
    plies_values = parse_csv_ints(args.plies)
    targets = parse_csv_ints(args.targets)
    if args.max_nodes < targets[-1]:
        parser.error("max-nodes must include the largest target")
    if (
        args.threads <= 0
        or args.num_plays < 2
        or args.chunk_positions <= 0
        or args.judge_plies <= max(plies_values)
        or args.judge_samples <= 0
        or args.judge_risk_plays == 1
        or args.judge_risk_plays < 0
        or args.judge_risk_plays > args.num_plays
        or args.minimum_bag < 0
        or args.maximum_bag < args.minimum_bag
        or not 0 < args.uniform_floor_per_mille <= 1000
        or not math.isfinite(args.wall_seconds)
        or args.wall_seconds < 0.0
    ):
        parser.error("invalid curve protocol")

    args.run_dir.mkdir(parents=True, exist_ok=True)
    chunks = args.run_dir / "chunks"
    chunks.mkdir(exist_ok=True)
    log = args.run_dir / "thinking-curves.log"
    err = args.run_dir / "thinking-curves.err"
    state_path = args.run_dir / "state.json"
    protocol = {
        "corpus": str(args.corpus),
        "corpus_sha256": sha256(args.corpus),
        "binary": str(args.binary),
        "binary_sha256": sha256(args.binary),
        "plies": list(plies_values),
        "targets": list(targets),
        "max_nodes": args.max_nodes,
        "num_plays": args.num_plays,
        "threads": args.threads,
        "minimum_bag": args.minimum_bag,
        "maximum_bag": args.maximum_bag,
        "judge_plies": args.judge_plies,
        "judge_samples": args.judge_samples,
        "judge_risk_plays": args.judge_risk_plays,
        "uniform_floor_per_mille": args.uniform_floor_per_mille,
        "chunk_positions": args.chunk_positions,
    }
    if state_path.exists():
        state = json.loads(state_path.read_text(encoding="utf-8"))
        if state.get("protocol") != protocol:
            raise ValueError("resume protocol does not match existing state")
    else:
        state = {
            "artifact_kind": "stratified_thinking_curve_run",
            "protocol": protocol,
            "status": "running",
            "start_epoch": time.time(),
            "attempts": 0,
        }

    eligible = eligible_indices(args.corpus, args.minimum_bag, args.maximum_bag)
    deadline = (
        time.monotonic() + args.wall_seconds if args.wall_seconds > 0.0 else math.inf
    )
    with log.open("a", encoding="utf-8") as canonical_log:
        canonical_log.write(
            "THINKING_CURVE_PANEL_RESUME "
            f"epoch={time.time():.6f} eligible={len(eligible)} "
            f"plies={args.plies} targets={args.targets}\n"
        )

    while time.monotonic() < deadline:
        made_progress = False
        for plies in plies_values:
            completed = completed_by_plies(log).get(plies, set())
            first_missing = validate_prefix(eligible, completed)
            if first_missing == len(eligible):
                continue
            remaining_wall = deadline - time.monotonic()
            if remaining_wall <= 0.0:
                break
            made_progress = True
            state["attempts"] = int(state.get("attempts", 0)) + 1
            attempt = int(state["attempts"])
            skip = eligible[first_missing]
            requested = min(args.chunk_positions, len(eligible) - first_missing)
            partial_log = chunks / f"attempt-{attempt:05d}-p{plies}-s{skip}.partial.log"
            partial_err = chunks / f"attempt-{attempt:05d}-p{plies}-s{skip}.partial.err"
            environment = os.environ.copy()
            environment.update(
                {
                    "THINKING_CURVE_CORPUS": str(args.corpus),
                    "THINKING_CURVE_SKIP_POSITIONS": str(skip),
                    "THINKING_CURVE_MAX_POSITIONS": str(requested),
                    "THINKING_CURVE_MIN_BAG": str(args.minimum_bag),
                    "THINKING_CURVE_MAX_BAG": str(args.maximum_bag),
                    "THINKING_CURVE_NUM_PLAYS": str(args.num_plays),
                    "THINKING_CURVE_PLIES": str(plies),
                    "THINKING_CURVE_THREADS": str(args.threads),
                    "THINKING_CURVE_MAX_NODES": str(args.max_nodes),
                    "THINKING_CURVE_UNIFORM_FLOOR_PER_MILLE": str(
                        args.uniform_floor_per_mille
                    ),
                    "THINKING_CURVE_JUDGE_PLIES": str(args.judge_plies),
                    "THINKING_CURVE_JUDGE_SAMPLES": str(args.judge_samples),
                    "THINKING_CURVE_JUDGE_RISK_PLAYS": str(
                        args.judge_risk_plays
                    ),
                    "THINKING_CURVE_REGRET_TRACE": "0",
                    "THINKING_CURVE_REGRET_STOP_TARGET": "0",
                    "THINKING_CURVE_WALL_SECONDS": (
                        "0" if math.isinf(deadline) else f"{remaining_wall:.6f}"
                    ),
                    "THINKING_CURVE_TARGET_NODES": args.targets,
                }
            )
            with partial_log.open("w", encoding="utf-8") as stdout, partial_err.open(
                "w", encoding="utf-8"
            ) as stderr:
                result = subprocess.run(
                    [str(args.binary), "thinkingcurve"],
                    env=environment,
                    stdout=stdout,
                    stderr=stderr,
                    check=False,
                )
            if result.returncode != 0:
                state.update(
                    {
                        "status": "failed",
                        "failed_attempt": attempt,
                        "returncode": result.returncode,
                    }
                )
                write_state(state_path, state)
                raise SystemExit(result.returncode)

            accepted = chunk_completed(partial_log, plies)
            expected = eligible[first_missing : first_missing + len(accepted)]
            if accepted != expected or len(accepted) > requested:
                raise ValueError(
                    f"chunk p{plies} completed unexpected source indices"
                )
            with log.open("a", encoding="utf-8") as canonical_log:
                canonical_log.write(partial_log.read_text(encoding="utf-8"))
            with err.open("a", encoding="utf-8") as canonical_err:
                canonical_err.write(partial_err.read_text(encoding="utf-8"))
            partial_log.replace(partial_log.with_name(partial_log.name.replace(".partial", ".accepted")))
            partial_err.replace(partial_err.with_name(partial_err.name.replace(".partial", ".accepted")))

            completed_now = completed_by_plies(log)
            state.update(
                {
                    "status": "running",
                    "last_update_epoch": time.time(),
                    "eligible_positions": len(eligible),
                    "completed_by_plies": {
                        str(value): len(completed_now.get(value, set()))
                        for value in plies_values
                    },
                }
            )
            write_state(state_path, state)
        if not made_progress:
            break

    completed_final = completed_by_plies(log)
    complete = all(
        validate_prefix(eligible, completed_final.get(plies, set())) == len(eligible)
        for plies in plies_values
    )
    state.update(
        {
            "status": "complete" if complete else "wall_limit",
            "end_epoch": time.time(),
            "eligible_positions": len(eligible),
            "completed_by_plies": {
                str(value): len(completed_final.get(value, set()))
                for value in plies_values
            },
        }
    )
    write_state(state_path, state)
    print(json.dumps(state, sort_keys=True))


if __name__ == "__main__":
    main()
