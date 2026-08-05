#!/usr/bin/env python3
"""Audit regret stopping against matched-n and full-budget SIM controls."""

from __future__ import annotations

import argparse
from collections import defaultdict
import json
import math
import statistics
from dataclasses import dataclass
from pathlib import Path
from typing import Callable

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


def optional_float(fields: dict[str, str], key: str) -> float:
    value = fields.get(key)
    return float(value) if value is not None else math.nan


def optional_int(fields: dict[str, str], key: str, default: int = -1) -> int:
    value = fields.get(key)
    return int(value) if value is not None else default


@dataclass(frozen=True)
class Point:
    source_index: int
    position: int
    game: str
    bag: int
    plies: int
    target_nodes: int
    control: bool
    control_kind: str
    iterations: int
    nodes: int
    elapsed_seconds: float
    selected_rank: int
    judge_regret: float
    judge_win_regret: float
    judge_spread_regret: float
    estimated_regret: float
    joint_estimated_regret: float
    regret_at_stop: float
    joint_regret_at_stop: float
    near_tie_challengers: int
    near_tie_challengers_at_stop: int
    status: int

    @property
    def legacy_stop_estimate(self) -> float:
        if math.isfinite(self.regret_at_stop):
            return self.regret_at_stop
        return self.estimated_regret

    @property
    def joint_stop_estimate(self) -> float:
        if math.isfinite(self.joint_regret_at_stop):
            return self.joint_regret_at_stop
        return self.joint_estimated_regret

    @property
    def near_ties_at_stop(self) -> int:
        if self.near_tie_challengers_at_stop >= 0:
            return self.near_tie_challengers_at_stop
        return self.near_tie_challengers


PointKey = tuple[int, int, int]


@dataclass(frozen=True)
class CombinedPrediction:
    source_index: int
    plies: int
    predicted_candidate_regret: float
    predicted_within_regret: float
    predicted_total_regret: float
    capped: bool
    checkpoints: int


def load_points(path: Path) -> dict[PointKey, dict[str, Point]]:
    points: dict[PointKey, dict[str, Point]] = {}
    with path.open(encoding="utf-8") as stream:
        for line in stream:
            if not line.startswith("THINKING_CURVE_POINT "):
                continue
            fields = parse_fields(line)
            if fields.get("final") != "0":
                continue
            control = fields.get("control", "0") == "1"
            control_kind = fields.get(
                "control_kind", "full" if control else "stopped"
            )
            point = Point(
                source_index=int(fields["source_index"]),
                position=optional_int(
                    fields, "position", int(fields["source_index"])
                ),
                game=fields.get("game", fields["source_index"]),
                bag=int(fields["bag"]),
                plies=int(fields["plies"]),
                target_nodes=int(fields["target_nodes"]),
                control=control,
                control_kind=control_kind,
                iterations=optional_int(fields, "iterations", 0),
                nodes=int(fields["nodes"]),
                elapsed_seconds=int(fields["elapsed_ns"]) / 1e9,
                selected_rank=int(fields["selected_rank"]),
                judge_regret=float(fields["judge_regret"]),
                judge_win_regret=optional_float(fields, "judge_win_regret"),
                judge_spread_regret=optional_float(
                    fields, "judge_spread_regret"
                ),
                estimated_regret=float(fields["estimated_regret"]),
                joint_estimated_regret=optional_float(
                    fields, "joint_estimated_regret"
                ),
                regret_at_stop=optional_float(fields, "regret_at_stop"),
                joint_regret_at_stop=optional_float(
                    fields, "joint_regret_at_stop"
                ),
                near_tie_challengers=optional_int(
                    fields, "near_tie_challengers"
                ),
                near_tie_challengers_at_stop=optional_int(
                    fields, "near_tie_challengers_at_stop"
                ),
                status=int(fields["bai_status"]),
            )
            key = (point.source_index, point.plies, point.target_nodes)
            by_mode = points.setdefault(key, {})
            if point.control_kind in by_mode:
                raise ValueError(f"duplicate {point.control_kind} point for {key}")
            by_mode[point.control_kind] = point
    return points


