#!/usr/bin/env python3
"""Build Markdown tables summarising the opening-rack static eval sweep.

Reads /tmp/opening_racks.tsv (the TSV produced by
`./bin/magpie_test openracks`) and writes OPENING_RACKS_REPORT.md.

Equity is in millipoints (Equity units in MAGPIE). 42 points = 42000.
This script renders millipoints as points (millipoints / 1000).
"""

from __future__ import annotations

import argparse
import csv
import sys
from collections import defaultdict
from dataclasses import dataclass
from pathlib import Path
from statistics import mean
from typing import Iterator


@dataclass
class Row:
    rack: str
    top_move: str
    top_eq: int
    top_kind: str
    top_exch_tile: str
    top_leave: str
    next_eq: int
    margin: int
    j_exch1_keep_eq: int | None
    j_exch1_keep_tile: str
    j_exch1_keep_leave: str
    j_miss_margin: int | None


def mp_to_points(mp: int | None) -> str:
    if mp is None:
        return ""
    return f"{mp / 1000:.3f}"


def iter_rows(path: Path) -> Iterator[Row]:
    with path.open() as f:
        reader = csv.DictReader(f, delimiter="\t")
        for r in reader:
            j_eq = r["j_exch1_keep_eq"]
            j_miss = r["j_miss_margin"]
            yield Row(
                rack=r["rack"],
                top_move=r["top_move"],
                top_eq=int(r["top_eq"]),
                top_kind=r["top_kind"],
                top_exch_tile=r["top_exch_tile"],
                top_leave=r["top_leave"],
                next_eq=int(r["next_eq"]),
                margin=int(r["margin"]),
                j_exch1_keep_eq=int(j_eq) if j_eq else None,
                j_exch1_keep_tile=r["j_exch1_keep_tile"],
                j_exch1_keep_leave=r["j_exch1_keep_leave"],
                j_miss_margin=int(j_miss) if j_miss else None,
            )


def build_leave_ranking(rows: list[Row]) -> list[dict]:
    """For each 6-tile leave that is ever the optimal exchange-1 leave, compute
    aggregates over the racks that select it."""
    buckets: dict[str, dict] = defaultdict(
        lambda: {
            "racks": 0,
            "margins": [],
            "tile": "",
            "example_rack": "",
            "max_margin_rack": "",
            "max_margin": -1,
        }
    )
    for row in rows:
        if row.top_kind != "exch1":
            continue
        leave = row.top_leave
        b = buckets[leave]
        b["racks"] += 1
        b["margins"].append(row.margin)
        if not b["tile"]:
            b["tile"] = row.top_exch_tile
            b["example_rack"] = row.rack
        if row.margin > b["max_margin"]:
            b["max_margin"] = row.margin
            b["max_margin_rack"] = row.rack

    out = []
    for leave, b in buckets.items():
        ms = b["margins"]
        out.append(
            {
                "leave": leave,
                "tile": b["tile"],
                "racks": b["racks"],
                "min_margin_mp": min(ms),
                "max_margin_mp": max(ms),
                "mean_margin_mp": int(round(mean(ms))),
                "max_margin_rack": b["max_margin_rack"],
            }
        )
    return out


def kept_letters(leave: str) -> set[str]:
    """Machine letters appearing in a leave string (one char per ml)."""
    return set(leave)


def build_letter_coverage(rows: list[Row]) -> list[dict]:
    """For each letter A-Z + blank, count opening racks where the top move is
    an exchange-1 that keeps that letter in its 6-tile leave."""
    counts: dict[str, int] = defaultdict(int)
    examples: dict[str, tuple[int, str, str]] = {}
    for row in rows:
        if row.top_kind != "exch1":
            continue
        for ch in kept_letters(row.top_leave):
            counts[ch] += 1
            cur = examples.get(ch)
            if cur is None or row.margin > cur[0]:
                examples[ch] = (row.margin, row.rack, row.top_leave)

    alphabet = list("ABCDEFGHIJKLMNOPQRSTUVWXYZ") + ["?"]
    out = []
    for ch in alphabet:
        ex = examples.get(ch, (0, "", ""))
        out.append(
            {
                "letter": ch,
                "racks_where_kept_as_optimal_exch1_leave": counts.get(ch, 0),
                "best_margin_mp": ex[0] if ch in examples else None,
                "best_rack": ex[1],
                "best_leave": ex[2],
            }
        )
    return out


def build_j_nearest_miss(rows: list[Row], limit: int = 50) -> list[Row]:
    """Racks with a J, sorted by how close the best J-keeping exchange-1 comes
    to the top play (smaller miss = closer).

    Excludes racks where the top play itself already keeps J on an exchange-1
    (there the miss would be zero and the question doesn't apply)."""
    candidates = [
        r
        for r in rows
        if r.j_miss_margin is not None and r.j_miss_margin > 0
    ]
    candidates.sort(key=lambda r: r.j_miss_margin)
    return candidates[:limit]


def count_j_top_exch1_keeps(rows: list[Row]) -> tuple[int, int]:
    """Sanity check: how many racks with J have the top play being an
    exchange-1 that keeps J? Expected: 0 (the claim under investigation)."""
    j_racks = 0
    j_top_keeps = 0
    for row in rows:
        if row.j_exch1_keep_eq is None and "J" not in row.rack:
            continue
        if "J" in row.rack:
            j_racks += 1
            if row.top_kind == "exch1" and "J" in row.top_leave:
                j_top_keeps += 1
    return j_racks, j_top_keeps


