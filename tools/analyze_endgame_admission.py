#!/usr/bin/env python3
"""Fit and audit conservative next-depth endgame work bounds."""

from __future__ import annotations

import argparse
import json
import math
import statistics
from dataclasses import dataclass
from pathlib import Path


FEATURE_NAMES = (
    "intercept",
    "rack_tiles_minus_13",
    "log_previous_nodes",
    "log_last_increment_nodes",
    "last_increment_missing",
    "log_prior_increment_nodes",
    "prior_increment_missing",
    "log_root_moves",
    "log_ply2_moves",
    "log_root_per_ply2",
    "log_previous_nodes_x_log_root_per_ply2",
    "log_last_increment_x_log_root_per_ply2",
    "player_rack_tiles",
    "opponent_rack_tiles",
    "score_spread_div_100",
    "absolute_score_spread_div_100",
    "consecutive_scoreless_turns",
    "position_features_missing",
    "best_move_changed",
    "search_value_changed",
)


@dataclass(frozen=True)
class Point:
    depth: int
    nodes: int
    elapsed_seconds: float
    root_total: int
    ply2_total: int
    tiny_move: int
    search_value: int


@dataclass
class Position:
    corpus: int
    position: int
    rack_tiles: int = 0
    player_rack_tiles: int = 0
    opponent_rack_tiles: int = 0
    score_spread: int = 0
    consecutive_scoreless_turns: int = 0
    has_cgp_features: bool = False
    points: dict[int, Point] | None = None
    censor_depth: int = 0
    censor_lower_added_nodes: int = 0
    censor_total_elapsed_seconds: float = 0.0

    def __post_init__(self) -> None:
        if self.points is None:
            self.points = {}


@dataclass(frozen=True)
class Observation:
    corpus: int
    position: int
    target_depth: int
    features: tuple[float, ...]
    added_nodes: int | None
    censor_lower_added_nodes: int
    added_seconds: float | None
    prior_increment_seconds: float | None
    prior_cumulative_seconds: float
    censor_lower_added_seconds: float


def parse_fields(line: str) -> dict[str, str]:
    fields: dict[str, str] = {}
    for token in line.split()[1:]:
        if "=" in token:
            key, value = token.split("=", 1)
            fields[key] = value
    return fields


def fraction_denominator(value: str) -> int:
    return int(value.split("/", 1)[1])


def load_corpus(corpus: int, paths: list[Path]) -> list[Position]:
    attempts: list[Position] = []
    for path in paths:
        # A tail rerun is a fresh ABDADA attempt, not a continuation of the
        # censored search. Keep attempts separate here; observations_for
        # conservatively retains the largest observed work per source
        # position instead of allowing a cheaper rerun to erase a censor.
        positions: dict[int, Position] = {}
        with path.open(encoding="utf-8") as stream:
            for line in stream:
                if line.startswith(("EGCURVEPOS ", "EGCURVETRUNC ",
                                    "EGADMITPOS ")):
                    fields = parse_fields(line)
                    position = int(fields["pos"])
                    item = positions.setdefault(
                        position, Position(corpus, position)
                    )
                    item.rack_tiles = int(fields["rack_tiles"])
                    if line.startswith("EGADMITPOS "):
                        item.censor_total_elapsed_seconds = (
                            int(fields["elapsed_ns"]) / 1.0e9
                        )
                    if (
                        line.startswith("EGADMITPOS ")
                        and fields.get("censored") == "0"
                    ):
                        item.censor_depth = 0
                        item.censor_lower_added_nodes = 0
                elif line.startswith(("EGCURVEPOINT ", "EGADMITPOINT ")):
                    fields = parse_fields(line)
                    position = int(fields["pos"])
                    item = positions.setdefault(
                        position, Position(corpus, position)
                    )
                    point = Point(
                        depth=int(fields["depth"]),
                        nodes=int(fields["nodes"]),
                        elapsed_seconds=int(fields["elapsed_ns"]) / 1.0e9,
                        root_total=fraction_denominator(fields["root"]),
                        ply2_total=fraction_denominator(fields["ply2"]),
                        tiny_move=int(fields["tiny"]),
                        search_value=int(fields["search"]),
                    )
                    item.points[point.depth] = point
                elif line.startswith("EGADMITCENSOR "):
                    fields = parse_fields(line)
                    position = int(fields["pos"])
                    item = positions.setdefault(
                        position, Position(corpus, position)
                    )
                    item.censor_depth = int(fields["target_depth"])
                    item.censor_lower_added_nodes = int(
                        fields["lower_added_nodes"]
                    )
        attempts.extend(positions.values())
    return attempts


def parse_range(value: str) -> tuple[int | None, int | None]:
    start_text, end_text = value.split(":", 1)
    return (
        int(start_text) if start_text else None,
        int(end_text) if end_text else None,
    )


def filter_positions(
    positions: list[Position], selected_range: tuple[int | None, int | None]
) -> list[Position]:
    start, end = selected_range
    return [
        position
        for position in positions
        if (start is None or position.position >= start)
        and (end is None or position.position < end)
    ]


