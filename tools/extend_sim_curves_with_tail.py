#!/usr/bin/env python3
"""Append imputed long-budget SIM boundaries to native turn curves."""

from __future__ import annotations

import argparse
from collections import defaultdict
import csv
import json
import math
from pathlib import Path


def load_retentions(path: Path, scenario: str) -> tuple[int, dict[int, list[tuple[int, float]]]]:
    document = json.loads(path.read_text(encoding="utf-8"))
    if document.get("artifact_kind") != "sim_population_tail_prior":
        raise ValueError("input is not a SIM population tail prior")
    if scenario not in document.get("scenarios", {}):
        raise ValueError(f"unknown tail scenario {scenario!r}")
    field = f"{scenario}_retention"
    priors: dict[int, list[tuple[int, float]]] = {}
    for prior in document["priors"]:
        plies = int(prior["plies"])
        values: list[tuple[int, float]] = []
        previous = 1.0
        for row in prior["targets"]:
            retention = float(row[field])
            if (
                not math.isfinite(retention)
                or retention < 0.0
                or retention > previous + 1.0e-12
            ):
                raise ValueError("tail retention must be monotone in [0, 1]")
            previous = retention
            values.append((int(row["target_nodes"]), retention))
        priors[plies] = values
    return int(document["baseline_nodes"]), priors


def extend_rows(
    rows: list[dict[str, str]],
    baseline_nodes: int,
    priors: dict[int, list[tuple[int, float]]],
    scenario: str,
) -> tuple[list[dict[str, str]], int]:
    grouped: dict[tuple[str, str, str, int], list[dict[str, str]]] = defaultdict(list)
    for row in rows:
        if row["mode"] == "sim":
            grouped[
                (row["game"], row["turn"], row["player"], int(row["plies"]))
            ].append(row)
    additions: list[dict[str, str]] = []
    for identity, options in grouped.items():
        plies = identity[3]
        if plies not in priors:
            raise ValueError(f"tail prior lacks plies={plies}")
        baseline = [
            row for row in options if row.get("option") == str(baseline_nodes)
        ]
        if not baseline:
            if (
                len(options) == 1
                and options[0].get("option") == "forced"
                and float(options[0]["regret"]) == 0.0
            ):
                continue
            raise ValueError(f"SIM curve {identity} lacks baseline option")
        if len(baseline) != 1:
            raise ValueError(f"SIM curve {identity} duplicates baseline option")
        source = baseline[0]
        baseline_regret = float(source["regret"])
        for target, retention in priors[plies]:
            if any(row.get("option") == str(target) for row in options):
                raise ValueError(f"SIM curve {identity} already has target {target}")
            row = dict(source)
            row["cost"] = str(target)
            row["nodes"] = str(target)
            row["observed_seconds"] = "0"
            row["regret"] = f"{baseline_regret * retention:.12f}"
            row["option"] = str(target)
            row["tail_imputed"] = "1"
            row["tail_scenario"] = scenario
            additions.append(row)
    output = rows + additions
    output.sort(
        key=lambda row: (
            row["game"],
            int(row["turn"]),
            int(row["player"]),
            row["mode"],
            int(row.get("plies", "0")),
            float(row["cost"]),
        )
    )
    return output, len(additions)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("curves", type=Path)
    parser.add_argument("--prior", required=True, type=Path)
    parser.add_argument("--scenario", choices=("central", "lower95"), required=True)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    baseline, priors = load_retentions(args.prior, args.scenario)
    with args.curves.open(newline="", encoding="utf-8") as stream:
        reader = csv.DictReader(stream)
        if reader.fieldnames is None:
            raise ValueError("curve CSV has no header")
        fieldnames = list(reader.fieldnames)
        rows = [dict(row) for row in reader]
    for row in rows:
        row["tail_imputed"] = "0"
        row["tail_scenario"] = ""
    fieldnames.extend(
        field
        for field in ("tail_imputed", "tail_scenario")
        if field not in fieldnames
    )
    output, added = extend_rows(
        rows, baseline, priors, args.scenario
    )
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=fieldnames)
        writer.writeheader()
        writer.writerows(output)
    print(
        f"source_rows={len(rows)} added_tail_rows={added} "
        f"output_rows={len(output)} scenario={args.scenario} output={args.output}"
    )


if __name__ == "__main__":
    main()
