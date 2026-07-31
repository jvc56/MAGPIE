#!/usr/bin/env python3
"""Summarize PEG oracle regret at completed structural boundaries.

Input is the flushed PEGSTRUCT output from:

    ./bin/magpie_test pegstructcurve

Quality is compared at completed greedy/halving stages, independent of
hardware speed. Wall and process-CPU time are retained only to fit the local
conversion from PEG's two work coordinates (scenarios and nested endgame
nodes) to seconds and to diagnose noisy or thermally drifting runs.
"""

from __future__ import annotations

import argparse
import csv
import itertools
import math
import re
import statistics
from dataclasses import dataclass
from pathlib import Path

import numpy as np


@dataclass(frozen=True)
class Arm:
    bag: int
    position: int
    order: str
    requested_stage: int
    move: str
    oracle_win: float
    oracle_spread: float
    best_win: float
    best_spread: float
    elapsed_seconds: float
    cpu_seconds: float
    scenarios: int
    endgame_nodes: int
    last_stage: int
    deep_candidates: int
    root_candidates: int
    candidate_evals: int
    greedy_elapsed_seconds: float
    greedy_cpu_seconds: float
    greedy_scenarios: int
    greedy_endgame_nodes: int
    deep_total: int
    partial: bool
    boundary_complete: bool

    @property
    def oracle_utility(self) -> float:
        return self.oracle_win + 1.0e-4 * self.oracle_spread

    @property
    def best_utility(self) -> float:
        return self.best_win + 1.0e-4 * self.best_spread

    @property
    def utility_regret(self) -> float:
        return self.best_utility - self.oracle_utility

    @property
    def win_regret(self) -> float:
        return self.best_win - self.oracle_win

    @property
    def spread_regret(self) -> float:
        return self.best_spread - self.oracle_spread

    @property
    def scheduled_cores(self) -> float:
        return self.cpu_seconds / self.elapsed_seconds


@dataclass(frozen=True)
class CandidateEvent:
    bag: int
    position: int
    requested_stage: int
    order: str
    phase: int
    depth: int
    rank: int
    completed: int
    total: int
    elapsed_ns: int
    cpu_ns: int
    scenarios: int
    endgame_nodes: int
    item_id: int
    win: float
    spread: float


@dataclass(frozen=True)
class StageStart:
    bag: int
    position: int
    requested_stage: int
    order: str
    phase: int
    depth: int
    total: int
    start_ns: int
    start_cpu_ns: int


@dataclass(frozen=True)
class OracleStatus:
    complete: bool
    target_depth: int
    scenario_stride: int
    protected: int
    scored: int


def parse_fields(line: str) -> dict[str, str]:
    fields: dict[str, str] = {}
    for token in line.split()[1:]:
        if "=" in token:
            key, value = token.split("=", 1)
            fields[key] = value
    return fields


def load_oracle_status(path: Path) -> dict[tuple[int, int], OracleStatus]:
    statuses: dict[tuple[int, int], OracleStatus] = {}
    with path.open(encoding="utf-8") as stream:
        for line in stream:
            if not line.startswith("PEGSTRUCTORACLE "):
                continue
            fields = parse_fields(line)
            protected = int(fields["protected"])
            scored = int(fields["scored"])
            target_depth = int(fields.get("target_depth", "0"))
            last_stage = int(fields["last_stage"])
            partial = bool(int(fields["partial"]))
            direct_fidelity = int(fields.get("direct_fidelity", "0"))
            if "complete" in fields:
                complete = bool(int(fields["complete"]))
            elif protected <= 1:
                complete = True
            elif direct_fidelity > 0:
                complete = (
                    last_stage >= 1
                    and not partial
                    and scored == protected
                )
            else:
                complete = (
                    last_stage + 1 >= target_depth
                    and not partial
                    and scored == protected
                )
            statuses[(int(fields["bag"]), int(fields["pos"]))] = OracleStatus(
                complete=complete,
                target_depth=target_depth,
                scenario_stride=int(fields.get("scenario_stride", "1")),
                protected=protected,
                scored=scored,
            )
    return statuses