def add_cgp_features(positions: list[Position], path: Path) -> None:
    by_position: dict[int, list[Position]] = {}
    for position in positions:
        by_position.setdefault(position.position, []).append(position)
    with path.open(encoding="utf-8") as stream:
        for position_index, line in enumerate(stream):
            matching_positions = by_position.get(position_index, [])
            if not matching_positions:
                continue
            fields = line.split()
            if len(fields) < 4:
                raise ValueError(f"malformed CGP at {path}:{position_index + 1}")
            racks = fields[1].split("/")
            scores = fields[2].split("/")
            if len(racks) != 2 or len(scores) != 2:
                raise ValueError(f"malformed CGP at {path}:{position_index + 1}")
            for position in matching_positions:
                position.player_rack_tiles = len(racks[0])
                position.opponent_rack_tiles = len(racks[1])
                position.score_spread = int(scores[0]) - int(scores[1])
                position.consecutive_scoreless_turns = int(fields[3])
                position.has_cgp_features = True


def safe_increment(newer: Point, older: Point | None) -> int | None:
    if older is None or newer.nodes <= older.nodes:
        return None
    return newer.nodes - older.nodes


def observation_for(
    position: Position, target_depth: int
) -> Observation | None:
    previous = position.points.get(target_depth - 1)
    if previous is None:
        return None
    current = position.points.get(target_depth)
    previous2 = position.points.get(target_depth - 2)
    previous3 = position.points.get(target_depth - 3)
    last_increment = safe_increment(previous, previous2)
    prior_increment = (
        safe_increment(previous2, previous3) if previous2 else None
    )
    move_changed = (
        previous2 is not None
        and previous.tiny_move != previous2.tiny_move
    )
    value_changed = (
        previous2 is not None
        and previous.search_value != previous2.search_value
    )
    log_previous_nodes = math.log1p(previous.nodes)
    log_last_increment = math.log1p(
        last_increment or previous.nodes
    )
    log_prior_increment = math.log1p(
        prior_increment or (last_increment or previous.nodes)
    )
    log_root_total = math.log1p(previous.root_total)
    log_ply2_total = math.log1p(previous.ply2_total)
    log_root_per_ply2 = math.log(
        (previous.root_total + 1) / (previous.ply2_total + 1)
    )
    features = (
        1.0,
        float(position.rack_tiles - 13),
        log_previous_nodes,
        log_last_increment,
        1.0 if last_increment is None else 0.0,
        log_prior_increment,
        1.0 if prior_increment is None else 0.0,
        log_root_total,
        log_ply2_total,
        log_root_per_ply2,
        log_previous_nodes * log_root_per_ply2,
        log_last_increment * log_root_per_ply2,
        float(position.player_rack_tiles),
        float(position.opponent_rack_tiles),
        position.score_spread / 100.0,
        abs(position.score_spread) / 100.0,
        float(position.consecutive_scoreless_turns),
        0.0 if position.has_cgp_features else 1.0,
        1.0 if move_changed else 0.0,
        1.0 if value_changed else 0.0,
    )
    added_nodes = (
        current.nodes - previous.nodes
        if current is not None and current.nodes > previous.nodes
        else None
    )
    censor_lower = (
        position.censor_lower_added_nodes
        if position.censor_depth == target_depth
        else 0
    )
    if added_nodes is None and censor_lower == 0:
        return None
    added_seconds = (
        current.elapsed_seconds - previous.elapsed_seconds
        if current is not None
        and current.elapsed_seconds > previous.elapsed_seconds
        else None
    )
    prior_increment_seconds = (
        previous.elapsed_seconds - previous2.elapsed_seconds
        if previous2 is not None
        and previous.elapsed_seconds > previous2.elapsed_seconds
        else None
    )
    censor_lower_seconds = (
        position.censor_total_elapsed_seconds - previous.elapsed_seconds
        if censor_lower > 0
        and position.censor_total_elapsed_seconds > previous.elapsed_seconds
        else 0.0
    )
    return Observation(
        corpus=position.corpus,
        position=position.position,
        target_depth=target_depth,
        features=features,
        added_nodes=added_nodes,
        censor_lower_added_nodes=censor_lower,
        added_seconds=added_seconds,
        prior_increment_seconds=prior_increment_seconds,
        prior_cumulative_seconds=previous.elapsed_seconds,
        censor_lower_added_seconds=censor_lower_seconds,
    )


def observations_for(
    corpora: list[list[Position]], target_depth: int
) -> list[Observation]:
    output: list[Observation] = []
    for corpus in corpora:
        worst_by_position: dict[int, Observation] = {}
        for position in corpus:
            observation = observation_for(position, target_depth)
            if observation is None:
                continue
            observed_work = (
                observation.added_nodes
                if observation.added_nodes is not None
                else observation.censor_lower_added_nodes
            )
            previous = worst_by_position.get(position.position)
            previous_work = (
                previous.added_nodes
                if previous is not None and previous.added_nodes is not None
                else previous.censor_lower_added_nodes
                if previous is not None
                else -1
            )
            if observed_work > previous_work:
                worst_by_position[position.position] = observation
        output.extend(worst_by_position.values())
    return output


def solve(matrix: list[list[float]], vector: list[float]) -> list[float]:
    size = len(vector)
    augmented = [
        matrix[row][:] + [vector[row]] for row in range(size)
    ]
    for column in range(size):
        pivot = max(
            range(column, size),
            key=lambda row: abs(augmented[row][column]),
        )
        if abs(augmented[pivot][column]) < 1.0e-12:
            raise ValueError("singular admission model")
        augmented[column], augmented[pivot] = (
            augmented[pivot],
            augmented[column],
        )
        divisor = augmented[column][column]
        augmented[column] = [
            value / divisor for value in augmented[column]
        ]
        for row in range(size):
            if row == column:
                continue
            factor = augmented[row][column]
            augmented[row] = [
                augmented[row][index]
                - factor * augmented[column][index]
                for index in range(size + 1)
            ]
    return [augmented[row][-1] for row in range(size)]


