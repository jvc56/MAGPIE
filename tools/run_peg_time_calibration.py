#!/usr/bin/env python3
"""Run the PEG time-budget calibration strictly one process at a time.

The panel is TSV with: position_id, bag_size, CGP. The C harness is invoked
through `bin/magpie_test pegtimecal` and emits tab-delimited PEGCAL_* records.
This driver preserves every record as JSONL, keeps the raw stream under obj/,
alternates arm order, inserts identical sentinels, and resumes completed work.

Example:
  python3 tools/run_peg_time_calibration.py \
    --panel tools/peg_time_calibration/heldout_positions.tsv \
    --output-dir obj/peg_time_calibration/initial_20
"""

from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
import os
import pathlib
import platform
import shutil
import subprocess
import sys
from typing import Any


ARM_BUDGETS = {"greedy": 0.0, "30s": 30.0, "60s": 60.0, "120s": 120.0}
FORWARD_ORDER = ("greedy", "30s", "60s", "120s")
REVERSE_ORDER = tuple(reversed(FORWARD_ORDER))
RECORD_NAMES = {
    "PEGCAL_EVENT": "event",
    "PEGCAL_STAGE": "stage",
    "PEGCAL_ARM": "arm",
    "PEGCAL_VALUE": "value",
    "PEGCAL_JUDGE": "judge",
    "PEGCAL_WARNING": "warning",
}


def parse_scalar(value: str) -> Any:
    if value in {"0", "1"}:
        return int(value)
    try:
        return int(value)
    except ValueError:
        pass
    try:
        return float(value)
    except ValueError:
        return value


def parse_machine_record(line: str) -> dict[str, Any] | None:
    fields = line.rstrip("\n").split("\t")
    kind = RECORD_NAMES.get(fields[0])
    if kind is None:
        return None
    record: dict[str, Any] = {"kind": kind}
    for field in fields[1:]:
        key, separator, value = field.partition("=")
        if not separator:
            continue
        record[key] = parse_scalar(value)
    return record


