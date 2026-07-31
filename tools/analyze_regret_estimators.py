#!/usr/bin/env python3
"""Compare independent- and correlated-arm online regret estimates.

The thinking-curve harness emits a small balanced panel of common-scenario
rollouts for the selected play and its most plausible challengers.  This
script uses that panel to estimate cross-play covariance while retaining the
means, variances, and sample counts from every rollout performed by BAI.
"""

from __future__ import annotations

import argparse
import csv
import math
import statistics
from dataclasses import dataclass
from pathlib import Path

import numpy as np


METRICS = ("utility", "win", "spread")


def parse_fields(line: str) -> dict[str, str]:
    fields: dict[str, str] = {}
    for token in line.split()[1:]:
        if "=" in token:
            key, value = token.split("=", 1)
            fields[key] = value
    return fields


def panel_key(fields: dict[str, str]) -> tuple[int, int, int]:
    return (
        int(fields["source_index"]),
        int(fields["plies"]),
        int(fields["target_nodes"]),
    )


@dataclass
class Panel:
    fields: dict[str, str]
    arms: dict[int, dict[str, str]]
    rows: dict[int, dict[int, tuple[float, float, float]]]


def load_panels(path: Path) -> dict[tuple[int, int, int], Panel]:
    panel_fields: dict[tuple[int, int, int], dict[str, str]] = {}
    arms: dict[tuple[int, int, int], dict[int, dict[str, str]]] = {}
    rows: dict[
        tuple[int, int, int], dict[int, dict[int, tuple[float, float, float]]]
    ] = {}
    with path.open(encoding="utf-8") as stream:
        for line in stream:
            if line.startswith("THINKING_CURVE_REGRET_PANEL "):
                fields = parse_fields(line)
                panel_fields[panel_key(fields)] = fields
            elif line.startswith("THINKING_CURVE_REGRET_ARM "):
                fields = parse_fields(line)
                key = panel_key(fields)
                arms.setdefault(key, {})[int(fields["rank"])] = fields
            elif line.startswith("THINKING_CURVE_REGRET_SAMPLE "):
                fields = parse_fields(line)
                key = panel_key(fields)
                row = int(fields["pair_row"])
                rank = int(fields["rank"])
                rows.setdefault(key, {}).setdefault(row, {})[rank] = (
                    float(fields["utility"]),
                    float(fields["win"]),
                    float(fields["spread"]),
                )

    panels: dict[tuple[int, int, int], Panel] = {}
    for key, fields in panel_fields.items():
        if key not in arms or key not in rows:
            raise ValueError(f"incomplete regret panel {key}")
        panels[key] = Panel(fields, arms[key], rows[key])
    return panels


def empirical_correlation(samples: np.ndarray) -> np.ndarray:
    flat = samples.reshape(samples.shape[0], -1)
    centered = flat - flat.mean(axis=0)
    covariance = np.einsum("ri,rj->ij", centered, centered) / max(
        1, samples.shape[0] - 1
    )
    scale = np.sqrt(np.maximum(np.diag(covariance), 0.0))
    denominator = np.outer(scale, scale)
    correlation = np.divide(
        covariance,
        denominator,
        out=np.zeros_like(covariance),
        where=denominator > 0,
    )
    np.fill_diagonal(correlation, 1.0)
    return np.clip(correlation, -1.0, 1.0)