@dataclass(frozen=True)
class RidgeModel:
    means: tuple[float, ...]
    scales: tuple[float, ...]
    coefficients: tuple[float, ...]
    target_ratio: bool

    def predict_log(self, features: tuple[float, ...]) -> float:
        standardized = [1.0]
        for index in range(1, len(features)):
            standardized.append(
                (features[index] - self.means[index])
                / self.scales[index]
            )
        prediction = sum(
            coefficient * value
            for coefficient, value in zip(
                self.coefficients, standardized, strict=True
            )
        )
        return prediction + (features[2] if self.target_ratio else 0.0)


@dataclass(frozen=True)
class KnnModel:
    means: tuple[float, ...]
    scales: tuple[float, ...]
    rows: tuple[tuple[float, ...], ...]
    targets: tuple[float, ...]
    neighbors: int
    target_quantile: float
    target_ratio: bool

    def predict_log(self, features: tuple[float, ...]) -> float:
        row = tuple(
            (features[index] - self.means[index]) / self.scales[index]
            for index in KNN_FEATURES
        )
        distances = sorted(
            (
                sum(
                    (left - right) ** 2
                    for left, right in zip(
                        row, training_row, strict=True
                    )
                ),
                target,
            )
            for training_row, target in zip(
                self.rows, self.targets, strict=True
            )
        )
        local_targets = [
            target
            for _, target in distances[: min(self.neighbors, len(distances))]
        ]
        prediction = percentile(local_targets, self.target_quantile)
        return prediction + (features[2] if self.target_ratio else 0.0)


KNN_FEATURES = (1, 2, 3, 5, 7, 8, 9, 12, 13, 14, 15, 16, 18, 19)


def fit_ridge(
    observations: list[Observation], penalty: float, target_ratio: bool
) -> RidgeModel:
    observed = [
        observation
        for observation in observations
        if observation.added_nodes is not None
        or observation.censor_lower_added_nodes > 0
    ]
    if not observed:
        raise ValueError("no admission observations")
    width = len(observed[0].features)
    means = [0.0] * width
    scales = [1.0] * width
    for column in range(1, width):
        values = [
            observation.features[column] for observation in observed
        ]
        means[column] = statistics.fmean(values)
        scale = statistics.stdev(values) if len(values) > 1 else 1.0
        scales[column] = scale if scale > 1.0e-9 else 1.0
    rows: list[list[float]] = []
    targets: list[float] = []
    for observation in observed:
        row = [1.0]
        for column in range(1, width):
            row.append(
                (observation.features[column] - means[column])
                / scales[column]
            )
        rows.append(row)
        target = math.log(
            observation.added_nodes
            or observation.censor_lower_added_nodes
        )
        if target_ratio:
            target -= observation.features[2]
        targets.append(target)
    gram = [[0.0 for _ in range(width)] for _ in range(width)]
    rhs = [0.0 for _ in range(width)]
    for row, target in zip(rows, targets, strict=True):
        for left in range(width):
            rhs[left] += row[left] * target
            for right in range(width):
                gram[left][right] += row[left] * row[right]
    for index in range(1, width):
        gram[index][index] += penalty
    return RidgeModel(
        means=tuple(means),
        scales=tuple(scales),
        coefficients=tuple(solve(gram, rhs)),
        target_ratio=target_ratio,
    )


def fit_knn(
    observations: list[Observation],
    neighbors: int,
    target_quantile: float,
    target_ratio: bool,
) -> KnnModel:
    observed = [
        observation
        for observation in observations
        if observation.added_nodes is not None
        or observation.censor_lower_added_nodes > 0
    ]
    if not observed:
        raise ValueError("no admission observations")
    width = len(observed[0].features)
    means = [0.0] * width
    scales = [1.0] * width
    for column in KNN_FEATURES:
        values = [
            observation.features[column] for observation in observed
        ]
        means[column] = statistics.fmean(values)
        scale = statistics.stdev(values) if len(values) > 1 else 1.0
        scales[column] = scale if scale > 1.0e-9 else 1.0
    rows = tuple(
        tuple(
            (observation.features[column] - means[column])
            / scales[column]
            for column in KNN_FEATURES
        )
        for observation in observed
    )
    targets = []
    for observation in observed:
        target = math.log(
            observation.added_nodes
            or observation.censor_lower_added_nodes
        )
        if target_ratio:
            target -= observation.features[2]
        targets.append(target)
    return KnnModel(
        means=tuple(means),
        scales=tuple(scales),
        rows=rows,
        targets=tuple(targets),
        neighbors=neighbors,
        target_quantile=target_quantile,
        target_ratio=target_ratio,
    )


def fit_model(
    observations: list[Observation],
    model_name: str,
    penalty: float,
    neighbors: int,
    neighbor_quantile: float,
    target_ratio: bool,
) -> RidgeModel | KnnModel:
    if model_name == "ridge":
        return fit_ridge(observations, penalty, target_ratio)
    if model_name == "knn":
        return fit_knn(
            observations, neighbors, neighbor_quantile, target_ratio
        )
    raise ValueError(f"unknown admission model: {model_name}")


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