def load_arms(
    paths: list[Path], *, require_complete_oracle: bool = False
) -> list[Arm]:
    # Resumed runs may repeat a position. The final flushed observation wins.
    arms: dict[tuple[int, int, int], Arm] = {}
    for path in paths:
        oracle_status = load_oracle_status(path)
        with path.open(encoding="utf-8") as stream:
            for line in stream:
                complete_row = line.startswith("PEGSTRUCT ")
                partial_row = line.startswith("PEGSTRUCTPARTIAL ")
                if not complete_row and not partial_row:
                    continue
                first_arm = re.search(r" s\d+=", line)
                prefix = line[: first_arm.start()] if first_arm else line
                fields = parse_fields(prefix)
                if "bestw" not in fields:
                    continue
                bag = int(fields["bag"])
                position = int(fields["pos"])
                if require_complete_oracle:
                    status = oracle_status.get((bag, position))
                    if status is None or not status.complete:
                        continue
                order = fields["order"]
                best_win = float(fields["bestw"])
                best_spread = float(fields["bests"])
                for arm_match in re.finditer(
                    r" s(?P<stage>\d+)=(?P<value>.*?)(?= s\d+=|$)", line
                ):
                    stage = int(arm_match.group("stage"))
                    value = arm_match.group("value")
                    pieces = value.split("|")
                    if len(pieces) not in (11, 15, 18):
                        raise ValueError(
                            f"{path}: expected 11, 15, or 18 fields in "
                            f"s{stage}, "
                            f"got {len(pieces)}"
                        )
                    has_greedy = len(pieces) >= 15
                    arm = Arm(
                        bag=bag,
                        position=position,
                        order=order,
                        requested_stage=stage,
                        move=pieces[0],
                        oracle_win=float(pieces[1]),
                        oracle_spread=float(pieces[2]),
                        best_win=best_win,
                        best_spread=best_spread,
                        elapsed_seconds=float(pieces[3]),
                        cpu_seconds=float(pieces[4]),
                        scenarios=int(pieces[5]),
                        endgame_nodes=int(pieces[6]),
                        last_stage=int(pieces[7]),
                        deep_candidates=int(pieces[8]),
                        root_candidates=int(pieces[9]),
                        candidate_evals=int(pieces[10]),
                        greedy_elapsed_seconds=(
                            float(pieces[11])
                            if has_greedy
                            else math.nan
                        ),
                        greedy_cpu_seconds=(
                            float(pieces[12])
                            if has_greedy
                            else math.nan
                        ),
                        greedy_scenarios=(
                            int(pieces[13]) if has_greedy else 0
                        ),
                        greedy_endgame_nodes=(
                            int(pieces[14]) if has_greedy else 0
                        ),
                        deep_total=(
                            int(pieces[15])
                            if len(pieces) == 18
                            else int(pieces[8])
                        ),
                        partial=(
                            bool(int(pieces[16]))
                            if len(pieces) == 18
                            else False
                        ),
                        boundary_complete=(
                            bool(int(pieces[17]))
                            if len(pieces) == 18
                            else complete_row
                        ),
                    )
                    if arm.elapsed_seconds > 0.0:
                        arms[(bag, position, stage)] = arm
    return list(arms.values())


def load_candidate_timeline(
    paths: list[Path],
) -> tuple[list[CandidateEvent], dict[tuple[int, int, int, int], StageStart]]:
    events: dict[tuple[int, int, int, int, int], CandidateEvent] = {}
    starts: dict[tuple[int, int, int, int], StageStart] = {}
    for path in paths:
        with path.open(encoding="utf-8") as stream:
            for line in stream:
                if line.startswith("PEGSTRUCTSTAGE "):
                    fields = parse_fields(line)
                    start = StageStart(
                        bag=int(fields["bag"]),
                        position=int(fields["pos"]),
                        requested_stage=int(fields["requested_stage"]),
                        order=fields["order"],
                        phase=int(fields["phase"]),
                        depth=int(fields["depth"]),
                        total=int(fields["total"]),
                        start_ns=int(fields["start_ns"]),
                        start_cpu_ns=int(fields["start_cpu_ns"]),
                    )
                    starts[
                        (
                            start.bag,
                            start.position,
                            start.requested_stage,
                            start.phase,
                        )
                    ] = start
                elif line.startswith("PEGSTRUCTCAND "):
                    fields = parse_fields(line)
                    event = CandidateEvent(
                        bag=int(fields["bag"]),
                        position=int(fields["pos"]),
                        requested_stage=int(fields["requested_stage"]),
                        order=fields["order"],
                        phase=int(fields["phase"]),
                        depth=int(fields["depth"]),
                        rank=int(fields.get("rank", "-1")),
                        completed=int(fields["completed"]),
                        total=int(fields["total"]),
                        elapsed_ns=int(fields["elapsed_ns"]),
                        cpu_ns=int(fields["cpu_ns"]),
                        scenarios=int(fields["scenarios"]),
                        endgame_nodes=int(fields["endgame_nodes"]),
                        item_id=int(fields["item"]),
                        win=float(fields["win"]),
                        spread=float(fields["spread"]),
                    )
                    events[
                        (
                            event.bag,
                            event.position,
                            event.requested_stage,
                            event.phase,
                            event.item_id,
                        )
                    ] = event
    return list(events.values()), starts


def mean_ci(values: list[float]) -> tuple[float, float, float]:
    mean = statistics.fmean(values)
    if len(values) < 2:
        return mean, math.nan, math.nan
    sem = statistics.stdev(values) / math.sqrt(len(values))
    return mean, mean - 1.96 * sem, mean + 1.96 * sem


def robust_cv(values: list[float]) -> float:
    if not values:
        return math.nan
    median = statistics.median(values)
    if median == 0.0:
        return math.nan
    mad = statistics.median(abs(value - median) for value in values)
    return 1.4826 * mad / abs(median)