def exchangeable_correlation(
    correlation: np.ndarray, arm_count: int, metric_count: int
) -> np.ndarray:
    """Pool correlations over arm identity while retaining metric structure."""
    within = np.empty((metric_count, metric_count))
    across = np.empty_like(within)
    for left_metric in range(metric_count):
        for right_metric in range(metric_count):
            within[left_metric, right_metric] = statistics.fmean(
                correlation[
                    arm * metric_count + left_metric,
                    arm * metric_count + right_metric,
                ]
                for arm in range(arm_count)
            )
            across[left_metric, right_metric] = statistics.fmean(
                correlation[
                    left_arm * metric_count + left_metric,
                    right_arm * metric_count + right_metric,
                ]
                for left_arm in range(arm_count)
                for right_arm in range(arm_count)
                if left_arm != right_arm
            )

    pooled = np.empty_like(correlation)
    for left_arm in range(arm_count):
        for right_arm in range(arm_count):
            block = within if left_arm == right_arm else across
            left = left_arm * metric_count
            right = right_arm * metric_count
            pooled[
                left : left + metric_count, right : right + metric_count
            ] = block
    pooled = (pooled + pooled.T) * 0.5
    np.fill_diagonal(pooled, 1.0)
    return np.clip(pooled, -1.0, 1.0)


def positive_semidefinite(matrix: np.ndarray) -> np.ndarray:
    symmetric = (matrix + matrix.T) * 0.5
    values, vectors = np.linalg.eigh(symmetric)
    floor = max(float(values[-1]), 1.0) * 1e-12
    values = np.maximum(values, floor)
    return np.einsum("ik,k,jk->ij", vectors, values, vectors)


def posterior_covariances(
    variances: np.ndarray,
    counts: np.ndarray,
    paired_correlation: np.ndarray,
    cross_arm_shrinkage: float,
) -> tuple[np.ndarray, np.ndarray]:
    arm_count, metric_count = variances.shape
    independent_correlation = np.zeros_like(paired_correlation)
    for arm in range(arm_count):
        start = arm * metric_count
        stop = start + metric_count
        independent_correlation[start:stop, start:stop] = (
            paired_correlation[start:stop, start:stop]
        )

    correlated_correlation = (
        cross_arm_shrinkage * independent_correlation
        + (1.0 - cross_arm_shrinkage) * paired_correlation
    )
    single_sample_scales = np.sqrt(np.maximum(variances.reshape(-1), 0.0))
    single_sample_covariance = (
        np.outer(single_sample_scales, single_sample_scales)
        * correlated_correlation
    )
    independent_single_sample_covariance = (
        np.outer(single_sample_scales, single_sample_scales)
        * independent_correlation
    )

    # Every play's PRNG stream is the same ordered stream of scenario seeds.
    # Unequally sampled arms therefore share min(n_i, n_j) observations:
    #
    # Cov(mean_i, mean_j) = Cov(X_i, X_j) / max(n_i, n_j).
    expanded_counts = np.repeat(counts, metric_count)
    mean_scale = 1.0 / np.maximum.outer(expanded_counts, expanded_counts)
    independent = independent_single_sample_covariance * mean_scale
    correlated = single_sample_covariance * mean_scale
    return positive_semidefinite(independent), positive_semidefinite(correlated)


def normal_draws(
    mean: np.ndarray, covariance: np.ndarray, standard_normals: np.ndarray
) -> np.ndarray:
    values, vectors = np.linalg.eigh(covariance)
    values = np.maximum(values, 0.0)
    transform = vectors * np.sqrt(values)
    return mean + np.einsum("rd,ed->re", standard_normals, transform)


def summarize_draws(
    draws: np.ndarray, selected: int
) -> dict[str, float]:
    reshaped = draws.reshape(draws.shape[0], -1, len(METRICS))
    best = np.argmax(reshaped[:, :, 0], axis=1)
    row = np.arange(reshaped.shape[0])
    utility_regret = np.maximum(
        reshaped[row, best, 0] - reshaped[:, selected, 0], 0.0
    )
    win_error = reshaped[row, best, 1] - reshaped[:, selected, 1]
    spread_error = reshaped[row, best, 2] - reshaped[:, selected, 2]
    return {
        "predicted_utility_regret": float(np.mean(utility_regret)),
        "predicted_utility_p50": float(np.quantile(utility_regret, 0.50)),
        "predicted_utility_p90": float(np.quantile(utility_regret, 0.90)),
        "predicted_utility_p95": float(np.quantile(utility_regret, 0.95)),
        "predicted_win_error": float(np.mean(win_error)),
        "predicted_spread_error": float(np.mean(spread_error)),
    }


