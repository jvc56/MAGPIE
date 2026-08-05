#!/usr/bin/env python3
"""Build a three-arm oracle corpus from fixed-node depth-sweep logs.

The existing nested-oracle test accepts three root moves per position.  This
tool maps the nominees from 2-, 4-, and 6-ply sweep logs into that format so
they can be judged by one deeper, shared-scenario simulation policy.  Positions
where all three depths nominate the same move are omitted because their regret
is exactly equal under any oracle.
"""

from __future__ import annotations

import argparse
import re
from dataclasses import dataclass
from pathlib import Path


SELECTION_RE = re.compile(
    r"^COMBINED_SWEEP_POSITION .*?\bposition=(\d+)\b"
    r".*?\bselected_source_rank=(\d+)\b"
)
CANDIDATE_RE = re.compile(
    r"^POSITIONAL_CANDIDATE "
    r".*?\bposition=(\d+)\b"
    r".*?\bgame=(\d+)\b"
    r".*?\bturn=(\d+)\b"
    r".*?\bbag=(\d+)\b"
    r".*?\brank=(\d+)\b"
    r".*?\bmove_raw=([^ ]+) cgp=(.*)$"
)


@dataclass(frozen=True)
class Candidate:
    game: int
    turn: int
    bag: int
    move_raw: str
    cgp: str


def read_selections(path: Path) -> dict[int, int]:
    selections: dict[int, int] = {}
    for line in path.read_text().splitlines():
        match = SELECTION_RE.match(line)
        if match:
            position, rank = map(int, match.groups())
            if position in selections:
                raise ValueError(f"duplicate selection for position {position} in {path}")
            selections[position] = rank
    if not selections:
        raise ValueError(f"no sweep selections found in {path}")
    return selections


def read_candidates(path: Path) -> dict[tuple[int, int], Candidate]:
    candidates: dict[tuple[int, int], Candidate] = {}
    for line in path.read_text().splitlines():
        match = CANDIDATE_RE.match(line)
        if not match:
            continue
        position, game, turn, bag, rank = map(int, match.groups()[:5])
        move_raw, cgp = match.groups()[5:]
        key = (position, rank)
        if key in candidates:
            raise ValueError(f"duplicate candidate {key} in {path}")
        candidates[key] = Candidate(game, turn, bag, move_raw, cgp)
    if not candidates:
        raise ValueError(f"no candidate rows found in {path}")
    return candidates


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--candidate-corpus", type=Path, required=True)
    parser.add_argument("--p2-log", type=Path, required=True)
    parser.add_argument("--p4-log", type=Path, required=True)
    parser.add_argument("--p6-log", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    candidates = read_candidates(args.candidate_corpus)
    selections = [
        read_selections(args.p2_log),
        read_selections(args.p4_log),
        read_selections(args.p6_log),
    ]
    common_positions = sorted(set.intersection(*(set(s) for s in selections)))

    rows: list[str] = []
    for position in common_positions:
        ranks = [selection[position] for selection in selections]
        if len(set(ranks)) == 1:
            continue
        selected = []
        for rank in ranks:
            key = (position, rank)
            if key not in candidates:
                raise ValueError(f"candidate corpus is missing position/rank {key}")
            selected.append(candidates[key])
        provenance = selected[0]
        if any(
            (candidate.game, candidate.turn, candidate.bag, candidate.cgp)
            != (provenance.game, provenance.turn, provenance.bag, provenance.cgp)
            for candidate in selected[1:]
        ):
            raise ValueError(f"inconsistent candidate provenance at position {position}")

        fields = [
            "KLV3_THREEWAY_DISAGREEMENT",
            str(len(rows) + 1),
            str(position),
            str(provenance.game),
            str(provenance.turn),
            str(provenance.bag),
            "0",
            "0",
            "p2",
            "p4",
            "p6",
            selected[0].move_raw,
            selected[1].move_raw,
            selected[2].move_raw,
            *(["0"] * 24),
            provenance.cgp,
        ]
        if len(fields) != 39:
            raise AssertionError(f"internal field-count error: {len(fields)}")
        rows.append("\t".join(fields))

    args.output.write_text("".join(f"{row}\n" for row in rows))
    print(
        f"wrote {len(rows)} disagreement positions from "
        f"{len(common_positions)} common positions to {args.output}"
    )


if __name__ == "__main__":
    main()