def conformal_quantile(scores: list[float], confidence: float) -> float:
    ordered = sorted(scores)
    rank = math.ceil((len(ordered) + 1) * confidence)
    if rank > len(ordered):
        return math.inf
    return ordered[rank - 1]


def prior_nps(observation: Observation) -> float | None:
    if (
        observation.prior_increment_seconds is not None
        and observation.features[4] == 0.0
    ):
        prior_increment_nodes = math.expm1(observation.features[3])
        if prior_increment_nodes > 0.0:
            return (
                prior_increment_nodes
                / observation.prior_increment_seconds
            )
    prior_nodes = math.expm1(observation.features[2])
    if prior_nodes <= 0.0 or observation.prior_cumulative_seconds <= 0.0:
        return None
    return prior_nodes / observation.prior_cumulative_seconds


def predicted_log_cost(
    model: RidgeModel | KnnModel,
    observation: Observation,
    metric: str,
) -> float | None:
    predicted_log_nodes = model.predict_log(observation.features)
    if metric == "nodes":
        return predicted_log_nodes
    live_nps = prior_nps(observation)
    if live_nps is None or live_nps <= 0.0:
        return None
    return predicted_log_nodes - math.log(live_nps)


def predicted_cost_and_bound(
    model: RidgeModel | KnnModel,
    observation: Observation,
    correction: float,
    metric: str,
    prior_node_floors: dict[int, float],
) -> tuple[float, float] | None:
    predicted = predicted_log_cost(model, observation, metric)
    if predicted is None:
        return None
    expected = math.exp(predicted)
    bound = expected * math.exp(correction)
    floor_multiplier = prior_node_floors.get(observation.target_depth, 0.0)
    if metric == "nodes" and floor_multiplier > 0.0:
        bound = max(
            bound,
            floor_multiplier * math.expm1(observation.features[2]),
        )
    return expected, bound


def admission_score(
    model: RidgeModel | KnnModel,
    observation: Observation,
    metric: str,
) -> tuple[float | None, float | None]:
    predicted = predicted_log_cost(model, observation, metric)
    if predicted is None:
        return None, None
    if metric == "nodes":
        actual = (
            float(observation.added_nodes)
            if observation.added_nodes is not None
            else None
        )
        censor_lower = float(observation.censor_lower_added_nodes)
    else:
        actual = observation.added_seconds
        censor_lower = observation.censor_lower_added_seconds
    if actual is not None and actual > 0.0:
        return math.log(actual) - predicted, None
    if censor_lower > 0.0:
        return None, math.log(censor_lower) - predicted
    return None, None


def cross_fitted_scores(
    corpora: list[list[Position]],
    target_depth: int,
    model_name: str,
    penalty: float,
    neighbors: int,
    neighbor_quantile: float,
    target_ratio: bool,
    metric: str,
) -> tuple[list[float], list[float]]:
    scores: list[float] = []
    censored_lower_scores: list[float] = []
    for heldout_index in range(len(corpora)):
        training = observations_for(
            [
                corpus
                for index, corpus in enumerate(corpora)
                if index != heldout_index
            ],
            target_depth,
        )
        heldout = observations_for([corpora[heldout_index]], target_depth)
        model = fit_model(
            training,
            model_name,
            penalty,
            neighbors,
            neighbor_quantile,
            target_ratio,
        )
        for observation in heldout:
            score, censored_score = admission_score(
                model, observation, metric
            )
            if score is not None:
                scores.append(score)
            elif censored_score is not None:
                censored_lower_scores.append(censored_score)
    return scores, censored_lower_scores


def calibration_scores(
    model: RidgeModel | KnnModel,
    observations: list[Observation],
    metric: str,
) -> tuple[list[float], list[float]]:
    scores: list[float] = []
    censored_lower_scores: list[float] = []
    for observation in observations:
        score, censored_score = admission_score(
            model, observation, metric
        )
        if score is not None:
            scores.append(score)
        elif censored_score is not None:
            censored_lower_scores.append(censored_score)
    return scores, censored_lower_scores


def censored_conformal_quantile(
    complete_scores: list[float],
    censored_lower_scores: list[float],
    confidence: float,
) -> tuple[float, float]:
    sample_size = len(complete_scores) + len(censored_lower_scores)
    rank = math.ceil((sample_size + 1) * confidence)
    # For a distribution-free upper bound, every right-censored score may be
    # arbitrarily large. Place them at +infinity. If the requested order
    # statistic reaches that tail, no finite bound is justified.
    conservative = (
        sorted(complete_scores)[rank - 1]
        if rank <= len(complete_scores)
        else math.inf
    )
    # The lower-bound diagnostic is deliberately optimistic. It shows the
    # smallest correction compatible with work already observed in censored
    # searches, but must never be advertised as calibrated coverage.
    optimistic = conformal_quantile(
        complete_scores + censored_lower_scores, confidence
    )
    return conservative, optimistic


def format_quantiles(values: list[float]) -> str:
    return "/".join(
        f"{percentile(values, probability):.3f}"
        for probability in (0.50, 0.90, 0.95, 0.99)
    )