def pair_calibration(
    means: np.ndarray,
    judge_means: np.ndarray,
    covariance: np.ndarray,
    selected: int,
) -> dict[str, float]:
    nll: list[float] = []
    squared_z: list[float] = []
    absolute_z: list[float] = []
    for challenger in range(len(means)):
        if challenger == selected:
            continue
        selected_utility = selected * len(METRICS)
        challenger_utility = challenger * len(METRICS)
        variance = (
            covariance[selected_utility, selected_utility]
            + covariance[challenger_utility, challenger_utility]
            - 2 * covariance[selected_utility, challenger_utility]
        )
        variance = max(float(variance), 1e-20)
        estimated_delta = means[challenger, 0] - means[selected, 0]
        judge_delta = judge_means[challenger] - judge_means[selected]
        error = estimated_delta - judge_delta
        z = error / math.sqrt(variance)
        nll.append(0.5 * (math.log(variance) + z * z))
        squared_z.append(z * z)
        absolute_z.append(abs(z))
    return {
        "pair_nll": statistics.fmean(nll),
        "pair_mean_squared_z": statistics.fmean(squared_z),
        "pair_mean_absolute_z": statistics.fmean(absolute_z),
    }


def analyze_panel(
    key: tuple[int, int, int],
    panel: Panel,
    draws: int,
    shrinkage: float,
    seed: int,
    paired_row_limit: int | None,
    correlation_override: np.ndarray | None,
) -> dict[str, object]:
    ranks = sorted(panel.arms)
    rank_to_arm = {rank: arm for arm, rank in enumerate(ranks)}
    selected_rank = int(panel.fields["selected_rank"])
    if selected_rank not in rank_to_arm:
        raise ValueError(f"selected play absent from risk panel {key}")

    expected_risk_count = int(panel.fields["risk_count"])
    if len(ranks) != expected_risk_count:
        raise ValueError(f"risk-arm count mismatch in panel {key}")
    paired_rows = int(panel.fields["paired_rows"])
    if len(panel.rows) != paired_rows:
        raise ValueError(f"paired-row count mismatch in panel {key}")
    if paired_row_limit is not None:
        paired_rows = min(paired_rows, paired_row_limit)

    sample_matrix = np.empty((paired_rows, len(ranks), len(METRICS)))
    for row_index, row_number in enumerate(sorted(panel.rows)[:paired_rows]):
        row = panel.rows[row_number]
        if set(row) != set(ranks):
            raise ValueError(f"incomplete common-scenario row in panel {key}")
        for arm, rank in enumerate(ranks):
            sample_matrix[row_index, arm] = row[rank]

    means = np.empty((len(ranks), len(METRICS)))
    variances = np.empty_like(means)
    counts = np.empty(len(ranks), dtype=np.float64)
    for arm, rank in enumerate(ranks):
        fields = panel.arms[rank]
        counts[arm] = int(fields["samples"])
        for metric, name in enumerate(METRICS):
            means[arm, metric] = float(fields[f"{name}_mean"])
            variances[arm, metric] = float(fields[f"{name}_variance"])

    observed_correlation = empirical_correlation(sample_matrix)
    correlation = (
        observed_correlation
        if correlation_override is None
        else correlation_override
    )
    if correlation.shape != observed_correlation.shape:
        raise ValueError(f"correlation-model shape mismatch in panel {key}")
    independent_covariance, correlated_covariance = posterior_covariances(
        variances, counts, correlation, shrinkage
    )
    rng = np.random.default_rng(seed)
    standard_normals = rng.standard_normal((draws, means.size))
    independent = summarize_draws(
        normal_draws(means.reshape(-1), independent_covariance, standard_normals),
        rank_to_arm[selected_rank],
    )
    correlated = summarize_draws(
        normal_draws(means.reshape(-1), correlated_covariance, standard_normals),
        rank_to_arm[selected_rank],
    )
    judge_means = np.asarray(
        [float(panel.arms[rank]["judge_utility"]) for rank in ranks]
    )
    selected_arm = rank_to_arm[selected_rank]
    independent_pair_calibration = pair_calibration(
        means, judge_means, independent_covariance, selected_arm
    )
    correlated_pair_calibration = pair_calibration(
        means, judge_means, correlated_covariance, selected_arm
    )

    utility_correlation = observed_correlation[
        0::len(METRICS), 0::len(METRICS)
    ]
    off_diagonal = utility_correlation[np.triu_indices(len(ranks), 1)]
    se_ratios: list[float] = []
    for challenger in range(len(ranks)):
        if challenger == selected_arm:
            continue
        selected_utility = selected_arm * len(METRICS)
        challenger_utility = challenger * len(METRICS)
        independent_difference_variance = (
            independent_covariance[selected_utility, selected_utility]
            + independent_covariance[challenger_utility, challenger_utility]
        )
        correlated_difference_variance = (
            correlated_covariance[selected_utility, selected_utility]
            + correlated_covariance[challenger_utility, challenger_utility]
            - 2
            * correlated_covariance[selected_utility, challenger_utility]
        )
        if independent_difference_variance > 0:
            se_ratios.append(
                math.sqrt(
                    max(correlated_difference_variance, 0.0)
                    / independent_difference_variance
                )
            )

    result: dict[str, object] = {
        "source_index": key[0],
        "plies": key[1],
        "target_nodes": key[2],
        "bag": int(panel.fields["bag"]),
        "risk_count": len(ranks),
        "paired_rows": paired_rows,
        "selected_rank": selected_rank,
        "judge_best_rank": int(panel.fields["judge_best_rank"]),
        "actual_utility_regret": float(panel.fields["actual_utility_regret"]),
        "actual_win_regret": float(panel.fields["actual_win_regret"]),
        "actual_spread_regret": float(panel.fields["actual_spread_regret"]),
        "mean_pairwise_utility_correlation": float(np.mean(off_diagonal)),
        "median_pairwise_utility_correlation": float(np.median(off_diagonal)),
        "median_correlated_to_independent_delta_se": float(
            np.median(se_ratios)
        ),
    }
    for estimator_name, estimate in (
        ("independent", independent),
        ("correlated", correlated),
    ):
        for name, value in estimate.items():
            result[f"{estimator_name}_{name}"] = value
    for estimator_name, calibration in (
        ("independent", independent_pair_calibration),
        ("correlated", correlated_pair_calibration),
    ):
        for name, value in calibration.items():
            result[f"{estimator_name}_{name}"] = value
    return result