def markdown_table(headers: list[str], rows: list[list[str]]) -> str:
    def cell(v):
        if v is None:
            return ""
        return str(v)

    out = [
        "| " + " | ".join(headers) + " |",
        "|" + "|".join("---" for _ in headers) + "|",
    ]
    for row in rows:
        out.append("| " + " | ".join(cell(v) for v in row) + " |")
    return "\n".join(out)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--tsv", type=Path, default=Path("/tmp/opening_racks.tsv")
    )
    parser.add_argument(
        "--out", type=Path, default=Path("OPENING_RACKS_REPORT.md")
    )
    parser.add_argument("--near-miss-limit", type=int, default=50)
    parser.add_argument("--leave-table-limit", type=int, default=50)
    args = parser.parse_args()

    if not args.tsv.exists():
        print(f"missing input: {args.tsv}", file=sys.stderr)
        return 2

    rows = list(iter_rows(args.tsv))
    total = len(rows)

    # Top-level counts.
    top_kind_counts: dict[str, int] = defaultdict(int)
    for r in rows:
        top_kind_counts[r.top_kind] += 1

    j_racks, j_top_keeps = count_j_top_exch1_keeps(rows)

    # Table: letter coverage.
    letter_rows = build_letter_coverage(rows)

    # Table: 6-tile leaves that are optimal exchange-1 leaves.
    leave_ranking = build_leave_ranking(rows)
    leave_ranking.sort(key=lambda d: d["racks"], reverse=True)

    # Nearest-miss J table.
    near_miss = build_j_nearest_miss(rows, limit=args.near_miss_limit)

    lines: list[str] = []
    lines.append("# Opening-rack static-evaluation sweep (CSW21, MAGPIE)")
    lines.append("")
    lines.append(
        "Every distinct 7-tile opening rack is enumerated; the top static-eval "
        "move is recorded (score + superleave value + MAGPIE's opening placement "
        "adjustment), plus the equity of the next-best move and the best "
        "exchange-1 play that keeps the J in the leave (for J-containing racks)."
    )
    lines.append("")
    lines.append(
        "Equity is expressed in millipoints in the TSV; this report renders it "
        "in points."
    )
    lines.append("")
    lines.append(f"- Total distinct 7-tile opening racks: **{total:,}**")
    lines.append("- Top-move breakdown:")
    for kind in ("tile", "exch1", "exch", "pass"):
        n = top_kind_counts.get(kind, 0)
        lines.append(f"  - `{kind}`: {n:,}")
    lines.append(
        f"- Racks containing a J: **{j_racks:,}**; of those, racks where the "
        f"top static play is an exchange-1 keeping the J: **{j_top_keeps:,}**."
    )
    lines.append("")

    # Section 1: letter coverage.
    lines.append("## Letter coverage: 6-tile leaves that contain each letter")
    lines.append("")
    lines.append(
        "For every letter, how many opening racks have a top static play that "
        "is an **exchange-1** whose resulting 6-tile leave contains that letter. "
        "The Discord claim under investigation is that the J is the only letter "
        "with zero such racks."
    )
    lines.append("")
    lines.append(
        markdown_table(
            [
                "letter",
                "racks where the letter is kept by the optimal exchange-1",
                "best margin (pts)",
                "best rack",
                "best leave",
            ],
            [
                [
                    r["letter"],
                    f"{r['racks_where_kept_as_optimal_exch1_leave']:,}",
                    mp_to_points(r["best_margin_mp"]),
                    r["best_rack"],
                    r["best_leave"],
                ]
                for r in letter_rows
            ],
        )
    )
    lines.append("")

    # Section 2: leave ranking.
    lines.append("## Top 6-tile leaves ranked by number of racks that pick them")
    lines.append("")
    lines.append(
        "Each row is a 6-tile leave that is the optimal exchange-1 leave on at "
        "least one opening rack. `racks` is how many opening racks choose it. "
        "`margin` columns are the gap above the next-best static move."
    )
    lines.append("")
    k = args.leave_table_limit
    top_k = leave_ranking[:k]
    lines.append(
        markdown_table(
            [
                "leave (kept)",
                "tile exchanged",
                "racks",
                "min margin (pts)",
                "mean margin (pts)",
                "max margin (pts)",
                "rack achieving max margin",
            ],
            [
                [
                    r["leave"],
                    r["tile"],
                    f"{r['racks']:,}",
                    mp_to_points(r["min_margin_mp"]),
                    mp_to_points(r["mean_margin_mp"]),
                    mp_to_points(r["max_margin_mp"]),
                    r["max_margin_rack"],
                ]
                for r in top_k
            ],
        )
    )
    lines.append("")
    if len(leave_ranking) > k:
        lines.append(
            f"_(showing top {k} of {len(leave_ranking):,} distinct leaves)_"
        )
        lines.append("")

    # Section 3: J nearest miss.
    lines.append("## J nearest miss")
    lines.append("")
    lines.append(
        "Racks containing a J, sorted by the smallest equity gap between the "
        "top static play and the best **exchange-1** play that trades a "
        "non-J tile (keeping J in the 6-tile leave). Lower `miss` = closer the "
        "J-keeping exchange-1 comes to being optimal."
    )
    lines.append("")
    lines.append(
        markdown_table(
            [
                "rack",
                "top move",
                "top eq (pts)",
                "best exch1 keeping J",
                "exch1 leave",
                "exch1 eq (pts)",
                "miss (pts)",
            ],
            [
                [
                    r.rack,
                    r.top_move,
                    mp_to_points(r.top_eq),
                    f"(exch {r.j_exch1_keep_tile})",
                    r.j_exch1_keep_leave,
                    mp_to_points(r.j_exch1_keep_eq),
                    mp_to_points(r.j_miss_margin),
                ]
                for r in near_miss
            ],
        )
    )
    lines.append("")

    args.out.write_text("\n".join(lines))
    print(f"wrote {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