def load_combined_predictions(
    path: Path,
) -> dict[tuple[int, int], CombinedPrediction]:
    predictions: dict[tuple[int, int], CombinedPrediction] = {}
    with path.open(encoding="utf-8") as stream:
        for line in stream:
            if not line.startswith("THINKING_CURVE_COMBINED_STOP "):
                continue
            fields = parse_fields(line)
            prediction = CombinedPrediction(
                source_index=int(fields["source_index"]),
                plies=int(fields["plies"]),
                predicted_candidate_regret=float(
                    fields["predicted_candidate_regret"]
                ),
                predicted_within_regret=float(
                    fields["predicted_within_regret"]
                ),
                predicted_total_regret=float(fields["predicted_total_regret"]),
                capped=bool(int(fields["capped"])),
                checkpoints=int(fields["checkpoints"]),
            )
            key = (prediction.source_index, prediction.plies)
            if key in predictions:
                raise ValueError(f"duplicate combined prediction for {key}")
            if (
                prediction.checkpoints <= 0
                or prediction.predicted_candidate_regret < 0.0
                or prediction.predicted_within_regret < 0.0
                or prediction.predicted_total_regret < 0.0
                or not all(
                    math.isfinite(value)
                    for value in (
                        prediction.predicted_candidate_regret,
                        prediction.predicted_within_regret,
                        prediction.predicted_total_regret,
                    )
                )
                or abs(
                    prediction.predicted_candidate_regret
                    + prediction.predicted_within_regret
                    - prediction.predicted_total_regret
                )
                > 2.0e-12
            ):
                raise ValueError(f"invalid combined prediction for {key}")
            predictions[key] = prediction
    return predictions


def pair_points(
    points: dict[PointKey, dict[str, Point]],
) -> list[tuple[Point, Point]]:
    grouped: dict[tuple[int, int], dict[str, list[Point]]] = defaultdict(
        lambda: defaultdict(list)
    )
    for by_mode in points.values():
        for point in by_mode.values():
            grouped[(point.source_index, point.plies)][point.control_kind].append(
                point
            )
    result: list[tuple[Point, Point]] = []
    for key, by_kind in grouped.items():
        full_controls = by_kind.get("full", [])
        if not full_controls:
            continue
        if len(full_controls) != 1:
            raise ValueError(f"multiple full controls for {key}")
        full = full_controls[0]
        stopped = by_kind.get("stopped", [])
        same_target = [
            point for point in stopped if point.target_nodes == full.target_nodes
        ]
        if len(same_target) == 1:
            chosen = same_target[0]
        elif len(stopped) == 1:
            # Combined-regret replay prints the observed stopping budget, while
            # the full arm retains its requested cap.
            chosen = stopped[0]
        else:
            raise ValueError(f"full control for {key} has no unique stopped arm")
        result.append((chosen, full))
    return result


def matched_points(
    points: dict[PointKey, dict[str, Point]],
) -> list[tuple[Point, Point]]:
    """Join the dynamic matched-n control to its stopped arm."""

    grouped: dict[tuple[int, int], dict[str, list[Point]]] = defaultdict(
        lambda: defaultdict(list)
    )
    for by_mode in points.values():
        for point in by_mode.values():
            grouped[(point.source_index, point.plies)][point.control_kind].append(
                point
            )
    result: list[tuple[Point, Point]] = []
    for key, by_kind in grouped.items():
        matches = by_kind.get("matched", [])
        if not matches:
            continue
        if len(matches) != 1:
            raise ValueError(f"multiple matched controls for {key}")
        matched = matches[0]
        stopped = [
            point
            for point in by_kind.get("stopped", [])
            if point.iterations == matched.iterations
        ]
        if len(stopped) != 1:
            raise ValueError(f"matched control for {key} has no unique stopped arm")
        result.append((stopped[0], matched))
    return result


@dataclass(frozen=True)
class Summary:
    mean: float
    low: float
    high: float
    p_value: float
    clusters: int


def summarize_values(values: list[float]) -> Summary:
    center = statistics.fmean(values)
    if len(values) < 2:
        return Summary(center, math.nan, math.nan, math.nan, len(values))
    standard_error = statistics.stdev(values) / math.sqrt(len(values))
    if standard_error == 0.0:
        p_value = 0.0 if center != 0.0 else 1.0
    else:
        p_value = student_t_two_sided_p_value(
            center / standard_error, len(values) - 1
        )
    radius = student_t_critical(0.95, len(values) - 1) * standard_error
    return Summary(
        center, center - radius, center + radius, p_value, len(values)
    )


