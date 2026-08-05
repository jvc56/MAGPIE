#!/usr/bin/env python3
"""Convert portable solver work curves into several runtime cost profiles.

Native curve rows retain SIM/endgame nodes and PEG's separate scenario,
candidate, nested-endgame-node, and fixed-cost coordinates. A JSON profile
grid supplies seconds per work unit. The output duplicates each boundary once
per profile and adds ``rate_profile`` plus a common ``cost`` in seconds, ready
for ``build_rest_game_value_labels.py``.

Using a profile grid rather than one benchmark machine lets the runtime model
condition on available processing power. Source-game splitting still happens
after expansion, so the same game can never leak between rate profiles and a
held-out split.
"""

from __future__ import annotations

import argparse
import csv
from dataclasses import dataclass
import json
import math
from pathlib import Path


@dataclass(frozen=True)
class RateProfile:
    identifier: int
    name: str
    sim_seconds_per_node: float
    endgame_seconds_per_node: float
    peg_fixed_seconds_per_chunk: float
    peg_seconds_per_scenario: float
    peg_seconds_per_endgame_node: float
    peg_seconds_per_candidate: float
    # Rollout nodes are portable counters, but their wall cost changes with
    # the requested horizon.  Optional per-ply rates keep a profile honest
    # when one turn curve contains several horizon choices.  Older profiles
    # continue to use the generic SIM rate.
    sim_seconds_per_node_p2: float | None = None
    sim_seconds_per_node_p4: float | None = None
    sim_seconds_per_node_p6: float | None = None
    peg_seconds_per_greedy_scenario: float | None = None
    peg_seconds_per_added_scenario: float | None = None


RATE_FIELDS = (
    "sim_seconds_per_node",
    "endgame_seconds_per_node",
    "peg_fixed_seconds_per_chunk",
    "peg_seconds_per_scenario",
    "peg_seconds_per_endgame_node",
    "peg_seconds_per_candidate",
)

OPTIONAL_SIM_RATE_FIELDS = (
    "sim_seconds_per_node_p2",
    "sim_seconds_per_node_p4",
    "sim_seconds_per_node_p6",
)

OPTIONAL_PEG_RATE_FIELDS = (
    "peg_seconds_per_greedy_scenario",
    "peg_seconds_per_added_scenario",
)


def nonnegative(value: object, field: str) -> float:
    parsed = float(value)
    if not math.isfinite(parsed) or parsed < 0.0:
        raise ValueError(f"{field} must be finite and nonnegative")
    return parsed


def read_profiles(path: Path) -> list[RateProfile]:
    document = json.loads(path.read_text(encoding="utf-8"))
    raw_profiles = document.get("profiles")
    if not isinstance(raw_profiles, list) or not raw_profiles:
        raise ValueError("rate profile JSON needs a nonempty profiles array")
    profiles: list[RateProfile] = []
    identifiers: set[int] = set()
    for raw in raw_profiles:
        identifier = int(raw["id"])
        if identifier < 0 or identifier in identifiers:
            raise ValueError("rate profile ids must be unique and nonnegative")
        identifiers.add(identifier)
        rates = {field: nonnegative(raw[field], field) for field in RATE_FIELDS}
        optional_rates = {
            field: (
                nonnegative(raw[field], field) if field in raw else None
            )
            for field in OPTIONAL_SIM_RATE_FIELDS
        }
        optional_peg_rates = {
            field: (
                nonnegative(raw[field], field) if field in raw else None
            )
            for field in OPTIONAL_PEG_RATE_FIELDS
        }
        profiles.append(
            RateProfile(
                identifier=identifier,
                name=str(raw.get("name", identifier)),
                **rates,
                **optional_rates,
                **optional_peg_rates,
            )
        )
    return sorted(profiles, key=lambda profile: profile.identifier)