def report_rate_diagnostics(
    target_depth: int, observations: list[Observation]
) -> None:
    ratios: list[float] = []
    for observation in observations:
        if (
            observation.added_nodes is None
            or observation.added_seconds is None
            or observation.prior_increment_seconds is None
            or observation.features[4] != 0.0
        ):
            continue
        prior_increment_nodes = math.expm1(observation.features[3])
        prior_nps = (
            prior_increment_nodes / observation.prior_increment_seconds
        )
        target_nps = observation.added_nodes / observation.added_seconds
        if prior_nps > 0.0 and target_nps > 0.0:
            ratios.append(target_nps / prior_nps)
    print(
        "EGADMISSION_RATE "
        f"depth={target_depth} n={len(ratios)} "
        f"next_vs_prior_nps01/05/10/50/90/95/99="
        + "/".join(
            f"{percentile(ratios, probability):.3f}"
            for probability in (0.01, 0.05, 0.10, 0.50, 0.90, 0.95, 0.99)
        )
    )


def evaluate(
    model: RidgeModel | KnnModel,
    validation: list[Observation],
    correction: float,
    metric: str,
    prior_node_floors: dict[int, float],
) -> tuple[int, int, int, int, int, list[float], list[float]]:
    complete = 0
    failures = 0
    definite_censored_failures = 0
    indeterminate_censors = 0
    unscored = 0
    ratios: list[float] = []
    expected_ratios: list[float] = []
    for observation in validation:
        prediction = predicted_cost_and_bound(
            model,
            observation,
            correction,
            metric,
            prior_node_floors,
        )
        if prediction is None:
            unscored += 1
            continue
        expected, bound = prediction
        actual = (
            float(observation.added_nodes)
            if metric == "nodes" and observation.added_nodes is not None
            else observation.added_seconds
            if metric == "wall"
            else None
        )
        censor_lower = (
            float(observation.censor_lower_added_nodes)
            if metric == "nodes"
            else observation.censor_lower_added_seconds
        )
        if actual is not None:
            complete += 1
            if actual > bound:
                failures += 1
            ratios.append(bound / actual)
            expected_ratios.append(expected / actual)
        elif censor_lower > bound:
            definite_censored_failures += 1
        else:
            indeterminate_censors += 1
    return (
        complete,
        failures,
        definite_censored_failures,
        indeterminate_censors,
        unscored,
        ratios,
        expected_ratios,
    )


def binomial_failure_upper_bound(
    failures: int, trials: int, confidence: float
) -> float:
    """One-sided Clopper-Pearson upper bound for a failure probability."""
    if trials <= 0 or failures < 0 or failures > trials:
        return math.nan
    if failures == trials:
        return 1.0
    alpha = 1.0 - confidence
    if failures == 0:
        return 1.0 - alpha ** (1.0 / trials)

    def cdf(probability: float) -> float:
        return sum(
            math.comb(trials, count)
            * probability**count
            * (1.0 - probability) ** (trials - count)
            for count in range(failures + 1)
        )

    low = 0.0
    high = 1.0
    for _ in range(80):
        midpoint = (low + high) / 2.0
        if cdf(midpoint) > alpha:
            low = midpoint
        else:
            high = midpoint
    return high


def report_budget_classification(
    target_depth: int,
    model: RidgeModel | KnnModel,
    validation: list[Observation],
    correction: float,
    metric: str,
    prior_node_floors: dict[int, float],
) -> None:
    budgets = (
        (
            100000.0,
            300000.0,
            1.0e6,
            3.0e6,
            1.0e7,
            3.0e7,
            1.0e8,
            3.0e8,
            1.0e9,
            1.5e9,
            2.0e9,
            3.0e9,
        )
        if metric == "nodes"
        else (
            0.1,
            0.3,
            1.0,
            3.0,
            10.0,
            30.0,
            100.0,
            300.0,
            600.0,
            900.0,
            1200.0,
            1800.0,
        )
    )
    for budget in budgets:
        admitted = 0
        fits = 0
        false_starts = 0
        false_rejections = 0
        indeterminate_censored = 0
        for observation in validation:
            prediction = predicted_cost_and_bound(
                model,
                observation,
                correction,
                metric,
                prior_node_floors,
            )
            if prediction is None:
                continue
            _, bound = prediction
            admit = bound <= budget
            actual = (
                float(observation.added_nodes)
                if metric == "nodes" and observation.added_nodes is not None
                else observation.added_seconds
                if metric == "wall"
                else None
            )
            censor_lower = (
                float(observation.censor_lower_added_nodes)
                if metric == "nodes"
                else observation.censor_lower_added_seconds
            )
            if admit:
                admitted += 1
            if actual is not None:
                fit = actual <= budget
                if fit:
                    fits += 1
                if admit and not fit:
                    false_starts += 1
                if not admit and fit:
                    false_rejections += 1
            elif censor_lower > budget:
                if admit:
                    false_starts += 1
            else:
                indeterminate_censored += 1
        print(
            "EGADMISSION_BUDGET "
            f"depth={target_depth} metric={metric} budget={budget:.3f} "
            f"prior_node_floor="
            f"{prior_node_floors.get(target_depth, 0.0):.3f} "
            f"admitted={admitted} fits={fits} "
            f"false_starts={false_starts} "
            f"false_rejections={false_rejections} "
            f"indeterminate_censored={indeterminate_censored}"
        )


