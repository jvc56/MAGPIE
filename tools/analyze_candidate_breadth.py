#!/usr/bin/env python3
"""Analyze nested SIM candidate limits under one common judge."""

from __future__ import annotations

import argparse
from collections import defaultdict
from dataclasses import dataclass
import json
from pathlib import Path

try:
    from tools.analyze_candidate_miss import (
        CandidatePoint,
        format_summary,
        parse_fields,
        summarize,
        wilson,
    )
except ModuleNotFoundError:  # Direct execution from tools/.
    from analyze_candidate_miss import (  # type: ignore[no-redef]
        CandidatePoint,
        format_summary,
        parse_fields,
        summarize,
        wilson,
    )


@dataclass(frozen=True)
class CandidateBreadthRoot:
    source_index: int
    points: dict[int, CandidatePoint]
    judge_utilities: dict[int, float]


@dataclass(frozen=True)
class RegretComponents:
    candidate_generation: float
    within_set_bai: float
    total: float


def decompose_regret(
    root: CandidateBreadthRoot, width: int
) -> RegretComponents:
    """Decompose common-risk-set regret at one nested candidate width.

    The common judge is deliberately limited to checkpoint-observable
    nominees.  Within that union the decomposition is exact: loss from the
    global judged best to the best judged rank admitted by ``width``, plus
    BAI's loss from that within-width best to the arm it selected.
    """

    if not root.judge_utilities:
        raise ValueError("root has no per-nominee common-judge rows")
    point = root.points[width]
    if point.selected_rank not in root.judge_utilities:
        raise ValueError("selected arm is absent from the common judge")
    admitted = [
        utility
        for rank, utility in root.judge_utilities.items()
        if rank < width
    ]
    if not admitted:
        raise ValueError(f"top-{width} contains no judged nominee")
    global_best = max(root.judge_utilities.values())
    admitted_best = max(admitted)
    selected = root.judge_utilities[point.selected_rank]
    candidate_generation = max(0.0, global_best - admitted_best)
    within_set_bai = max(0.0, admitted_best - selected)
    total = max(0.0, global_best - selected)
    if abs(candidate_generation + within_set_bai - total) > 2e-9:
        raise ValueError("candidate and within-set regret do not add to total")
    if abs(total - point.judge_regret) > 2e-9:
        raise ValueError("per-nominee rows do not reproduce point regret")
    return RegretComponents(candidate_generation, within_set_bai, total)


def load_roots(path: Path) -> list[CandidateBreadthRoot]:
    grouped: dict[tuple[int, int], dict[int, CandidatePoint]] = defaultdict(dict)
    judge_rows: dict[tuple[int, int], dict[int, float]] = defaultdict(dict)
    primary_sizes: dict[tuple[int, int], int] = {}
    with path.open(encoding="utf-8") as stream:
        for line in stream:
            if line.startswith("THINKING_CURVE_JUDGE_CANDIDATE "):
                row = parse_fields(line)
                key = (int(row["source_index"]), int(row["plies"]))
                rank = int(row["rank"])
                utility = float(row["utility"])
                if rank in judge_rows[key]:
                    raise ValueError(f"duplicate judge rank {rank} for {key}")
                judge_rows[key][rank] = utility
                continue
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
            if point.arm_plays in grouped[key]:
                raise ValueError(f"duplicate width {point.arm_plays} for {key}")
            grouped[key][point.arm_plays] = point
            if kind == "stopped":
                if key in primary_sizes:
                    raise ValueError(f"duplicate primary point for {key}")
                primary_sizes[key] = point.arm_plays

    roots: list[CandidateBreadthRoot] = []
    expected_widths: tuple[int, ...] | None = None
    for key, points in grouped.items():
        if key not in primary_sizes or len(points) < 2:
            raise ValueError(f"incomplete nested candidate controls for {key}")
        widths = tuple(sorted(points))
        if widths[-1] != primary_sizes[key]:
            raise ValueError(f"primary is not the widest arm for {key}")
        targets = {point.target_nodes for point in points.values()}
        if len(targets) != 1:
            raise ValueError(f"candidate limits use different budgets for {key}")
        if expected_widths is None:
            expected_widths = widths
        elif widths != expected_widths:
            raise ValueError("candidate widths differ across roots")
        roots.append(CandidateBreadthRoot(key[0], points, judge_rows.get(key, {})))
    if not roots:
        raise ValueError("log has no nested candidate controls")
    return sorted(roots, key=lambda root: root.source_index)


