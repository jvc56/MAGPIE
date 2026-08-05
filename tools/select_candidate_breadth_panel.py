#!/usr/bin/env python3
"""Select the representative complement of a preregistered hard-root panel.

The source panel is already balanced by trajectory policy and bag band.  The
excluded roots were selected only from checkpoint-visible hardness fields, so
their complement is outcome-free as well.  Keeping both panels allows the
final analysis to reconstruct the complete balanced source population while
the new run remains fresh to the earlier candidate-set judge.
"""

from __future__ import annotations

import argparse
from collections import defaultdict
import hashlib
import json
from pathlib import Path
import re

try:
    from tools.extract_time_value_positions import fields
except ModuleNotFoundError:  # Direct execution from tools/.
    from extract_time_value_positions import fields  # type: ignore[no-redef]


def _stable_rank(seed: int, *parts: str) -> str:
    payload = "\0".join((str(seed), *parts)).encode("utf-8")
    return hashlib.sha256(payload).hexdigest()


def select_candidate_breadth_panel(
    source: Path,
    source_manifest: Path,
    exclude_manifest: Path,
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

    excluded_document = json.loads(exclude_manifest.read_text(encoding="utf-8"))
    excluded = {
        int(row["selection_source_index"])
        for row in excluded_document.get("mapping", [])
    }
    if not excluded or min(excluded) < 0 or max(excluded) >= len(source_rows):
        raise ValueError("exclude manifest does not map into the source corpus")
    if len(excluded) != len(excluded_document.get("mapping", [])):
        raise ValueError("exclude manifest repeats a source root")

    choices: dict[tuple[str, str], list[dict[str, object]]] = defaultdict(list)
    strata_excluded: dict[str, int] = defaultdict(int)
    for source_index, row in enumerate(source_rows):
        meta = metadata[source_index]
        policy = str(meta["trajectory_policy"])
        bag_band = str(meta["bag_band"])
        key = (policy, bag_band)
        if source_index in excluded:
            strata_excluded["|".join(key)] += 1
            continue
        choices[key].append(
            {
                "source_index": source_index,
                "line": source_lines[source_index],
                "row": row,
                "meta": meta,
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
        candidates.sort(key=lambda candidate: str(candidate["stable_rank"]))
        selected.extend(candidates[:per_stratum])

    selected.sort(
        key=lambda candidate: _stable_rank(
            seed,
            "panel",
            str(candidate["source_index"]),
            str(candidate["stable_rank"]),
        )
    )
    identities = {
        (str(candidate["row"]["source_seed"]), str(candidate["row"]["start"]))
        for candidate in selected
    }
    if len(identities) != len(selected):
        raise ValueError("selected panel contains repeated source games")

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
                f"{prefix} breadth_source_index={candidate['source_index']} cgp={cgp}"
            )
            row = candidate["row"]
            meta = candidate["meta"]
            assert isinstance(row, dict) and isinstance(meta, dict)
            mapping.append(
                {
                    "position": position,
                    "breadth_source_index": candidate["source_index"],
                    "panel_position": int(meta["panel_position"]),
                    "game": int(row["game"]),
                    "source_seed": row["source_seed"],
                    "start": row["start"],
                    "turn": int(row["turn"]),
                    "bag": int(row["bag"]),
                    "trajectory_policy": meta["trajectory_policy"],
                    "bag_band": meta["bag_band"],
                }
            )

    manifest: dict[str, object] = {
        "artifact_kind": "candidate_breadth_representative_complement",
        "source": str(source),
        "source_sha256": hashlib.sha256(source.read_bytes()).hexdigest(),
        "source_manifest": str(source_manifest),
        "source_manifest_sha256": hashlib.sha256(
            source_manifest.read_bytes()
        ).hexdigest(),
        "exclude_manifest": str(exclude_manifest),
        "exclude_manifest_sha256": hashlib.sha256(
            exclude_manifest.read_bytes()
        ).hexdigest(),
        "selection_rule": "outcome-free hard-panel complement then stable hash",
        "oracle_fields_used": False,
        "seed": seed,
        "per_stratum": per_stratum,
        "roots": len(selected),
        "source_games": len(identities),
        "excluded_roots": len(excluded),
        "strata_available": strata_available,
        "strata_excluded": dict(sorted(strata_excluded.items())),
        "strata_selected": {key: per_stratum for key in sorted(strata_available)},
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
    parser.add_argument("--source-manifest", required=True, type=Path)
    parser.add_argument("--exclude-manifest", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--manifest", required=True, type=Path)
    parser.add_argument("--per-stratum", type=int, default=12)
    parser.add_argument("--seed", type=int, default=82603)
    args = parser.parse_args()
    manifest = select_candidate_breadth_panel(
        args.source,
        args.source_manifest,
        args.exclude_manifest,
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
