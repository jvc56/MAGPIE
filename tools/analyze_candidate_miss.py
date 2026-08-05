#!/usr/bin/env python3
"""Analyze a common-judge top-k versus candidate-limit SIM panel."""

from __future__ import annotations

import argparse
from collections import defaultdict
from dataclasses import dataclass
import json
import math
from pathlib import Path
import statistics

try:
    from tools.clustered_inference import (
        student_t_critical,
        student_t_two_sided_p_value,
    )
except ModuleNotFoundError:  # Direct execution from tools/.
    from clustered_inference import (  # type: ignore[no-redef]
        student_t_critical,
        student_t_two_sided_p_value,
    )


def parse_fields(line: str) -> dict[str, str]:
    result: dict[str, str] = {}
    for token in line.split()[1:]:
        if "=" in token:
            key, value = token.split("=", 1)
            result[key] = value
    return result


@dataclass(frozen=True)
class CandidatePoint:
    source_index: int
    game: str
    bag: int
    plies: int
    target_nodes: int
    arm_plays: int
    control_kind: str
    iterations: int
    nodes: int
    selected_rank: int
    judge_best_rank: int
    judge_regret: float
    judge_win_regret: float
    judge_spread_regret: float


def load_pairs(path: Path) -> list[tuple[CandidatePoint, CandidatePoint]]:
    grouped: dict[tuple[int, int], dict[str, CandidatePoint]] = defaultdict(dict)
    with path.open(encoding="utf-8") as stream:
        for line in stream:
            if not line.startswith("THINKING_CURVE_POINT "):
                continue
            row = parse_fields(line)
            if row.get("final") != "0":
                continue
            kind = row.get("control_kind", "stopped")
            if kind not in {"stopped", "candidate_limit"}:
                continue
            point = CandidatePoint(
                source_index=int(row["source_index"]),
                game=row.get("game", row["source_index"]),
                bag=int(row["bag"]),
                plies=int(row["plies"]),
                target_nodes=int(row["target_nodes"]),
                arm_plays=int(row.get("arm_plays", row["candidates"])),
                control_kind=kind,
                iterations=int(row["iterations"]),
                nodes=int(row["nodes"]),
                selected_rank=int(row["selected_rank"]),
                judge_best_rank=int(row["judge_best_rank"]),
                judge_regret=float(row["judge_regret"]),
                judge_win_regret=float(row["judge_win_regret"]),
                judge_spread_regret=float(row["judge_spread_regret"]),
            )
            key = (point.source_index, point.plies)
            if kind in grouped[key]:
                raise ValueError(f"duplicate {kind} point for {key}")
            grouped[key][kind] = point
    result: list[tuple[CandidatePoint, CandidatePoint]] = []
    for key, points in grouped.items():
        if set(points) != {"stopped", "candidate_limit"}:
            raise ValueError(f"incomplete candidate-control pair for {key}")
        primary = points["stopped"]
        limited = points["candidate_limit"]
        if primary.target_nodes != limited.target_nodes:
            raise ValueError(f"candidate-control target mismatch for {key}")
        if limited.arm_plays >= primary.arm_plays:
            raise ValueError(f"candidate-control set is not smaller for {key}")
        result.append((primary, limited))
    if not result:
        raise ValueError("log has no candidate-control pairs")
    return sorted(result, key=lambda pair: pair[0].source_index)


@dataclass(frozen=True)
class Summary:
    mean: float
    low: float
    high: float
    p_value: float
    clusters: int


def summarize(values: list[float]) -> Summary:
    mean = statistics.fmean(values)
    if len(values) < 2:
        return Summary(mean, math.nan, math.nan, math.nan, len(values))
    standard_error = statistics.stdev(values) / math.sqrt(len(values))
    if standard_error == 0.0:
        p_value = 1.0 if mean == 0.0 else 0.0
    else:
        p_value = student_t_two_sided_p_value(
            mean / standard_error, len(values) - 1
        )
    radius = student_t_critical(0.95, len(values) - 1) * standard_error
    return Summary(mean, mean - radius, mean + radius, p_value, len(values))


def format_summary(summary: Summary, signed: bool = False) -> str:
    sign = "+" if signed else ""
    return (
        f"{summary.mean:{sign}.9f} "
        f"ci95=[{summary.low:{sign}.9f},{summary.high:{sign}.9f}] "
        f"p={summary.p_value:.6g} n={summary.clusters}"
    )


