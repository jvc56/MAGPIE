#!/usr/bin/env python3
"""Rerun only censored endgame/PEG panel roots with larger frozen caps.

Selections come from the root-audit CSVs produced by
``extract_time_value_mode_curves.py``.  Single-hard-root games run first, so
the job restores the greatest number of complete games as early as possible.
Each root is atomically accepted only after the original mode-specific output
audit succeeds.  A still-censored result is retained as evidence and marked
unresolved rather than silently retried under a different protocol.
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import time

try:
    from tools.run_time_value_mode_curve_panel import (
        fields,
        load_mapping,
        mapping_row,
        sha256,
        validate_endgame,
        validate_peg,
        write_json,
    )
except ModuleNotFoundError:
    from run_time_value_mode_curve_panel import (
        fields,
        load_mapping,
        mapping_row,
        sha256,
        validate_endgame,
        validate_peg,
        write_json,
    )


def load_game_hard_counts(path: Path) -> dict[str, int]:
    with path.open(newline="", encoding="utf-8") as stream:
        return {
            row["game"]: int(row["hard_roots"])
            for row in csv.DictReader(stream)
        }


def load_selections(
    paths: list[Path], hard_counts: dict[str, int]
) -> list[dict[str, str]]:
    selected: dict[tuple[str, int, int], dict[str, str]] = {}
    for path in paths:
        with path.open(newline="", encoding="utf-8") as stream:
            for row in csv.DictReader(stream):
                if int(row.get("usable", "0")) != 0:
                    continue
                mode = row["mode"]
                bag = int(row["bag"])
                index = int(row["mode_index"])
                key = (mode, bag, index)
                if key in selected:
                    raise ValueError(f"duplicate hard-root selection {key}")
                if row["game"] not in hard_counts:
                    raise ValueError(f"selection game lacks eligibility row: {row['game']}")
                selected[key] = dict(row)
    rows = list(selected.values())
    rows.sort(
        key=lambda row: (
            hard_counts[row["game"]],
            row["game"],
            0 if row["mode"] == "endgame" else 1,
            int(row["bag"]),
            int(row["mode_index"]),
        )
    )
    if not rows:
        raise ValueError("hard-root selection is empty")
    return rows


def accepted_keys(path: Path) -> set[tuple[str, int, int]]:
    result: set[tuple[str, int, int]] = set()
    if not path.exists():
        return result
    with path.open(encoding="utf-8") as stream:
        for line in stream:
            if line.startswith("MODECONT_ACCEPT "):
                row = fields(line)
                result.add(
                    (row["mode"], int(row["bag"]), int(row["index"]))
                )
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", required=True, type=Path)
    parser.add_argument("--input-dir", required=True, type=Path)
    parser.add_argument("--mapping", required=True, type=Path)
    parser.add_argument("--eligibility", required=True, type=Path)
    parser.add_argument("--selection", action="append", required=True, type=Path)
    parser.add_argument("--run-dir", required=True, type=Path)
    parser.add_argument("--threads", type=int, default=os.cpu_count() or 1)
    parser.add_argument("--cap-seconds", type=float, default=180.0)
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args()

    repo = Path(__file__).resolve().parent.parent
    os.chdir(repo)
    binary = args.binary.resolve()
    input_dir = args.input_dir.resolve()
    mapping_path = args.mapping.resolve()
    eligibility_path = args.eligibility.resolve()
    selections_paths = [path.resolve() for path in args.selection]
    run_dir = args.run_dir.resolve()
    if args.threads <= 0 or args.cap_seconds <= 0.0:
        parser.error("threads and cap-seconds must be positive")
    if not binary.is_file() or not mapping_path.is_file():
        parser.error("binary and mapping must exist")
    inputs = {
        "endgame": input_dir / "endgame-cgps.txt",
        **{bag: input_dir / f"random_{bag}peg.txt" for bag in range(1, 5)},
    }
    if any(not path.is_file() for path in inputs.values()):
        parser.error("mode input files are incomplete")

    hard_counts = load_game_hard_counts(eligibility_path)
    selections = load_selections(selections_paths, hard_counts)
    if args.dry_run:
        print(
            f"hard_roots={len(selections)} games="
            f"{len({row['game'] for row in selections})} "
            f"single_root_first={sum(hard_counts[row['game']] == 1 for row in selections)}"
        )
        for ordinal, row in enumerate(selections[:10], 1):
            print(
                f"{ordinal}: mode={row['mode']} bag={row['bag']} "
                f"index={row['mode_index']} game={row['game']} "
                f"game_hard_roots={hard_counts[row['game']]}"
            )
        return 0

    run_dir.mkdir(parents=True, exist_ok=True)
    if shutil.disk_usage(run_dir).free < 3 * 1024**3:
        raise RuntimeError("less than 3 GiB free")
    chunks = run_dir / "chunks"
    chunks.mkdir(exist_ok=True)
    canonical = run_dir / "mode-curves.log"
    canonical_err = run_dir / "mode-curves.err"
    state_path = run_dir / "state.json"
    protocol = {
        "binary": str(binary),
        "binary_sha256": sha256(binary),
        "mapping": str(mapping_path),
        "mapping_sha256": sha256(mapping_path),
        "eligibility": str(eligibility_path),
        "eligibility_sha256": sha256(eligibility_path),
        "selections": [
            {"path": str(path), "sha256": sha256(path)}
            for path in selections_paths
        ],
        "selection_count": len(selections),
        "threads": args.threads,
        "cap_seconds": args.cap_seconds,
        "endgame": {"plies": 4, "judge_plies": 4},
        "peg": {
            "kmax_bags_1_3": 2,
            "kmax_bag_4": 1,
            "oracle_plies": 4,
            "oracle_stride": 1,
        },
        "ordering": "fewest hard roots per source game, then game and mode",
    }
    if state_path.exists():
        state = json.loads(state_path.read_text(encoding="utf-8"))
        if state.get("protocol") != protocol:
            raise ValueError("resume protocol does not match existing state")
    else:
        state = {
            "artifact_kind": "time_value_hard_root_continuation",
            "protocol": protocol,
            "status": "running",
            "start_epoch": time.time(),
            "attempts": 0,
        }
    state["status"] = "running"
    write_json(state_path, state)
    mapping = load_mapping(mapping_path)
    accepted = accepted_keys(canonical)
    selected_keys = {
        (row["mode"], int(row["bag"]), int(row["mode_index"]))
        for row in selections
    }
    if not accepted <= selected_keys:
        raise ValueError("canonical log contains an unselected root")
    with canonical.open("a", encoding="utf-8") as stream:
        stream.write(f"MODECONT_RESUME epoch={time.time():.6f}\n")

    for row in selections:
        mode = row["mode"]
        bag = int(row["bag"])
        index = int(row["mode_index"])
        key = (mode, bag, index)
        if key in accepted:
            continue
        state["attempts"] = int(state["attempts"]) + 1
        attempt = int(state["attempts"])
        state.update({"phase": mode, "bag": bag, "index": index})
        write_json(state_path, state)
        partial_log = chunks / (
            f"attempt-{attempt:05d}-{mode}{bag}-p{index}.partial.log"
        )
        partial_err = chunks / (
            f"attempt-{attempt:05d}-{mode}{bag}-p{index}.partial.err"
        )
        environment = os.environ.copy()
        if mode == "endgame":
            environment.update(
                {
                    "MAGPIE_EG_CURVE_CGP": str(inputs["endgame"]),
                    "MAGPIE_EG_CURVE_START": str(index),
                    "MAGPIE_EG_CURVE_MAX": str(index + 1),
                    "MAGPIE_EG_CURVE_THREADS": str(args.threads),
                    "MAGPIE_EG_CURVE_PLIES": "4",
                    "MAGPIE_EG_CURVE_JUDGE_PLIES": "4",
                    "MAGPIE_EG_CURVE_MAX_SECONDS": str(args.cap_seconds),
                    "MAGPIE_EG_CURVE_JUDGE_MAX_SECONDS": str(args.cap_seconds),
                    "MAGPIE_EG_CURVE_JUDGE": "1",
                    "MAGPIE_EG_CURVE_JUDGE_MIN_DEPTH": "1",
                }
            )
            command = [str(binary), "egcurve"]
        else:
            environment.update(
                {
                    "MAGPIE_PEG_STRUCT_DIR": str(input_dir),
                    "MAGPIE_PEG_STRUCT_START": str(index),
                    "MAGPIE_PEG_STRUCT_MAX": str(index + 1),
                    "MAGPIE_PEG_STRUCT_KMAX": "1" if bag == 4 else "2",
                    "MAGPIE_PEG_STRUCT_THREADS": str(args.threads),
                    "MAGPIE_PEG_STRUCT_ARM_MAX": str(args.cap_seconds),
                    "MAGPIE_PEG_STRUCT_ORACLE": str(args.cap_seconds),
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
                command,
                env=environment,
                stdout=stdout,
                stderr=stderr,
                check=False,
            )
        if result.returncode != 0:
            state.update({"status": "failed", "returncode": result.returncode})
            write_json(state_path, state)
            raise RuntimeError(f"{mode}{bag} position {index} failed")
        text = partial_log.read_text(encoding="utf-8")
        validation = (
            validate_endgame(text, index)
            if mode == "endgame"
            else validate_peg(text, bag, index)
        )
        resolved = (
            not bool(validation["censored"])
            if mode == "endgame"
            else bool(validation["oracle_complete"])
            and bool(validation["all_boundaries_complete"])
        )
        accepted_log = partial_log.with_name(
            partial_log.name.replace(".partial.", ".accepted.")
        )
        accepted_err = partial_err.with_name(
            partial_err.name.replace(".partial.", ".accepted.")
        )
        partial_log.replace(accepted_log)
        partial_err.replace(accepted_err)
        mapped = mapping_row(mapping, mode, bag, index)
        marker = (
            f"MODECONT_ACCEPT mode={mode} bag={bag} index={index} "
            f"source_position={mapped['source_position']} game={row['game']} "
            f"turn={mapped['turn']} player={mapped['player']} "
            f"policy={mapped['trajectory_policy']} resolved={int(resolved)} "
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
        accepted.add(key)
        state.pop("returncode", None)
        state["last_validation"] = validation
        state["accepted"] = len(accepted)
        state["resolved"] = sum(
            int(fields(line)["resolved"])
            for line in canonical.read_text(encoding="utf-8").splitlines()
            if line.startswith("MODECONT_ACCEPT ")
        )
        write_json(state_path, state)

    state.update({"status": "complete", "finish_epoch": time.time()})
    write_json(state_path, state)
    with canonical.open("a", encoding="utf-8") as stream:
        stream.write(f"MODE_HARD_ROOT_CONTINUATION_DONE epoch={time.time():.6f}\n")
    print(f"MODE_HARD_ROOT_CONTINUATION_DONE run_dir={run_dir}", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