def clustered_values(
    pairs: list[tuple[Point, Point]],
    metric: Callable[[Point, Point], float],
) -> list[float]:
    by_game: dict[str, list[float]] = {}
    for stopped, control in pairs:
        by_game.setdefault(stopped.game, []).append(metric(stopped, control))
    return [statistics.fmean(values) for values in by_game.values()]


def percentile(values: list[float], fraction: float) -> float:
    ordered = sorted(values)
    if len(ordered) == 1:
        return ordered[0]
    index = fraction * (len(ordered) - 1)
    low = math.floor(index)
    high = math.ceil(index)
    weight = index - low
    return ordered[low] * (1.0 - weight) + ordered[high] * weight


def format_summary(summary: Summary, signed: bool = False) -> str:
    sign = "+" if signed else ""
    return (
        f"{summary.mean:{sign}.9f} "
        f"ci95=[{summary.low:{sign}.9f},{summary.high:{sign}.9f}] "
        f"p={summary.p_value:.6g}"
    )


def summarize(
    label: str,
    pairs: list[tuple[Point, Point]],
    margin: float,
    comparison: str = "stopped_vs_full",
) -> None:
    node_fractions = [a.nodes / b.nodes for a, b in pairs]
    stopped_regret = summarize_values(
        clustered_values(pairs, lambda a, _b: a.judge_regret)
    )
    control_regret = summarize_values(
        clustered_values(pairs, lambda _a, b: b.judge_regret)
    )
    regret_delta = summarize_values(
        clustered_values(pairs, lambda a, b: a.judge_regret - b.judge_regret)
    )
    node_savings = summarize_values(
        clustered_values(pairs, lambda a, b: 1.0 - a.nodes / b.nodes)
    )
    agreement = statistics.fmean(
        float(a.selected_rank == b.selected_rank) for a, b in pairs
    )
    gate = (
        "pass"
        if regret_delta.clusters >= 2 and regret_delta.high <= margin
        else "fail"
    )
    win_delta_values = clustered_values(
        [
            pair
            for pair in pairs
            if math.isfinite(pair[0].judge_win_regret)
            and math.isfinite(pair[1].judge_win_regret)
        ],
        lambda a, b: a.judge_win_regret - b.judge_win_regret,
    )
    spread_delta_values = clustered_values(
        [
            pair
            for pair in pairs
            if math.isfinite(pair[0].judge_spread_regret)
            and math.isfinite(pair[1].judge_spread_regret)
        ],
        lambda a, b: a.judge_spread_regret - b.judge_spread_regret,
    )
    win_text = (
        format_summary(summarize_values(win_delta_values), signed=True)
        if win_delta_values
        else "unavailable"
    )
    spread_text = (
        format_summary(summarize_values(spread_delta_values), signed=True)
        if spread_delta_values
        else "unavailable"
    )
    print(
        f"REGRET_STOP_SUMMARY comparison={comparison} stratum={label} "
        f"positions={len(pairs)} "
        f"games={regret_delta.clusters} "
        f"stopped_regret={format_summary(stopped_regret)} "
        f"control_regret={format_summary(control_regret)} "
        f"paired_regret_delta={format_summary(regret_delta, signed=True)} "
        f"paired_win_regret_delta={win_text} "
        f"paired_spread_regret_delta={spread_text} "
        f"node_savings={node_savings.mean:.4%} "
        f"savings_ci95=[{node_savings.low:.4%},{node_savings.high:.4%}] "
        f"median_node_fraction={statistics.median(node_fractions):.6f} "
        f"p90_node_fraction={percentile(node_fractions, 0.90):.6f} "
        f"choice_agreement={agreement:.4%} margin={margin:.9f} "
        f"noninferiority_gate={gate}"
    )