def fit_nonnegative_cost(
    arms: list[Arm], target: str
) -> tuple[float, float, float, float, float, str, np.ndarray]:
    """Fit incremental post-greedy time from added scenarios and nodes.

    Each structural arm repeats the exact greedy seed and records it
    separately. TimeManager will have already paid that prefix when deciding
    whether to buy another PEG stage, so fitting absolute arm time would count
    the prefix twice and confound position-specific move-generation cost with
    refinement cost. Enumerating every nonempty active set of scenarios,
    nested-endgame nodes, and completed candidates gives the exact
    nonnegative least-squares solution for these three coordinates.
    """

    refinement_arms = [
        arm
        for arm in arms
        if arm.scenarios > arm.greedy_scenarios
        or arm.endgame_nodes > arm.greedy_endgame_nodes
    ]
    if not refinement_arms:
        raise ValueError("no post-greedy PEG refinement observations")
    y = np.asarray(
        [
            (
                arm.elapsed_seconds - arm.greedy_elapsed_seconds
                if target == "wall"
                else arm.cpu_seconds - arm.greedy_cpu_seconds
            )
            for arm in refinement_arms
        ],
        dtype=float,
    )
    added_scenarios_raw = [
        arm.scenarios - arm.greedy_scenarios for arm in refinement_arms
    ]
    added_nodes_raw = [
        arm.endgame_nodes - arm.greedy_endgame_nodes
        for arm in refinement_arms
    ]
    added_candidates_raw = [
        max(0, arm.candidate_evals - arm.root_candidates)
        for arm in refinement_arms
    ]
    scenario_scale = max(statistics.median(added_scenarios_raw), 1.0)
    node_scale = max(
        statistics.median(added_nodes_raw), 1.0
    )
    candidate_scale = max(statistics.median(added_candidates_raw), 1.0)
    scenarios = np.asarray(
        [value / scenario_scale for value in added_scenarios_raw]
    )
    nodes = np.asarray([value / node_scale for value in added_nodes_raw])
    candidate_feature = np.asarray(
        [value / candidate_scale for value in added_candidates_raw]
    )

    fits: list[tuple[float, np.ndarray, str]] = []
    features = {
        "scenarios": scenarios,
        "nodes": nodes,
        "candidates": candidate_feature,
    }
    for feature_count in range(1, len(features) + 1):
        for names in itertools.combinations(features, feature_count):
            columns = tuple(features[name] for name in names)
            label = "+".join(names)
            matrix = np.column_stack(columns)
            beta, _, _, _ = np.linalg.lstsq(matrix, y, rcond=None)
            if np.all(beta >= -1.0e-12):
                prediction = matrix @ np.maximum(beta, 0.0)
                fits.append(
                    (float(np.sum((y - prediction) ** 2)), beta, label)
                )
    if not fits:
        raise ValueError("no feasible PEG cost model")
    _, beta, label = min(fits, key=lambda candidate: candidate[0])
    beta = np.maximum(beta, 0.0)
    fixed = 0.0
    active_names = label.split("+")
    coefficients = {
        name: float(beta[index])
        for index, name in enumerate(active_names)
    }
    scenario_coefficient = coefficients.get("scenarios", 0.0) / scenario_scale
    node_coefficient = coefficients.get("nodes", 0.0) / node_scale
    candidate_coefficient = (
        coefficients.get("candidates", 0.0) / candidate_scale
    )
    incremental_prediction = (
        scenario_coefficient
        * np.asarray(added_scenarios_raw, dtype=float)
        + node_coefficient * np.asarray(added_nodes_raw, dtype=float)
        + candidate_coefficient
        * np.asarray(added_candidates_raw, dtype=float)
    )
    total = float(np.sum((y - np.mean(y)) ** 2))
    residual = float(np.sum((y - incremental_prediction) ** 2))
    r_squared = 1.0 - residual / total if total > 0.0 else math.nan
    prediction_by_identity = {
        id(arm): (
            (
                arm.greedy_elapsed_seconds
                if target == "wall"
                else arm.greedy_cpu_seconds
            )
            + scenario_coefficient
            * (arm.scenarios - arm.greedy_scenarios)
            + node_coefficient
            * (arm.endgame_nodes - arm.greedy_endgame_nodes)
            + candidate_coefficient
            * max(0, arm.candidate_evals - arm.root_candidates)
        )
        for arm in refinement_arms
    }
    prediction = np.asarray(
        [prediction_by_identity.get(id(arm), math.nan) for arm in arms]
    )
    return (
        fixed,
        scenario_coefficient,
        node_coefficient,
        candidate_coefficient,
        r_squared,
        label,
        prediction,
    )


