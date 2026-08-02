#!/usr/bin/env python3
"""Select a deterministic, game-clustered SIM tail panel.

The long-budget continuation must use the same top-k nomination regime as the
broad panel, but it does not need every turn from every source game. This tool
selects at most one SIM root from each complete source game while balancing
trajectory policy and bag phase. It rewrites the dense position index required
by the resumable thinking-curve runner and retains the original panel position
in both the row and a manifest.
"""

from __future__ import annotations

import argparse
from collections import Counter, defaultdict
import hashlib
import json
from pathlib import Path
import re

try:
    from tools.extract_time_value_positions import fields
except ModuleNotFoundError:
    from extract_time_value_positions import fields


BAG_BANDS = (
    ("late_5_15", 5, 15),
    ("middle_late_16_35", 16, 35),
    ("middle_early_36_60", 36, 60),
    ("early_61_100", 61, 100),
)


def stable_rank(seed: int, *parts: str) -> str:
    payload = "\0".join((str(seed), *parts)).encode("utf-8")
    return hashlib.sha256(payload).hexdigest()


def bag_band(bag: int) -> str | None:
    for name, minimum, maximum in BAG_BANDS:
        if minimum <= bag <= maximum:
            return name
    return None


def parse_policy_targets(values: list[str]) -> dict[str, int]:
    targets: dict[str, int] = {}
    for value in values:
        if "=" not in value:
            raise ValueError("policy targets must use POLICY=ROOTS")
        policy, raw_count = value.rsplit("=", 1)
        count = int(raw_count)
        if not policy or count <= 0 or policy in targets:
            raise ValueError("policy targets must be unique and positive")
        targets[policy] = count
    if not targets:
        raise ValueError("at least one policy target is required")
    return targets


def _band_quotas(target: int) -> dict[str, int]:
    names = [name for name, _, _ in BAG_BANDS]
    return {
        name: target // len(names) + (index < target % len(names))
        for index, name in enumerate(names)
    }


def select_tail_panel(
    source: Path,
    output: Path,
    manifest_path: Path,
    policy_targets: dict[str, int],
    seed: int,
) -> dict[str, object]:
    rows_by_policy_band_game: dict[
        tuple[str, str, tuple[str, str]], list[tuple[str, dict[str, str]]]
    ] = defaultdict(list)
    source_lines = source.read_text(encoding="utf-8").splitlines(keepends=True)
    for expected_position, line in enumerate(source_lines):
        if not line.startswith("TIME_VALUE_POSITION "):
            raise ValueError("source corpus contains a non-position row")
        row = fields(line)
        if int(row["position"]) != expected_position:
            raise ValueError("source positions must be globally consecutive")
        band = bag_band(int(row["bag"]))
        if band is None:
            continue
        identity = (row["source_seed"], row["start"])
        rows_by_policy_band_game[
            (row["trajectory_policy"], band, identity)
        ].append((line, row))

    selected: list[tuple[str, dict[str, str], str]] = []
    used_games: set[tuple[str, str]] = set()
    available_counts: Counter[tuple[str, str]] = Counter()
    selected_counts: Counter[tuple[str, str]] = Counter()
    for policy, target in policy_targets.items():
        quotas = _band_quotas(target)
        for band, quota in quotas.items():
            choices: list[tuple[str, tuple[str, str], str, dict[str, str]]] = []
            identities = {
                identity
                for candidate_policy, candidate_band, identity in rows_by_policy_band_game
                if candidate_policy == policy and candidate_band == band
            }
            available_counts[(policy, band)] = len(identities)
            for identity in identities:
                candidates = rows_by_policy_band_game[(policy, band, identity)]
                line, row = min(
                    candidates,
                    key=lambda item: stable_rank(
                        seed, "root", policy, band, *identity, item[1]["position"]
                    ),
                )
                choices.append(
                    (
                        stable_rank(seed, "game", policy, band, *identity),
                        identity,
                        line,
                        row,
                    )
                )
            eligible = [choice for choice in sorted(choices) if choice[1] not in used_games]
            if len(eligible) < quota:
                raise ValueError(
                    f"stratum {policy}|{band} needs {quota} unique games but "
                    f"only {len(eligible)} remain"
                )
            for _, identity, line, row in eligible[:quota]:
                used_games.add(identity)
                selected.append((line, row, band))
                selected_counts[(policy, band)] += 1

    selected.sort(
        key=lambda item: stable_rank(
            seed,
            "panel",
            item[1]["trajectory_policy"],
            item[2],
            item[1]["source_seed"],
            item[1]["start"],
        )
    )
    output.parent.mkdir(parents=True, exist_ok=True)
    mapping: list[dict[str, object]] = []
    with output.open("w", encoding="utf-8") as stream:
        for position, (line, row, band) in enumerate(selected):
            panel_position = int(row["position"])
            rewritten = re.sub(
                r"\bposition=\d+", f"position={position}", line, count=1
            )
            if " cgp=" not in rewritten:
                raise ValueError("position row lacks cgp")
            prefix, cgp = rewritten.split(" cgp=", 1)
            stream.write(
                f"{prefix} panel_position={panel_position} "
                f"selection_bag_band={band} cgp={cgp}"
            )
            mapping.append(
                {
                    "position": position,
                    "panel_position": panel_position,
                    "game": int(row["game"]),
                    "source_seed": row["source_seed"],
                    "start": row["start"],
                    "turn": int(row["turn"]),
                    "bag": int(row["bag"]),
                    "trajectory_policy": row["trajectory_policy"],
                    "bag_band": band,
                }
            )

    manifest: dict[str, object] = {
        "artifact_kind": "matched_sim_tail_root_panel",
        "source": str(source),
        "source_sha256": hashlib.sha256(source.read_bytes()).hexdigest(),
        "seed": seed,
        "selection_unit": "one_root_per_complete_source_game",
        "policy_targets": policy_targets,
        "roots": len(selected),
        "source_games": len(used_games),
        "bag_bands": [
            {"name": name, "minimum": minimum, "maximum": maximum}
            for name, minimum, maximum in BAG_BANDS
        ],
        "strata_available": {
            "|".join(key): value for key, value in sorted(available_counts.items())
        },
        "strata_selected": {
            "|".join(key): value for key, value in sorted(selected_counts.items())
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
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--manifest", required=True, type=Path)
    parser.add_argument("--policy-target", action="append", default=[])
    parser.add_argument("--seed", type=int, default=82402)
    args = parser.parse_args()
    manifest = select_tail_panel(
        args.source,
        args.output,
        args.manifest,
        parse_policy_targets(args.policy_target),
        args.seed,
    )
    print(
        f"roots={manifest['roots']} source_games={manifest['source_games']} "
        f"output={args.output} manifest={args.manifest}"
    )


if __name__ == "__main__":
    main()
