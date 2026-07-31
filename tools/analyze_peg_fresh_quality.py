#!/usr/bin/env python3
"""Analyze the locked representative PEG policy panel and deeper oracle."""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import pathlib
import random
import statistics
from typing import Any

from run_peg_time_calibration import load_records


METRICS = ("utility", "win", "spread")
PRIMARY_COMPARISONS = {
    "full32_minus_adaptive": ("min8_patience8", "fixed_32"),
    "deep16_minus_adaptive": ("min8_patience8", "deep_16_8"),
    "deep24_minus_deep16": ("deep_16_8", "deep_24_12"),
}
STOPPING_COMPARISONS = {
    "full32_minus_min8p4": ("min8_patience4", "fixed_32"),
    "full32_minus_min12p8": ("min12_patience8", "fixed_32"),
    "min8p8_minus_min8p4": ("min8_patience4", "min8_patience8"),
    "min12p8_minus_min8p8": ("min8_patience8", "min12_patience8"),
}
STAGE_COMPARISONS = {
    "fixed4_minus_fixed2": ("fixed_2", "fixed_4"),
    "fixed8_minus_fixed4": ("fixed_4", "fixed_8"),
    "fixed12_minus_fixed8": ("fixed_8", "fixed_12"),
}
CURVE_COUNTS = (2, 4, 8, 12, 32)


def percentile(values: list[float], probability: float) -> float:
    ordered = sorted(values)
    if not ordered:
        return math.nan
    index = (len(ordered) - 1) * probability
    lower = math.floor(index)
    upper = math.ceil(index)
    if lower == upper:
        return ordered[lower]
    fraction = index - lower
    return ordered[lower] * (1 - fraction) + ordered[upper] * fraction


def describe(values: list[float]) -> dict[str, float | int]:
    if not values:
        return {"n": 0}
    return {
        "n": len(values),
        "mean": statistics.mean(values),
        "median": statistics.median(values),
        "p90": percentile(values, 0.90),
        "p95": percentile(values, 0.95),
        "max": max(values),
    }


def stable_seed(text: str) -> int:
    return int.from_bytes(
        hashlib.sha256(text.encode()).digest()[:8], "big"
    )


def values_index(
    records: list[dict[str, Any]],
) -> tuple[
    dict[tuple[str, str], dict[str, Any]],
    dict[int, dict[str, dict[str, float]]],
]:
    judges = {
        (str(record["position"]), str(record["label"])): record
        for record in records
        if record.get("kind") == "judge"
        and int(record.get("accepted", 0)) == 1
    }
    values: dict[int, dict[str, dict[str, float]]] = {}
    for record in records:
        if record.get("kind") != "value":
            continue
        values.setdefault(int(record["sequence"]), {})[
            str(record["move"])
        ] = {
            metric: float(record[metric]) for metric in METRICS
        }
    return judges, values


def oracle_values(
    judges: dict[tuple[str, str], dict[str, Any]],
    values: dict[int, dict[str, dict[str, float]]],
    position: str,
    label: str,
    nominees: list[str],
) -> dict[str, dict[str, float]] | None:
    judge = judges.get((position, label))
    if judge is None:
        return None
    result = values.get(int(judge["sequence"]), {})
    if set(result) != set(nominees):
        return None
    return result


def delta(
    values: dict[str, dict[str, float]],
    before: str,
    after: str,
    metric: str,
) -> float:
    return float(values[after][metric]) - float(values[before][metric])


def build_comparison_rows(
    maps: list[dict[str, Any]],
    judges: dict[tuple[str, str], dict[str, Any]],
    values: dict[int, dict[str, dict[str, float]]],
    before_policy: str,
    after_policy: str,
) -> list[dict[str, Any]]:
    rows = []
    for quality_map in maps:
        position = str(quality_map["position"])
        bag = int(quality_map["bag"])
        before = str(quality_map["policies"][before_policy]["move"])
        after = str(quality_map["policies"][after_policy]["move"])
        disagree = before != after
        values3 = oracle_values(
            judges,
            values,
            position,
            "fresh_direct3_stride4",
            [str(move) for move in quality_map["nominees"]],
        )
        values4 = oracle_values(
            judges,
            values,
            position,
            "fresh_direct4_stride4",
            [str(move) for move in quality_map["nominees"]],
        )
        row = {
            "position": position,
            "bag": bag,
            "before_move": before,
            "after_move": after,
            "disagree": disagree,
            "direct3": {},
            "direct4": {},
        }
        for metric in METRICS:
            if not disagree:
                row["direct3"][metric] = 0.0
                row["direct4"][metric] = 0.0
            else:
                row["direct3"][metric] = (
                    delta(values3, before, after, metric)
                    if values3 is not None
                    else None
                )
                row["direct4"][metric] = (
                    delta(values4, before, after, metric)
                    if values4 is not None
                    else None
                )
        rows.append(row)
    return rows