def median_order_bias(arms: list[Arm], prediction: np.ndarray) -> float:
    residuals: dict[str, list[float]] = {"asc": [], "desc": []}
    for arm, predicted in zip(arms, prediction, strict=True):
        if (
            math.isfinite(predicted)
            and predicted > 0.0
            and arm.order in residuals
        ):
            residuals[arm.order].append(
                arm.elapsed_seconds / float(predicted) - 1.0
            )
    # Order is alternated between positions, not repeated within a position.
    # A tiny group therefore confounds ordinary position residuals with order;
    # require enough positions on both sides before treating this as a gate.
    if len(residuals["asc"]) < 10 or len(residuals["desc"]) < 10:
        return math.nan
    return statistics.median(residuals["desc"]) - statistics.median(
        residuals["asc"]
    )


def half_drift(values: list[float]) -> float:
    half = len(values) // 2
    if half == 0:
        return math.nan
    early = statistics.median(values[:half])
    if early == 0.0:
        return math.nan
    return statistics.median(values[-half:]) / early - 1.0


def sentinel_diagnostic(
    arms: list[Arm], max_cv: float, max_drift: float, max_core_cv: float
) -> tuple[bool, float, float, float, int]:
    """Compare the identical greedy prefix repeated by every arm."""

    by_position: dict[tuple[int, int], list[Arm]] = {}
    for arm in arms:
        if (
            arm.greedy_elapsed_seconds > 0.0
            and arm.greedy_scenarios > 0
            and math.isfinite(arm.greedy_cpu_seconds)
        ):
            by_position.setdefault((arm.bag, arm.position), []).append(arm)

    normalized_in_order: list[tuple[tuple[int, int, int], float]] = []
    scheduled_cores: list[float] = []
    repeated_positions = 0
    for (bag, position), group in by_position.items():
        if len(group) < 2:
            continue
        repeated_positions += 1
        rates = [
            arm.greedy_scenarios / arm.greedy_elapsed_seconds for arm in group
        ]
        center = statistics.median(rates)
        core_center = statistics.median(
            arm.greedy_cpu_seconds / arm.greedy_elapsed_seconds
            for arm in group
        )
        for arm, rate in zip(group, rates, strict=True):
            # Reconstruct execution slot from the alternating arm order.
            max_stage = max(candidate.requested_stage for candidate in group)
            slot = (
                arm.requested_stage
                if arm.order == "asc"
                else max_stage - arm.requested_stage
            )
            normalized_in_order.append(
                ((bag, position, slot), rate / center)
            )
            if core_center > 0.0:
                scheduled_cores.append(
                    (
                        arm.greedy_cpu_seconds
                        / arm.greedy_elapsed_seconds
                    )
                    / core_center
                )
    normalized_in_order.sort()
    normalized = [value for _, value in normalized_in_order]
    rate_cv = robust_cv(normalized)
    drift = half_drift(normalized)
    core_cv = robust_cv(scheduled_cores)
    enough = len(normalized) >= 6 and repeated_positions >= 3
    quiet = (
        enough
        and rate_cv <= max_cv
        and abs(drift) <= max_drift
        and core_cv <= max_core_cv
    )
    return quiet, rate_cv, drift, core_cv, len(normalized)


def summarize(label: str, arms: list[Arm]) -> list[dict[str, object]]:
    by_stage: dict[int, list[Arm]] = {}
    by_position: dict[tuple[int, int], dict[int, Arm]] = {}
    for arm in arms:
        by_stage.setdefault(arm.requested_stage, []).append(arm)
        by_position.setdefault((arm.bag, arm.position), {})[
            arm.requested_stage
        ] = arm

    rows: list[dict[str, object]] = []
    for stage, group in sorted(by_stage.items()):
        regret = [arm.utility_regret for arm in group]
        mean, low, high = mean_ci(regret)
        paired_gain: list[float] = []
        delta_scenarios: list[int] = []
        delta_nodes: list[int] = []
        delta_seconds: list[float] = []
        if stage > 0:
            for arm in group:
                previous = by_position[(arm.bag, arm.position)].get(stage - 1)
                if previous is None:
                    continue
                paired_gain.append(previous.utility_regret - arm.utility_regret)
                delta_scenarios.append(arm.scenarios - previous.scenarios)
                delta_nodes.append(arm.endgame_nodes - previous.endgame_nodes)
                delta_seconds.append(
                    arm.elapsed_seconds - previous.elapsed_seconds
                )
        gain, gain_low, gain_high = (
            mean_ci(paired_gain)
            if paired_gain
            else (math.nan, math.nan, math.nan)
        )
        row: dict[str, object] = {
            "group": label,
            "stage": stage,
            "positions": len(group),
            "mean_utility_regret": mean,
            "ci95_low": low,
            "ci95_high": high,
            "mean_win_regret": statistics.fmean(
                arm.win_regret for arm in group
            ),
            "mean_spread_regret": statistics.fmean(
                arm.spread_regret for arm in group
            ),
            "oracle_best_rate": statistics.fmean(
                abs(arm.utility_regret) < 1.0e-12 for arm in group
            ),
            "median_seconds": statistics.median(
                arm.elapsed_seconds for arm in group
            ),
            "median_scenarios": statistics.median(
                arm.scenarios for arm in group
            ),
            "median_endgame_nodes": statistics.median(
                arm.endgame_nodes for arm in group
            ),
            "mean_scheduled_cores": statistics.fmean(
                arm.scheduled_cores for arm in group
            ),
            "robust_scheduled_cores_cv": robust_cv(
                [arm.scheduled_cores for arm in group]
            ),
            "marginal_utility_gain": gain,
            "marginal_ci95_low": gain_low,
            "marginal_ci95_high": gain_high,
            "median_incremental_seconds": (
                statistics.median(delta_seconds)
                if delta_seconds
                else math.nan
            ),
            "median_incremental_scenarios": (
                statistics.median(delta_scenarios)
                if delta_scenarios
                else math.nan
            ),
            "median_incremental_endgame_nodes": (
                statistics.median(delta_nodes) if delta_nodes else math.nan
            ),
        }
        rows.append(row)
        print(
            "PEG_STRUCT_VALUE "
            f"group={label} stage={stage} n={len(group)} "
            f"regret={mean:.8f} ci95=[{low:.8f},{high:.8f}] "
            f"win_regret={row['mean_win_regret']:.8f} "
            f"spread_regret={row['mean_spread_regret']:.4f} "
            f"oracle_best={row['oracle_best_rate']:.4f} "
            f"median_seconds={row['median_seconds']:.4f} "
            f"scenarios={row['median_scenarios']:.0f} "
            f"endgame_nodes={row['median_endgame_nodes']:.0f} "
            f"marginal_gain={gain:+.8f} "
            f"marginal_ci95=[{gain_low:+.8f},{gain_high:+.8f}]"
        )
    return rows


