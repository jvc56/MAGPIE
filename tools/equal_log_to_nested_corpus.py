#!/usr/bin/env python3
"""Convert equal-sample oracle disagreements into a nested-oracle corpus.

The corpus disagreement key is position + 1.  That deliberately preserves the
online oracle's per-position seed, so a deeper corpus run extends rather than
replaces the original deterministic outer-sample stream.
"""

from __future__ import annotations

import argparse
from pathlib import Path
import re


def field(line: str, name: str) -> str:
    match = re.search(rf"(?:^| ){re.escape(name)}=([^ ]+)", line)
    if match is None:
        raise ValueError(f"missing {name} in: {line[:160]}")
    return match.group(1)


def convert(line: str) -> str:
    if " cgp=" not in line:
        raise ValueError("equal-sample position has no CGP")
    prefix, cgp = line.rstrip("\r\n").split(" cgp=", 1)
    position = int(field(prefix, "position"))
    klv2_raw = field(prefix, "klv2_move_raw")
    klv3_raw = field(prefix, "klv3_move_raw")
    klv2_iterations = field(prefix, "klv2_iterations")
    klv3_iterations = field(prefix, "klv3_iterations")
    klv2_nodes = field(prefix, "klv2_nodes")
    klv3_nodes = field(prefix, "klv3_nodes")
    klv2_seconds = field(prefix, "klv2_seconds")
    klv3_seconds = field(prefix, "klv3_seconds")
    fields = [
        "KLV3_THREEWAY_DISAGREEMENT",
        str(position + 1),
        str(position),
        field(prefix, "game"),
        field(prefix, "turn"),
        field(prefix, "bag"),
        "0",
        "0",
        "baseline",
        "baseline",
        "prior",
        klv2_raw,
        klv2_raw,
        klv3_raw,
        klv2_iterations,
        klv2_iterations,
        klv3_iterations,
        klv2_nodes,
        klv2_nodes,
        klv3_nodes,
        klv2_seconds,
        klv2_seconds,
        klv3_seconds,
        *("0" for _ in range(15)),
        cgp,
    ]
    if len(fields) != 39:
        raise AssertionError(f"internal corpus field count is {len(fields)}, not 39")
    return "\t".join(fields)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("log", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--overwrite", action="store_true")
    args = parser.parse_args()

    mode = "w" if args.overwrite else "x"
    converted = 0
    with args.log.open(encoding="utf-8", errors="strict") as source:
        try:
            output = args.output.open(mode, encoding="utf-8")
        except FileExistsError:
            parser.error(f"{args.output} already exists; use --overwrite intentionally")
        with output:
            for line in source:
                if line.startswith("KLV3_EQUAL_POSITION "):
                    output.write(convert(line) + "\n")
                    converted += 1
    if converted == 0:
        args.output.unlink(missing_ok=True)
        parser.error(f"{args.log} contains no equal-sample disagreements")
    print(f"converted {converted} disagreements to {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