def report_validation_failures(
    target_depth: int,
    model: RidgeModel | KnnModel,
    validation: list[Observation],
    correction: float,
    metric: str,
    prior_node_floors: dict[int, float],
) -> None:
    for observation in validation:
        prediction = predicted_cost_and_bound(
            model,
            observation,
            correction,
            metric,
            prior_node_floors,
        )
        if prediction is None:
            continue
        expected, bound = prediction
        actual = (
            float(observation.added_nodes)
            if metric == "nodes" and observation.added_nodes is not None
            else observation.added_seconds
            if metric == "wall"
            else None
        )
        censor_lower = (
            float(observation.censor_lower_added_nodes)
            if metric == "nodes"
            else observation.censor_lower_added_seconds
        )
        if actual is not None and actual <= bound:
            continue
        if actual is None and censor_lower <= bound:
            continue
        observed = actual if actual is not None else censor_lower
        print(
            "EGADMISSION_FAILURE "
            f"depth={target_depth} metric={metric} "
            f"corpus={observation.corpus} pos={observation.position} "
            f"kind={'complete' if actual is not None else 'censored_lower'} "
            f"expected={expected:.3f} bound={bound:.3f} "
            f"observed={observed:.3f} "
            f"observed_over_bound={observed / bound:.3f} "
            f"prior_nodes={math.expm1(observation.features[2]):.0f} "
            f"last_increment={math.expm1(observation.features[3]):.0f} "
            f"root={math.expm1(observation.features[7]):.0f} "
            f"ply2={math.expm1(observation.features[8]):.0f} "
            f"racks={observation.features[12]:.0f}/"
            f"{observation.features[13]:.0f} "
            f"spread={observation.features[14] * 100.0:.0f} "
            f"move_changed={observation.features[18]:.0f} "
            f"value_changed={observation.features[19]:.0f}"
        )


def parse_corpus_argument(value: str) -> list[Path]:
    return [Path(item) for item in value.split(",") if item]


def parse_prior_node_floors(values: list[str] | None) -> dict[int, float]:
    floors: dict[int, float] = {}
    for value in values or []:
        depth_text, multiplier_text = value.split(":", 1)
        depth = int(depth_text)
        multiplier = float(multiplier_text)
        if depth <= 0 or not math.isfinite(multiplier) or multiplier < 0.0:
            raise ValueError("prior-node floors must be DEPTH:MULTIPLIER")
        floors[depth] = multiplier
    return floors


def format_c_double(value: float) -> str:
    if not math.isfinite(value):
        raise ValueError("runtime admission model contains a nonfinite value")
    return f"{value:.17g}"


def format_c_array(values: tuple[float, ...]) -> str:
    return ", ".join(format_c_double(value) for value in values)