def mean_ci(values: list[float]) -> tuple[float, float, float]:
    mean = statistics.fmean(values)
    if len(values) < 2:
        return mean, mean, mean
    sem = statistics.stdev(values) / math.sqrt(len(values))
    return mean, mean - 1.96 * sem, mean + 1.96 * sem


def bootstrap_mean_ci(
    values: np.ndarray, seed: int, replicates: int = 50_000
) -> tuple[float, float]:
    if len(values) < 2:
        value = float(values[0])
        return value, value
    rng = np.random.default_rng(seed)
    indices = rng.integers(0, len(values), size=(replicates, len(values)))
    means = np.mean(values[indices], axis=1)
    return float(np.quantile(means, 0.025)), float(np.quantile(means, 0.975))


def paired_randomization_pvalue(values: np.ndarray, seed: int) -> float:
    if len(values) == 0 or np.all(values == 0):
        return 1.0
    rng = np.random.default_rng(seed)
    observed = abs(float(np.mean(values)))
    exceedances = 0
    permutations = 100_000
    chunk = 2_000
    for start in range(0, permutations, chunk):
        count = min(chunk, permutations - start)
        signs = rng.choice((-1.0, 1.0), size=(count, len(values)))
        permuted = np.abs(np.mean(signs * values, axis=1))
        exceedances += int(np.count_nonzero(permuted >= observed))
    return (exceedances + 1) / (permutations + 1)