def normal_mean_pvalue(values: list[float]) -> float:
    if len(values) < 2:
        return math.nan
    standard_error = statistics.stdev(values) / math.sqrt(len(values))
    if standard_error == 0.0:
        return 0.0 if statistics.fmean(values) != 0.0 else 1.0
    z_score = statistics.fmean(values) / standard_error
    return math.erfc(abs(z_score) / math.sqrt(2.0))


def summarize_pair_gain(
    label: str, quality_arms: list[Arm], timing_arms: list[Arm]
) -> None:
    """Report the value of stage 1 over greedy under a common pair judge.

    A pair-conditioned oracle cannot estimate regret against moves that neither
    arm nominated. It can, however, estimate the decision TimeManager needs:
    how much utility the refinement's nominated move gains over the greedy
    nominee. Positions where both arms nominate the same move are exact zeroes
    and need no oracle work.
    """

    def pair_map(arms: list[Arm]) -> dict[tuple[int, int], dict[int, Arm]]:
        result: dict[tuple[int, int], dict[int, Arm]] = {}
        for arm in arms:
            if arm.requested_stage in (0, 1):
                result.setdefault((arm.bag, arm.position), {})[
                    arm.requested_stage
                ] = arm
        return result

    quality_pairs = pair_map(quality_arms)
    timing_pairs = pair_map(timing_arms)
    gains: dict[tuple[int, int], tuple[float, float, float, bool]] = {}
    evaluated_nontrivial = 0
    for key, pair in quality_pairs.items():
        if 0 not in pair or 1 not in pair:
            continue
        greedy = pair[0]
        refined = pair[1]
        if greedy.oracle_win < 0.0 or refined.oracle_win < 0.0:
            continue
        same_move = greedy.move == refined.move
        gains[key] = (
            refined.oracle_utility - greedy.oracle_utility,
            refined.oracle_win - greedy.oracle_win,
            refined.oracle_spread - greedy.oracle_spread,
            same_move,
        )
        evaluated_nontrivial += 0 if same_move else 1

    # Equal nominations have exactly zero pair-conditioned regret regardless
    # of whether the expensive judge ran. Add those from the timing corpus.
    for key, pair in timing_pairs.items():
        if key in gains or 0 not in pair or 1 not in pair:
            continue
        if pair[0].move == pair[1].move:
            gains[key] = (0.0, 0.0, 0.0, True)

    values = [gain[0] for gain in gains.values()]
    win_values = [gain[1] for gain in gains.values()]
    spread_values = [gain[2] for gain in gains.values()]
    conditional = [gain[0] for gain in gains.values() if not gain[3]]
    trivial = sum(gain[3] for gain in gains.values())
    eligible_nontrivial = sum(
        0 in pair and 1 in pair and pair[0].move != pair[1].move
        for pair in timing_pairs.values()
    )
    unjudged_nontrivial = max(0, eligible_nontrivial - evaluated_nontrivial)
    mean, low, high = (
        mean_ci(values) if values else (math.nan, math.nan, math.nan)
    )
    conditional_mean, conditional_low, conditional_high = (
        mean_ci(conditional)
        if conditional
        else (math.nan, math.nan, math.nan)
    )
    print(
        "PEG_PAIR_VALUE "
        f"group={label} n={len(values)} "
        f"evaluated_nontrivial={evaluated_nontrivial} "
        f"trivial_agreements={trivial} "
        f"unjudged_nontrivial={unjudged_nontrivial} "
        f"utility_gain={mean:+.8f} ci95=[{low:+.8f},{high:+.8f}] "
        f"p={normal_mean_pvalue(values):.6g} "
        f"win_gain={statistics.fmean(win_values) if win_values else math.nan:+.8f} "
        f"spread_gain={statistics.fmean(spread_values) if spread_values else math.nan:+.4f} "
        f"conditional_gain={conditional_mean:+.8f} "
        f"conditional_ci95=[{conditional_low:+.8f},{conditional_high:+.8f}]"
    )