def load_strata(manifest: Path | None) -> dict[int, tuple[str, str]]:
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


def _event_line(label: str, values: list[bool]) -> str:
    events = sum(values)
    rate, low, high = wilson(events, len(values))
    return (
        f"{label} events={events}/{len(values)} rate={rate:.6f} "
        f"wilson95=[{low:.6f},{high:.6f}]"
    )


def analyze(
    roots: list[CandidateBreadthRoot],
    strata: dict[int, tuple[str, str]],
) -> str:
    widths = tuple(sorted(roots[0].points))
    widest = widths[-1]
    lines = [
        f"roots={len(roots)} widths={','.join(str(width) for width in widths)} "
        f"primary_plays={widest}"
    ]
    if all(root.judge_utilities for root in roots):
        for width in widths:
            components = [decompose_regret(root, width) for root in roots]
            lines.extend(
                [
                    f"components=top{width}",
                    "candidate_generation_regret "
                    + format_summary(
                        summarize([item.candidate_generation for item in components])
                    ),
                    "within_set_bai_regret "
                    + format_summary(
                        summarize([item.within_set_bai for item in components])
                    ),
                    "total_current_turn_regret "
                    + format_summary(summarize([item.total for item in components])),
                ]
            )
    for limit in widths[:-1]:
        full = [root.points[widest] for root in roots]
        limited = [root.points[limit] for root in roots]
        utility = [small.judge_regret - wide.judge_regret for wide, small in zip(full, limited)]
        win = [
            small.judge_win_regret - wide.judge_win_regret
            for wide, small in zip(full, limited)
        ]
        spread = [
            small.judge_spread_regret - wide.judge_spread_regret
            for wide, small in zip(full, limited)
        ]
        outside = [wide.selected_rank >= limit for wide in full]
        judge_outside = [wide.judge_best_rank >= limit for wide in full]
        disagreement = [wide.selected_rank != small.selected_rank for wide, small in zip(full, limited)]
        miss_mass = [max(value, 0.0) if event else 0.0 for value, event in zip(utility, outside)]
        dilution = [max(-value, 0.0) for value in utility]
        node_ratio = [wide.nodes / small.nodes for wide, small in zip(full, limited)]
        lines.extend(
            [
                f"comparison=top{widest}-minus-top{limit}",
                "utility_gain " + format_summary(summarize(utility), True),
                "win_gain " + format_summary(summarize(win), True),
                "spread_gain " + format_summary(summarize(spread), True),
                "candidate_miss_mass " + format_summary(summarize(miss_mass)),
                "candidate_dilution_loss " + format_summary(summarize(dilution)),
                "node_ratio " + format_summary(summarize(node_ratio)),
                _event_line("selection_disagreement", disagreement),
                _event_line("wide_selected_outside_limit", outside),
                _event_line("common_judge_best_outside_limit", judge_outside),
                _event_line(
                    "positive_candidate_miss",
                    [event and value > 0.0 for event, value in zip(outside, utility)],
                ),
            ]
        )
        if strata:
            grouped: dict[str, list[int]] = defaultdict(list)
            for index, root in enumerate(roots):
                if root.source_index not in strata:
                    raise ValueError(f"manifest lacks source {root.source_index}")
                policy, bag_band = strata[root.source_index]
                grouped[f"policy:{policy}"].append(index)
                grouped[f"bag:{bag_band}"].append(index)
            for label in sorted(grouped):
                indices = grouped[label]
                summary = summarize([utility[index] for index in indices])
                lines.append(
                    f"comparison=top{widest}-minus-top{limit} stratum={label} "
                    f"utility_gain={format_summary(summary, True)}"
                )

    for smaller, larger in zip(widths, widths[1:]):
        gains = [
            root.points[smaller].judge_regret - root.points[larger].judge_regret
            for root in roots
        ]
        lines.append(
            f"adjacent=top{larger}-minus-top{smaller} "
            f"utility_gain={format_summary(summarize(gains), True)}"
        )
    return "\n".join(lines)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", type=Path)
    parser.add_argument("--manifest", type=Path)
    args = parser.parse_args()
    print(analyze(load_roots(args.log), load_strata(args.manifest)))


if __name__ == "__main__":
    main()
