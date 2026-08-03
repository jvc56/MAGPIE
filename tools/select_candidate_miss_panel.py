#!/usr/bin/env python3
"""Select a balanced hard-root panel without using oracle outcomes.

Hardness is ranked only by checkpoint-observable fields from a prior stopped
SIM arm: cap status, near-tie count, estimated joint regret, and work used.
The common-judge regret label is deliberately not parsed, so selection cannot
prefer roots because a prior oracle happened to expose a candidate miss.
"""

from __future__ import annotations

import argparse
from collections import defaultdict
import hashlib
import json
import math
from pathlib import Path
import re

try:
    from tools.extract_time_value_positions import fields
except ModuleNotFoundError:  # Direct execution from tools/.
    from extract_time_value_positions import fields  # type: ignore[no-redef]


def _stable_rank(seed: int, *parts: str) -> str:
    payload = "\0".join((str(seed), *parts)).encode("utf-8")
    return hashlib.sha256(payload).hexdigest()


def _stop_rows(path: Path) -> dict[int, dict[str, str]]:
    rows: dict[int, dict[str, str]] = {}
    with path.open(encoding="utf-8") as stream:
        for line in stream:
            if not line.startswith("THINKING_CURVE_POINT "):
                continue
            row = fields(line)
            if row.get("final") != "0" or row.get("control_kind") != "stopped":
                continue
            source_index = int(row["source_index"])
            if source_index in rows:
                raise ValueError(f"multiple stopped checkpoints for source {source_index}")
            rows[source_index] = row
    if not rows:
        raise ValueError("calibration log has no stopped checkpoints")
    return rows


def _finite_float(row: dict[str, str], *keys: str) -> float:
    for key in keys:
        value = float(row.get(key, "nan"))
        if math.isfinite(value):
            return value
    return 0.0


