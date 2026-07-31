#!/usr/bin/env python3
"""Quantify how fixed-depth endgame work changes on successive turns."""

from __future__ import annotations

import argparse
import math
import statistics
from dataclasses import dataclass
from pathlib import Path

FUTURE_DEPTH5_RESERVE_ID = (
    "csw24-eg-future-depth5-observed-max-shadow-v0-20260729"
)
MAX_RACK_TILES = 14


@dataclass(frozen=True)
class Turn:
    source: int
    position: int
    ply: int
    player: int
    rack_tiles: int
    player_rack_tiles: int
    nodes: int
    depth: int
    seconds: float
    completed: bool


def parse_fields(line: str) -> dict[str, str]:
    fields: dict[str, str] = {}
    for token in line.split()[1:]:
        if "=" in token:
            key, value = token.split("=", 1)
            fields[key] = value
    return fields


def percentile(values: list[float], probability: float) -> float:
    ordered = sorted(values)
    if not ordered:
        return math.nan
    index = probability * (len(ordered) - 1)
    low = int(math.floor(index))
    high = int(math.ceil(index))
    if low == high:
        return ordered[low]
    weight = index - low
    return ordered[low] * (1.0 - weight) + ordered[high] * weight


def load(
    paths: list[Path],
) -> tuple[list[Turn], dict[tuple[int, int], bool]]:
    turns: dict[tuple[int, int, int], Turn] = {}
    ended: dict[tuple[int, int], bool] = {}
    for source, path in enumerate(paths):
        with path.open(encoding="utf-8") as stream:
            for line in stream:
                if line.startswith("POTURN "):
                    fields = parse_fields(line)
                    position = int(fields["pos"])
                    ply = int(fields["ply"])
                    turns[source, position, ply] = Turn(
                        source=source,
                        position=position,
                        ply=ply,
                        player=int(fields["player"]),
                        rack_tiles=int(fields["rack_tiles"]),
                        player_rack_tiles=int(fields["player_rack_tiles"]),
                        nodes=int(fields["nodes"]),
                        depth=int(fields["depth"]),
                        seconds=float(fields["time"]),
                        completed=bool(int(fields["completed"])),
                    )
                elif line.startswith("POROW "):
                    fields = parse_fields(line)
                    ended[source, int(line.split()[1])] = bool(
                        int(fields["ended"])
                    )
    return list(turns.values()), ended


@dataclass(frozen=True)
class Observation:
    turn: Turn
    turns_after: int
    next_nodes_ratio: float | None
    next_seconds_ratio: float | None
    future_nodes: int
    future_seconds: float

    @property
    def future_nodes_ratio(self) -> float:
        return self.future_nodes / self.turn.nodes

    @property
    def future_seconds_ratio(self) -> float:
        return self.future_seconds / self.turn.seconds


def observations(
    turns: list[Turn], ended: dict[tuple[int, int], bool]
) -> tuple[list[Observation], int]:
    by_player: dict[tuple[int, int, int], list[Turn]] = {}
    incomplete_sequences = 0
    for turn in turns:
        by_player.setdefault(
            (turn.source, turn.position, turn.player), []
        ).append(turn)

    output: list[Observation] = []
    for (source, position, _), sequence in by_player.items():
        sequence.sort(key=lambda turn: turn.ply)
        if not ended.get((source, position), False) or not all(
            turn.completed for turn in sequence
        ):
            incomplete_sequences += 1
            continue
        for index, turn in enumerate(sequence):
            future = sequence[index + 1 :]
            next_turn = future[0] if future else None
            output.append(
                Observation(
                    turn=turn,
                    turns_after=len(future),
                    next_nodes_ratio=(
                        next_turn.nodes / turn.nodes if next_turn else None
                    ),
                    next_seconds_ratio=(
                        next_turn.seconds / turn.seconds
                        if next_turn
                        else None
                    ),
                    future_nodes=sum(item.nodes for item in future),
                    future_seconds=sum(item.seconds for item in future),
                )
            )
    return output, incomplete_sequences


def format_quantiles(values: list[float]) -> str:
    return "/".join(
        f"{percentile(values, probability):.3f}"
        for probability in (0.50, 0.75, 0.90, 0.95, 0.99)
    )


