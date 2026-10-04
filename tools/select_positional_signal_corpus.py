#!/usr/bin/env python3
"""Select corpus positions with the largest deployed positional signal."""

from __future__ import annotations

import argparse
from pathlib import Path


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--count", required=True, type=int)
    parser.add_argument(
        "--skip-before-position",
        default=0,
        type=int,
        help="Exclude positions below this corpus position id.",
    )
    return parser.parse_args()


def parse_fields(line: str) -> dict[str, str]:
    prefix = line.split(" move=", 1)[0]
    fields: dict[str, str] = {}
    for token in prefix.split()[1:]:
        key, value = token.split("=", 1)
        fields[key] = value
    return fields


def main() -> None:
    args = parse_args()
    lines_by_position: dict[int, list[str]] = {}
    values_by_position: dict[int, list[tuple[int, float, float]]] = {}
    with args.input.open(encoding="utf-8") as stream:
        for line in stream:
            if not line.startswith("POSITIONAL_CANDIDATE "):
                continue
            fields = parse_fields(line)
            position = int(fields["position"])
            if position < args.skip_before_position:
                continue
            lines_by_position.setdefault(position, []).append(line)
            values_by_position.setdefault(position, []).append(
                (
                    int(fields["rank"]),
                    float(fields["base_equity"]),
                    float(fields["compact_adjustment"]),
                )
            )

    signals: list[tuple[float, int]] = []
    for position, rows in values_by_position.items():
        rows.sort()
        leader_equity = rows[0][1]
        eligible = [
            adjustment
            for rank, equity, adjustment in rows[:3]
            if equity >= leader_equity - 3.0 - 1e-9
        ]
        mean_absolute_deployed_adjustment = (
            sum(abs(adjustment * 0.75) for adjustment in eligible)
            / len(eligible)
        )
        signals.append((mean_absolute_deployed_adjustment, position))

    selected = sorted(signals, reverse=True)[: args.count]
    selected_positions = {position for _, position in selected}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open("w", encoding="utf-8") as stream:
        for position in sorted(selected_positions):
            stream.writelines(lines_by_position[position])
    cutoff = selected[-1][0]
    print(
        f"selected={len(selected)} available={len(signals)} "
        f"minimum_mean_absolute_adjustment={cutoff:.6f} "
        f"maximum_mean_absolute_adjustment={selected[0][0]:.6f} "
        f"output={args.output}"
    )


if __name__ == "__main__":
    main()