def print_summary(results: list[dict[str, object]]) -> None:
    actual = [float(row["actual_utility_regret"]) for row in results]
    actual_mean = statistics.fmean(actual)
    actual_low, actual_high = bootstrap_mean_ci(np.asarray(actual), 0xAC7)
    print(
        "REGRET_ESTIMATOR_ACTUAL "
        f"panels={len(results)} mean_utility_regret={actual_mean:.9f} "
        f"bootstrap_ci95=[{actual_low:.9f},{actual_high:.9f}]"
    )
    errors: dict[str, np.ndarray] = {}
    for estimator in ("independent", "correlated"):
        predicted = [
            float(row[f"{estimator}_predicted_utility_regret"])
            for row in results
        ]
        residuals = np.asarray(predicted) - np.asarray(actual)
        errors[estimator] = np.abs(residuals)
        mean_predicted, low, high = mean_ci(predicted)
        bias, bias_low, bias_high = mean_ci(residuals.tolist())
        bias_pvalue = paired_randomization_pvalue(
            residuals, 0x1DE0 if estimator == "independent" else 0xC0DE
        )
        win_prediction = statistics.fmean(
            float(row[f"{estimator}_predicted_win_error"]) for row in results
        )
        actual_win_values = np.asarray(
            [float(row["actual_win_regret"]) for row in results]
        )
        predicted_win_values = np.asarray(
            [
                float(row[f"{estimator}_predicted_win_error"])
                for row in results
            ]
        )
        win_actual = float(np.mean(actual_win_values))
        win_residuals = predicted_win_values - actual_win_values
        spread_prediction = statistics.fmean(
            float(row[f"{estimator}_predicted_spread_error"])
            for row in results
        )
        actual_spread_values = np.asarray(
            [float(row["actual_spread_regret"]) for row in results]
        )
        predicted_spread_values = np.asarray(
            [
                float(row[f"{estimator}_predicted_spread_error"])
                for row in results
            ]
        )
        spread_actual = float(np.mean(actual_spread_values))
        spread_residuals = predicted_spread_values - actual_spread_values
        p90_coverage = statistics.fmean(
            float(row["actual_utility_regret"])
            <= float(row[f"{estimator}_predicted_utility_p90"])
            for row in results
        )
        p95_coverage = statistics.fmean(
            float(row["actual_utility_regret"])
            <= float(row[f"{estimator}_predicted_utility_p95"])
            for row in results
        )
        print(
            "REGRET_ESTIMATOR_SUMMARY "
            f"estimator={estimator} panels={len(results)} "
            f"mean_predicted_utility_regret={mean_predicted:.9f} "
            f"ci95=[{low:.9f},{high:.9f}] "
            f"bias={bias:+.9f} "
            f"bias_ci95=[{bias_low:+.9f},{bias_high:+.9f}] "
            f"bias_p={bias_pvalue:.6g} "
            f"mae={float(np.mean(np.abs(residuals))):.9f} "
            f"rmse={float(np.sqrt(np.mean(residuals**2))):.9f} "
            f"p90_coverage={p90_coverage:.4f} p95_coverage={p95_coverage:.4f} "
            f"mean_predicted_win_error={win_prediction:+.9f} "
            f"mean_actual_win_error={win_actual:+.9f} "
            f"win_bias={float(np.mean(win_residuals)):+.9f} "
            f"win_mae={float(np.mean(np.abs(win_residuals))):.9f} "
            f"mean_predicted_spread_error={spread_prediction:+.6f} "
            f"mean_actual_spread_error={spread_actual:+.6f} "
            f"spread_bias={float(np.mean(spread_residuals)):+.6f} "
            f"spread_mae={float(np.mean(np.abs(spread_residuals))):.6f}"
        )

    correlations = [
        float(row["median_pairwise_utility_correlation"]) for row in results
    ]
    se_ratios = [
        float(row["median_correlated_to_independent_delta_se"])
        for row in results
    ]
    for comparison_index, (left, right) in enumerate(
        (("correlated", "independent"),)
    ):
        mae_delta = errors[left] - errors[right]
        delta_mean = float(np.mean(mae_delta))
        delta_low, delta_high = bootstrap_mean_ci(
            mae_delta, 0xD311A + comparison_index
        )
        pvalue = paired_randomization_pvalue(
            mae_delta, 0xBA1 + comparison_index
        )
        baseline_mae = float(np.mean(errors[right]))
        relative_change = (
            delta_mean / baseline_mae if baseline_mae > 0.0 else 0.0
        )
        print(
            "REGRET_ESTIMATOR_COMPARISON "
            f"metric=absolute_error_{left}_minus_{right} "
            f"mean={delta_mean:+.9f} "
            f"relative_change={relative_change:+.4f} "
            f"bootstrap_ci95=[{delta_low:+.9f},{delta_high:+.9f}] "
            f"paired_randomization_p={pvalue:.6g} "
            f"pairwise_utility_correlation_p10={float(np.quantile(correlations, 0.1)):+.4f} "
            f"median_pairwise_utility_correlation={statistics.median(correlations):+.4f} "
            f"pairwise_utility_correlation_p90={float(np.quantile(correlations, 0.9)):+.4f} "
            f"median_delta_se_ratio={statistics.median(se_ratios):.4f}"
        )

    for estimator in ("independent", "correlated"):
        print(
            "REGRET_PAIR_CALIBRATION "
            f"estimator={estimator} panels={len(results)} "
            f"mean_nll={statistics.fmean(float(row[f'{estimator}_pair_nll']) for row in results):+.6f} "
            "mean_squared_z="
            f"{statistics.fmean(float(row[f'{estimator}_pair_mean_squared_z']) for row in results):.6f} "
            "mean_absolute_z="
            f"{statistics.fmean(float(row[f'{estimator}_pair_mean_absolute_z']) for row in results):.6f}"
        )
    pair_nll_delta = np.asarray(
        [
            float(row["correlated_pair_nll"])
            - float(row["independent_pair_nll"])
            for row in results
        ]
    )
    pair_nll_low, pair_nll_high = bootstrap_mean_ci(pair_nll_delta, 0xC411B)
    print(
        "REGRET_PAIR_COMPARISON metric=nll_correlated_minus_independent "
        f"mean={float(np.mean(pair_nll_delta)):+.6f} "
        f"bootstrap_ci95=[{pair_nll_low:+.6f},{pair_nll_high:+.6f}] "
        f"paired_randomization_p={paired_randomization_pvalue(pair_nll_delta, 0xCA11):.6g}"
    )

    strata: dict[int, list[dict[str, object]]] = {}
    for row in results:
        stratum = (int(row["bag"]) // 10) * 10
        strata.setdefault(stratum, []).append(row)
    for stratum, group in sorted(strata.items()):
        stratum_actual = np.asarray(
            [float(row["actual_utility_regret"]) for row in group]
        )
        independent_prediction = np.asarray(
            [
                float(row["independent_predicted_utility_regret"])
                for row in group
            ]
        )
        correlated_prediction = np.asarray(
            [
                float(row["correlated_predicted_utility_regret"])
                for row in group
            ]
        )
        stratum_mae_delta = np.mean(
            np.abs(correlated_prediction - stratum_actual)
            - np.abs(independent_prediction - stratum_actual)
        )
        print(
            "REGRET_ESTIMATOR_STRATUM "
            f"bag={stratum}-{stratum + 9} panels={len(group)} "
            f"mean_actual={float(np.mean(stratum_actual)):.9f} "
            f"mean_independent={float(np.mean(independent_prediction)):.9f} "
            f"mean_correlated={float(np.mean(correlated_prediction)):.9f} "
            f"mae_delta={float(stratum_mae_delta):+.9f} "
            "median_pairwise_utility_correlation="
            f"{statistics.median(float(row['median_pairwise_utility_correlation']) for row in group):+.4f} "
            "median_delta_se_ratio="
            f"{statistics.median(float(row['median_correlated_to_independent_delta_se']) for row in group):.4f}"
        )


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("log", type=Path)
    parser.add_argument("--csv", type=Path)
    parser.add_argument("--draws", type=int, default=50_000)
    parser.add_argument(
        "--cross-arm-shrinkage",
        type=float,
        default=0.10,
        help="fraction by which empirical cross-arm correlations shrink to zero",
    )
    parser.add_argument(
        "--paired-rows",
        type=int,
        help="use only this many common-scenario rows from each panel",
    )
    parser.add_argument(
        "--correlation-source",
        choices=("paired", "loo-pooled"),
        default="paired",
        help=(
            "estimate correlations from each panel or from an exchangeable "
            "leave-one-panel-out prior"
        ),
    )
    parser.add_argument("--seed", type=int, default=20260729)
    args = parser.parse_args()
    if args.draws < 1:
        parser.error("--draws must be positive")
    if not 0 <= args.cross_arm_shrinkage <= 1:
        parser.error("--cross-arm-shrinkage must be between 0 and 1")
    if args.paired_rows is not None and args.paired_rows < 2:
        parser.error("--paired-rows must be at least 2")

    panels = load_panels(args.log)
    sorted_keys = sorted(panels)
    correlation_overrides: dict[
        tuple[int, int, int], np.ndarray
    ] = {}
    if args.correlation_source == "loo-pooled":
        if len(sorted_keys) < 2:
            parser.error("--correlation-source loo-pooled needs multiple panels")
        pooled_models: dict[tuple[int, int, int], np.ndarray] = {}
        for key in sorted_keys:
            panel = panels[key]
            ranks = sorted(panel.arms)
            paired_rows = int(panel.fields["paired_rows"])
            if args.paired_rows is not None:
                paired_rows = min(paired_rows, args.paired_rows)
            samples = np.empty((paired_rows, len(ranks), len(METRICS)))
            for row_index, row_number in enumerate(
                sorted(panel.rows)[:paired_rows]
            ):
                row = panel.rows[row_number]
                if set(row) != set(ranks):
                    raise ValueError(
                        f"incomplete common-scenario row in panel {key}"
                    )
                for arm, rank in enumerate(ranks):
                    samples[row_index, arm] = row[rank]
            pooled_models[key] = exchangeable_correlation(
                empirical_correlation(samples), len(ranks), len(METRICS)
            )
        for key in sorted_keys:
            training_models = [
                model
                for training_key, model in pooled_models.items()
                if training_key != key
            ]
            if any(
                model.shape != training_models[0].shape
                for model in training_models
            ):
                raise ValueError(
                    "loo-pooled correlation requires equal risk-set sizes"
                )
            correlation_overrides[key] = np.mean(training_models, axis=0)

    results = [
        analyze_panel(
            key,
            panels[key],
            args.draws,
            args.cross_arm_shrinkage,
            args.seed + index,
            args.paired_rows,
            correlation_overrides.get(key),
        )
        for index, key in enumerate(sorted_keys)
    ]
    if not results:
        raise SystemExit("no complete THINKING_CURVE_REGRET_PANEL rows found")

    if args.csv:
        args.csv.parent.mkdir(parents=True, exist_ok=True)
        with args.csv.open("w", newline="", encoding="utf-8") as stream:
            writer = csv.DictWriter(stream, fieldnames=list(results[0]))
            writer.writeheader()
            writer.writerows(results)
    print_summary(results)


if __name__ == "__main__":
    main()