def report(label: str, group: list[Observation]) -> None:
    if not group:
        return
    next_node_ratios = [
        item.next_nodes_ratio
        for item in group
        if item.next_nodes_ratio is not None
    ]
    next_second_ratios = [
        item.next_seconds_ratio
        for item in group
        if item.next_seconds_ratio is not None
    ]
    future_node_ratios = [item.future_nodes_ratio for item in group]
    future_second_ratios = [item.future_seconds_ratio for item in group]
    future_nodes = [float(item.future_nodes) for item in group]
    print(
        "EGTRAJECTORY "
        f"group={label} n={len(group)} "
        f"next_n={len(next_node_ratios)} "
        f"turns_after50/90="
        f"{percentile([float(item.turns_after) for item in group], 0.50):.1f}/"
        f"{percentile([float(item.turns_after) for item in group], 0.90):.1f} "
        f"next_nodes_ratio50/75/90/95/99="
        f"{format_quantiles(next_node_ratios)} "
        f"future_nodes_ratio50/75/90/95/99="
        f"{format_quantiles(future_node_ratios)} "
        f"future_nodes_ratio_max={max(future_node_ratios):.3f} "
        f"future_nodes50/75/90/95/99={format_quantiles(future_nodes)} "
        f"future_nodes_max={max(future_nodes):.0f} "
        f"next_seconds_ratio50/75/90/95/99="
        f"{format_quantiles(next_second_ratios)} "
        f"future_seconds_ratio50/75/90/95/99="
        f"{format_quantiles(future_second_ratios)} "
        f"future_seconds_ratio_max={max(future_second_ratios):.3f}"
    )


def future_reserve_table(group: list[Observation]) -> list[int]:
    table: list[int] = []
    for rack_tiles in range(MAX_RACK_TILES + 1):
        eligible = [
            item.future_nodes
            for item in group
            if item.turn.rack_tiles <= rack_tiles
        ]
        table.append(max(eligible, default=0))
    return table


def report_future_reserve_audit(data: list[Observation]) -> None:
    table = future_reserve_table(data)
    print(
        "EGTRAJECTORY_RESERVE_TABLE "
        f"id={FUTURE_DEPTH5_RESERVE_ID} "
        f"nodes={','.join(str(value) for value in table)}"
    )

    sources = sorted({item.turn.source for item in data})
    for label, group in (
        ("all", data),
        ("initial", [item for item in data if item.turn.ply == 0]),
    ):
        for heldout_source in sources:
            training = [
                item for item in group if item.turn.source != heldout_source
            ]
            heldout = [
                item for item in group if item.turn.source == heldout_source
            ]
            if not training or not heldout:
                continue
            training_table = future_reserve_table(training)
            misses = [
                item
                for item in heldout
                if item.future_nodes
                > training_table[
                    min(MAX_RACK_TILES, max(0, item.turn.rack_tiles))
                ]
            ]
            excess_ratios = []
            for item in misses:
                predicted = training_table[
                    min(MAX_RACK_TILES, max(0, item.turn.rack_tiles))
                ]
                excess_ratios.append(
                    item.future_nodes / predicted
                    if predicted > 0
                    else math.inf
                )
            print(
                "EGTRAJECTORY_RESERVE_AUDIT "
                f"group={label} heldout_source={heldout_source} "
                f"train_n={len(training)} heldout_n={len(heldout)} "
                f"misses={len(misses)} "
                f"coverage={1.0 - len(misses) / len(heldout):.4f} "
                f"worst_excess_ratio="
                f"{max(excess_ratios, default=0.0):.3f}"
            )


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("logs", nargs="+", type=Path)
    args = parser.parse_args()

    turns, ended = load(args.logs)
    data, incomplete_sequences = observations(turns, ended)
    if not data:
        raise ValueError("no complete POTURN trajectories")

    positions = {
        (item.turn.source, item.turn.position) for item in data
    }
    players = {
        (item.turn.source, item.turn.position, item.turn.player)
        for item in data
    }
    print(
        "EGTRAJECTORY_SUMMARY "
        f"positions={len(positions)} player_sequences={len(players)} "
        f"turns={len(data)} incomplete_sequences={incomplete_sequences} "
        f"median_turn_nodes="
        f"{statistics.median(item.turn.nodes for item in data):.0f}"
    )
    report("all", data)
    initial = [item for item in data if item.turn.ply == 0]
    report("initial", initial)
    for rack_tiles in sorted({item.turn.rack_tiles for item in initial}):
        report(
            f"initial_tiles{rack_tiles}",
            [
                item
                for item in initial
                if item.turn.rack_tiles == rack_tiles
            ],
        )
    report(
        "future_needed",
        [item for item in data if item.turns_after > 0],
    )
    for rack_tiles in sorted({item.turn.rack_tiles for item in data}):
        report(
            f"tiles{rack_tiles}",
            [item for item in data if item.turn.rack_tiles == rack_tiles],
        )
    report_future_reserve_audit(data)


if __name__ == "__main__":
    main()