def correction_by_bag(
    rows: list[dict[str, Any]], metric: str
) -> tuple[dict[int, float], dict[int, list[float]], float]:
    corrections: dict[int, list[float]] = {bag: [] for bag in range(1, 5)}
    for row in rows:
        direct3 = row["direct3"][metric]
        direct4 = row["direct4"][metric]
        if row["disagree"] and direct3 is not None and direct4 is not None:
            corrections[int(row["bag"])].append(
                float(direct4) - float(direct3)
            )
    pooled = [
        value for bag_values in corrections.values() for value in bag_values
    ]
    pooled_mean = statistics.mean(pooled) if pooled else 0.0
    means = {
        bag: (
            statistics.mean(corrections[bag])
            if corrections[bag]
            else pooled_mean
        )
        for bag in range(1, 5)
    }
    return means, corrections, pooled_mean


def adjusted_rows(
    rows: list[dict[str, Any]], metric: str
) -> tuple[list[dict[str, Any]], dict[int, float], bool]:
    corrections, samples, _ = correction_by_bag(rows, metric)
    direct4_complete = all(
        (not row["disagree"]) or row["direct4"][metric] is not None
        for row in rows
    )
    adjusted = []
    for row in rows:
        if not row["disagree"]:
            value = 0.0
            source = "agreement_exact"
        elif direct4_complete:
            value = float(row["direct4"][metric])
            source = "direct4_measured"
        elif row["direct3"][metric] is not None:
            value = float(row["direct3"][metric]) + corrections[int(row["bag"])]
            source = "direct3_plus_bag_correction"
        else:
            continue
        adjusted.append(
            {
                "position": row["position"],
                "bag": int(row["bag"]),
                "disagree": bool(row["disagree"]),
                "value": value,
                "source": source,
            }
        )
    return adjusted, corrections, direct4_complete


def bootstrap_estimate(
    rows: list[dict[str, Any]],
    metric: str,
    *,
    replicates: int,
    seed_text: str,
) -> tuple[float, list[float], str]:
    complete_direct4 = all(
        (not row["disagree"]) or row["direct4"][metric] is not None
        for row in rows
    )
    by_bag = {
        bag: [row for row in rows if int(row["bag"]) == bag]
        for bag in range(1, 5)
    }
    if any(not bag_rows for bag_rows in by_bag.values()):
        return math.nan, [], "incomplete"
    rng = random.Random(stable_seed(seed_text))
    if complete_direct4:
        point = statistics.mean(
            0.0
            if not row["disagree"]
            else float(row["direct4"][metric])
            for row in rows
        )
        bootstraps = []
        for _ in range(replicates):
            sample = [
                rng.choice(by_bag[bag])
                for bag in range(1, 5)
                for _ in range(len(by_bag[bag]))
            ]
            bootstraps.append(
                statistics.mean(
                    0.0
                    if not row["disagree"]
                    else float(row["direct4"][metric])
                    for row in sample
                )
            )
        return point, bootstraps, "measured_direct4"

    if any(
        row["disagree"] and row["direct3"][metric] is None for row in rows
    ):
        return math.nan, [], "incomplete_direct3"
    correction_means, correction_samples, pooled_mean = correction_by_bag(
        rows, metric
    )
    point = statistics.mean(
        0.0
        if not row["disagree"]
        else float(row["direct3"][metric])
        + correction_means[int(row["bag"])]
        for row in rows
    )
    pooled_samples = [
        value for values in correction_samples.values() for value in values
    ]
    bootstraps = []
    for _ in range(replicates):
        sampled_corrections = {}
        for bag in range(1, 5):
            candidates = correction_samples[bag]
            if candidates:
                sampled_corrections[bag] = statistics.mean(
                    rng.choice(candidates) for _ in range(len(candidates))
                )
            elif pooled_samples:
                sampled_corrections[bag] = statistics.mean(
                    rng.choice(pooled_samples)
                    for _ in range(len(pooled_samples))
                )
            else:
                sampled_corrections[bag] = pooled_mean
        sample = [
            rng.choice(by_bag[bag])
            for bag in range(1, 5)
            for _ in range(len(by_bag[bag]))
        ]
        bootstraps.append(
            statistics.mean(
                0.0
                if not row["disagree"]
                else float(row["direct3"][metric])
                + sampled_corrections[int(row["bag"])]
                for row in sample
            )
        )
    return point, bootstraps, "model_direct3_plus_direct4_correction"