def percentile(values: list[float], quantile: float) -> float:
    if not values:
        return math.nan
    return float(np.percentile(np.asarray(values, dtype=float), quantile))


def summarize_partial(label: str, arms: list[Arm]) -> None:
    by_phase: dict[int, list[Arm]] = {}
    for arm in arms:
        by_phase.setdefault(arm.last_stage, []).append(arm)
    for phase, group in sorted(by_phase.items()):
        regret = [arm.utility_regret for arm in group]
        mean, low, high = mean_ci(regret)
        fractions = [
            arm.deep_candidates / arm.deep_total
            for arm in group
            if arm.deep_total > 0
        ]
        print(
            "PEG_PARTIAL_VALUE "
            f"group={label} phase={phase} n={len(group)} "
            f"regret={mean:.8f} ci95=[{low:.8f},{high:.8f}] "
            f"win_regret={statistics.fmean(arm.win_regret for arm in group):.8f} "
            f"spread_regret="
            f"{statistics.fmean(arm.spread_regret for arm in group):.4f} "
            f"completed50={statistics.median(arm.deep_candidates for arm in group):.0f} "
            f"total50={statistics.median(arm.deep_total for arm in group):.0f} "
            f"fraction50={statistics.median(fractions) if fractions else math.nan:.4f} "
            f"seconds50={statistics.median(arm.elapsed_seconds for arm in group):.4f} "
            f"scenarios50={statistics.median(arm.scenarios for arm in group):.0f} "
            f"endgame_nodes50={statistics.median(arm.endgame_nodes for arm in group):.0f}"
        )


def summarize_candidate_timeline(
    label: str,
    events: list[CandidateEvent],
    starts: dict[tuple[int, int, int, int], StageStart],
    arms: list[Arm],
) -> None:
    arm_by_key = {
        (arm.bag, arm.position, arm.requested_stage): arm for arm in arms
    }
    by_phase: dict[
        int, dict[tuple[int, int, int], list[CandidateEvent]]
    ] = {}
    for event in events:
        by_phase.setdefault(event.phase, {}).setdefault(
            (event.bag, event.position, event.requested_stage), []
        ).append(event)

    for phase, arm_events in sorted(by_phase.items()):
        coverages: list[float] = []
        totals: list[int] = []
        completed: list[int] = []
        item_scenarios: list[float] = []
        item_nodes: list[float] = []
        boundary_seconds: dict[int, list[float]] = {
            target: [] for target in (1, 2, 4, 8, 16, 32)
        }
        boundary_scenarios: dict[int, list[float]] = {
            target: [] for target in boundary_seconds
        }
        boundary_nodes: dict[int, list[float]] = {
            target: [] for target in boundary_seconds
        }
        for arm_key, group in arm_events.items():
            group.sort(key=lambda event: (event.elapsed_ns, event.item_id))
            start = starts.get((*arm_key, phase))
            if start is not None:
                start_ns = start.start_ns
            else:
                # Older calibration fragments predate PEGSTRUCTSTAGE. For the
                # first refinement, the separately recorded greedy checkpoint
                # is the exact prefix to subtract. Deeper phases require the
                # explicit stage-start record and deliberately fall back to
                # solve-relative time rather than inventing a boundary.
                arm = arm_by_key.get(arm_key)
                start_ns = (
                    int(arm.greedy_elapsed_seconds * 1.0e9)
                    if phase == 1
                    and arm is not None
                    and math.isfinite(arm.greedy_elapsed_seconds)
                    else 0
                )
            total = (
                start.total
                if start is not None
                else max(event.total for event in group)
            )
            totals.append(total)
            completed.append(len(group))
            if total > 0:
                coverages.append(len(group) / total)
            cumulative_scenarios = 0
            cumulative_nodes = 0
            for ordinal, event in enumerate(group, start=1):
                cumulative_scenarios += event.scenarios
                cumulative_nodes += event.endgame_nodes
                item_scenarios.append(float(event.scenarios))
                item_nodes.append(float(event.endgame_nodes))
                if ordinal in boundary_seconds:
                    boundary_seconds[ordinal].append(
                        max(0, event.elapsed_ns - start_ns) / 1.0e9
                    )
                    boundary_scenarios[ordinal].append(
                        float(cumulative_scenarios)
                    )
                    boundary_nodes[ordinal].append(float(cumulative_nodes))

        print(
            "PEG_CANDIDATE_TIMELINE "
            f"group={label} phase={phase} arms={len(arm_events)} "
            f"events={sum(completed)} completed50={statistics.median(completed):.0f} "
            f"total50={statistics.median(totals):.0f} "
            f"coverage50={statistics.median(coverages) if coverages else math.nan:.4f} "
            f"coverage10={percentile(coverages, 10):.4f} "
            f"coverage90={percentile(coverages, 90):.4f} "
            f"item_scenarios50={statistics.median(item_scenarios):.0f} "
            f"item_scenarios90={percentile(item_scenarios, 90):.0f} "
            f"item_nodes50={statistics.median(item_nodes):.0f} "
            f"item_nodes90={percentile(item_nodes, 90):.0f} "
            f"item_nodes99={percentile(item_nodes, 99):.0f}"
        )
        for target in boundary_seconds:
            if not boundary_seconds[target]:
                continue
            print(
                "PEG_CANDIDATE_BOUNDARY "
                f"group={label} phase={phase} completed={target} "
                f"arms={len(boundary_seconds[target])} "
                f"seconds50={statistics.median(boundary_seconds[target]):.6f} "
                f"seconds90={percentile(boundary_seconds[target], 90):.6f} "
                f"scenarios50={statistics.median(boundary_scenarios[target]):.0f} "
                f"endgame_nodes50={statistics.median(boundary_nodes[target]):.0f} "
                f"endgame_nodes90={percentile(boundary_nodes[target], 90):.0f}"
            )