def row_cost(row: dict[str, str], profile: RateProfile) -> float:
    mode = row["mode"].lower()
    nodes = nonnegative(row.get("nodes", 0), "nodes")
    scenarios = nonnegative(row.get("scenarios", 0), "scenarios")
    candidates = nonnegative(row.get("candidates", 0), "candidates")
    fixed = nonnegative(row.get("fixed_seconds", 0), "fixed_seconds")
    if mode == "sim":
        plies = int(row.get("plies", "0"))
        ply_rate = getattr(
            profile, f"sim_seconds_per_node_p{plies}", None
        )
        return fixed + nodes * (
            profile.sim_seconds_per_node if ply_rate is None else ply_rate
        )
    if mode == "endgame":
        return fixed + nodes * profile.endgame_seconds_per_node
    if mode == "peg":
        if "greedy_scenarios" in row:
            greedy_scenarios = nonnegative(
                row.get("greedy_scenarios", 0), "greedy_scenarios"
            )
            added_scenarios = nonnegative(
                row.get("added_scenarios", 0), "added_scenarios"
            )
            added_nodes = nonnegative(
                row.get("added_endgame_nodes", 0),
                "added_endgame_nodes",
            )
            added_candidates = nonnegative(
                row.get("added_candidates", 0), "added_candidates"
            )
            greedy_rate = (
                profile.peg_seconds_per_scenario
                if profile.peg_seconds_per_greedy_scenario is None
                else profile.peg_seconds_per_greedy_scenario
            )
            added_scenario_rate = (
                profile.peg_seconds_per_scenario
                if profile.peg_seconds_per_added_scenario is None
                else profile.peg_seconds_per_added_scenario
            )
            return (
                fixed
                + profile.peg_fixed_seconds_per_chunk
                + greedy_scenarios * greedy_rate
                + added_scenarios * added_scenario_rate
                + added_nodes * profile.peg_seconds_per_endgame_node
                + added_candidates * profile.peg_seconds_per_candidate
            )
        return (
            fixed
            + profile.peg_fixed_seconds_per_chunk
            + scenarios * profile.peg_seconds_per_scenario
            + nodes * profile.peg_seconds_per_endgame_node
            + candidates * profile.peg_seconds_per_candidate
        )
    if mode == "static":
        return fixed
    raise ValueError(f"unsupported analysis mode {mode!r}")


def convert_rows(
    rows: list[dict[str, str]], profiles: list[RateProfile]
) -> list[dict[str, str]]:
    converted: list[dict[str, str]] = []
    for row in rows:
        for profile in profiles:
            output = dict(row)
            output["rate_profile"] = str(profile.identifier)
            output["rate_profile_name"] = profile.name
            output["cost"] = f"{row_cost(row, profile):.12g}"
            converted.append(output)
    return converted


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("curves", nargs="+", type=Path)
    parser.add_argument("--profiles", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()

    profiles = read_profiles(args.profiles)
    rows: list[dict[str, str]] = []
    fieldnames: list[str] | None = None
    for path in args.curves:
        with path.open(newline="", encoding="utf-8") as stream:
            reader = csv.DictReader(stream)
            if reader.fieldnames is None:
                raise ValueError(f"curve CSV has no header: {path}")
            if fieldnames is None:
                fieldnames = list(reader.fieldnames)
            elif set(reader.fieldnames) != set(fieldnames):
                raise ValueError("all curve CSV inputs must use the same schema")
            rows.extend(dict(row) for row in reader)
    if not rows or fieldnames is None:
        raise ValueError("curve inputs contain no rows")
    required = {"game", "turn", "player", "mode", "regret"}
    missing = required - set(fieldnames)
    if missing:
        raise ValueError(f"curve CSV missing fields: {','.join(sorted(missing))}")

    converted = convert_rows(rows, profiles)
    output_fields = [
        field for field in fieldnames if field not in {"cost", "rate_profile"}
    ] + ["rate_profile", "rate_profile_name", "cost"]
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=output_fields)
        writer.writeheader()
        writer.writerows(converted)
    print(
        f"source_rows={len(rows)} profiles={len(profiles)} "
        f"output_rows={len(converted)} output={args.output}"
    )


if __name__ == "__main__":
    main()
