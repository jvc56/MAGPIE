#!/usr/bin/env python3
"""Analyze the preregistered judge-light Rule-of-Zero prospective panel."""

from __future__ import annotations

import argparse
from collections import defaultdict
import json
import math
from pathlib import Path
import statistics

try:
    from tools.clustered_inference import (
        regularized_incomplete_beta,
        student_t_critical,
    )
    from tools.extract_time_value_positions import fields
except ModuleNotFoundError:  # Direct execution from tools/.
    from clustered_inference import (  # type: ignore[no-redef]
        regularized_incomplete_beta,
        student_t_critical,
    )
    from extract_time_value_positions import fields  # type: ignore[no-redef]

# Historical-minimum equal-slice landmark (preregistered): reported mismatch
# sensitivity only, never gated. The 6M gate landmark and 8M full horizon come
# from the harness's own equal_horizon and full points.
SENSITIVITY_LANDMARK_NODES = 3652554


def clopper_pearson_upper(events: int, trials: int, confidence: float = 0.95) -> float:
    """Exact one-sided binomial upper confidence limit."""

    if not 0 <= events <= trials or trials <= 0 or not 0.0 < confidence < 1.0:
        raise ValueError("invalid binomial confidence-limit arguments")
    if events == trials:
        return 1.0
    target = confidence
    a = events + 1.0
    b = trials - events
    low = 0.0
    high = 1.0
    for _ in range(100):
        midpoint = (low + high) / 2.0
        if regularized_incomplete_beta(a, b, midpoint) < target:
            low = midpoint
        else:
            high = midpoint
    return (low + high) / 2.0


def mean_interval(values: list[float]) -> dict[str, object]:
    if not values or any(not math.isfinite(value) for value in values):
        raise ValueError("mean interval needs finite observations")
    mean = statistics.fmean(values)
    if len(values) == 1:
        radius = 0.0
    else:
        radius = student_t_critical(0.95, len(values) - 1) * (
            statistics.stdev(values) / math.sqrt(len(values))
        )
    return {
        "observations": len(values),
        "mean": mean,
        "ci95": [mean - radius, mean + radius],
    }


def mismatch_summary(values: list[int]) -> dict[str, object]:
    events = sum(values)
    return {
        "trials": len(values),
        "events": events,
        "rate": events / len(values),
        "one_sided_95_upper": clopper_pearson_upper(events, len(values)),
    }


def _manifest_metadata(path: Path) -> dict[int, dict[str, object]]:
    document = json.loads(path.read_text(encoding="utf-8"))
    mapping = document.get("mapping")
    if not isinstance(mapping, list) or not mapping:
        raise ValueError("selection manifest lacks root mapping")
    result = {int(row["position"]): row for row in mapping}
    if len(result) != len(mapping):
        raise ValueError("selection manifest repeats a position")
    identities = {(str(row["source_seed"]), str(row["start"])) for row in mapping}
    if len(identities) != len(mapping):
        raise ValueError("analysis requires one root per complete source game")
    return result


