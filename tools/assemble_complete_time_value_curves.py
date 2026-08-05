#!/usr/bin/env python3
"""Join mode-native curves while retaining only fully observed source games."""

from __future__ import annotations

import argparse
import csv
from pathlib import Path

try:
    from tools.extract_time_value_positions import fields
except ModuleNotFoundError:
    from extract_time_value_positions import fields


def eligible_games(path: Path) -> set[str]:
    with path.open(newline="", encoding="utf-8") as stream:
        rows = list(csv.DictReader(stream))
    games = {row["game"] for row in rows if int(row["eligible"])}
    if not games:
        raise ValueError("eligibility file contains no eligible games")
    return games


def source_roots(path: Path, games: set[str]) -> set[tuple[str, int, int]]:
    roots: set[tuple[str, int, int]] = set()
    with path.open(encoding="utf-8") as stream:
        for line in stream:
            if not line.startswith("TIME_VALUE_POSITION "):
                raise ValueError("source corpus contains a non-position row")
            row = fields(line)
            game = f"{row['source_seed']}:{row['start']}"
            if game in games:
                roots.add((game, int(row["turn"]), int(row["player"])))
    return roots


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("curves", nargs="+", type=Path)
    parser.add_argument("--eligibility", required=True, type=Path)
    parser.add_argument("--source-corpus", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()

    games = eligible_games(args.eligibility)
    rows: list[dict[str, str]] = []
    fieldnames: list[str] | None = None
    identities: set[tuple[str, int, int, str, int, str]] = set()
    curve_roots: set[tuple[str, int, int]] = set()
    root_modes: dict[tuple[str, int, int], str] = {}
    for path in args.curves:
        with path.open(newline="", encoding="utf-8") as stream:
            reader = csv.DictReader(stream)
            if reader.fieldnames is None:
                raise ValueError(f"curve CSV has no header: {path}")
            if fieldnames is None:
                fieldnames = list(reader.fieldnames)
            elif reader.fieldnames != fieldnames:
                raise ValueError("curve CSV schemas or column orders differ")
            for row in reader:
                if row["game"] not in games:
                    continue
                root = (row["game"], int(row["turn"]), int(row["player"]))
                previous_mode = root_modes.setdefault(root, row["mode"])
                if previous_mode != row["mode"]:
                    raise ValueError(f"root {root} mixes solver modes")
                identity = (
                    *root,
                    row["mode"],
                    int(row.get("plies", "0")),
                    row.get("option", ""),
                )
                if identity in identities:
                    raise ValueError(f"duplicate curve option {identity}")
                identities.add(identity)
                curve_roots.add(root)
                rows.append(dict(row))

    if fieldnames is None or not rows:
        raise ValueError("curve inputs contain no eligible rows")
    expected_roots = source_roots(args.source_corpus, games)
    if curve_roots != expected_roots:
        missing = sorted(expected_roots - curve_roots)[:5]
        extra = sorted(curve_roots - expected_roots)[:5]
        raise ValueError(
            f"complete-game coverage mismatch missing={missing} extra={extra}"
        )
    rows.sort(
        key=lambda row: (
            row["game"],
            int(row["turn"]),
            int(row["player"]),
            row["mode"],
            int(row.get("plies", "0")),
            float(row["cost"]),
        )
    )
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=fieldnames)
        writer.writeheader()
        writer.writerows(rows)
    print(
        f"games={len(games)} roots={len(curve_roots)} options={len(rows)} "
        f"output={args.output}"
    )


if __name__ == "__main__":
    main()