def write_c_model(
    path: Path,
    artifact_id: str,
    workers: int,
    tt_fraction_of_mem: float,
    use_heuristics: bool,
    minimum_enforceable_depth: int,
    confidence: float,
    depth_models: list[tuple[int, RidgeModel, float, float]],
) -> None:
    if not depth_models:
        raise ValueError("no depth models to export")
    if workers <= 0:
        raise ValueError("artifact workers must be positive")
    if minimum_enforceable_depth <= 0:
        raise ValueError("minimum enforceable depth must be positive")
    if (
        not math.isfinite(tt_fraction_of_mem)
        or tt_fraction_of_mem < 0.0
        or tt_fraction_of_mem > 1.0
    ):
        raise ValueError("artifact TT fraction must be in [0, 1]")
    quoted_artifact_id = json.dumps(artifact_id, ensure_ascii=True)
    lines = [
        "/*",
        " * Generated by tools/analyze_endgame_admission.py.",
        " * Do not edit coefficients by hand; regenerate from the frozen corpus.",
        " */",
        "#ifndef ENDGAME_ADMISSION_MODEL_DEFAULT_DATA_H",
        "#define ENDGAME_ADMISSION_MODEL_DEFAULT_DATA_H",
        "",
        "static const EndgameAdmissionDepthModel",
        "    ENDGAME_ADMISSION_DEFAULT_DEPTH_MODELS[] = {",
    ]
    for target_depth, model, node_correction, wall_correction in depth_models:
        if len(model.means) != len(FEATURE_NAMES):
            raise ValueError("unexpected admission feature count")
        lines.extend(
            [
                "        {",
                f"            .target_depth = {target_depth},",
                "            .enforceable = "
                f"{'true' if target_depth >= minimum_enforceable_depth else 'false'},",
                "            .means = {",
                f"                {format_c_array(model.means)},",
                "            },",
                "            .scales = {",
                f"                {format_c_array(model.scales)},",
                "            },",
                "            .coefficients = {",
                f"                {format_c_array(model.coefficients)},",
                "            },",
                "            .node_correction_multiplier = "
                f"{format_c_double(math.exp(node_correction))},",
                "            .wall_correction_multiplier = "
                f"{format_c_double(math.exp(wall_correction))},",
                "        },",
            ]
        )
    lines.extend(
        [
            "};",
            "",
            "static const EndgameAdmissionModel ENDGAME_ADMISSION_DEFAULT_MODEL = {",
            "    .artifact_version = ENDGAME_ADMISSION_MODEL_ARTIFACT_VERSION,",
            "    .feature_schema_version = "
            "ENDGAME_ADMISSION_FEATURE_SCHEMA_VERSION,",
            "    .source_progress_schema_version = "
            "ANALYSIS_PROGRESS_SCHEMA_VERSION,",
            f"    .artifact_id = {quoted_artifact_id},",
            f"    .workers = {workers},",
            f"    .tt_fraction_of_mem = {format_c_double(tt_fraction_of_mem)},",
            f"    .use_heuristics = {'true' if use_heuristics else 'false'},",
            "    .target_ratio = true,",
            f"    .completion_confidence = {format_c_double(confidence)},",
            "    .depth_model_count =",
            "        sizeof(ENDGAME_ADMISSION_DEFAULT_DEPTH_MODELS) /",
            "        sizeof(ENDGAME_ADMISSION_DEFAULT_DEPTH_MODELS[0]),",
            "    .depth_models = ENDGAME_ADMISSION_DEFAULT_DEPTH_MODELS,",
            "};",
            "",
            "#endif",
            "",
        ]
    )
    path.write_text("\n".join(lines), encoding="utf-8")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--fit-corpus",
        action="append",
        required=True,
        help="comma-separated logs for one independent corpus; repeat",
    )
    parser.add_argument(
        "--fit-cgp",
        action="append",
        help="CGP file corresponding to each --fit-corpus",
    )
    parser.add_argument(
        "--fit-range",
        action="append",
        help="half-open position range corresponding to each fit corpus",
    )
    parser.add_argument(
        "--calibration-corpus",
        action="append",
        help="explicit held-out calibration logs; disables cross-fitting",
    )
    parser.add_argument(
        "--calibration-cgp",
        action="append",
        help="CGP file corresponding to each calibration corpus",
    )
    parser.add_argument(
        "--calibration-range",
        action="append",
        help="half-open position range for each calibration corpus",
    )
    parser.add_argument(
        "--validate",
        help="comma-separated logs for one validation corpus",
    )
    parser.add_argument("--validate-cgp", type=Path)
    parser.add_argument("--depths", default="4,5")
    parser.add_argument("--penalty", type=float, default=1.0)
    parser.add_argument("--model", choices=("ridge", "knn"), default="ridge")
    parser.add_argument(
        "--target", choices=("absolute", "ratio"), default="ratio"
    )
    parser.add_argument(
        "--metric", choices=("nodes", "wall"), default="nodes"
    )
    parser.add_argument("--neighbors", type=int, default=24)
    parser.add_argument("--neighbor-quantile", type=float, default=0.75)
    parser.add_argument("--confidence", type=float, default=0.99)
    parser.add_argument(
        "--prior-node-floor",
        action="append",
        help=(
            "post-calibration safety floor as DEPTH:MULTIPLIER; "
            "bound is at least multiplier times prior cumulative nodes"
        ),
    )
    parser.add_argument(
        "--export-c-model",
        type=Path,
        help=(
            "write the calibrated ridge node model and joint wall correction "
            "as a C runtime artifact"
        ),
    )
    parser.add_argument(
        "--artifact-id",
        default="unversioned",
        help="stable provenance identifier embedded in --export-c-model",
    )
    parser.add_argument("--artifact-workers", type=int, default=10)
    parser.add_argument("--artifact-tt-fraction", type=float, default=0.05)
    parser.add_argument(
        "--artifact-min-enforceable-depth",
        type=int,
        default=5,
        help=(
            "first target depth whose start was not part of ABDADA's "
            "calibration warm-up"
        ),
    )
    parser.add_argument(
        "--artifact-disable-heuristics",
        action="store_true",
        help="mark the exported model as trained with endgame heuristics off",
    )
    args = parser.parse_args()
    if not 0.0 < args.confidence < 1.0:
        raise ValueError("confidence must be between zero and one")
    if args.neighbors <= 0:
        raise ValueError("neighbors must be positive")
    if not 0.0 <= args.neighbor_quantile <= 1.0:
        raise ValueError("neighbor quantile must be between zero and one")
    prior_node_floors = parse_prior_node_floors(args.prior_node_floor)
    if args.export_c_model:
        if args.model != "ridge" or args.target != "ratio":
            raise ValueError(
                "--export-c-model requires --model ridge --target ratio"
            )
        if args.metric != "nodes":
            raise ValueError("--export-c-model requires --metric nodes")
        if args.prior_node_floor:
            raise ValueError(
                "--export-c-model does not support post-hoc node floors"
            )
        if not args.calibration_corpus:
            raise ValueError(
                "--export-c-model requires an explicit calibration corpus"
            )

    corpora = [
        load_corpus(index, parse_corpus_argument(value))
        for index, value in enumerate(args.fit_corpus)
    ]
    if args.fit_cgp:
        if len(args.fit_cgp) != len(corpora):
            raise ValueError("--fit-cgp count must match --fit-corpus")
        for corpus, cgp_path in zip(
            corpora, args.fit_cgp, strict=True
        ):
            add_cgp_features(corpus, Path(cgp_path))
    if args.fit_range:
        if len(args.fit_range) != len(corpora):
            raise ValueError("--fit-range count must match --fit-corpus")
        corpora = [
            filter_positions(corpus, parse_range(selected_range))
            for corpus, selected_range in zip(
                corpora, args.fit_range, strict=True
            )
        ]
    calibration_corpora: list[list[Position]] = []
    if args.calibration_corpus:
        calibration_corpora = [
            load_corpus(
                len(corpora) + index,
                parse_corpus_argument(value),
            )
            for index, value in enumerate(args.calibration_corpus)
        ]
        if args.calibration_cgp:
            if len(args.calibration_cgp) != len(calibration_corpora):
                raise ValueError(
                    "--calibration-cgp count must match "
                    "--calibration-corpus"
                )
            for corpus, cgp_path in zip(
                calibration_corpora,
                args.calibration_cgp,
                strict=True,
            ):
                add_cgp_features(corpus, Path(cgp_path))
        if args.calibration_range:
            if len(args.calibration_range) != len(calibration_corpora):
                raise ValueError(
                    "--calibration-range count must match "
                    "--calibration-corpus"
                )
            calibration_corpora = [
                filter_positions(corpus, parse_range(selected_range))
                for corpus, selected_range in zip(
                    calibration_corpora,
                    args.calibration_range,
                    strict=True,
                )
            ]
    validation_positions = (
        load_corpus(
            len(corpora),
            parse_corpus_argument(args.validate),
        )
        if args.validate
        else []
    )
    if args.validate_cgp:
        if not validation_positions:
            raise ValueError("--validate-cgp requires --validate")
        add_cgp_features(validation_positions, args.validate_cgp)
    exported_depth_models: list[
        tuple[int, RidgeModel, float, float]
    ] = []
    for target_depth in (int(value) for value in args.depths.split(",")):
        training = observations_for(corpora, target_depth)
        report_rate_diagnostics(target_depth, training)
        model = fit_model(
            training,
            args.model,
            args.penalty,
            args.neighbors,
            args.neighbor_quantile,
            args.target == "ratio",
        )
        if calibration_corpora:
            scores, censored_lower_scores = calibration_scores(
                model,
                observations_for(calibration_corpora, target_depth),
                args.metric,
            )
        else:
            scores, censored_lower_scores = cross_fitted_scores(
                corpora,
                target_depth,
                args.model,
                args.penalty,
                args.neighbors,
                args.neighbor_quantile,
                args.target == "ratio",
                args.metric,
            )
        correction, optimistic_correction = censored_conformal_quantile(
            scores, censored_lower_scores, args.confidence
        )
        if args.export_c_model:
            assert isinstance(model, RidgeModel)
            wall_scores, wall_censored_lower_scores = calibration_scores(
                model,
                observations_for(calibration_corpora, target_depth),
                "wall",
            )
            wall_correction, _ = censored_conformal_quantile(
                wall_scores,
                wall_censored_lower_scores,
                args.confidence,
            )
            if not math.isfinite(correction) or not math.isfinite(
                wall_correction
            ):
                raise ValueError(
                    f"depth {target_depth} has no finite runtime correction"
                )
            exported_depth_models.append(
                (target_depth, model, correction, wall_correction)
            )
        validation = observations_for(
            [validation_positions], target_depth
        )
        (
            complete,
            failures,
            definite_censored_failures,
            indeterminate_censors,
            unscored,
            ratios,
            expected_ratios,
        ) = evaluate(
            model,
            validation,
            correction,
            args.metric,
            prior_node_floors,
        )
        if validation:
            report_budget_classification(
                target_depth,
                model,
                validation,
                correction,
                args.metric,
                prior_node_floors,
            )
            report_validation_failures(
                target_depth,
                model,
                validation,
                correction,
                args.metric,
                prior_node_floors,
            )
        correction_multiplier = (
            math.exp(correction) if math.isfinite(correction) else math.inf
        )
        optimistic_multiplier = (
            math.exp(optimistic_correction)
            if math.isfinite(optimistic_correction)
            else math.inf
        )
        resolved_trials = complete + definite_censored_failures
        resolved_failures = failures + definite_censored_failures
        resolved_coverage = (
            (resolved_trials - resolved_failures) / resolved_trials
            if resolved_trials
            else math.nan
        )
        worst_case_trials = resolved_trials + indeterminate_censors
        worst_case_coverage = (
            (resolved_trials - resolved_failures) / worst_case_trials
            if worst_case_trials
            else math.nan
        )
        print(
            "EGADMISSION "
            f"depth={target_depth} model={args.model} "
            f"target={args.target} "
            f"metric={args.metric} "
            f"prior_node_floor="
            f"{prior_node_floors.get(target_depth, 0.0):.3f} "
            f"fit_n={len(training)} "
            f"calibration_n="
            f"{len(scores) + len(censored_lower_scores)} "
            f"calibration_censored={len(censored_lower_scores)} "
            f"confidence={args.confidence:.4f} "
            f"correction_multiplier={correction_multiplier:.3f} "
            f"optimistic_censored_multiplier={optimistic_multiplier:.3f} "
            f"validation_n={len(validation)} complete={complete} "
            f"complete_failures={failures} "
            f"definite_censored_failures={definite_censored_failures} "
            f"indeterminate_censors={indeterminate_censors} "
            f"unscored={unscored} "
            f"resolved_failures={resolved_failures} "
            f"resolved_coverage={resolved_coverage:.4f} "
            f"worst_case_coverage={worst_case_coverage:.4f} "
            f"failure_upper95="
            f"{binomial_failure_upper_bound(resolved_failures, resolved_trials, 0.95):.4f} "
            f"bound_ratio50/90/95/99={format_quantiles(ratios)} "
            f"expected_ratio50/90/95/99="
            f"{format_quantiles(expected_ratios)}"
        )
    if args.export_c_model:
        write_c_model(
            args.export_c_model,
            args.artifact_id,
            args.artifact_workers,
            args.artifact_tt_fraction,
            not args.artifact_disable_heuristics,
            args.artifact_min_enforceable_depth,
            args.confidence,
            exported_depth_models,
        )
        print(
            "EGADMISSION_EXPORT "
            f"path={args.export_c_model} artifact={args.artifact_id} "
            f"depths={len(exported_depth_models)}"
        )


if __name__ == "__main__":
    main()