def select_candidate_miss_panel(
    source: Path,
    calibration_log: Path,
    source_manifest: Path,
    output: Path,
    manifest_path: Path,
    per_stratum: int,
    seed: int,
) -> dict[str, object]:
    if per_stratum <= 0:
        raise ValueError("per-stratum count must be positive")
    source_lines = source.read_text(encoding="utf-8").splitlines(keepends=True)
    source_rows: list[dict[str, str]] = []
    for position, line in enumerate(source_lines):
        if not line.startswith("TIME_VALUE_POSITION "):
            raise ValueError("source corpus contains a non-position row")
        row = fields(line)
        if int(row["position"]) != position:
            raise ValueError("source positions must be globally consecutive")
        source_rows.append(row)

    source_document = json.loads(source_manifest.read_text(encoding="utf-8"))
    metadata = {
        int(row["position"]): row for row in source_document.get("mapping", [])
    }
    if set(metadata) != set(range(len(source_rows))):
        raise ValueError("source manifest does not exactly map the corpus")
    stop_rows = _stop_rows(calibration_log)
    if set(stop_rows) != set(range(len(source_rows))):
        raise ValueError("calibration log does not exactly cover the source corpus")

    choices: dict[tuple[str, str], list[dict[str, object]]] = defaultdict(list)
    for source_index, row in enumerate(source_rows):
        meta = metadata[source_index]
        stop = stop_rows[source_index]
        policy = str(meta["trajectory_policy"])
        bag_band = str(meta["bag_band"])
        near_ties = int(
            stop.get(
                "near_tie_challengers_at_stop",
                stop.get("near_tie_challengers", "-1"),
            )
        )
        estimated_regret = _finite_float(
            stop, "joint_regret_at_stop", "joint_estimated_regret"
        )
        capped = int(stop["bai_status"]) == 3
        choices[(policy, bag_band)].append(
            {
                "source_index": source_index,
                "line": source_lines[source_index],
                "row": row,
                "meta": meta,
                "capped": capped,
                "near_ties": near_ties,
                "estimated_regret": estimated_regret,
                "nodes": int(stop["nodes"]),
                "stable_rank": _stable_rank(
                    seed, policy, bag_band, row["source_seed"], row["start"]
                ),
            }
        )

    selected: list[dict[str, object]] = []
    strata_available: dict[str, int] = {}
    for stratum in sorted(choices):
        candidates = choices[stratum]
        key = "|".join(stratum)
        strata_available[key] = len(candidates)
        if len(candidates) < per_stratum:
            raise ValueError(
                f"stratum {key} needs {per_stratum} roots but has {len(candidates)}"
            )
        candidates.sort(
            key=lambda candidate: (
                -int(bool(candidate["capped"])),
                -int(candidate["near_ties"]),
                -float(candidate["estimated_regret"]),
                -int(candidate["nodes"]),
                str(candidate["stable_rank"]),
            )
        )
        selected.extend(candidates[:per_stratum])

    selected.sort(
        key=lambda candidate: _stable_rank(
            seed,
            "panel",
            str(candidate["source_index"]),
            str(candidate["stable_rank"]),
        )
    )
    output.parent.mkdir(parents=True, exist_ok=True)
    mapping: list[dict[str, object]] = []
    with output.open("w", encoding="utf-8") as stream:
        for position, candidate in enumerate(selected):
            line = str(candidate["line"])
            rewritten = re.sub(r"\bposition=\d+", f"position={position}", line, count=1)
            if " cgp=" not in rewritten:
                raise ValueError("position row lacks cgp")
            prefix, cgp = rewritten.split(" cgp=", 1)
            stream.write(
                f"{prefix} selection_source_index={candidate['source_index']} "
                f"candidate_hardness_near_ties={candidate['near_ties']} "
                f"candidate_hardness_capped={int(bool(candidate['capped']))} "
                f"cgp={cgp}"
            )
            row = candidate["row"]
            meta = candidate["meta"]
            assert isinstance(row, dict) and isinstance(meta, dict)
            mapping.append(
                {
                    "position": position,
                    "selection_source_index": candidate["source_index"],
                    "panel_position": int(meta["panel_position"]),
                    "game": int(row["game"]),
                    "source_seed": row["source_seed"],
                    "start": row["start"],
                    "turn": int(row["turn"]),
                    "bag": int(row["bag"]),
                    "trajectory_policy": meta["trajectory_policy"],
                    "bag_band": meta["bag_band"],
                    "hardness_capped": bool(candidate["capped"]),
                    "hardness_near_ties": candidate["near_ties"],
                    "hardness_estimated_regret": candidate["estimated_regret"],
                    "hardness_nodes": candidate["nodes"],
                }
            )

    manifest: dict[str, object] = {
        "artifact_kind": "candidate_miss_hard_root_panel",
        "source": str(source),
        "source_sha256": hashlib.sha256(source.read_bytes()).hexdigest(),
        "calibration_log": str(calibration_log),
        "calibration_log_sha256": hashlib.sha256(
            calibration_log.read_bytes()
        ).hexdigest(),
        "source_manifest": str(source_manifest),
        "source_manifest_sha256": hashlib.sha256(
            source_manifest.read_bytes()
        ).hexdigest(),
        "selection_rule": (
            "checkpoint_only: capped, near_ties, joint_estimated_regret, nodes"
        ),
        "oracle_fields_used": False,
        "seed": seed,
        "per_stratum": per_stratum,
        "roots": len(selected),
        "source_games": len(
            {(row["source_seed"], row["start"]) for row in mapping}
        ),
        "strata_available": strata_available,
        "strata_selected": {
            key: per_stratum for key in sorted(strata_available)
        },
        "mapping": mapping,
    }
    manifest_path.parent.mkdir(parents=True, exist_ok=True)
    manifest_path.write_text(
        json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    return manifest


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("--calibration-log", required=True, type=Path)
    parser.add_argument("--source-manifest", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--manifest", required=True, type=Path)
    parser.add_argument("--per-stratum", type=int, default=3)
    parser.add_argument("--seed", type=int, default=82403)
    args = parser.parse_args()
    manifest = select_candidate_miss_panel(
        args.source,
        args.calibration_log,
        args.source_manifest,
        args.output,
        args.manifest,
        args.per_stratum,
        args.seed,
    )
    print(
        f"roots={manifest['roots']} source_games={manifest['source_games']} "
        f"output={args.output} manifest={args.manifest}"
    )


if __name__ == "__main__":
    main()