def inference(
    point: float, bootstraps: list[float]
) -> dict[str, float | int]:
    if not bootstraps or math.isnan(point):
        return {"n_bootstrap": 0, "mean": point}
    centered = [value - point for value in bootstraps]
    p_two_sided = (
        1
        + sum(abs(value) >= abs(point) for value in centered)
    ) / (len(centered) + 1)
    return {
        "n_bootstrap": len(bootstraps),
        "mean": point,
        "ci_low": percentile(bootstraps, 0.025),
        "ci_high": percentile(bootstraps, 0.975),
        "p_two_sided_centered_bootstrap": p_two_sided,
    }


def comparison_summary(
    rows: list[dict[str, Any]], name: str, replicates: int
) -> dict[str, Any]:
    result = {
        "positions": len(rows),
        "disagreements": sum(bool(row["disagree"]) for row in rows),
        "disagreement_rate": (
            sum(bool(row["disagree"]) for row in rows) / len(rows)
            if rows
            else math.nan
        ),
        "metrics": {},
        "by_bag": {},
        "direct3_exact_root_wins": {},
        "direct4_common_exact_root_wins": {},
    }
    for metric in METRICS:
        point, bootstraps, source = bootstrap_estimate(
            rows,
            metric,
            replicates=replicates,
            seed_text=f"{name}|{metric}",
        )
        adjusted, correction, direct4_complete = adjusted_rows(rows, metric)
        conditional = [row["value"] for row in adjusted if row["disagree"]]
        direct3_panel = [
            float(row["direct3"][metric])
            for row in rows
            if row["direct3"][metric] is not None
        ]
        direct4_panel = [
            float(row["direct4"][metric])
            for row in rows
            if row["direct4"][metric] is not None
        ]
        result["metrics"][metric] = {
            **inference(point, bootstraps),
            "oracle_source": source,
            "conditional_disagreement": describe(conditional),
            "direct3_panel_mean": (
                statistics.mean(direct3_panel)
                if direct3_panel
                else math.nan
            ),
            "direct4_panel_mean": (
                statistics.mean(direct4_panel)
                if direct4_panel
                else math.nan
            ),
            "direct4_minus_direct3_panel_mean": (
                statistics.mean(direct4_panel)
                - statistics.mean(direct3_panel)
                if len(direct3_panel) == len(direct4_panel)
                and direct3_panel
                else math.nan
            ),
            "bag_direct4_minus_direct3_correction": {
                str(bag): correction[bag] for bag in range(1, 5)
            },
            "direct4_complete_for_all_disagreements": direct4_complete,
        }
        result["by_bag"][metric] = {}
        for bag in range(1, 5):
            bag_values = [
                row["value"]
                for row in adjusted
                if int(row["bag"]) == bag
            ]
            bag_summary = describe(bag_values)
            if bag_values:
                rng = random.Random(
                    stable_seed(f"{name}|{metric}|bag{bag}")
                )
                bag_bootstraps = [
                    statistics.mean(
                        rng.choice(bag_values)
                        for _ in range(len(bag_values))
                    )
                    for _ in range(replicates)
                ]
                bag_summary.update(
                    {
                        "ci_low": percentile(bag_bootstraps, 0.025),
                        "ci_high": percentile(bag_bootstraps, 0.975),
                    }
                )
            result["by_bag"][metric][str(bag)] = bag_summary

    direct3 = [
        float(row["direct3"]["utility"])
        for row in rows
        if row["direct3"]["utility"] is not None
    ]
    direct4_common = [
        float(row["direct4"]["utility"])
        for row in rows
        if row["direct4"]["utility"] is not None
    ]
    result["direct3_exact_root_wins"] = {
        "after": sum(value > 0 for value in direct3),
        "before": sum(value < 0 for value in direct3),
        "ties": sum(value == 0 for value in direct3),
        "n": len(direct3),
    }
    result["direct4_common_exact_root_wins"] = {
        "after": sum(value > 0 for value in direct4_common),
        "before": sum(value < 0 for value in direct4_common),
        "ties": sum(value == 0 for value in direct4_common),
        "n": len(direct4_common),
    }
    return result


def policy_work(policy: dict[str, Any]) -> dict[str, float]:
    work = policy["work"]
    return {
        "completed_candidates": float(work["completed_candidates"]),
        "scenarios": float(work["cumulative_scenarios"]),
        "nested_nodes": float(work["nested_endgame_nodes"]),
        "wall_seconds": float(work["completed_seconds"]),
        "process_cpu_seconds": float(work["process_cpu_seconds"]),
    }