def reliability_observations(
    label: str,
    observations: list[tuple[float, float, str]],
    stratum: str = "all",
) -> None:
    if not observations:
        print(
            f"REGRET_RELIABILITY estimator={label} stratum={stratum} positions=0"
        )
        return
    denominator = sum(predicted * predicted for predicted, _actual, _ in observations)
    slope = (
        sum(predicted * actual for predicted, actual, _ in observations)
        / denominator
        if denominator > 0.0
        else math.nan
    )
    errors_by_game: dict[str, list[float]] = {}
    for predicted, actual, game in observations:
        errors_by_game.setdefault(game, []).append(predicted - actual)
    error = summarize_values(
        [statistics.fmean(values) for values in errors_by_game.values()]
    )

    ordered = sorted(observations)
    decile_count = min(10, len(ordered))
    underpredicting_deciles = 0
    worst_ratio = 0.0
    for decile in range(decile_count):
        start = decile * len(ordered) // decile_count
        end = (decile + 1) * len(ordered) // decile_count
        group = ordered[start:end]
        predicted_mean = statistics.fmean(row[0] for row in group)
        actual_mean = statistics.fmean(row[1] for row in group)
        ratio = (
            actual_mean / predicted_mean
            if predicted_mean > 0.0
            else math.inf if actual_mean > 0.0 else 1.0
        )
        worst_ratio = max(worst_ratio, ratio)
        if ratio > 2.0:
            underpredicting_deciles += 1
    slope_gate = math.isfinite(slope) and 0.7 <= slope <= 1.4
    decile_gate = underpredicting_deciles == 0
    print(
        f"REGRET_RELIABILITY estimator={label} stratum={stratum} "
        f"positions={len(observations)} "
        f"games={len(errors_by_game)} calibration_slope={slope:.6f} "
        f"estimate_minus_judge={format_summary(error, signed=True)} "
        f"deciles={decile_count} underpredicting_deciles="
        f"{underpredicting_deciles} worst_actual_over_predicted={worst_ratio:.6g} "
        f"slope_gate={'pass' if slope_gate else 'fail'} "
        f"decile_gate={'pass' if decile_gate else 'fail'}"
    )


def reliability(
    label: str, pairs: list[tuple[Point, Point]], stratum: str = "all"
) -> None:
    estimator: Callable[[Point], float]
    estimator = (
        (lambda point: point.legacy_stop_estimate)
        if label == "legacy"
        else (lambda point: point.joint_stop_estimate)
    )
    reliability_observations(
        label,
        [
            (estimator(stopped), stopped.judge_regret, stopped.game)
            for stopped, _control in pairs
            if math.isfinite(estimator(stopped))
            and estimator(stopped) >= 0.0
            and math.isfinite(stopped.judge_regret)
        ],
        stratum,
    )