def analyze(log: Path, manifest: Path) -> dict[str, object]:
    metadata = _manifest_metadata(manifest)
    rules: dict[int, dict[str, str]] = {}
    points: dict[int, dict[str, dict[str, str]]] = defaultdict(dict)
    done: dict[int, dict[str, str]] = {}
    shadow_rows = 0
    checkpoint_marks: dict[int, list[tuple[int, int]]] = defaultdict(list)
    with log.open(encoding="utf-8") as stream:
        for line in stream:
            if line.startswith("THINKING_CURVE_RULE_ZERO_CHECKPOINT "):
                row = fields(line)
                checkpoint_marks[int(row["source_index"])].append(
                    (int(row["nodes"]), int(row["selected_rank"]))
                )
            elif line.startswith("THINKING_CURVE_RULE_ZERO "):
                row = fields(line)
                source = int(row["source_index"])
                if source in rules:
                    raise ValueError("duplicate Rule-of-Zero summary")
                rules[source] = row
            elif line.startswith("THINKING_CURVE_POINT "):
                row = fields(line)
                if row.get("final") != "0":
                    continue
                source = int(row["source_index"])
                kind = row["control_kind"]
                if kind in points[source]:
                    raise ValueError("duplicate control point")
                points[source][kind] = row
            elif line.startswith("THINKING_CURVE_POSITION_DONE "):
                row = fields(line)
                source = int(row["source_index"])
                if source in done:
                    raise ValueError("duplicate done row")
                done[source] = row
            elif line.startswith("THINKING_CURVE_HORIZON_SHADOW "):
                shadow_rows += 1
    roots = set(rules)
    if roots != set(done) or roots != set(metadata) or not roots:
        raise ValueError("analysis roots do not match selection manifest")

    horizon_mismatches: list[int] = []
    equal_mismatches: list[int] = []
    horizon_missed_values: list[float] = []
    equal_missed_values: list[float] = []
    node_savings: list[float] = []
    landmark_savings: list[float] = []
    sensitivity_mismatches: list[int] = []
    matched_bias: list[float] = []
    stopped_early = 0
    judged = 0
    audit = 0
    stop_in_risk_set = 0
    stop_out_of_risk_set_mismatches = 0
    strata: dict[tuple[str, str], list[int]] = defaultdict(list)
    for source in sorted(roots):
        rule = rules[source]
        root_points = points[source]
        required = {"stopped", "full", "equal_horizon"}
        if not required <= set(root_points):
            raise ValueError(f"source {source} lacks decision points")
        stopped = root_points["stopped"]
        full = root_points["full"]
        equal = root_points["equal_horizon"]
        horizon_mismatch = int(stopped["selected_rank"] != full["selected_rank"])
        equal_mismatch = int(stopped["selected_rank"] != equal["selected_rank"])
        if horizon_mismatch != int(rule["horizon_mismatch"]):
            raise ValueError("horizon mismatch does not reconcile")
        if equal_mismatch != int(rule["equal_horizon_mismatch"]):
            raise ValueError("equal-horizon mismatch does not reconcile")
        horizon_mismatches.append(horizon_mismatch)
        equal_mismatches.append(equal_mismatch)
        horizon_missed_values.append(
            float(stopped["judge_regret"]) - float(full["judge_regret"])
        )
        equal_missed_values.append(
            float(stopped["judge_regret"]) - float(equal["judge_regret"])
        )
        full_nodes = int(full["nodes"])
        if full_nodes <= 0:
            raise ValueError("full arm has no work")
        node_savings.append(1.0 - int(stopped["nodes"]) / full_nodes)
        equal_nodes = int(equal["nodes"])
        if equal_nodes <= 0:
            raise ValueError("equal-slice landmark has no work")
        # Usefulness-floor savings are measured against the 6M gate landmark
        # (the equal-slice point), not the full 8M horizon.
        landmark_savings.append(1.0 - int(stopped["nodes"]) / equal_nodes)
        # Reported sensitivity: mismatch against the historical-minimum
        # landmark, derived from checkpoint rows with the same first-
        # checkpoint-at-or-past-target rule the harness uses for landmarks.
        marks = sorted(checkpoint_marks.get(source, []))
        sensitivity_rank = int(full["selected_rank"])
        if SENSITIVITY_LANDMARK_NODES < full_nodes:
            for mark_nodes, mark_rank in marks:
                if mark_nodes >= SENSITIVITY_LANDMARK_NODES:
                    sensitivity_rank = mark_rank
                    break
        sensitivity_mismatches.append(
            int(int(stopped["selected_rank"]) != sensitivity_rank)
        )
        stopped_early += int(rule["capped"]) == 0
        judged += int(rule["judge_performed"])
        audit += int(rule["audit_selected"])
        # Risk-set membership at the stopping checkpoint (packet section 6):
        # count uncapped stops whose full-horizon selection was in the risk
        # set, and mismatches where it was not (the ambiguity class the p2
        # panel could not resolve).
        if int(rule["capped"]) == 0:
            in_risk_set = int(rule["stop_full_horizon_rank_in_risk_set"])
            stop_in_risk_set += in_risk_set
            if horizon_mismatch and not in_risk_set:
                stop_out_of_risk_set_mismatches += 1
        if int(rule["matched_control"]):
            matched = root_points.get("matched")
            if matched is None:
                raise ValueError("matched subset root lacks its control")
            matched_bias.append(
                float(stopped["judge_regret"]) - float(matched["judge_regret"])
            )
        info = metadata[int(rule["position"])]
        strata[(str(info["trajectory_policy"]), str(info["bag_band"]))].append(
            horizon_mismatch
        )

    horizon = mismatch_summary(horizon_mismatches)
    equal = mismatch_summary(equal_mismatches)
    sensitivity = mismatch_summary(sensitivity_mismatches)
    horizon_value = mean_interval(horizon_missed_values)
    equal_value = mean_interval(equal_missed_values)
    savings = mean_interval(node_savings)
    savings_at_landmark = mean_interval(landmark_savings)
    matched = mean_interval(matched_bias)
    # Primary gate (notes/rule_zero_primary_gate_decision.md): judged missed
    # value at the 0.001 threshold. Mismatch counts are reported secondaries
    # with exact bounds and are never gated.
    value_gate = (
        float(horizon_value["ci95"][1]) <= 0.001
        and float(equal_value["ci95"][1]) <= 0.001
    )
    matched_gate = (
        float(matched["ci95"][0]) <= 0.0 <= float(matched["ci95"][1])
        and float(matched["ci95"][1]) <= 0.001
    )
    # Usefulness floor (packet section 7.2): the rule must stop at least 30%
    # of roots and save at least 25% of nodes at the 6M gate landmark.
    stop_rate = stopped_early / len(roots)
    usefulness_floor = (
        stop_rate >= 0.30 and float(savings_at_landmark["mean"]) >= 0.25
    )
    return {
        "artifact_kind": "rule_zero_prospective_analysis",
        "roots": len(roots),
        "complete_game_inference_units": len(roots),
        "stopped_early": stopped_early,
        "judged_roots": judged,
        "random_audit_roots": audit,
        "shadow_checkpoint_rows": shadow_rows,
        "stop_full_horizon_rank_in_risk_set": {
            "in_risk_set": stop_in_risk_set,
            "stops": stopped_early,
            "rate": stop_in_risk_set / stopped_early if stopped_early else None,
            "out_of_risk_set_mismatches": stop_out_of_risk_set_mismatches,
        },
        "full_horizon_mismatch": horizon,
        "equal_slice_landmark_mismatch": equal,
        "historical_minimum_landmark_mismatch_sensitivity": sensitivity,
        "full_horizon_judged_missed_value": horizon_value,
        "equal_slice_judged_missed_value": equal_value,
        "node_savings_vs_full_horizon": savings,
        "node_savings_at_gate_landmark": savings_at_landmark,
        "stop_rate": stop_rate,
        "matched_optional_stopping_bias": matched,
        "strata": {
            "|".join(key): mismatch_summary(values)
            for key, values in sorted(strata.items())
        },
        "gates": {
            "primary_judged_missed_value_upper_at_most_0.001": value_gate,
            "usefulness_floor_stop_rate_0.30_savings_0.25_at_landmark": (
                usefulness_floor
            ),
            "matched_subset_no_harmful_bias": matched_gate,
            "secondary_mismatch_reported_not_gated": {
                "full_horizon_one_sided_95_upper": horizon[
                    "one_sided_95_upper"
                ],
                "equal_slice_one_sided_95_upper": equal["one_sided_95_upper"],
            },
            "panel_pass": value_gate and usefulness_floor and matched_gate,
        },
        "live_allocation": "disabled_pending_separate_terminal_game_gate",
    }


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", type=Path)
    parser.add_argument("--manifest", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    result = analyze(args.log, args.manifest)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(
        json.dumps(result, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    print(json.dumps(result, indent=2, sort_keys=True))


if __name__ == "__main__":
    main()