def work_summary(
    maps: list[dict[str, Any]],
    comparisons: dict[str, tuple[str, str]],
    quality: dict[str, Any],
) -> dict[str, Any]:
    result = {"policies": {}, "marginal_efficiency": {}}
    policy_names = list(next(iter(maps))["policies"]) if maps else []
    for policy_name in policy_names:
        rows = [
            policy_work(quality_map["policies"][policy_name])
            for quality_map in maps
        ]
        result["policies"][policy_name] = {
            metric: describe([row[metric] for row in rows])
            for metric in rows[0]
        }
    for name, (before, after) in comparisons.items():
        deltas = []
        for quality_map in maps:
            before_work = policy_work(quality_map["policies"][before])
            after_work = policy_work(quality_map["policies"][after])
            deltas.append(
                {
                    metric: after_work[metric] - before_work[metric]
                    for metric in before_work
                }
            )
        utility = float(quality[name]["metrics"]["utility"]["mean"])
        mean_work = {
            metric: statistics.mean(row[metric] for row in deltas)
            for metric in deltas[0]
        }
        result["marginal_efficiency"][name] = {
            "mean_incremental_work": mean_work,
            "utility_per_million_scenarios": (
                utility / (mean_work["scenarios"] / 1.0e6)
                if mean_work["scenarios"] > 0
                else math.nan
            ),
            "utility_per_million_nested_nodes": (
                utility / (mean_work["nested_nodes"] / 1.0e6)
                if mean_work["nested_nodes"] > 0
                else math.nan
            ),
            "utility_per_local_predicted_second": (
                utility / mean_work["wall_seconds"]
                if mean_work["wall_seconds"] > 0
                else math.nan
            ),
            "seconds_are_local_conversion_only": True,
        }
    return result


def oracle_sensitivity(
    maps: list[dict[str, Any]],
    judges: dict[tuple[str, str], dict[str, Any]],
    values: dict[int, dict[str, dict[str, float]]],
) -> dict[str, Any]:
    common = []
    flips = []
    for quality_map in maps:
        position = str(quality_map["position"])
        nominees = [str(move) for move in quality_map["nominees"]]
        values3 = oracle_values(
            judges, values, position, "fresh_direct3_stride4", nominees
        )
        values4 = oracle_values(
            judges, values, position, "fresh_direct4_stride4", nominees
        )
        if values3 is None or values4 is None:
            continue
        common.append(position)
        best3 = max(nominees, key=lambda move: values3[move]["utility"])
        best4 = max(nominees, key=lambda move: values4[move]["utility"])
        if best3 != best4:
            flips.append(
                {
                    "position": position,
                    "bag": int(quality_map["bag"]),
                    "direct3_best": best3,
                    "direct4_best": best4,
                }
            )
    return {
        "common_positions": len(common),
        "best_nominee_flips": len(flips),
        "best_nominee_flip_rate": (
            len(flips) / len(common) if common else math.nan
        ),
        "flip_details": flips,
    }


def stride_sensitivity(
    maps: list[dict[str, Any]],
    judges: dict[tuple[str, str], dict[str, Any]],
    values: dict[int, dict[str, dict[str, float]]],
) -> dict[str, Any]:
    result: dict[str, Any] = {"depths": {}, "primary_comparisons": {}}
    common_positions: dict[int, list[dict[str, Any]]] = {3: [], 4: []}
    for depth in (3, 4):
        flips = []
        nominee_differences = []
        for quality_map in maps:
            position = str(quality_map["position"])
            nominees = [str(move) for move in quality_map["nominees"]]
            stride4 = oracle_values(
                judges,
                values,
                position,
                f"fresh_direct{depth}_stride4",
                nominees,
            )
            stride2 = oracle_values(
                judges,
                values,
                position,
                f"fresh_direct{depth}_stride2",
                nominees,
            )
            if stride4 is None or stride2 is None:
                continue
            common_positions[depth].append(quality_map)
            best4 = max(
                nominees, key=lambda move: stride4[move]["utility"]
            )
            best2 = max(
                nominees, key=lambda move: stride2[move]["utility"]
            )
            if best4 != best2:
                flips.append(
                    {
                        "position": position,
                        "bag": int(quality_map["bag"]),
                        "stride4_best": best4,
                        "stride2_best": best2,
                    }
                )
            nominee_differences.extend(
                float(stride2[move]["utility"])
                - float(stride4[move]["utility"])
                for move in nominees
            )
        result["depths"][str(depth)] = {
            "common_positions": len(common_positions[depth]),
            "best_nominee_flips": len(flips),
            "best_nominee_flip_rate": (
                len(flips) / len(common_positions[depth])
                if common_positions[depth]
                else math.nan
            ),
            "nominee_utility_stride2_minus_stride4": describe(
                nominee_differences
            ),
            "mean_absolute_nominee_utility_difference": (
                statistics.mean(abs(value) for value in nominee_differences)
                if nominee_differences
                else math.nan
            ),
            "flip_details": flips,
        }

    for name, (before_policy, after_policy) in PRIMARY_COMPARISONS.items():
        differences = []
        sign_flips = 0
        comparable_disagreements = 0
        for quality_map in common_positions[4]:
            position = str(quality_map["position"])
            nominees = [str(move) for move in quality_map["nominees"]]
            stride4 = oracle_values(
                judges,
                values,
                position,
                "fresh_direct4_stride4",
                nominees,
            )
            stride2 = oracle_values(
                judges,
                values,
                position,
                "fresh_direct4_stride2",
                nominees,
            )
            if stride4 is None or stride2 is None:
                continue
            before = str(
                quality_map["policies"][before_policy]["move"]
            )
            after = str(quality_map["policies"][after_policy]["move"])
            delta4 = (
                0.0
                if before == after
                else delta(stride4, before, after, "utility")
            )
            delta2 = (
                0.0
                if before == after
                else delta(stride2, before, after, "utility")
            )
            differences.append(delta2 - delta4)
            if before != after:
                comparable_disagreements += 1
                if delta4 * delta2 < 0:
                    sign_flips += 1
        result["primary_comparisons"][name] = {
            "positions": len(differences),
            "policy_disagreements": comparable_disagreements,
            "utility_stride2_minus_stride4": describe(differences),
            "sign_flips": sign_flips,
        }
    return result


