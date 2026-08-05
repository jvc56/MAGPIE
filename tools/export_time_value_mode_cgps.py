#!/usr/bin/env python3
"""Export panel PEG/endgame CGPs with lossless source-position mappings."""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
from pathlib import Path

try:
    from tools.extract_time_value_positions import fields
except ModuleNotFoundError:
    from extract_time_value_positions import fields


MAPPING_FIELDS = (
    "mode",
    "mode_index",
    "source_position",
    "game",
    "source_seed",
    "start",
    "turn",
    "player",
    "bag",
    "own_rack",
    "opp_rack",
    "spread",
    "predicted_future_turns",
    "trajectory_policy",
)


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def export_modes(corpus: Path, output_dir: Path) -> dict[str, object]:
    output_dir.mkdir(parents=True, exist_ok=True)
    peg_rows: dict[int, list[tuple[str, dict[str, str]]]] = {
        bag: [] for bag in range(1, 5)
    }
    endgame_rows: list[tuple[str, dict[str, str]]] = []
    with corpus.open(encoding="utf-8") as stream:
        for line in stream:
            if not line.startswith("TIME_VALUE_POSITION ") or " cgp=" not in line:
                raise ValueError("malformed time-value position row")
            row = fields(line)
            cgp = line.split(" cgp=", 1)[1].rstrip("\r\n")
            bag = int(row["bag"])
            if bag == 0:
                endgame_rows.append((cgp, row))
            elif 1 <= bag <= 4:
                peg_rows[bag].append((cgp, row))

    mappings: list[dict[str, str | int]] = []
    outputs: list[dict[str, object]] = []
    endgame_path = output_dir / "endgame-cgps.txt"
    endgame_path.write_text(
        "".join(cgp + "\n" for cgp, _ in endgame_rows), encoding="utf-8"
    )
    outputs.append(
        {
            "mode": "endgame",
            "path": str(endgame_path),
            "positions": len(endgame_rows),
            "sha256": digest(endgame_path),
        }
    )
    for mode_index, (_, row) in enumerate(endgame_rows):
        mappings.append(
            {
                "mode": "endgame",
                "mode_index": mode_index,
                "source_position": row["position"],
                **{field: row.get(field, "") for field in MAPPING_FIELDS[3:]},
            }
        )

    for bag, rows in peg_rows.items():
        path = output_dir / f"random_{bag}peg.txt"
        path.write_text("".join(cgp + "\n" for cgp, _ in rows), encoding="utf-8")
        outputs.append(
            {
                "mode": f"peg{bag}",
                "path": str(path),
                "positions": len(rows),
                "sha256": digest(path),
            }
        )
        for mode_index, (_, row) in enumerate(rows):
            mappings.append(
                {
                    "mode": "peg",
                    "mode_index": mode_index,
                    "source_position": row["position"],
                    **{field: row.get(field, "") for field in MAPPING_FIELDS[3:]},
                }
            )

    mapping_path = output_dir / "position-map.csv"
    with mapping_path.open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=MAPPING_FIELDS)
        writer.writeheader()
        writer.writerows(mappings)
    manifest = {
        "artifact_kind": "time_value_mode_cgp_export",
        "source": str(corpus),
        "source_sha256": digest(corpus),
        "positions": len(mappings),
        "outputs": outputs,
        "mapping": str(mapping_path),
        "mapping_sha256": digest(mapping_path),
    }
    (output_dir / "manifest.json").write_text(
        json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    return manifest


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("corpus", type=Path)
    parser.add_argument("--output-dir", required=True, type=Path)
    args = parser.parse_args()
    manifest = export_modes(args.corpus, args.output_dir)
    print(
        f"positions={manifest['positions']} output_dir={args.output_dir}"
    )


if __name__ == "__main__":
    main()