def combined_reliability(
    pairs: list[tuple[Point, Point]],
    predictions: dict[tuple[int, int], CombinedPrediction],
    stratum: str = "all",
) -> None:
    observations: list[tuple[float, float, str]] = []
    components_by_game: dict[str, list[CombinedPrediction]] = defaultdict(list)
    capped = 0
    for stopped, _control in pairs:
        prediction = predictions.get((stopped.source_index, stopped.plies))
        if prediction is None:
            continue
        observations.append(
            (prediction.predicted_total_regret, stopped.judge_regret, stopped.game)
        )
        components_by_game[stopped.game].append(prediction)
        capped += int(prediction.capped)
    reliability_observations("combined", observations, stratum)
    if not observations:
        return
    candidate = summarize_values(
        [
            statistics.fmean(
                prediction.predicted_candidate_regret for prediction in group
            )
            for group in components_by_game.values()
        ]
    )
    within = summarize_values(
        [
            statistics.fmean(
                prediction.predicted_within_regret for prediction in group
            )
            for group in components_by_game.values()
        ]
    )
    total = summarize_values(
        [
            statistics.fmean(
                prediction.predicted_total_regret for prediction in group
            )
            for group in components_by_game.values()
        ]
    )
    print(
        f"COMBINED_REGRET_COMPONENTS stratum={stratum} "
        f"positions={len(observations)} games={len(components_by_game)} "
        f"candidate={format_summary(candidate)} "
        f"within_set={format_summary(within)} total={format_summary(total)} "
        f"capped={capped} cap_rate={capped / len(observations):.4%}"
    )


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("logs", nargs="+", type=Path)
    parser.add_argument(
        "--noninferiority-margin",
        type=float,
        default=0.0,
        help="maximum allowed stopped-minus-control utility regret",
    )
    parser.add_argument(
        "--manifest",
        type=Path,
        help="optional selection manifest carrying policy and bag-band strata",
    )
    args = parser.parse_args()

    all_quality_pairs: list[tuple[Point, Point]] = []
    all_optional_pairs: list[tuple[Point, Point]] = []
    all_combined: dict[tuple[int, int], CombinedPrediction] = {}
    for path in args.logs:
        points = load_points(path)
        combined = load_combined_predictions(path)
        quality = pair_points(points)
        optional = matched_points(points)
        if not quality and not optional:
            raise ValueError(f"{path}: no stopped/control pairs")
        print(
            f"REGRET_STOP_FILE path={path} full_pairs={len(quality)} "
            f"matched_pairs={len(optional)} combined_predictions={len(combined)}"
        )
        if quality:
            summarize(
                "file", quality, args.noninferiority_margin, "stopped_vs_full"
            )
            all_quality_pairs.extend(quality)
        if optional:
            if any(stopped.iterations != control.iterations for stopped, control in optional):
                raise ValueError(f"{path}: matched controls differ in iterations")
            summarize(
                "file",
                optional,
                args.noninferiority_margin,
                "stopped_vs_matched_n",
            )
            all_optional_pairs.extend(optional)
        if combined:
            calibration = optional or quality
            expected_keys = {
                (stopped.source_index, stopped.plies)
                for stopped, _control in calibration
            }
            if set(combined) != expected_keys:
                raise ValueError(
                    f"{path}: combined predictions do not map calibration roots"
                )
            combined_reliability(calibration, combined, "file")
            overlap = set(all_combined) & set(combined)
            if overlap:
                raise ValueError(f"duplicate combined roots across logs: {overlap}")
            all_combined.update(combined)

    if all_quality_pairs:
        summarize(
            "all",
            all_quality_pairs,
            args.noninferiority_margin,
            "stopped_vs_full",
        )
    if all_optional_pairs:
        summarize(
            "all",
            all_optional_pairs,
            args.noninferiority_margin,
            "stopped_vs_matched_n",
        )
    calibration_pairs = all_optional_pairs or all_quality_pairs
    reliability("legacy", calibration_pairs)
    reliability("joint", calibration_pairs)
    if all_combined:
        combined_reliability(calibration_pairs, all_combined)
    for label, low, high in (
        ("opening", 61, 100),
        ("midgame", 25, 60),
        ("preendgame", 1, 24),
    ):
        stratum = [
            pair for pair in calibration_pairs if low <= pair[0].bag <= high
        ]
        if len({pair[0].game for pair in stratum}) >= 2:
            summarize(label, stratum, args.noninferiority_margin, "calibration")
            reliability("legacy", stratum, label)
            reliability("joint", stratum, label)
            if all_combined:
                combined_reliability(stratum, all_combined, label)
    for label, predicate in (
        ("ties0", lambda value: value == 0),
        ("ties1", lambda value: value == 1),
        ("ties2_3", lambda value: 2 <= value <= 3),
        ("ties4plus", lambda value: value >= 4),
    ):
        stratum = [
            pair
            for pair in calibration_pairs
            if predicate(pair[0].near_ties_at_stop)
        ]
        if len({pair[0].game for pair in stratum}) >= 2:
            summarize(label, stratum, args.noninferiority_margin, "calibration")
            reliability("legacy", stratum, label)
            reliability("joint", stratum, label)
            if all_combined:
                combined_reliability(stratum, all_combined, label)

    if args.manifest is not None:
        document = json.loads(args.manifest.read_text(encoding="utf-8"))
        metadata = {
            int(row["position"]): (
                str(row["trajectory_policy"]),
                str(row["bag_band"]),
            )
            for row in document.get("mapping", [])
        }
        if any(pair[0].source_index not in metadata for pair in calibration_pairs):
            raise ValueError("manifest does not map every calibration source")
        groups: dict[str, list[tuple[Point, Point]]] = defaultdict(list)
        for pair in calibration_pairs:
            policy, bag_band = metadata[pair[0].source_index]
            groups[f"policy:{policy}"].append(pair)
            groups[f"bag:{bag_band}"].append(pair)
        for label in sorted(groups):
            stratum = groups[label]
            if len({pair[0].game for pair in stratum}) < 2:
                continue
            summarize(label, stratum, args.noninferiority_margin, "calibration")
            reliability("legacy", stratum, label)
            reliability("joint", stratum, label)
            if all_combined:
                combined_reliability(stratum, all_combined, label)


if __name__ == "__main__":
    main()