def sentinel_diagnostics(records: list[dict[str, Any]]) -> dict[str, Any]:
    sentinels = sorted(
        (
            record
            for record in records
            if record.get("kind") == "arm"
            and record.get("mode") == "sentinel"
            and record.get("position") == "fresh-quality-sentinel"
            and record.get("status") == "completed"
        ),
        key=lambda row: int(row["sequence"]),
    )
    if not sentinels:
        return {"n": 0}
    throughput = [
        float(row["nested_endgame_nodes"]) / float(row["wall_seconds"])
        for row in sentinels
    ]
    median = statistics.median(throughput)
    normalized = [value / median for value in throughput]
    mean_normalized = statistics.mean(normalized)
    cv = (
        statistics.pstdev(normalized) / mean_normalized
        if len(normalized) > 1 and mean_normalized
        else 0.0
    )
    deviations = [abs(value - 1.0) for value in normalized]
    mad = statistics.median(deviations)
    noisy_threshold = max(0.20, 3.0 * mad)
    width = max(1, len(normalized) // 4)
    early = statistics.mean(normalized[:width])
    late = statistics.mean(normalized[-width:])
    return {
        "n": len(sentinels),
        "normalized_throughput_cv": cv,
        "early_to_late_drift_fraction": (
            late / early - 1.0 if early else math.nan
        ),
        "scheduled_core_occupancy": describe(
            [float(row["scheduled_core_occupancy"]) for row in sentinels]
        ),
        "noisy_threshold_fraction_from_median": noisy_threshold,
        "noisy_segments": [
            {
                "label": str(row["label"]),
                "sequence": int(row["sequence"]),
                "normalized_throughput": value,
            }
            for row, value in zip(sentinels, normalized, strict=True)
            if abs(value - 1.0) > noisy_threshold
        ],
    }


def analyze(
    records: list[dict[str, Any]], replicates: int
) -> dict[str, Any]:
    maps_by_position = {
        str(record["position"]): record
        for record in records
        if record.get("kind") == "fresh_quality_map"
    }
    maps = sorted(
        maps_by_position.values(),
        key=lambda row: (int(row["bag"]), str(row["position"])),
    )
    judges, values = values_index(records)
    comparisons = dict(PRIMARY_COMPARISONS)
    comparisons.update(STOPPING_COMPARISONS)
    comparisons.update(STAGE_COMPARISONS)
    comparisons.update(
        {
            f"full32_minus_fixed{count}": (
                f"fixed_{count}",
                "fixed_32",
            )
            for count in CURVE_COUNTS
        }
    )
    quality = {}
    for name, (before, after) in comparisons.items():
        rows = build_comparison_rows(
            maps, judges, values, before, after
        )
        quality[name] = comparison_summary(rows, name, replicates)

    primary = quality["full32_minus_adaptive"]
    utility = primary["metrics"]["utility"]
    noninferiority = {}
    # Positive values are regret of adaptive relative to full 32.
    point, bootstraps, source = bootstrap_estimate(
        build_comparison_rows(
            maps,
            judges,
            values,
            "min8_patience8",
            "fixed_32",
        ),
        "utility",
        replicates=replicates,
        seed_text="noninferiority",
    )
    for margin in (0.00025, 0.0005, 0.001):
        noninferiority[f"{margin:.5f}"] = {
            "margin": margin,
            "mean_regret": point,
            "upper_95": percentile(bootstraps, 0.975)
            if bootstraps
            else math.nan,
            "noninferior": bool(
                bootstraps
                and percentile(bootstraps, 0.975) < margin
            ),
            "one_sided_bootstrap_p": (
                (1 + sum(value >= margin for value in bootstraps))
                / (len(bootstraps) + 1)
                if bootstraps
                else math.nan
            ),
            "oracle_source": source,
        }

    return {
        "schema_version": 1,
        "panel_positions": len(maps),
        "per_bag": {
            str(bag): sum(int(row["bag"]) == bag for row in maps)
            for bag in range(1, 5)
        },
        "source_game_clusters": (
            "one position per independent source game; position and source "
            "cluster are identical"
        ),
        "bootstrap": {
            "replicates": replicates,
            "method": (
                "bag-stratified source-game bootstrap; model mode separately "
                "resamples the direct-4 correction subset"
            ),
        },
        "oracle_accounting": {
            label: {
                "accepted": sum(
                    record.get("kind") == "judge"
                    and record.get("label") == label
                    and int(record.get("accepted", 0)) == 1
                    for record in records
                ),
                "partial_or_rejected": sum(
                    record.get("kind") == "judge"
                    and record.get("label") == label
                    and int(record.get("accepted", 0)) != 1
                    for record in records
                ),
            }
            for label in (
                "fresh_direct3_stride4",
                "fresh_direct4_stride4",
                "fresh_direct3_stride2",
                "fresh_direct4_stride2",
            )
        },
        "quality": quality,
        "primary_noninferiority": noninferiority,
        "work": work_summary(maps, comparisons, quality) if maps else {},
        "oracle_sensitivity": oracle_sensitivity(
            maps, judges, values
        ),
        "stride_sensitivity": stride_sensitivity(
            maps, judges, values
        ),
        "sentinels": sentinel_diagnostics(records),
        "interpretation": (
            "measured when direct 4 covers every disagreement; otherwise "
            "model-based direct-3 full panel plus bag-conditioned direct-4 "
            "correction with both sampling stages bootstrapped"
        ),
    }


def fmt_ci(metric: dict[str, Any], scale: float = 1.0) -> str:
    if "ci_low" not in metric:
        return "NA"
    return (
        f"{metric['mean'] * scale:+.5f} "
        f"[{metric['ci_low'] * scale:+.5f}, "
        f"{metric['ci_high'] * scale:+.5f}]"
    )


def make_markdown(result: dict[str, Any]) -> str:
    lines = [
        "# Fresh representative PEG quality calibration",
        "",
        f"Frozen panel maps: {result['panel_positions']}/200. "
        f"Oracle interpretation: {result['interpretation']}.",
        "",
        "Agreements enter every panel-wide estimate as exact zero. CIs use a "
        "bag-stratified source-game bootstrap; every panel position has a "
        "different source game.",
        "",
        "## Primary comparisons",
        "",
        "| comparison | disagreements | utility (95% CI) | win pp (95% CI) | "
        "spread (95% CI) | oracle basis |",
        "|---|---:|---:|---:|---:|---|",
    ]
    for name in PRIMARY_COMPARISONS:
        row = result["quality"][name]
        lines.append(
            f"| {name} | {row['disagreements']}/{row['positions']} | "
            f"{fmt_ci(row['metrics']['utility'])} | "
            f"{fmt_ci(row['metrics']['win'], 100.0)} | "
            f"{fmt_ci(row['metrics']['spread'])} | "
            f"{row['metrics']['utility']['oracle_source']} |"
        )
    lines.extend(
        [
            "",
            "Panel-wide values above include agreements as exact zero. "
            "Conditional disagreement means and exact root-win counts are:",
            "",
            "| comparison | conditional utility | conditional win pp | "
            "conditional spread | direct-4 after/before/tie |",
            "|---|---:|---:|---:|---:|",
        ]
    )
    for name in PRIMARY_COMPARISONS:
        row = result["quality"][name]
        wins = row["direct4_common_exact_root_wins"]
        lines.append(
            f"| {name} | "
            f"{row['metrics']['utility']['conditional_disagreement']['mean']:+.5f} | "
            f"{row['metrics']['win']['conditional_disagreement']['mean'] * 100:+.3f} | "
            f"{row['metrics']['spread']['conditional_disagreement']['mean']:+.3f} | "
            f"{wins['after']}/{wins['before']}/{wins['ties']} |"
        )
    lines.extend(
        [
            "",
            "### Bag-conditioned primary effects",
            "",
            "| comparison | bag | utility (95% CI) | win pp (95% CI) | "
            "spread (95% CI) |",
            "|---|---:|---:|---:|---:|",
        ]
    )
    for name in PRIMARY_COMPARISONS:
        row = result["quality"][name]
        for bag in range(1, 5):
            key = str(bag)
            lines.append(
                f"| {name} | {bag} | "
                f"{fmt_ci(row['by_bag']['utility'][key])} | "
                f"{fmt_ci(row['by_bag']['win'][key], 100.0)} | "
                f"{fmt_ci(row['by_bag']['spread'][key])} |"
            )
    lines.extend(
        [
            "",
            "## Adaptive noninferiority",
            "",
            "Regret is `full 32 - min 8 / patience 8`; noninferiority requires "
            "the bootstrap upper 95% bound to be below the margin.",
            "",
            "| margin | mean regret | upper 95% | noninferior | one-sided p |",
            "|---:|---:|---:|---|---:|",
        ]
    )
    for row in result["primary_noninferiority"].values():
        lines.append(
            f"| {row['margin']:.5f} | {row['mean_regret']:+.6f} | "
            f"{row['upper_95']:+.6f} | {row['noninferior']} | "
            f"{row['one_sided_bootstrap_p']:.4f} |"
        )
    lines.extend(
        [
            "",
            "## Candidate-count regret curve",
            "",
            "| completed 2-ply candidates | disagreement with full 32 | "
            "full-minus-checkpoint utility (95% CI) |",
            "|---:|---:|---:|",
        ]
    )
    for count in CURVE_COUNTS:
        row = result["quality"][f"full32_minus_fixed{count}"]
        lines.append(
            f"| {count} | {row['disagreements']}/{row['positions']} | "
            f"{fmt_ci(row['metrics']['utility'])} |"
        )
    lines.extend(
        [
            "",
            "## Value of each completed 2-ply stage",
            "",
            "| added completed candidates | panel utility gain (95% CI) | "
            "disagreements | mean added scenarios | mean added nested nodes | "
            "M5 mean added s |",
            "|---|---:|---:|---:|---:|---:|",
        ]
    )
    stage_rows = [
        ("2→4", "fixed4_minus_fixed2"),
        ("4→8", "fixed8_minus_fixed4"),
        ("8→12", "fixed12_minus_fixed8"),
        ("12→32", "full32_minus_fixed12"),
    ]
    for label, name in stage_rows:
        quality = result["quality"][name]
        work = result["work"]["marginal_efficiency"][name][
            "mean_incremental_work"
        ]
        lines.append(
            f"| {label} | {fmt_ci(quality['metrics']['utility'])} | "
            f"{quality['disagreements']}/{quality['positions']} | "
            f"{work['scenarios']:.0f} | {work['nested_nodes']:.0f} | "
            f"{work['wall_seconds']:.2f} |"
        )
    lines.extend(
        [
            "",
            "## Adaptive stopping choices",
            "",
            "| stop policy | disagreement with full 32 | full-minus-stop "
            "utility (95% CI) | mean completed candidates | mean scenarios | "
            "mean nested nodes | M5 mean s |",
            "|---|---:|---:|---:|---:|---:|---:|",
        ]
    )
    stopping_rows = [
        ("min 8 / patience 4", "min8_patience4", "full32_minus_min8p4"),
        ("min 8 / patience 8", "min8_patience8", "full32_minus_adaptive"),
        ("min 12 / patience 8", "min12_patience8", "full32_minus_min12p8"),
        ("full 32", "fixed_32", "full32_minus_fixed32"),
    ]
    for label, policy, comparison in stopping_rows:
        quality = result["quality"][comparison]
        work = result["work"]["policies"][policy]
        lines.append(
            f"| {label} | {quality['disagreements']}/"
            f"{quality['positions']} | "
            f"{fmt_ci(quality['metrics']['utility'])} | "
            f"{work['completed_candidates']['mean']:.2f} | "
            f"{work['scenarios']['mean']:.0f} | "
            f"{work['nested_nodes']['mean']:.0f} | "
            f"{work['wall_seconds']['mean']:.2f} |"
        )
    lines.extend(
        [
            "",
            "| adaptive extension | panel utility gain (95% CI) | "
            "disagreements | mean added candidates | mean added nested nodes | "
            "M5 mean added s |",
            "|---|---:|---:|---:|---:|---:|",
        ]
    )
    adaptive_extensions = [
        ("patience 4→8 after min 8", "min8p8_minus_min8p4"),
        ("min 8→12 at patience 8", "min12p8_minus_min8p8"),
    ]
    for label, name in adaptive_extensions:
        quality = result["quality"][name]
        work = result["work"]["marginal_efficiency"][name][
            "mean_incremental_work"
        ]
        lines.append(
            f"| {label} | {fmt_ci(quality['metrics']['utility'])} | "
            f"{quality['disagreements']}/{quality['positions']} | "
            f"{work['completed_candidates']:.2f} | "
            f"{work['nested_nodes']:.0f} | {work['wall_seconds']:.2f} |"
        )
    sensitivity = result["oracle_sensitivity"]
    stride = result["stride_sensitivity"]
    lines.extend(
        [
            "",
            "## Oracle sensitivity",
            "",
            f"Common direct-3/direct-4 positions: "
            f"{sensitivity['common_positions']}; best-nominee flips: "
            f"{sensitivity['best_nominee_flips']} "
            f"({sensitivity['best_nominee_flip_rate']:.1%}).",
            "",
            "| comparison | direct-3 panel utility | direct-4 panel utility | "
            "direct-4 minus direct-3 |",
            "|---|---:|---:|---:|",
        ]
    )
    for name in PRIMARY_COMPARISONS:
        metric = result["quality"][name]["metrics"]["utility"]
        lines.append(
            f"| {name} | {metric['direct3_panel_mean']:+.5f} | "
            f"{metric['direct4_panel_mean']:+.5f} | "
            f"{metric['direct4_minus_direct3_panel_mean']:+.5f} |"
        )
    lines.extend(
        [
            "",
            "Stride-2 sensitivity was prospectively checked on two positions "
            "per bag:",
            "",
            "| depth | common | stride-4/stride-2 best flips | mean absolute "
            "nominee utility difference |",
            "|---:|---:|---:|---:|",
        ]
    )
    for depth in ("3", "4"):
        row = stride["depths"][depth]
        lines.append(
            f"| {depth} | {row['common_positions']} | "
            f"{row['best_nominee_flips']} "
            f"({row['best_nominee_flip_rate']:.1%}) | "
            f"{row['mean_absolute_nominee_utility_difference']:.5f} |"
        )
    lines.extend(
        [
            "",
            "## Work interpretation",
            "",
            "Scenarios, nested nodes, and completed candidates are the "
            "strength/work axes. Utility per second is an M5 scheduling "
            "conversion only; selected-machine NPS is never a strength feature.",
            "",
            "| comparison | utility / M scenarios | utility / M nested nodes | "
            "utility / M5 local s |",
            "|---|---:|---:|---:|",
        ]
    )
    for name in PRIMARY_COMPARISONS:
        row = result["work"]["marginal_efficiency"][name]
        lines.append(
            f"| {name} | {row['utility_per_million_scenarios']:.6f} | "
            f"{row['utility_per_million_nested_nodes']:.6f} | "
            f"{row['utility_per_local_predicted_second']:.6f} |"
        )
    sentinels = result["sentinels"]
    lines.extend(
        [
            "",
            "## Load diagnostics",
            "",
            f"{sentinels['n']} identical sentinels: normalized-throughput CV "
            f"{sentinels['normalized_throughput_cv']:.3f}; early/late drift "
            f"{sentinels['early_to_late_drift_fraction']:+.1%}; median "
            f"scheduled-core occupancy "
            f"{sentinels['scheduled_core_occupancy']['median']:.3f}. "
            f"Noisy segments: {len(sentinels['noisy_segments'])}.",
            "",
            "Timing from a noisy segment is only a local conversion; completed "
            "quality observations remain structurally valid.",
            "",
        ]
    )
    return "\n".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--records",
        type=pathlib.Path,
        default=pathlib.Path("obj/peg_quality_20260730/records.jsonl"),
    )
    parser.add_argument("--json", type=pathlib.Path, required=True)
    parser.add_argument("--markdown", type=pathlib.Path, required=True)
    parser.add_argument("--bootstrap", type=int, default=5000)
    args = parser.parse_args()
    repo = pathlib.Path(__file__).resolve().parent.parent
    records_path = (
        args.records if args.records.is_absolute() else repo / args.records
    )
    output_json = args.json if args.json.is_absolute() else repo / args.json
    output_markdown = (
        args.markdown
        if args.markdown.is_absolute()
        else repo / args.markdown
    )
    result = analyze(load_records(records_path), args.bootstrap)
    output_json.parent.mkdir(parents=True, exist_ok=True)
    output_markdown.parent.mkdir(parents=True, exist_ok=True)
    output_json.write_text(
        json.dumps(result, indent=2, sort_keys=True) + "\n"
    )
    output_markdown.write_text(make_markdown(result))
    print(
        f"analyzed fresh quality maps={result['panel_positions']} "
        f"direct3={result['oracle_accounting']['fresh_direct3_stride4']['accepted']} "
        f"direct4={result['oracle_accounting']['fresh_direct4_stride4']['accepted']}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