def wilson(events: int, total: int) -> tuple[float, float, float]:
    z = 1.959963984540054
    rate = events / total
    denominator = 1.0 + z * z / total
    center = (rate + z * z / (2.0 * total)) / denominator
    radius = (
        z
        * math.sqrt(rate * (1.0 - rate) / total + z * z / (4.0 * total * total))
        / denominator
    )
    return rate, center - radius, center + radius


def _strata(manifest: Path | None) -> dict[int, tuple[str, str]]:
    if manifest is None:
        return {}
    document = json.loads(manifest.read_text(encoding="utf-8"))
    return {
        int(row["position"]): (
            str(row["trajectory_policy"]),
            str(row["bag_band"]),
        )
        for row in document.get("mapping", [])
    }


def analyze(
    pairs: list[tuple[CandidatePoint, CandidatePoint]],
    strata: dict[int, tuple[str, str]],
) -> str:
    utility_gains = [limited.judge_regret - full.judge_regret for full, limited in pairs]
    win_gains = [
        limited.judge_win_regret - full.judge_win_regret for full, limited in pairs
    ]
    spread_gains = [
        limited.judge_spread_regret - full.judge_spread_regret
        for full, limited in pairs
    ]
    primary_outside = [
        full.selected_rank >= limited.arm_plays for full, limited in pairs
    ]
    miss_events = [
        outside and gain > 0.0
        for outside, gain in zip(primary_outside, utility_gains)
    ]
    oracle_outside = [
        full.judge_best_rank >= limited.arm_plays for full, limited in pairs
    ]
    disagreement = [
        full.selected_rank != limited.selected_rank for full, limited in pairs
    ]
    miss_mass = [
        max(gain, 0.0) if outside else 0.0
        for gain, outside in zip(utility_gains, primary_outside)
    ]
    dilution_loss = [max(-gain, 0.0) for gain in utility_gains]
    node_ratio = [full.nodes / limited.nodes for full, limited in pairs]

    lines = [
        f"pairs={len(pairs)} primary_plays={pairs[0][0].arm_plays} "
        f"candidate_limit_plays={pairs[0][1].arm_plays}",
        "utility_gain_full_minus_limit " + format_summary(summarize(utility_gains), True),
        "win_gain_full_minus_limit " + format_summary(summarize(win_gains), True),
        "spread_gain_full_minus_limit " + format_summary(summarize(spread_gains), True),
        "observed_candidate_miss_mass " + format_summary(summarize(miss_mass)),
        "candidate_dilution_loss " + format_summary(summarize(dilution_loss)),
        "full_to_limit_node_ratio " + format_summary(summarize(node_ratio)),
    ]
    for label, values in (
        ("selection_disagreement", disagreement),
        ("full_selected_outside_limit", primary_outside),
        ("common_judge_best_outside_limit", oracle_outside),
        ("positive_candidate_miss", miss_events),
    ):
        events = sum(values)
        rate, low, high = wilson(events, len(values))
        lines.append(
            f"{label} events={events}/{len(values)} rate={rate:.6f} "
            f"wilson95=[{low:.6f},{high:.6f}]"
        )

    if strata:
        grouped: dict[str, list[int]] = defaultdict(list)
        for index, (full, _) in enumerate(pairs):
            if full.source_index not in strata:
                raise ValueError(f"manifest lacks source {full.source_index}")
            policy, bag_band = strata[full.source_index]
            grouped[f"policy:{policy}"].append(index)
            grouped[f"bag:{bag_band}"].append(index)
        for label in sorted(grouped):
            indices = grouped[label]
            summary = summarize([utility_gains[index] for index in indices])
            events = sum(miss_events[index] for index in indices)
            rate, low, high = wilson(events, len(indices))
            lines.append(
                f"stratum={label} utility_gain={format_summary(summary, True)} "
                f"miss_events={events}/{len(indices)} "
                f"miss_wilson95=[{low:.6f},{high:.6f}] rate={rate:.6f}"
            )
    return "\n".join(lines)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", type=Path)
    parser.add_argument("--manifest", type=Path)
    args = parser.parse_args()
    pairs = load_pairs(args.log)
    print(analyze(pairs, _strata(args.manifest)))


if __name__ == "__main__":
    main()
