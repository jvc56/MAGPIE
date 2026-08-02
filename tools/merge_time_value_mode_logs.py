#!/usr/bin/env python3
"""Overlay resolved hard-root continuations onto a complete mode-curve log.

The original mode panel contains every endgame and PEG root, including capped
attempts.  A continuation log contains only selected hard roots and may itself
contain unresolved attempts.  This tool replaces all root-keyed diagnostic
rows for each *resolved* continuation root while retaining the original rows
for unresolved roots.  Configuration and runner bookkeeping rows are kept for
provenance but are ignored by the downstream curve extractor.
"""

from __future__ import annotations

import argparse
import re
from pathlib import Path


RootKey = tuple[str, int, int]

POS_RE = re.compile(r"(?:^| )pos=(\d+)(?: |$)")
BAG_RE = re.compile(r"(?:^| )bag=(\d+)(?: |$)")


def line_key(line: str) -> RootKey | None:
    """Return the solver root identified by a diagnostic row, if any."""

    pos_match = POS_RE.search(line)
    if pos_match is None:
        return None
    position = int(pos_match.group(1))
    if line.startswith("EGCURVE"):
        return ("endgame", 0, position)
    if line.startswith("PEGSTRUCT"):
        bag_match = BAG_RE.search(line)
        if bag_match is None:
            raise ValueError(f"PEG diagnostic has pos but no bag: {line.rstrip()}")
        return ("peg", int(bag_match.group(1)), position)
    return None


def marker_fields(line: str) -> dict[str, str]:
    result: dict[str, str] = {}
    for token in line.split()[1:]:
        if "=" in token:
            key, value = token.split("=", 1)
            result[key] = value
    return result


def resolved_keys(lines: list[str]) -> set[RootKey]:
    keys: set[RootKey] = set()
    accepted: set[RootKey] = set()
    for line in lines:
        if not line.startswith("MODECONT_ACCEPT "):
            continue
        row = marker_fields(line)
        key = (row["mode"], int(row["bag"]), int(row["index"]))
        if key in accepted:
            raise ValueError(f"duplicate continuation marker for {key}")
        accepted.add(key)
        if int(row["resolved"]):
            keys.add(key)
    if not accepted:
        raise ValueError("continuation log contains no accepted roots")
    return keys


def validate_overlay(lines: list[str], keys: set[RootKey]) -> None:
    observed = {key for line in lines if (key := line_key(line)) in keys}
    missing = keys - observed
    if missing:
        raise ValueError(f"resolved roots have no diagnostic rows: {sorted(missing)}")
    terminals: set[RootKey] = set()
    for line in lines:
        key = line_key(line)
        if key not in keys:
            continue
        if line.startswith("EGCURVEPOS ") or line.startswith("PEGSTRUCT "):
            terminals.add(key)
    missing_terminals = keys - terminals
    if missing_terminals:
        raise ValueError(
            "resolved roots lack complete terminal rows: "
            f"{sorted(missing_terminals)}"
        )


def merged_lines(base_lines: list[str], overlay_lines: list[str]) -> list[str]:
    keys = resolved_keys(overlay_lines)
    validate_overlay(overlay_lines, keys)
    result = [line for line in base_lines if line_key(line) not in keys]
    result.append(
        "MODECURVE_OVERLAY "
        f"resolved_roots={len(keys)} source=hard_root_continuation\n"
    )
    result.extend(line for line in overlay_lines if line_key(line) in keys)
    return result


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("base", type=Path)
    parser.add_argument("continuation", type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()

    base_lines = args.base.read_text(encoding="utf-8").splitlines(keepends=True)
    overlay_lines = args.continuation.read_text(
        encoding="utf-8"
    ).splitlines(keepends=True)
    output = merged_lines(base_lines, overlay_lines)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    temporary = args.output.with_suffix(args.output.suffix + ".partial")
    temporary.write_text("".join(output), encoding="utf-8")
    temporary.replace(args.output)
    keys = resolved_keys(overlay_lines)
    print(f"resolved_roots={len(keys)} output={args.output}")


if __name__ == "__main__":
    main()
