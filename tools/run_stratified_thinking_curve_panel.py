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


def _control_kind(row: dict[str, str]) -> str:
    kind = row.get("control_kind")
    if kind is not None:
        return kind
    return "full" if row.get("control", "0") == "1" else "stopped"


def validate_chunk_accounting(
    path: Path,
    *,
    plies: int,
    targets: tuple[int, ...],
    max_nodes: int,
    num_plays: int,
    judge_samples: int,
    regret_stop_target: float,
    regret_stop_use_joint: bool,
    regret_trace: bool,
    regret_risk_plays: int,
    regret_paired_samples: int,
    fixed_control: bool,
    matched_control: bool,
) -> list[int]:
    """Validate every accepted root before its private log is appended."""

    text = path.read_text(encoding="utf-8")
    if "THINKING_CURVE_DONE " not in text:
        raise ValueError("successful chunk lacks THINKING_CURVE_DONE")
    points: dict[int, list[dict[str, str]]] = defaultdict(list)
    done: dict[int, dict[str, str]] = {}
    panels: dict[int, list[dict[str, str]]] = defaultdict(list)
    arms: dict[tuple[int, int], int] = defaultdict(int)
    samples: dict[tuple[int, int], int] = defaultdict(int)
    forced: set[int] = set()
    for line in text.splitlines():
        if line.startswith("THINKING_CURVE_POINT "):
            row = fields(line)
            if int(row["plies"]) == plies and row.get("final") == "0":
                points[int(row["source_index"])].append(row)
        elif line.startswith("THINKING_CURVE_POSITION_DONE "):
            row = fields(line)
            if int(row["plies"]) != plies:
                continue
            source_index = int(row["source_index"])
            if source_index in done:
                raise ValueError(f"duplicate done row for source {source_index}")
            done[source_index] = row
        elif line.startswith("THINKING_CURVE_FORCED_POSITION "):
            row = fields(line)
            if int(row["plies"]) == plies:
                forced.add(int(row["source_index"]))
        elif line.startswith("THINKING_CURVE_REGRET_PANEL "):
            row = fields(line)
            if int(row["plies"]) == plies:
                panels[int(row["source_index"])].append(row)
        elif line.startswith("THINKING_CURVE_REGRET_ARM "):
            row = fields(line)
            if int(row["plies"]) == plies:
                arms[(int(row["source_index"]), int(row["target_nodes"]))] += 1
        elif line.startswith("THINKING_CURVE_REGRET_SAMPLE "):
            row = fields(line)
            if int(row["plies"]) == plies:
                samples[(int(row["source_index"]), int(row["target_nodes"]))] += 1

    if set(done) & forced:
        raise ValueError("chunk marks a root both forced and completed")
    accepted = sorted(set(done) | forced)
    if not accepted:
        raise ValueError("successful chunk completed no positions")
    included_targets = tuple(target for target in targets if target <= max_nodes)
    if not included_targets:
        raise ValueError("protocol has no in-range targets")
    expected_control_kinds = {"stopped"}
    if fixed_control:
        expected_control_kinds.add("full")
    if matched_control:
        expected_control_kinds.add("matched")

    for source_index, done_row in done.items():
        if int(done_row.get("dropped", "-1")) != 0:
            raise ValueError(f"source {source_index} dropped progress events")
        root_points = points.get(source_index, [])
        by_kind: dict[str, list[dict[str, str]]] = defaultdict(list)
        for row in root_points:
            kind = _control_kind(row)
            if kind not in {"stopped", "matched", "full"}:
                raise ValueError(f"source {source_index} has unknown control kind")
            by_kind[kind].append(row)
        if set(by_kind) != expected_control_kinds:
            raise ValueError(
                f"source {source_index} control kinds {sorted(by_kind)} "
                f"!= {sorted(expected_control_kinds)}"
            )
        stopped = by_kind["stopped"]
        if tuple(int(row["target_nodes"]) for row in stopped) != included_targets:
            raise ValueError(f"source {source_index} has wrong stopped targets")
        expected_events = 2 * (
            len(included_targets) + int(fixed_control) + int(matched_control)
        )
        if int(done_row["events"]) != expected_events:
            raise ValueError(f"source {source_index} has wrong event count")

        for row in stopped:
            target_nodes = int(row["target_nodes"])
            iterations = int(row["iterations"])
            nodes = int(row["nodes"])
            status = int(row["bai_status"])
            maximum_iterations = target_nodes // (plies + 1)
            if iterations > maximum_iterations or nodes > target_nodes:
                raise ValueError(f"source {source_index} exceeded stopped budget")
            if status == 3 and iterations != maximum_iterations:
                raise ValueError(f"source {source_index} missed fixed iteration cap")
            if status == 4:
                estimate_key = (
                    "joint_regret_at_stop"
                    if regret_stop_use_joint
                    else "regret_at_stop"
                )
                estimate = float(row[estimate_key])
                if not math.isfinite(estimate) or estimate > regret_stop_target + 1e-12:
                    raise ValueError(f"source {source_index} stopped above target")
            if status not in {1, 3, 4}:
                raise ValueError(f"source {source_index} has invalid BAI status")
            minimum = int(row["min_play_iterations"])
            if iterations < minimum * num_plays:
                raise ValueError(f"source {source_index} missed sampling floor")

        primary = stopped[-1]
        primary_iterations = int(primary["iterations"])
        if matched_control:
            if len(by_kind["matched"]) != 1:
                raise ValueError(f"source {source_index} lacks one matched control")
            matched = by_kind["matched"][0]
            matched_target = primary_iterations * (plies + 1)
            if int(matched["target_nodes"]) != matched_target:
                raise ValueError(f"source {source_index} matched target is not exact")
            if int(matched["iterations"]) != primary_iterations:
                raise ValueError(f"source {source_index} matched iterations differ")
            if int(matched["nodes"]) > matched_target:
                raise ValueError(f"source {source_index} matched nodes exceed target")
            if int(done_row.get("matched_control", "0")) != 1:
                raise ValueError(f"source {source_index} done row lacks matched flag")
            for key, expected in (
                ("matched_target_nodes", matched_target),
                ("matched_iterations", primary_iterations),
                ("matched_nodes", int(matched["nodes"])),
            ):
                if int(done_row.get(key, "-1")) != expected:
                    raise ValueError(f"source {source_index} mismatches {key}")
        if fixed_control:
            if len(by_kind["full"]) != 1:
                raise ValueError(f"source {source_index} lacks one full control")
            full = by_kind["full"][0]
            full_target = included_targets[-1]
            maximum_iterations = full_target // (plies + 1)
            if int(full["target_nodes"]) != full_target:
                raise ValueError(f"source {source_index} full target differs")
            if int(full["bai_status"]) == 3 and int(full["iterations"]) != maximum_iterations:
                raise ValueError(f"source {source_index} full iterations differ")
            if int(full["iterations"]) > maximum_iterations or int(full["nodes"]) > full_target:
                raise ValueError(f"source {source_index} full control exceeded budget")
            if int(done_row.get("control", "0")) != 1:
                raise ValueError(f"source {source_index} done row lacks full flag")
            if int(done_row.get("control_iterations", "-1")) != int(full["iterations"]):
                raise ValueError(f"source {source_index} full iterations do not join")
            if int(done_row.get("control_nodes", "-1")) != int(full["nodes"]):
                raise ValueError(f"source {source_index} full nodes do not join")

        if int(done_row["final_iterations"]) != primary_iterations:
            raise ValueError(f"source {source_index} final iterations do not join")
        if int(done_row["final_nodes"]) != int(primary["nodes"]):
            raise ValueError(f"source {source_index} final nodes do not join")
        judge_candidates = int(done_row["judge_candidates"])
        judge_iterations = int(done_row["judge_iterations"])
        judge_forced = int(done_row["judge_forced"])
        if judge_forced:
            if judge_candidates != 1 or judge_iterations != 0:
                raise ValueError(f"source {source_index} forced judge is invalid")
        elif judge_candidates < 2 or judge_iterations != judge_samples * judge_candidates:
            raise ValueError(f"source {source_index} judge accounting differs")

        root_panels = panels.get(source_index, [])
        if regret_trace:
            if len(root_panels) != len(included_targets):
                raise ValueError(f"source {source_index} lacks regret panels")
            for panel in root_panels:
                target_nodes = int(panel["target_nodes"])
                risk_count = int(panel["risk_count"])
                paired_rows = int(panel["paired_rows"])
                if not 2 <= risk_count <= regret_risk_plays:
                    raise ValueError(f"source {source_index} has invalid risk set")
                if paired_rows != regret_paired_samples:
                    raise ValueError(f"source {source_index} has partial regret rows")
                if int(panel["probe_iterations"]) != risk_count * paired_rows:
                    raise ValueError(f"source {source_index} probe iterations differ")
                key = (source_index, target_nodes)
                if arms[key] != risk_count or samples[key] != risk_count * paired_rows:
                    raise ValueError(f"source {source_index} regret trace does not reconcile")
        elif root_panels:
            raise ValueError(f"source {source_index} has unexpected regret panels")
    return accepted


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
    parser.add_argument("--min-play-iterations", type=int, default=0)
    parser.add_argument("--regret-stop-target", type=float, default=0.0)
    parser.add_argument("--regret-stop-use-joint", action="store_true")
    parser.add_argument("--regret-correlation", type=float, default=0.48)
    parser.add_argument("--regret-calibration", type=float, default=1.0)
    parser.add_argument("--regret-check-interval", type=int, default=256)
    parser.add_argument("--regret-min-samples-per-arm", type=int, default=32)
    parser.add_argument("--regret-trace", action="store_true")
    parser.add_argument("--regret-risk-plays", type=int, default=8)
    parser.add_argument("--regret-paired-samples", type=int, default=512)
    parser.add_argument("--fixed-control", action="store_true")
    parser.add_argument("--matched-control", action="store_true")
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
        or args.min_play_iterations < 0
        or not math.isfinite(args.regret_stop_target)
        or args.regret_stop_target < 0.0
        or not math.isfinite(args.regret_correlation)
        or not 0.0 <= args.regret_correlation < 1.0
        or not math.isfinite(args.regret_calibration)
        or args.regret_calibration <= 0.0
        or args.regret_check_interval <= 0
        or args.regret_min_samples_per_arm <= 0
        or not 2 <= args.regret_risk_plays <= args.num_plays
        or args.regret_paired_samples <= 0
        or not math.isfinite(args.wall_seconds)
        or args.wall_seconds < 0.0
    ):
        parser.error("invalid curve protocol")
    if (args.fixed_control or args.matched_control) and args.regret_stop_target <= 0.0:
        parser.error("controls require a positive regret stop target")
    if (args.fixed_control or args.matched_control) and len(targets) != 1:
        parser.error("optional-stopping controls require exactly one target")
    for plies in plies_values:
        if args.min_play_iterations * args.num_plays > targets[0] // (plies + 1):
            parser.error("minimum play iterations do not fit the smallest target")

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
        "min_play_iterations": args.min_play_iterations,
        "regret_stop_target": args.regret_stop_target,
        "regret_stop_use_joint": args.regret_stop_use_joint,
        "regret_correlation": args.regret_correlation,
        "regret_calibration": args.regret_calibration,
        "regret_check_interval": args.regret_check_interval,
        "regret_min_samples_per_arm": args.regret_min_samples_per_arm,
        "regret_trace": args.regret_trace,
        "regret_risk_plays": args.regret_risk_plays,
        "regret_paired_samples": args.regret_paired_samples,
        "fixed_control": args.fixed_control,
        "matched_control": args.matched_control,
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
                    "THINKING_CURVE_MIN_PLAY_ITERATIONS": str(
                        args.min_play_iterations
                    ),
                    "THINKING_CURVE_JUDGE_PLIES": str(args.judge_plies),
                    "THINKING_CURVE_JUDGE_SAMPLES": str(args.judge_samples),
                    "THINKING_CURVE_JUDGE_RISK_PLAYS": str(
                        args.judge_risk_plays
                    ),
                    "THINKING_CURVE_REGRET_TRACE": str(int(args.regret_trace)),
                    "THINKING_CURVE_REGRET_RISK_PLAYS": str(
                        args.regret_risk_plays
                    ),
                    "THINKING_CURVE_REGRET_PAIRED_SAMPLES": str(
                        args.regret_paired_samples
                    ),
                    "THINKING_CURVE_REGRET_STOP_TARGET": str(
                        args.regret_stop_target
                    ),
                    "THINKING_CURVE_REGRET_STOP_USE_JOINT": str(
                        int(args.regret_stop_use_joint)
                    ),
                    "THINKING_CURVE_REGRET_CORRELATION": str(
                        args.regret_correlation
                    ),
                    "THINKING_CURVE_REGRET_CALIBRATION": str(
                        args.regret_calibration
                    ),
                    "THINKING_CURVE_REGRET_CHECK_INTERVAL": str(
                        args.regret_check_interval
                    ),
                    "THINKING_CURVE_REGRET_MIN_SAMPLES_PER_ARM": str(
                        args.regret_min_samples_per_arm
                    ),
                    "THINKING_CURVE_FIXED_CONTROL": str(int(args.fixed_control)),
                    "THINKING_CURVE_MATCHED_CONTROL": str(
                        int(args.matched_control)
                    ),
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

            accepted = validate_chunk_accounting(
                partial_log,
                plies=plies,
                targets=targets,
                max_nodes=args.max_nodes,
                num_plays=args.num_plays,
                judge_samples=args.judge_samples,
                regret_stop_target=args.regret_stop_target,
                regret_stop_use_joint=args.regret_stop_use_joint,
                regret_trace=args.regret_trace,
                regret_risk_plays=args.regret_risk_plays,
                regret_paired_samples=args.regret_paired_samples,
                fixed_control=args.fixed_control,
                matched_control=args.matched_control,
            )
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