def summarize_stage_completion_tail(
    label: str, arms: list[Arm], requested_stage: int = 1
) -> None:
    """Report the whole-stage unit the current parallel dispatcher can gate."""

    attempted = [
        arm for arm in arms if arm.requested_stage == requested_stage
    ]
    if not attempted:
        return
    complete = [arm for arm in attempted if arm.boundary_complete]
    if not complete:
        print(
            "PEG_STAGE_TAIL "
            f"group={label} stage={requested_stage} "
            f"attempted={len(attempted)} complete=0 "
            f"censored={len(attempted)} has_bound=0"
        )
        return
    incremental_scenarios = [
        max(0, arm.scenarios - arm.greedy_scenarios) for arm in complete
    ]
    incremental_nodes = [
        max(0, arm.endgame_nodes - arm.greedy_endgame_nodes)
        for arm in complete
    ]
    incremental_candidates = [
        max(0, arm.candidate_evals - arm.root_candidates)
        for arm in complete
    ]
    censored = len(attempted) - len(complete)
    # With n independent source positions, the sample maximum covers the true
    # p99 with confidence 1 - .99^n. Candidate events within a position are
    # not treated as independent. Any right-censored position invalidates the
    # completed-only maximum as an upper bound.
    provisional_confidence = (
        1.0 - 0.99 ** len(complete) if censored == 0 else 0.0
    )
    print(
        "PEG_STAGE_TAIL "
        f"group={label} stage={requested_stage} "
        f"attempted={len(attempted)} complete={len(complete)} "
        f"censored={censored} "
        f"scenarios50={statistics.median(incremental_scenarios):.0f} "
        f"scenarios_max={max(incremental_scenarios)} "
        f"endgame_nodes50={statistics.median(incremental_nodes):.0f} "
        f"endgame_nodes_max={max(incremental_nodes)} "
        f"candidates50={statistics.median(incremental_candidates):.0f} "
        f"candidates_max={max(incremental_candidates)} "
        f"provisional_p99_confidence={provisional_confidence:.8f} "
        f"has_bound={int(censored == 0)}"
    )


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("logs", nargs="+", type=Path)
    parser.add_argument(
        "--quality-log",
        action="append",
        type=Path,
        default=[],
        help=(
            "pair-oracle log used for quality; may be repeated. Timing/cost "
            "always comes from the positional logs."
        ),
    )
    parser.add_argument("--csv", type=Path)
    parser.add_argument("--max-order-bias", type=float, default=0.10)
    parser.add_argument("--max-sentinel-cv", type=float, default=0.10)
    parser.add_argument("--max-sentinel-drift", type=float, default=0.10)
    parser.add_argument("--max-core-cv", type=float, default=0.10)
    args = parser.parse_args()

    timing_arms = load_arms(args.logs)
    if not timing_arms:
        raise ValueError("no complete PEGSTRUCT observations")
    quality_paths = args.quality_log if args.quality_log else args.logs
    quality_arms = load_arms(
        quality_paths, require_complete_oracle=True
    )
    complete_arms = [arm for arm in timing_arms if arm.boundary_complete]
    quality_complete_arms = [
        arm for arm in quality_arms if arm.boundary_complete
    ]
    quality_partial_arms = [
        arm for arm in quality_arms if not arm.boundary_complete
    ]
    if not complete_arms:
        raise ValueError("no completed PEG structural boundaries")
    candidate_events, stage_starts = load_candidate_timeline(args.logs)

    output_rows: list[dict[str, object]] = []
    if quality_complete_arms:
        output_rows.extend(summarize("all", quality_complete_arms))
        for bag in sorted({arm.bag for arm in quality_complete_arms}):
            output_rows.extend(
                summarize(
                    f"bag{bag}",
                    [
                        arm
                        for arm in quality_complete_arms
                        if arm.bag == bag
                    ],
                )
            )
    else:
        print("PEG_STRUCT_VALUE quality=unavailable")
    if quality_partial_arms:
        summarize_partial("all", quality_partial_arms)
        for bag in sorted({arm.bag for arm in quality_partial_arms}):
            summarize_partial(
                f"bag{bag}",
                [arm for arm in quality_partial_arms if arm.bag == bag],
            )
    summarize_pair_gain("all", quality_arms, timing_arms)
    for bag in sorted({arm.bag for arm in timing_arms}):
        summarize_pair_gain(
            f"bag{bag}",
            [arm for arm in quality_arms if arm.bag == bag],
            [arm for arm in timing_arms if arm.bag == bag],
        )
    summarize_stage_completion_tail("all", timing_arms)
    for bag in sorted({arm.bag for arm in timing_arms}):
        summarize_stage_completion_tail(
            f"bag{bag}",
            [arm for arm in timing_arms if arm.bag == bag],
        )
    if candidate_events:
        summarize_candidate_timeline(
            "all", candidate_events, stage_starts, timing_arms
        )
        for bag in sorted({event.bag for event in candidate_events}):
            summarize_candidate_timeline(
                f"bag{bag}",
                [event for event in candidate_events if event.bag == bag],
                stage_starts,
                [arm for arm in timing_arms if arm.bag == bag],
            )

    grouped_arms = (
        [("all", complete_arms)]
        + [
            (
                f"bag{bag}",
                [arm for arm in complete_arms if arm.bag == bag],
            )
            for bag in sorted({arm.bag for arm in complete_arms})
        ]
    )
    sentinel_by_group: dict[str, bool] = {}
    for label, group in grouped_arms:
        quiet, sentinel_cv, sentinel_drift, sentinel_core_cv, probes = (
            sentinel_diagnostic(
                group,
                args.max_sentinel_cv,
                args.max_sentinel_drift,
                args.max_core_cv,
            )
        )
        sentinel_by_group[label] = quiet
        status = "yes" if quiet else ("unknown" if probes < 6 else "no")
        print(
            "PEG_STRUCT_SENTINEL "
            f"group={label} probes={probes} "
            f"normalized_rate_cv={sentinel_cv:.4f} "
            f"late_vs_early={sentinel_drift:+.4f} "
            f"normalized_core_cv={sentinel_core_cv:.4f} quiet={status}"
        )

    for label, group in grouped_arms:
        if len(group) < 3:
            continue
        (
            fixed,
            scenario_coefficient,
            node_coefficient,
            candidate_coefficient,
            r_squared,
            active,
            prediction,
        ) = fit_nonnegative_cost(group, "wall")
        order_bias = median_order_bias(group, prediction)
        core_cv = robust_cv([arm.scheduled_cores for arm in group])
        order_known = math.isfinite(order_bias)
        quiet = (
            sentinel_by_group[label]
            and order_known
            and abs(order_bias) <= args.max_order_bias
        )
        quiet_status = (
            "yes"
            if quiet
            else ("unknown" if not order_known else "no")
        )
        greedy_rates = [
            arm.greedy_elapsed_seconds / arm.greedy_scenarios
            for arm in group
            if arm.greedy_elapsed_seconds > 0.0
            and arm.greedy_scenarios > 0
        ]
        greedy_us_per_scenario = (
            1.0e6 * statistics.median(greedy_rates)
            if greedy_rates
            else math.nan
        )
        print(
            "PEG_STRUCT_COST "
            f"group={label} n={len(group)} active={active} "
            f"post_greedy_fixed_ms={1000.0 * fixed:.4f} "
            f"greedy_us_per_scenario={greedy_us_per_scenario:.6f} "
            f"us_per_added_scenario={1.0e6 * scenario_coefficient:.6f} "
            f"ns_per_endgame_node={1.0e9 * node_coefficient:.6f} "
            f"ms_per_added_candidate={1000.0 * candidate_coefficient:.6f} "
            f"r2={r_squared:.5f} desc_vs_asc_residual={order_bias:+.4f} "
            f"scheduled_core_cv={core_cv:.4f} "
            f"quiet={quiet_status}"
        )

    if args.csv:
        args.csv.parent.mkdir(parents=True, exist_ok=True)
        with args.csv.open("w", newline="", encoding="utf-8") as stream:
            fieldnames = list(output_rows[0]) if output_rows else []
            writer = csv.DictWriter(stream, fieldnames=fieldnames)
            if fieldnames:
                writer.writeheader()
                writer.writerows(output_rows)


if __name__ == "__main__":
    main()
