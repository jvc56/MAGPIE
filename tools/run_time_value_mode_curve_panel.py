#!/usr/bin/env python3
"""Run resumable endgame and PEG curves over a mapped game panel.

Every position is run in its own subprocess and private log.  Output reaches
the canonical log only after the subprocess exits successfully and the mode's
terminal/accounting records validate.  This makes an interrupted long run
safe to resume without treating a partial position as a quality label.
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import time


FIELD_RE = re.compile(r"(?:^| )([A-Za-z_]+)=([^ ]+)")


def fields(line: str) -> dict[str, str]:
    return dict(FIELD_RE.findall(line.rstrip("\n")))


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def write_json(path: Path, document: dict[str, object]) -> None:
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(
        json.dumps(document, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    temporary.replace(path)


def load_mapping(path: Path) -> dict[tuple[str, int, int], dict[str, str]]:
    mapping: dict[tuple[str, int, int], dict[str, str]] = {}
    with path.open(newline="", encoding="utf-8") as stream:
        for row in csv.DictReader(stream):
            mode = row["mode"]
            bag = int(row["bag"])
            index = int(row["mode_index"])
            key = (mode, bag, index)
            if key in mapping:
                raise ValueError(f"duplicate position-map key {key}")
            mapping[key] = row
    return mapping


def accepted_positions(path: Path) -> dict[tuple[str, int], set[int]]:
    accepted: dict[tuple[str, int], set[int]] = {}
    if not path.exists():
        return accepted
    with path.open(encoding="utf-8") as stream:
        for line in stream:
            if not line.startswith("MODEPANEL_ACCEPT "):
                continue
            row = fields(line)
            key = (row["mode"], int(row["bag"]))
            accepted.setdefault(key, set()).add(int(row["index"]))
    return accepted


def require_contiguous(done: set[int], count: int, label: str) -> int:
    if any(index < 0 or index >= count for index in done):
        raise ValueError(f"{label} canonical log has an out-of-range index")
    first_missing = 0
    while first_missing in done:
        first_missing += 1
    if any(index > first_missing for index in done):
        raise ValueError(f"{label} canonical log is not a contiguous prefix")
    return first_missing


def validate_endgame(text: str, index: int) -> dict[str, object]:
    lines = text.splitlines()
    cfg = [line for line in lines if line.startswith("EGCURVECFG ")]
    terminals = [
        line
        for line in lines
        if line.startswith("EGCURVEPOS ")
        or line.startswith("EGCURVETRUNC ")
        or line.startswith("EGCURVEJUDGETRUNC ")
    ]
    errors = [line for line in lines if line.startswith("EGCURVEERR ")]
    if len(cfg) != 1 or len(terminals) != 1 or errors:
        raise ValueError(
            "endgame output needs one config, one terminal, and no errors"
        )
    terminal = fields(terminals[0])
    if int(terminal["pos"]) != index:
        raise ValueError("endgame terminal position does not match request")
    return {
        "terminal": terminals[0].split(" ", 1)[0],
        "censored": terminals[0].startswith(
            ("EGCURVETRUNC ", "EGCURVEJUDGETRUNC ")
        ),
    }


def validate_peg(text: str, bag: int, index: int) -> dict[str, object]:
    lines = text.splitlines()
    cfg = [line for line in lines if line.startswith("PEGSTRUCTCFG ")]
    terminals = [
        line
        for line in lines
        if line.startswith("PEGSTRUCT bag=")
        or line.startswith("PEGSTRUCTPARTIAL ")
    ]
    oracles = [line for line in lines if line.startswith("PEGSTRUCTORACLE ")]
    errors = [line for line in lines if line.startswith("PEGSTRUCTERR ")]
    if len(cfg) != 1 or len(terminals) != 1 or len(oracles) != 1 or errors:
        raise ValueError(
            "PEG output needs one config, oracle, terminal, and no errors"
        )
    for line in (terminals[0], oracles[0]):
        row = fields(line)
        if int(row["bag"]) != bag or int(row["pos"]) != index:
            raise ValueError("PEG terminal/oracle position does not match request")
    stage_rows: dict[tuple[int, str, int, int], int] = {}
    candidate_rows: dict[tuple[int, str, int, int], list[int]] = {}
    for line in lines:
        if line.startswith("PEGSTRUCTSTAGE "):
            row = fields(line)
            key = (
                int(row["requested_stage"]),
                row["order"],
                int(row["phase"]),
                int(row["depth"]),
            )
            if key in stage_rows:
                raise ValueError(f"duplicate PEG stage start {key}")
            stage_rows[key] = int(row["total"])
        elif line.startswith("PEGSTRUCTCAND "):
            row = fields(line)
            key = (
                int(row["requested_stage"]),
                row["order"],
                int(row["phase"]),
                int(row["depth"]),
            )
            candidate_rows.setdefault(key, []).append(int(row["completed"]))
    if not stage_rows or not candidate_rows:
        raise ValueError("PEG output is missing stage/candidate timestamps")
    for key, completions in candidate_rows.items():
        if key not in stage_rows:
            raise ValueError(f"PEG candidates lack a stage start {key}")
        if len(completions) != len(set(completions)):
            raise ValueError(f"duplicate PEG candidate completion {key}")
        if set(completions) != set(range(1, max(completions) + 1)):
            raise ValueError(f"non-contiguous PEG candidate completions {key}")
        if max(completions) > stage_rows[key]:
            raise ValueError(f"too many PEG candidates for stage {key}")
    oracle = fields(oracles[0])
    if int(oracle["target_depth"]) != 4 or int(oracle["direct_fidelity"]) != 4:
        raise ValueError("PEG oracle is not direct-4")
    terminal_name = terminals[0].split(" ", 1)[0]
    return {
        "terminal": terminal_name,
        "all_boundaries_complete": terminal_name == "PEGSTRUCT",
        "oracle_complete": int(oracle["complete"]) == 1,
        "timestamped_candidates": sum(len(rows) for rows in candidate_rows.values()),
        "timestamped_stages": len(stage_rows),
    }


def mapping_row(
    mapping: dict[tuple[str, int, int], dict[str, str]],
    mode: str,
    bag: int,
    index: int,
) -> dict[str, str]:
    key = (mode, bag, index)
    if key not in mapping:
        raise ValueError(f"position-map lacks {key}")
    return mapping[key]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", required=True, type=Path)
    parser.add_argument("--input-dir", required=True, type=Path)
    parser.add_argument("--mapping", required=True, type=Path)
    parser.add_argument("--run-dir", required=True, type=Path)
    parser.add_argument("--threads", type=int, default=os.cpu_count() or 1)
    parser.add_argument("--endgame-plies", type=int, default=4)
    parser.add_argument("--endgame-judge-plies", type=int, default=4)
    parser.add_argument("--endgame-max-seconds", type=float, default=30.0)
    parser.add_argument("--endgame-judge-max-seconds", type=float, default=30.0)
    parser.add_argument("--peg-kmax-1-3", type=int, default=2)
    parser.add_argument("--peg-kmax-4", type=int, default=1)
    parser.add_argument("--peg-arm-max-1-3", type=float, default=60.0)
    parser.add_argument("--peg-arm-max-4", type=float, default=30.0)
    parser.add_argument("--peg-oracle-seconds", type=float, default=30.0)
    parser.add_argument("--only", choices=("all", "endgame", "peg"), default="all")
    parser.add_argument("--max-endgame", type=int, default=0)
    parser.add_argument("--max-peg-per-bag", type=int, default=0)
    args = parser.parse_args()

    repo = Path(__file__).resolve().parent.parent
    os.chdir(repo)
    binary = args.binary.resolve()
    input_dir = args.input_dir.resolve()
    mapping_path = args.mapping.resolve()
    run_dir = args.run_dir.resolve()
    if args.threads <= 0 or any(
        value <= 0
        for value in (
            args.endgame_max_seconds,
            args.endgame_judge_max_seconds,
            args.peg_arm_max_1_3,
            args.peg_arm_max_4,
            args.peg_oracle_seconds,
        )
    ):
        parser.error("threads and all caps must be positive")
    if not binary.is_file() or not mapping_path.is_file():
        parser.error("binary and mapping must exist")
    inputs = {
        "endgame": input_dir / "endgame-cgps.txt",
        **{bag: input_dir / f"random_{bag}peg.txt" for bag in range(1, 5)},
    }
    if any(not path.is_file() for path in inputs.values()):
        parser.error("mode input files are incomplete")
    run_dir.mkdir(parents=True, exist_ok=True)
    if shutil.disk_usage(run_dir).free < 3 * 1024**3:
        raise RuntimeError("less than 3 GiB free")
    chunks = run_dir / "chunks"
    chunks.mkdir(exist_ok=True)
    canonical = run_dir / "mode-curves.log"
    canonical_err = run_dir / "mode-curves.err"
    state_path = run_dir / "state.json"
    counts = {name: sum(1 for line in path.open() if line.strip()) for name, path in inputs.items()}
    if args.max_endgame:
        counts["endgame"] = min(counts["endgame"], args.max_endgame)
    for bag in range(1, 5):
        if args.max_peg_per_bag:
            counts[bag] = min(counts[bag], args.max_peg_per_bag)
    protocol = {
        "binary": str(binary),
        "binary_sha256": sha256(binary),
        "mapping": str(mapping_path),
        "mapping_sha256": sha256(mapping_path),
        "inputs": {str(key): {"path": str(path), "sha256": sha256(path)} for key, path in inputs.items()},
        "counts": {str(key): value for key, value in counts.items()},
        "threads": args.threads,
        "endgame": {
            "plies": args.endgame_plies,
            "judge_plies": args.endgame_judge_plies,
            "max_seconds": args.endgame_max_seconds,
            "judge_max_seconds": args.endgame_judge_max_seconds,
            "judge_min_depth": 1,
        },
        "peg": {
            "kmax_bags_1_3": args.peg_kmax_1_3,
            "kmax_bag_4": args.peg_kmax_4,
            "arm_max_bags_1_3": args.peg_arm_max_1_3,
            "arm_max_bag_4": args.peg_arm_max_4,
            "oracle_seconds": args.peg_oracle_seconds,
            "oracle_plies": 4,
            "oracle_stride": 1,
        },
        "execution": "endgame then PEG; one subprocess/position; accepted-prefix resume",
    }
    if state_path.exists():
        state = json.loads(state_path.read_text(encoding="utf-8"))
        if state.get("protocol") != protocol:
            raise ValueError("resume protocol does not match existing state")
    else:
        state = {
            "artifact_kind": "time_value_mode_curve_panel",
            "protocol": protocol,
            "status": "running",
            "start_epoch": time.time(),
            "attempts": 0,
        }
    state["status"] = "running"
    write_json(state_path, state)
    mapping = load_mapping(mapping_path)

    phases: list[tuple[str, int, int]] = []
    if args.only in ("all", "endgame"):
        phases.append(("endgame", 0, counts["endgame"]))
    if args.only in ("all", "peg"):
        phases.extend(("peg", bag, counts[bag]) for bag in range(1, 5))

    with canonical.open("a", encoding="utf-8") as stream:
        stream.write(f"MODEPANEL_RESUME epoch={time.time():.6f} only={args.only}\n")
    for mode, bag, count in phases:
        accepted = accepted_positions(canonical).get((mode, bag), set())
        start = require_contiguous(accepted, count, f"{mode}{bag}")
        for index in range(start, count):
            state["attempts"] = int(state["attempts"]) + 1
            attempt = int(state["attempts"])
            state.update({"phase": mode, "bag": bag, "index": index})
            write_json(state_path, state)
            partial_log = chunks / f"attempt-{attempt:05d}-{mode}{bag}-p{index}.partial.log"
            partial_err = chunks / f"attempt-{attempt:05d}-{mode}{bag}-p{index}.partial.err"
            environment = os.environ.copy()
            if mode == "endgame":
                environment.update(
                    {
                        "MAGPIE_EG_CURVE_CGP": str(inputs["endgame"]),
                        "MAGPIE_EG_CURVE_START": str(index),
                        "MAGPIE_EG_CURVE_MAX": str(index + 1),
                        "MAGPIE_EG_CURVE_THREADS": str(args.threads),
                        "MAGPIE_EG_CURVE_PLIES": str(args.endgame_plies),
                        "MAGPIE_EG_CURVE_JUDGE_PLIES": str(args.endgame_judge_plies),
                        "MAGPIE_EG_CURVE_MAX_SECONDS": str(args.endgame_max_seconds),
                        "MAGPIE_EG_CURVE_JUDGE_MAX_SECONDS": str(args.endgame_judge_max_seconds),
                        "MAGPIE_EG_CURVE_JUDGE": "1",
                        "MAGPIE_EG_CURVE_JUDGE_MIN_DEPTH": "1",
                    }
                )
                command = [str(binary), "egcurve"]
            else:
                kmax = args.peg_kmax_4 if bag == 4 else args.peg_kmax_1_3
                arm_max = args.peg_arm_max_4 if bag == 4 else args.peg_arm_max_1_3
                environment.update(
                    {
                        "MAGPIE_PEG_STRUCT_DIR": str(input_dir),
                        "MAGPIE_PEG_STRUCT_START": str(index),
                        "MAGPIE_PEG_STRUCT_MAX": str(index + 1),
                        "MAGPIE_PEG_STRUCT_KMAX": str(kmax),
                        "MAGPIE_PEG_STRUCT_THREADS": str(args.threads),
                        "MAGPIE_PEG_STRUCT_ARM_MAX": str(arm_max),
                        "MAGPIE_PEG_STRUCT_ORACLE": str(args.peg_oracle_seconds),
                        "MAGPIE_PEG_STRUCT_MIN_BAG": str(bag),
                        "MAGPIE_PEG_STRUCT_MAX_BAG": str(bag),
                        "MAGPIE_PEG_STRUCT_ORACLE_PLIES": "4",
                        "MAGPIE_PEG_STRUCT_ORACLE_STRIDE": "1",
                    }
                )
                command = [str(binary), "pegstructcurve"]
            with partial_log.open("w", encoding="utf-8") as stdout, partial_err.open(
                "w", encoding="utf-8"
            ) as stderr:
                result = subprocess.run(
                    command, env=environment, stdout=stdout, stderr=stderr, check=False
                )
            if result.returncode != 0:
                state.update({"status": "failed", "returncode": result.returncode})
                write_json(state_path, state)
                raise RuntimeError(f"{mode}{bag} position {index} failed")
            text = partial_log.read_text(encoding="utf-8")
            try:
                validation = (
                    validate_endgame(text, index)
                    if mode == "endgame"
                    else validate_peg(text, bag, index)
                )
            except Exception as error:
                state.update({"status": "failed", "validation_error": str(error)})
                write_json(state_path, state)
                raise
            row = mapping_row(mapping, mode, bag, index)
            accepted_log = partial_log.with_name(partial_log.name.replace(".partial.", ".accepted."))
            accepted_err = partial_err.with_name(partial_err.name.replace(".partial.", ".accepted."))
            partial_log.replace(accepted_log)
            partial_err.replace(accepted_err)
            marker = (
                f"MODEPANEL_ACCEPT mode={mode} bag={bag} index={index} "
                f"source_position={row['source_position']} game={row['game']} "
                f"turn={row['turn']} player={row['player']} policy={row['trajectory_policy']} "
                f"terminal={validation['terminal']}\n"
            )
            with canonical.open("a", encoding="utf-8") as stream:
                stream.write(marker)
                stream.write(text)
            error_text = accepted_err.read_text(encoding="utf-8")
            if error_text:
                with canonical_err.open("a", encoding="utf-8") as stream:
                    stream.write(marker)
                    stream.write(error_text)
            state.pop("validation_error", None)
            state.pop("returncode", None)
            state["last_validation"] = validation
            state["completed"] = {
                f"{accepted_mode}{accepted_bag}": len(indices)
                for (accepted_mode, accepted_bag), indices in accepted_positions(canonical).items()
            }
            write_json(state_path, state)

    state.update({"status": "complete", "finish_epoch": time.time()})
    write_json(state_path, state)
    with canonical.open("a", encoding="utf-8") as stream:
        stream.write(f"MODE_CURVE_PANEL_DONE epoch={time.time():.6f}\n")
    print(f"MODE_CURVE_PANEL_DONE run_dir={run_dir}", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