def read_panel(path: pathlib.Path) -> list[dict[str, Any]]:
    positions: list[dict[str, Any]] = []
    for line_number, raw in enumerate(path.read_text().splitlines(), 1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        fields = raw.split("\t", 2)
        if len(fields) != 3:
            raise ValueError(f"{path}:{line_number}: expected three TSV fields")
        position, bag_text, cgp = fields
        bag = int(bag_text)
        if bag not in {1, 2, 3, 4}:
            raise ValueError(f"{path}:{line_number}: invalid bag size {bag}")
        positions.append({"position": position, "bag": bag, "cgp": cgp})
    if not positions:
        raise ValueError(f"{path}: panel is empty")
    bag_counts = {bag: 0 for bag in range(1, 5)}
    for position in positions:
        bag_counts[position["bag"]] += 1
    if len(set(bag_counts.values())) != 1:
        raise ValueError(f"panel is not balanced by bag size: {bag_counts}")
    return positions


def sha256(path: pathlib.Path) -> str | None:
    if not path.exists():
        return None
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        while block := stream.read(8 * 1024 * 1024):
            digest.update(block)
    return digest.hexdigest()


def append_jsonl(path: pathlib.Path, record: dict[str, Any]) -> None:
    with path.open("a", encoding="utf-8") as stream:
        stream.write(json.dumps(record, sort_keys=True) + "\n")
        stream.flush()


def load_records(path: pathlib.Path) -> list[dict[str, Any]]:
    if not path.exists():
        return []
    records = []
    for line in path.read_text().splitlines():
        if line.strip():
            records.append(json.loads(line))
    return records


def latest_arm(
    records: list[dict[str, Any]], position: str, label: str
) -> dict[str, Any] | None:
    matches = [
        record
        for record in records
        if record.get("kind") == "arm"
        and record.get("mode") == "arm"
        and record.get("position") == position
        and record.get("label") == label
    ]
    return matches[-1] if matches else None


def has_summary(
    records: list[dict[str, Any]], kind: str, position: str, label: str
) -> bool:
    return any(
        record.get("kind") == kind
        and record.get("position") == position
        and record.get("label") == label
        for record in records
    )


class SequentialRunner:
    def __init__(
        self,
        binary: pathlib.Path,
        output_dir: pathlib.Path,
        records_path: pathlib.Path,
        raw_path: pathlib.Path,
        records: list[dict[str, Any]],
    ) -> None:
        self.binary = binary
        self.output_dir = output_dir
        self.records_path = records_path
        self.raw_path = raw_path
        self.records = records
        self.sequence = max(
            (int(record.get("sequence", -1)) for record in records), default=-1
        )

    def run(
        self,
        *,
        position: dict[str, Any],
        mode: str,
        label: str,
        budget: float,
        block: str,
        moves: list[str] | None = None,
        stride: int | None = None,
    ) -> list[dict[str, Any]]:
        self.sequence += 1
        environment = os.environ.copy()
        environment.update(
            {
                "PEG_CAL_MODE": mode,
                "PEG_CAL_POSITION": position["position"],
                "PEG_CAL_CGP": position["cgp"],
                "PEG_CAL_LABEL": label,
                "PEG_CAL_BUDGET": str(budget),
            }
        )
        if moves is not None:
            environment["PEG_CAL_MOVES"] = "|".join(moves)
        if stride is not None:
            environment["PEG_CAL_STRIDE"] = str(stride)

        started = dt.datetime.now(dt.timezone.utc).isoformat()
        header = {
            "kind": "invocation",
            "sequence": self.sequence,
            "position": position["position"],
            "bag": position["bag"],
            "mode": mode,
            "label": label,
            "block": block,
            "budget_seconds": budget,
            "stride": stride,
            "started_utc": started,
        }
        append_jsonl(self.records_path, header)
        self.records.append(header)

        command = [str(self.binary), "pegtimecal"]
        parsed: list[dict[str, Any]] = []
        with self.raw_path.open("a", encoding="utf-8") as raw:
            raw.write(
                f"\n# invocation sequence={self.sequence} position="
                f"{position['position']} mode={mode} label={label}\n"
            )
            raw.flush()
            process = subprocess.Popen(
                command,
                cwd=self.binary.parent.parent,
                env=environment,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
                bufsize=1,
            )
            assert process.stdout is not None
            for line in process.stdout:
                raw.write(line)
                raw.flush()
                record = parse_machine_record(line)
                if record is None:
                    continue
                record.update(
                    {
                        "sequence": self.sequence,
                        "bag": position["bag"],
                        "block": block,
                    }
                )
                append_jsonl(self.records_path, record)
                self.records.append(record)
                parsed.append(record)
            return_code = process.wait()

        result = {
            "kind": "process",
            "sequence": self.sequence,
            "position": position["position"],
            "bag": position["bag"],
            "mode": mode,
            "label": label,
            "block": block,
            "return_code": return_code,
            "finished_utc": dt.datetime.now(dt.timezone.utc).isoformat(),
        }
        append_jsonl(self.records_path, result)
        self.records.append(result)
        if return_code != 0:
            print(
                f"FAILED seq={self.sequence} {position['position']} "
                f"{mode}/{label} rc={return_code}",
                flush=True,
            )
        else:
            summary = next(
                (
                    record
                    for record in reversed(parsed)
                    if record["kind"] in {"arm", "judge"}
                ),
                None,
            )
            detail = ""
            if summary and summary["kind"] == "arm":
                detail = (
                    f" move={summary.get('move')} status={summary.get('status')}"
                    f" cands={summary.get('refine_completed_candidates')}/"
                    f"{summary.get('refine_candidate_total')}"
                )
            elif summary:
                detail = f" accepted={summary.get('accepted')}"
            print(
                f"done seq={self.sequence} {position['position']} "
                f"{mode}/{label}{detail}",
                flush=True,
            )
        return parsed


def create_manifest(
    repo: pathlib.Path,
    panel: pathlib.Path,
    positions: list[dict[str, Any]],
    output_dir: pathlib.Path,
    arguments: argparse.Namespace,
) -> None:
    assets = {}
    for relative in (
        "data/lexica/CSW24.kwg",
        "data/lexica/CSW24.klv2",
        "data/lexica/CSW24.wmp",
        "data/lexica/CSW24.rit",
    ):
        assets[relative] = sha256(repo / relative)
    git_head = subprocess.run(
        ["git", "rev-parse", "HEAD"],
        cwd=repo,
        check=True,
        capture_output=True,
        text=True,
    ).stdout.strip()
    manifest = {
        "created_utc": dt.datetime.now(dt.timezone.utc).isoformat(),
        "git_head": git_head,
        "platform": platform.platform(),
        "python": sys.version,
        "panel": str(panel),
        "panel_sha256": sha256(panel),
        "positions": [
            {"position": position["position"], "bag": position["bag"]}
            for position in positions
        ],
        "arm_budgets_seconds": ARM_BUDGETS,
        "threads": 10,
        "build": "no_pgo_release",
        "rit": True,
        "oracle": {
            "fidelity": "direct 3-ply",
            "stride": 4,
            "budget_seconds": arguments.oracle_budget,
            "common_sample": "deterministic weight-strata boundaries",
            "only_distinct_nominees": True,
        },
        "sensitivity": {
            "stride": 2,
            "positions_per_bag": arguments.sensitivity_per_bag,
        },
        "asset_sha256": assets,
    }
    (output_dir / "manifest.json").write_text(
        json.dumps(manifest, indent=2, sort_keys=True) + "\n"
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--panel",
        type=pathlib.Path,
        default=pathlib.Path("tools/peg_time_calibration/heldout_positions.tsv"),
    )
    parser.add_argument(
        "--binary", type=pathlib.Path, default=pathlib.Path("bin/magpie_test")
    )
    parser.add_argument("--output-dir", type=pathlib.Path, required=True)
    parser.add_argument("--oracle-budget", type=float, default=600.0)
    parser.add_argument("--sensitivity-per-bag", type=int, default=2)
    args = parser.parse_args()

    repo = pathlib.Path(__file__).resolve().parent.parent
    panel = (repo / args.panel).resolve() if not args.panel.is_absolute() else args.panel
    binary = (
        (repo / args.binary).resolve()
        if not args.binary.is_absolute()
        else args.binary.resolve()
    )
    output_dir = (
        (repo / args.output_dir).resolve()
        if not args.output_dir.is_absolute()
        else args.output_dir.resolve()
    )
    if not binary.exists():
        raise FileNotFoundError(
            f"{binary} is missing; build with "
            "`make -j10 magpie_test BUILD=no_pgo_release`"
        )
    output_dir.mkdir(parents=True, exist_ok=True)
    free_bytes = shutil.disk_usage(output_dir).free
    if free_bytes < 5 * 1024**3:
        raise RuntimeError(f"only {free_bytes / 1024**3:.1f} GiB free")

    positions = read_panel(panel)
    records_path = output_dir / "records.jsonl"
    raw_path = output_dir / "raw.log"
    records = load_records(records_path)
    # Refresh on resume so a repaired panel or a committed harness update is
    # reflected in provenance. records.jsonl remains append-only.
    create_manifest(repo, panel, positions, output_dir, args)
    runner = SequentialRunner(binary, output_dir, records_path, raw_path, records)

    sentinel = next(
        (position for position in positions if position["bag"] == 2), positions[0]
    )
    sentinel_position = dict(sentinel)
    sentinel_position["position"] = "sentinel"

    def run_sentinel(label: str) -> None:
        if has_summary(runner.records, "arm", "sentinel", label):
            return
        runner.run(
            position=sentinel_position,
            mode="sentinel",
            label=label,
            budget=0.0,
            block=label,
        )

    run_sentinel("before")
    split = len(positions) // 2
    for position_index, position in enumerate(positions):
        block = "early" if position_index < split else "late"
        if position_index == split:
            run_sentinel("between")
        order = FORWARD_ORDER if position_index % 2 == 0 else REVERSE_ORDER
        for label in order:
            if latest_arm(runner.records, position["position"], label):
                continue
            runner.run(
                position=position,
                mode="arm",
                label=label,
                budget=ARM_BUDGETS[label],
                block=block,
            )

        arms = [
            latest_arm(runner.records, position["position"], label)
            for label in FORWARD_ORDER
        ]
        if any(arm is None for arm in arms):
            append_jsonl(
                records_path,
                {
                    "kind": "censored_position",
                    "position": position["position"],
                    "bag": position["bag"],
                    "reason": "missing_arm",
                    "block": block,
                },
            )
            continue
        moves = list(dict.fromkeys(str(arm["move"]) for arm in arms if arm))
        if len(moves) == 1:
            if not has_summary(
                runner.records, "agreement", position["position"], "exact"
            ):
                agreement = {
                    "kind": "agreement",
                    "position": position["position"],
                    "bag": position["bag"],
                    "label": "exact",
                    "move": moves[0],
                    "accepted": 1,
                    "pair_regret": 0.0,
                    "block": block,
                }
                append_jsonl(records_path, agreement)
                runner.records.append(agreement)
            continue
        if not has_summary(
            runner.records, "judge", position["position"], "stride4"
        ):
            runner.run(
                position=position,
                mode="judge",
                label="stride4",
                budget=args.oracle_budget,
                block=block,
                moves=moves,
                stride=4,
            )

        sensitivity_runs_for_bag = sum(
            1
            for record in runner.records
            if record.get("kind") == "judge"
            and record.get("label") == "stride2"
            and int(record.get("bag", -1)) == position["bag"]
        )
        if (
            sensitivity_runs_for_bag < args.sensitivity_per_bag
            and not has_summary(
                runner.records, "judge", position["position"], "stride2"
            )
        ):
            runner.run(
                position=position,
                mode="judge",
                label="stride2",
                budget=args.oracle_budget,
                block=block,
                moves=moves,
                stride=2,
            )

    run_sentinel("after")
    print(f"complete artifacts={output_dir}", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
