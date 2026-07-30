#!/usr/bin/env python3
"""Freeze balanced PEG panels from prospectively generated autoplay GCGs.

Input is the PEG_PANEL stream emitted by `magpie_test pegextractpanel`.
Selection uses only source identity, event order, and bag size. It never reads
PEG moves, values, timing, disagreements, or oracle output.
"""

from __future__ import annotations

import argparse
from collections import deque
import hashlib
import json
import os
import pathlib
import subprocess
from typing import Any


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def sha256_file(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def parse_record(line: str) -> dict[str, Any] | None:
    if not line.startswith("PEG_PANEL\t"):
        return None
    fields: dict[str, str] = {}
    for field in line.rstrip("\n").split("\t")[1:]:
        key, value = field.split("=", 1)
        fields[key] = value
    return {
        "source_index": int(fields["source_index"]),
        "source": fields["source"],
        "event": int(fields["event"]),
        "turn": int(fields["turn"]),
        "bag": int(fields["bag"]),
        "cgp": fields["cgp"],
    }


def choose_balanced(
    rows: list[dict[str, Any]], per_bag: int
) -> list[dict[str, Any]]:
    rows_by_source: dict[str, dict[int, dict[str, Any]]] = {}
    for row in rows:
        rows_by_source.setdefault(str(row["source"]), {})[
            int(row["bag"])
        ] = row
    sources = sorted(
        rows_by_source,
        key=lambda source: (
            int(
                min(
                    row["source_index"]
                    for row in rows_by_source[source].values()
                )
            ),
            source,
        ),
    )

    # Compute an exact source-to-bag b-matching. A greedy allocation can fail
    # even when a valid independent panel exists because low-bag positions
    # overlap heavily within the same source games.
    node_source = 0
    first_game = 1
    first_bag = first_game + len(sources)
    node_sink = first_bag + 4
    graph: list[list[list[int]]] = [[] for _ in range(node_sink + 1)]

    def add_edge(start: int, end: int, capacity: int) -> list[int]:
        forward = [end, len(graph[end]), capacity]
        reverse = [start, len(graph[start]), 0]
        graph[start].append(forward)
        graph[end].append(reverse)
        return forward

    game_bag_edges: dict[tuple[str, int], list[int]] = {}
    for source_index, source in enumerate(sources):
        game_node = first_game + source_index
        add_edge(node_source, game_node, 1)
        for bag in sorted(rows_by_source[source]):
            game_bag_edges[(source, bag)] = add_edge(
                game_node, first_bag + bag - 1, 1
            )
    for bag in range(1, 5):
        add_edge(first_bag + bag - 1, node_sink, per_bag)

    flow = 0
    while True:
        level = [-1] * len(graph)
        level[node_source] = 0
        queue = deque([node_source])
        while queue:
            node = queue.popleft()
            for end, _, capacity in graph[node]:
                if capacity > 0 and level[end] < 0:
                    level[end] = level[node] + 1
                    queue.append(end)
        if level[node_sink] < 0:
            break
        next_edge = [0] * len(graph)

        def augment(node: int, amount: int) -> int:
            if node == node_sink:
                return amount
            while next_edge[node] < len(graph[node]):
                edge = graph[node][next_edge[node]]
                end, reverse_index, capacity = edge
                if capacity > 0 and level[end] == level[node] + 1:
                    pushed = augment(end, min(amount, capacity))
                    if pushed:
                        edge[2] -= pushed
                        graph[end][reverse_index][2] += pushed
                        return pushed
                next_edge[node] += 1
            return 0

        while (pushed := augment(node_source, 1)) > 0:
            flow += pushed
    required = 4 * per_bag
    if flow != required:
        counts = {
            bag: sum(bag in rows_by_source[source] for source in sources)
            for bag in range(1, 5)
        }
        raise ValueError(
            f"only {flow}/{required} positions can be assigned to independent "
            f"sources; raw bag source counts={counts}"
        )

    selected_by_bag: dict[int, list[dict[str, Any]]] = {
        bag: [] for bag in range(1, 5)
    }
    for source in sources:
        for bag in sorted(rows_by_source[source]):
            # A saturated forward edge has assigned this source to this bag.
            if game_bag_edges[(source, bag)][2] == 0:
                selected_by_bag[bag].append(rows_by_source[source][bag])
                break
    for bag in range(1, 5):
        if len(selected_by_bag[bag]) != per_bag:
            raise AssertionError(
                f"matching assigned {len(selected_by_bag[bag])} rows to bag "
                f"{bag}, expected {per_bag}"
            )
    return [
        row
        for bag in range(1, 5)
        for row in selected_by_bag[bag]
    ]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--input",
        type=pathlib.Path,
        action="append",
        required=True,
        help="PEG_PANEL extraction log; repeat for replacement batches",
    )
    parser.add_argument("--panel", type=pathlib.Path, required=True)
    parser.add_argument("--manifest", type=pathlib.Path, required=True)
    parser.add_argument("--cohort", required=True)
    parser.add_argument("--per-bag", type=int, required=True)
    parser.add_argument("--seed-family", required=True)
    parser.add_argument(
        "--generator-commit",
        required=True,
        help="commit containing the extractor used to create the inputs",
    )
    parser.add_argument(
        "--autoplay-command",
        action="append",
        required=True,
        help="exact command for each input batch, in matching order",
    )
    args = parser.parse_args()
    if len(args.input) != len(args.autoplay_command):
        raise ValueError(
            "provide exactly one --autoplay-command for every --input"
        )

    rows = [
        row
        for input_path in args.input
        for line in input_path.read_text().splitlines()
        if (row := parse_record(line)) is not None
    ]
    if not rows:
        raise ValueError(f"{args.input} contain no PEG_PANEL rows")
    unique_pairs = {(str(row["source"]), int(row["bag"])) for row in rows}
    if len(unique_pairs) != len(rows):
        raise ValueError("input has duplicate source/bag rows")
    selected = choose_balanced(rows, args.per_bag)

    panel_lines = ["# position_id\tbag_size\tCGP"]
    manifest_rows = []
    bag_indices = {bag: 0 for bag in range(1, 5)}
    for row in selected:
        bag = int(row["bag"])
        bag_indices[bag] += 1
        position = f"{args.cohort}-b{bag}-p{bag_indices[bag]:03d}"
        panel_lines.append(f"{position}\t{bag}\t{row['cgp']}")
        source_path = pathlib.Path(str(row["source"]))
        manifest_rows.append(
            {
                "position": position,
                "bag": bag,
                "source_game": source_path.name,
                "source_path": str(source_path),
                "source_game_sha256": sha256_file(source_path),
                "source_index": int(row["source_index"]),
                "event": int(row["event"]),
                "turn": int(row["turn"]),
                "cgp_sha256": sha256_bytes(str(row["cgp"]).encode()),
            }
        )
    panel_text = "\n".join(panel_lines) + "\n"
    args.panel.parent.mkdir(parents=True, exist_ok=True)
    args.manifest.parent.mkdir(parents=True, exist_ok=True)
    args.panel.write_text(panel_text)
    raw_counts = {
        str(bag): sum(int(row["bag"]) == bag for row in rows)
        for bag in range(1, 5)
    }
    manifest = {
        "schema_version": 1,
        "cohort": args.cohort,
        "generator_commit": args.generator_commit,
        "freezer_commit_base": subprocess.check_output(
            ["git", "rev-parse", "HEAD"], text=True
        ).strip(),
        "freezer_sha256": sha256_file(pathlib.Path(__file__)),
        "environment": {
            "build": "no_pgo_release",
            "lexicon": "CSW24",
            "rit": True,
            "hardware_concurrency": os.cpu_count(),
            "autoplay_binary_sha256": sha256_file(
                pathlib.Path("bin/magpie")
            ),
            "extractor_binary_sha256": sha256_file(
                pathlib.Path("bin/magpie_test")
            ),
        },
        "selection_locked_before_peg": True,
        "selection_inputs": ["source_game", "event", "turn", "bag"],
        "selection_excludes": [
            "peg_moves",
            "peg_values",
            "disagreements",
            "oracle_outcomes",
            "elapsed_time",
        ],
        "source_policy": "at most one selected position per source game",
        "seed_family": args.seed_family,
        "autoplay_commands": args.autoplay_command,
        "extract_inputs": [
            {
                "path": str(input_path),
                "sha256": sha256_file(input_path),
            }
            for input_path in args.input
        ],
        "raw_candidate_counts": raw_counts,
        "raw_source_games": len({str(row["source"]) for row in rows}),
        "panel": {
            "path": str(args.panel),
            "sha256": sha256_bytes(panel_text.encode()),
            "positions": len(selected),
            "per_bag": args.per_bag,
            "independent_source_games": len(
                {str(row["source"]) for row in selected}
            ),
        },
        "positions": manifest_rows,
    }
    args.manifest.write_text(
        json.dumps(manifest, indent=2, sort_keys=True) + "\n"
    )
    print(
        f"froze {len(selected)} positions from "
        f"{manifest['panel']['independent_source_games']} independent games "
        f"at {args.panel}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
