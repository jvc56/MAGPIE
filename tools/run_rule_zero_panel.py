#!/usr/bin/env python3
"""Run the preregistered judge-light Rule-of-Zero prospective panel.

Each root is computed in a private subprocess.  Raw output is strictly audited,
then decorated with label-free shadow predictions from the previously frozen
horizon-differential model, audited again, and only then appended to the
canonical log.  A killed or malformed root is never accepted.
"""

from __future__ import annotations

import argparse
from collections import defaultdict
import json
import math
import os
from pathlib import Path
import subprocess
import time

try:
    from tools.extract_time_value_positions import fields
    from tools.fit_horizon_differential_regret import (
        HorizonModel,
        HorizonRow,
        load_horizon_model_artifact,
        predict,
    )
    from tools.run_stratified_thinking_curve_panel import (
        completed_by_plies,
        eligible_indices,
        sha256,
        validate_prefix,
        write_state,
    )
    from tools.select_matched_sim_tail_panel import bag_band
except ModuleNotFoundError:  # Direct execution from tools/.
    from extract_time_value_positions import fields  # type: ignore[no-redef]
    from fit_horizon_differential_regret import (  # type: ignore[no-redef]
        HorizonModel,
        HorizonRow,
        load_horizon_model_artifact,
        predict,
    )
    from run_stratified_thinking_curve_panel import (  # type: ignore[no-redef]
        completed_by_plies,
        eligible_indices,
        sha256,
        validate_prefix,
        write_state,
    )
    from select_matched_sim_tail_panel import bag_band  # type: ignore[no-redef]


def _int(row: dict[str, str], key: str) -> int:
    try:
        return int(row[key])
    except (KeyError, ValueError) as error:
        raise ValueError(f"missing or invalid integer field {key}") from error


def _float(row: dict[str, str], key: str) -> float:
    try:
        return float(row[key])
    except (KeyError, ValueError) as error:
        raise ValueError(f"missing or invalid float field {key}") from error


def _finite_checkpoint(row: dict[str, str]) -> bool:
    values = (
        _float(row, "estimated_best"),
        _float(row, "estimated_challenger"),
        _float(row, "estimated_regret"),
        _float(row, "joint_estimated_regret"),
    )
    return (
        _int(row, "counters_valid") == 1
        and all(math.isfinite(value) for value in values)
        and values[2] >= 0.0
        and values[3] >= 0.0
        and _int(row, "near_tie_challengers") >= 0
    )


def _shadow_row(
    checkpoint: dict[str, str],
    position: dict[str, str],
    rule: dict[str, str],
) -> HorizonRow:
    best = _float(checkpoint, "estimated_best")
    challenger = _float(checkpoint, "estimated_challenger")
    bag = _int(checkpoint, "bag")
    band = bag_band(bag)
    if band is None:
        raise ValueError("Rule-of-Zero checkpoint is outside a SIM bag band")
    return HorizonRow(
        source_index=_int(checkpoint, "source_index"),
        game=f"prospective-{checkpoint['source_index']}",
        policy=position["trajectory_policy"],
        bag=bag,
        bag_band=band,
        width=_int(checkpoint, "arm_plays"),
        checkpoint=_int(checkpoint, "checkpoint"),
        iterations=_int(checkpoint, "iterations"),
        nodes=_int(checkpoint, "nodes"),
        horizon_iterations=_int(rule, "full_iterations"),
        horizon_nodes=_int(rule, "full_nodes"),
        landmark_nodes=0,
        static_cutoff_gap=_float(position, "static_primary_gap"),
        estimated_regret=_float(checkpoint, "estimated_regret"),
        joint_estimated_regret=_float(checkpoint, "joint_estimated_regret"),
        bai_gap=max(0.0, best - challenger),
        near_tie_challengers=_int(checkpoint, "near_tie_challengers"),
        stable_checkpoints=_int(checkpoint, "stable_checkpoints"),
        stable_iterations=_int(checkpoint, "stable_iterations"),
        selected_switches=_int(checkpoint, "selected_switches"),
        selected_rank=_int(checkpoint, "selected_rank"),
        # Horizon identities and judge values are labels.  They are deliberately
        # replaced by inert placeholders for prospective shadow scoring.
        horizon_selected_rank=_int(checkpoint, "selected_rank"),
        current_judge_regret=0.0,
        horizon_judge_regret=0.0,
        horizon_mismatch=0,
        signed_horizon_improvement=0.0,
    )


def decorate_shadow_scores(
    path: Path, model: HorizonModel | None, model_sha256: str
) -> None:
    if model is None:
        return
    text = path.read_text(encoding="utf-8")
    if "THINKING_CURVE_HORIZON_SHADOW " in text:
        raise ValueError("chunk already contains shadow-score rows")
    positions: dict[int, dict[str, str]] = {}
    rules: dict[int, dict[str, str]] = {}
    checkpoints: dict[int, list[dict[str, str]]] = defaultdict(list)
    for line in text.splitlines():
        if line.startswith("THINKING_CURVE_POSITION "):
            row = fields(line)
            positions[_int(row, "source_index")] = row
        elif line.startswith("THINKING_CURVE_RULE_ZERO_CHECKPOINT "):
            row = fields(line)
            checkpoints[_int(row, "source_index")].append(row)
        elif line.startswith("THINKING_CURVE_RULE_ZERO "):
            row = fields(line)
            rules[_int(row, "source_index")] = row
    if set(positions) != set(rules) or set(positions) != set(checkpoints):
        raise ValueError("cannot shadow-score incomplete Rule-of-Zero output")
    shadow_lines: list[str] = []
    for source in sorted(positions):
        for checkpoint in sorted(
            checkpoints[source], key=lambda row: _int(row, "checkpoint")
        ):
            valid = _finite_checkpoint(checkpoint)
            if valid:
                item = predict(
                    model, _shadow_row(checkpoint, positions[source], rules[source]), -1
                )
                probability = item.mismatch_probability
                severity = item.conditional_signed_improvement
                expected = item.expected_signed_improvement
                score = item.stopping_score
            else:
                probability = severity = expected = score = math.nan
            shadow_lines.append(
                "THINKING_CURVE_HORIZON_SHADOW "
                f"source_index={source} position={checkpoint['position']} "
                f"game={checkpoint['game']} bag={checkpoint['bag']} "
                f"plies={checkpoint['plies']} checkpoint={checkpoint['checkpoint']} "
                f"iterations={checkpoint['iterations']} nodes={checkpoint['nodes']} "
                f"mismatch_probability={probability:.17g} "
                f"conditional_signed_improvement={severity:.17g} "
                f"expected_signed_improvement={expected:.17g} "
                f"stopping_score={score:.17g} valid={int(valid)} "
                f"model_sha256={model_sha256} decision_role=shadow_only\n"
            )
    path.write_text(text + "".join(shadow_lines), encoding="utf-8")


def _parse_chunk(path: Path, plies: int) -> dict[str, object]:
    text = path.read_text(encoding="utf-8")
    if "THINKING_CURVE_DONE " not in text:
        raise ValueError("successful chunk lacks THINKING_CURVE_DONE")
    parsed: dict[str, object] = {
        "positions": {},
        "checkpoints": defaultdict(list),
        "rules": {},
        "points": defaultdict(list),
        "done": {},
        "judges": defaultdict(list),
        "shadows": defaultdict(list),
    }
    positions = parsed["positions"]
    checkpoints = parsed["checkpoints"]
    rules = parsed["rules"]
    points = parsed["points"]
    done = parsed["done"]
    judges = parsed["judges"]
    shadows = parsed["shadows"]
    assert isinstance(positions, dict)
    assert isinstance(checkpoints, defaultdict)
    assert isinstance(rules, dict)
    assert isinstance(points, defaultdict)
    assert isinstance(done, dict)
    assert isinstance(judges, defaultdict)
    assert isinstance(shadows, defaultdict)
    for line in text.splitlines():
        row: dict[str, str]
        if line.startswith("THINKING_CURVE_FORCED_POSITION "):
            raise ValueError("prospective panel cannot accept a forced root")
        if line.startswith("THINKING_CURVE_POSITION "):
            row = fields(line)
            if _int(row, "plies") == plies:
                source = _int(row, "source_index")
                if source in positions:
                    raise ValueError("duplicate Rule-of-Zero position row")
                positions[source] = row
        elif line.startswith("THINKING_CURVE_RULE_ZERO_CHECKPOINT "):
            row = fields(line)
            if _int(row, "plies") == plies:
                checkpoints[_int(row, "source_index")].append(row)
        elif line.startswith("THINKING_CURVE_RULE_ZERO "):
            row = fields(line)
            if _int(row, "plies") == plies:
                source = _int(row, "source_index")
                if source in rules:
                    raise ValueError("duplicate Rule-of-Zero summary row")
                rules[source] = row
        elif line.startswith("THINKING_CURVE_POINT "):
            row = fields(line)
            if _int(row, "plies") == plies and row.get("final") == "0":
                points[_int(row, "source_index")].append(row)
        elif line.startswith("THINKING_CURVE_POSITION_DONE "):
            row = fields(line)
            if _int(row, "plies") == plies:
                source = _int(row, "source_index")
                if source in done:
                    raise ValueError("duplicate Rule-of-Zero done row")
                done[source] = row
        elif line.startswith("THINKING_CURVE_JUDGE_CANDIDATE "):
            row = fields(line)
            if _int(row, "plies") == plies:
                judges[_int(row, "source_index")].append(row)
        elif line.startswith("THINKING_CURVE_HORIZON_SHADOW "):
            row = fields(line)
            if _int(row, "plies") == plies:
                shadows[_int(row, "source_index")].append(row)
    return parsed


def validate_rule_zero_chunk_accounting(
    path: Path,
    *,
    plies: int,
    max_nodes: int,
    minimum_nodes: int,
    minimum_stable_checkpoints: int,
    equal_slice_nodes: int,
    checkpoint_interval: int,
    matched_modulus: int,
    matched_remainder: int,
    audit_modulus: int,
    audit_remainder: int,
    judge_samples: int,
    judge_risk_plays: int,
    shadow_model: HorizonModel | None = None,
    shadow_model_sha256: str | None = None,
) -> list[int]:
    parsed = _parse_chunk(path, plies)
    positions = parsed["positions"]
    checkpoints = parsed["checkpoints"]
    rules = parsed["rules"]
    points = parsed["points"]
    done = parsed["done"]
    judges = parsed["judges"]
    shadows = parsed["shadows"]
    assert isinstance(positions, dict)
    assert isinstance(checkpoints, defaultdict)
    assert isinstance(rules, dict)
    assert isinstance(points, defaultdict)
    assert isinstance(done, dict)
    assert isinstance(judges, defaultdict)
    assert isinstance(shadows, defaultdict)
    roots = set(done)
    if not roots or set(positions) != roots or set(rules) != roots:
        raise ValueError("Rule-of-Zero chunk root sections differ")
    for source in sorted(roots):
        position = positions[source]
        rule = rules[source]
        done_row = done[source]
        root_checkpoints = sorted(
            checkpoints.get(source, []), key=lambda row: _int(row, "checkpoint")
        )
        if not root_checkpoints or _int(rule, "checkpoints") != len(root_checkpoints):
            raise ValueError(f"source {source} has incomplete checkpoint rows")
        if (
            _int(position, "rule_zero") != 1
            or _int(done_row, "rule_zero") != 1
            or _int(done_row, "dropped") != 0
            or _int(rule, "minimum_nodes") != minimum_nodes
            or _int(rule, "minimum_stable_checkpoints")
            != minimum_stable_checkpoints
            or _int(rule, "equal_slice_nodes") != equal_slice_nodes
            or _int(rule, "checkpoint_interval") != checkpoint_interval
            or _int(rule, "matched_modulus") != matched_modulus
            or _int(rule, "matched_remainder") != matched_remainder
            or _int(rule, "audit_modulus") != audit_modulus
            or _int(rule, "audit_remainder") != audit_remainder
        ):
            raise ValueError(f"source {source} Rule-of-Zero protocol differs")

        previous_rank: int | None = None
        previous_iterations = 0
        previous_nodes = 0
        stable_start = 0
        stable = 0
        switches = 0
        first_eligible: dict[str, str] | None = None
        for index, checkpoint in enumerate(root_checkpoints):
            iterations = _int(checkpoint, "iterations")
            nodes = _int(checkpoint, "nodes")
            rank = _int(checkpoint, "selected_rank")
            width = _int(checkpoint, "arm_plays")
            if (
                _int(checkpoint, "checkpoint") != index
                or iterations <= previous_iterations
                or nodes <= previous_nodes
                or nodes > max_nodes
                or not 0 <= rank < width
            ):
                raise ValueError(f"source {source} checkpoint work/identity differs")
            if rank == previous_rank:
                stable += 1
            else:
                if previous_rank is not None:
                    switches += 1
                previous_rank = rank
                stable = 1
                stable_start = iterations
            near_ties = _int(checkpoint, "near_tie_challengers")
            valid = near_ties >= 0
            eligible = (
                valid
                and nodes >= minimum_nodes
                and stable >= minimum_stable_checkpoints
                and near_ties == 0
            )
            expected = {
                "stable_checkpoints": stable,
                "stable_iterations": iterations - stable_start,
                "selected_switches": switches,
                "counters_valid": int(valid),
                "rule_eligible": int(eligible),
            }
            for key, value in expected.items():
                if _int(checkpoint, key) != value:
                    raise ValueError(f"source {source} checkpoint {key} differs")
            # Risk-set membership (packet section 6): required as of progress
            # schema 7. The mask holds candidate-rank bits; the incumbent's
            # own bit is never set, the popcount must equal the near-tie
            # count, and the full-horizon indicator must be derivable from
            # the mask plus the incumbent rank. Fail-closed rows report 0/0.
            if "risk_set_ranks" not in checkpoint or (
                "full_horizon_rank_in_risk_set" not in checkpoint
            ):
                raise ValueError(
                    f"source {source} checkpoint lacks risk-set telemetry"
                )
            risk_mask = int(checkpoint["risk_set_ranks"], 16)
            in_risk_set = _int(checkpoint, "full_horizon_rank_in_risk_set")
            full_rank_at_checkpoint = _int(checkpoint, "full_selected_rank")
            if valid:
                if bin(risk_mask).count("1") != near_ties:
                    raise ValueError(
                        f"source {source} risk-set popcount differs"
                    )
                if (risk_mask >> rank) & 1:
                    raise ValueError(
                        f"source {source} risk set contains the incumbent"
                    )
                if width < 64 and risk_mask >> width:
                    raise ValueError(
                        f"source {source} risk set exceeds the panel width"
                    )
                expected_in_risk_set = int(
                    full_rank_at_checkpoint == rank
                    or (
                        full_rank_at_checkpoint < 64
                        and (risk_mask >> full_rank_at_checkpoint) & 1
                    )
                )
                if in_risk_set != expected_in_risk_set:
                    raise ValueError(
                        f"source {source} risk-set indicator differs"
                    )
            elif risk_mask != 0 or in_risk_set != 0:
                raise ValueError(
                    f"source {source} invalid checkpoint reports a risk set"
                )
            if eligible and first_eligible is None:
                first_eligible = checkpoint
            selected = _int(checkpoint, "rule_selected")
            if selected != int(first_eligible is checkpoint):
                raise ValueError(f"source {source} selected the wrong checkpoint")
            previous_iterations = iterations
            previous_nodes = nodes

        capped = first_eligible is None
        chosen = first_eligible
        stop_checkpoint = len(root_checkpoints) if capped else _int(chosen, "checkpoint")
        if _int(rule, "capped") != int(capped) or _int(rule, "stop_checkpoint") != stop_checkpoint:
            raise ValueError(f"source {source} cap/stop checkpoint differs")
        # Summary risk-set fields must mirror the replay-selected stopping
        # checkpoint. Capped roots reuse the full decision, whose mask is 0
        # (FINISH events carry no mask); the keys must still be present.
        if "stop_risk_set_ranks" not in rule or (
            "stop_full_horizon_rank_in_risk_set" not in rule
        ):
            raise ValueError(f"source {source} summary lacks risk-set fields")
        if not capped:
            if int(rule["stop_risk_set_ranks"], 16) != int(
                chosen["risk_set_ranks"], 16
            ) or _int(rule, "stop_full_horizon_rank_in_risk_set") != _int(
                chosen, "full_horizon_rank_in_risk_set"
            ):
                raise ValueError(
                    f"source {source} summary risk-set fields differ"
                )
        full_iterations = _int(rule, "full_iterations")
        full_nodes = _int(rule, "full_nodes")
        full_rank = _int(rule, "full_rank")
        if full_nodes > max_nodes or full_iterations > max_nodes // (plies + 1):
            raise ValueError(f"source {source} full arm exceeds its cap")
        expected_stop_iterations = full_iterations if capped else _int(chosen, "iterations")
        expected_stop_nodes = full_nodes if capped else _int(chosen, "nodes")
        expected_stop_rank = full_rank if capped else _int(chosen, "selected_rank")
        for key, expected in (
            ("stopped_iterations", expected_stop_iterations),
            ("stopped_nodes", expected_stop_nodes),
            ("stopped_rank", expected_stop_rank),
        ):
            if _int(rule, key) != expected:
                raise ValueError(f"source {source} {key} does not join")

        by_kind: dict[str, list[dict[str, str]]] = defaultdict(list)
        for point in points.get(source, []):
            by_kind[point["control_kind"]].append(point)
        has_matched = source % matched_modulus == matched_remainder
        expected_kinds = {"stopped", "full", "equal_horizon"}
        if has_matched:
            expected_kinds.add("matched")
        if set(by_kind) != expected_kinds or any(len(rows) != 1 for rows in by_kind.values()):
            raise ValueError(f"source {source} point kinds differ")
        stopped = by_kind["stopped"][0]
        full = by_kind["full"][0]
        equal = by_kind["equal_horizon"][0]
        for row, iterations, nodes, rank in (
            (stopped, expected_stop_iterations, expected_stop_nodes, expected_stop_rank),
            (full, full_iterations, full_nodes, full_rank),
        ):
            if (
                _int(row, "iterations") != iterations
                or _int(row, "nodes") != nodes
                or _int(row, "selected_rank") != rank
            ):
                raise ValueError(f"source {source} point does not join summary")
        equal_target = min(equal_slice_nodes, full_nodes)
        equal_checkpoint = next(
            (row for row in root_checkpoints if _int(row, "nodes") >= equal_target),
            None,
        )
        equal_iterations = full_iterations if equal_checkpoint is None else _int(equal_checkpoint, "iterations")
        equal_nodes = full_nodes if equal_checkpoint is None else _int(equal_checkpoint, "nodes")
        equal_rank = full_rank if equal_checkpoint is None else _int(equal_checkpoint, "selected_rank")
        for key, expected in (
            ("equal_iterations", equal_iterations),
            ("equal_nodes", equal_nodes),
            ("equal_rank", equal_rank),
        ):
            if _int(rule, key) != expected:
                raise ValueError(f"source {source} {key} differs")
        if (
            _int(equal, "iterations") != equal_iterations
            or _int(equal, "nodes") != equal_nodes
            or _int(equal, "selected_rank") != equal_rank
        ):
            raise ValueError(f"source {source} equal horizon point differs")

        matched_rank = -1
        if has_matched:
            matched = by_kind["matched"][0]
            matched_target = expected_stop_iterations * (plies + 1)
            full_minimum = _int(full, "min_play_iterations")
            initial_floor = full_minimum * _int(full, "arm_plays")
            expected_sampling_rule = (
                "round_robin_initial"
                if expected_stop_iterations < initial_floor
                else "top_two_ids"
            )
            expected_matched_minimum = (
                1 if expected_sampling_rule == "round_robin_initial" else full_minimum
            )
            if (
                _int(rule, "matched_control") != 1
                or _int(rule, "matched_target_nodes") != matched_target
                or _int(rule, "matched_iterations") != expected_stop_iterations
                or _int(rule, "matched_min_play_iterations")
                != expected_matched_minimum
                or rule.get("matched_sampling_rule") != expected_sampling_rule
                or _int(matched, "target_nodes") != matched_target
                or _int(matched, "iterations") != expected_stop_iterations
                or _int(matched, "nodes") > matched_target
                or _int(matched, "min_play_iterations")
                != expected_matched_minimum
            ):
                raise ValueError(f"source {source} matched arm is not exact")
            matched_rank = _int(matched, "selected_rank")
            if _int(rule, "matched_rank") != matched_rank:
                raise ValueError(f"source {source} matched rank differs")
        elif (
            _int(rule, "matched_control") != 0
            or _int(rule, "matched_min_play_iterations") != 0
            or rule.get("matched_sampling_rule") != "top_two_ids"
        ):
            raise ValueError(f"source {source} has an unexpected matched arm")

        horizon_mismatch = int(expected_stop_rank != full_rank)
        equal_mismatch = int(expected_stop_rank != equal_rank)
        matched_mismatch = int(has_matched and expected_stop_rank != matched_rank)
        audit = int(source % audit_modulus == audit_remainder)
        judge_performed = int(
            bool(audit or horizon_mismatch or equal_mismatch or matched_mismatch)
        )
        for key, expected in (
            ("horizon_mismatch", horizon_mismatch),
            ("equal_horizon_mismatch", equal_mismatch),
            ("matched_mismatch", matched_mismatch),
            ("audit_selected", audit),
            ("judge_performed", judge_performed),
        ):
            if _int(rule, key) != expected:
                raise ValueError(f"source {source} {key} differs")
        judge_rows = judges.get(source, [])
        judge_candidates = _int(done_row, "judge_candidates")
        judge_iterations = _int(done_row, "judge_iterations")
        if judge_performed:
            if len(judge_rows) != judge_candidates or judge_candidates <= 0:
                raise ValueError(f"source {source} selective judge rows differ")
            ranks = {_int(row, "rank") for row in judge_rows}
            required = {expected_stop_rank, full_rank, equal_rank}
            if has_matched:
                required.add(matched_rank)
            if not required <= ranks or len(ranks) != judge_candidates:
                raise ValueError(f"source {source} selective judge union differs")
            forced = _int(done_row, "judge_forced")
            expected_judge_iterations = 0 if forced else judge_samples * judge_candidates
            if judge_iterations != expected_judge_iterations:
                raise ValueError(f"source {source} judge iterations differ")
            if audit and judge_candidates > judge_risk_plays + 3:
                raise ValueError(f"source {source} audit judge union is too wide")
        elif judge_rows or judge_candidates != 0 or judge_iterations != 0:
            raise ValueError(f"source {source} unexpectedly ran the judge")

        expected_events = 2 + len(root_checkpoints) + 2 * int(has_matched)
        if (
            _int(done_row, "events") != expected_events
            or _int(done_row, "final_iterations") != expected_stop_iterations
            or _int(done_row, "final_nodes") != expected_stop_nodes
            or _int(done_row, "matched_control") != int(has_matched)
            or _int(done_row, "judge_performed") != judge_performed
        ):
            raise ValueError(f"source {source} terminal accounting differs")

        root_shadows = sorted(
            shadows.get(source, []), key=lambda row: _int(row, "checkpoint")
        )
        if shadow_model is None:
            if root_shadows:
                raise ValueError(f"source {source} has premature shadow rows")
        else:
            if len(root_shadows) != len(root_checkpoints) or not shadow_model_sha256:
                raise ValueError(f"source {source} shadow row count differs")
            for checkpoint, shadow in zip(root_checkpoints, root_shadows, strict=True):
                if (
                    _int(shadow, "checkpoint") != _int(checkpoint, "checkpoint")
                    or shadow.get("model_sha256") != shadow_model_sha256
                    or shadow.get("decision_role") != "shadow_only"
                ):
                    raise ValueError(f"source {source} shadow identity differs")
                valid = _finite_checkpoint(checkpoint)
                if _int(shadow, "valid") != int(valid):
                    raise ValueError(f"source {source} shadow validity differs")
                values = [
                    _float(shadow, key)
                    for key in (
                        "mismatch_probability",
                        "conditional_signed_improvement",
                        "expected_signed_improvement",
                        "stopping_score",
                    )
                ]
                if valid:
                    expected = predict(
                        shadow_model, _shadow_row(checkpoint, position, rule), -1
                    )
                    targets = (
                        expected.mismatch_probability,
                        expected.conditional_signed_improvement,
                        expected.expected_signed_improvement,
                        expected.stopping_score,
                    )
                    if any(
                        not math.isfinite(value) or abs(value - target) > 1.0e-12
                        for value, target in zip(values, targets, strict=True)
                    ):
                        raise ValueError(f"source {source} shadow score differs")
                elif not all(math.isnan(value) for value in values):
                    raise ValueError(f"source {source} invalid shadow is not NaN")
    return sorted(roots)


def _validate_panel_design(manifest: Path, expected_roots: int) -> None:
    document = json.loads(manifest.read_text(encoding="utf-8"))
    mapping = document.get("mapping")
    if not isinstance(mapping, list) or len(mapping) != expected_roots:
        raise ValueError("selection manifest has the wrong number of roots")
    identities = {(str(row["source_seed"]), str(row["start"])) for row in mapping}
    if len(identities) != expected_roots:
        raise ValueError("prospective panel is not one root per complete game")
    strata: dict[tuple[str, str], int] = defaultdict(int)
    for row in mapping:
        strata[(str(row["trajectory_policy"]), str(row["bag_band"]))] += 1
    if len(strata) != 8 or set(strata.values()) != {expected_roots // 8}:
        raise ValueError("prospective panel is not balanced over eight strata")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--corpus", required=True, type=Path)
    parser.add_argument("--manifest", required=True, type=Path)
    parser.add_argument("--binary", required=True, type=Path)
    parser.add_argument("--shadow-model", type=Path, default=None)
    parser.add_argument("--preregistration", required=True, type=Path)
    parser.add_argument("--run-dir", required=True, type=Path)
    parser.add_argument("--expected-roots", type=int, default=320)
    parser.add_argument("--plies", type=int, default=6)
    parser.add_argument("--max-nodes", type=int, default=3_000_000)
    parser.add_argument("--num-plays", type=int, default=60)
    parser.add_argument("--threads", type=int, default=10)
    parser.add_argument("--minimum-nodes", type=int, default=100_000)
    parser.add_argument("--minimum-stable-checkpoints", type=int, default=2)
    parser.add_argument("--equal-slice-nodes", type=int, default=3_000_000)
    parser.add_argument("--checkpoint-interval", type=int, default=256)
    parser.add_argument("--matched-modulus", type=int, default=3)
    parser.add_argument("--matched-remainder", type=int, default=0)
    parser.add_argument("--audit-modulus", type=int, default=10)
    parser.add_argument("--audit-remainder", type=int, default=0)
    parser.add_argument("--judge-plies", type=int, default=10)
    parser.add_argument("--judge-samples", type=int, default=100_000)
    parser.add_argument("--judge-risk-plays", type=int, default=8)
    parser.add_argument("--uniform-floor-per-mille", type=int, default=100)
    parser.add_argument("--chunk-positions", type=int, default=1)
    parser.add_argument("--wall-seconds", type=float, default=0.0)
    args = parser.parse_args()

    repository = Path(__file__).resolve().parent.parent
    for name in ("corpus", "manifest", "binary", "shadow_model", "preregistration"):
        value = getattr(args, name)
        if value is not None:
            setattr(args, name, value.resolve())
    args.run_dir = args.run_dir.resolve()
    os.chdir(repository)
    frozen_inputs = [args.corpus, args.manifest, args.binary, args.preregistration]
    if args.shadow_model is not None:
        frozen_inputs.append(args.shadow_model)
    if any(not path.is_file() for path in frozen_inputs):
        parser.error("all frozen inputs must exist")
    preregistration = json.loads(args.preregistration.read_text(encoding="utf-8"))
    if preregistration.get("artifact_kind") != "rule_zero_prospective_preregistration":
        parser.error("invalid preregistration artifact")
    arm = preregistration.get("arm", {})
    rule = preregistration.get("rule", {})
    if (
        args.expected_roots < 300
        or args.expected_roots % 8 != 0
        # The arm and rule constants are frozen in the preregistration; the
        # runner refuses any invocation that does not match them exactly.
        or args.plies != int(arm.get("plies", -1))
        or args.max_nodes != int(arm.get("maximum_nodes", -1))
        or args.num_plays != int(arm.get("sorted_candidate_width", -1))
        or args.threads != int(arm.get("threads", -1))
        or args.uniform_floor_per_mille != int(arm.get("uniform_floor_per_mille", -1))
        or args.minimum_nodes != int(rule.get("minimum_nodes", -1))
        or args.minimum_stable_checkpoints
        != int(rule.get("minimum_stable_checkpoints", -1))
        or args.checkpoint_interval
        != int(rule.get("checkpoint_interval_iterations", -1))
        or args.equal_slice_nodes > args.max_nodes
        or args.checkpoint_interval <= 0
        or args.matched_modulus <= 0
        or not 0 <= args.matched_remainder < args.matched_modulus
        or args.audit_modulus <= 0
        or not 0 <= args.audit_remainder < args.audit_modulus
        or args.judge_plies <= args.plies
        or args.judge_samples <= 0
        or not 2 <= args.judge_risk_plays <= args.num_plays
        or not 0 < args.uniform_floor_per_mille <= 1000
        or args.chunk_positions <= 0
        or args.wall_seconds < 0.0
    ):
        parser.error("invalid Rule-of-Zero prospective protocol")
    _validate_panel_design(args.manifest, args.expected_roots)
    if args.shadow_model is not None:
        shadow_model = load_horizon_model_artifact(args.shadow_model)
        shadow_sha = sha256(args.shadow_model)
    else:
        shadow_model = None
        shadow_sha = ""
    eligible = eligible_indices(args.corpus, 5, 100)
    if eligible != list(range(args.expected_roots)):
        parser.error("selected corpus is not the exact dense prospective panel")

    args.run_dir.mkdir(parents=True, exist_ok=True)
    chunks = args.run_dir / "chunks"
    chunks.mkdir(exist_ok=True)
    log = args.run_dir / "thinking-curves.log"
    err = args.run_dir / "thinking-curves.err"
    state_path = args.run_dir / "state.json"
    protocol = {
        "corpus": str(args.corpus),
        "corpus_sha256": sha256(args.corpus),
        "manifest": str(args.manifest),
        "manifest_sha256": sha256(args.manifest),
        "binary": str(args.binary),
        "binary_sha256": sha256(args.binary),
        "shadow_model": "" if args.shadow_model is None else str(args.shadow_model),
        "shadow_model_sha256": shadow_sha,
        "preregistration": str(args.preregistration),
        "preregistration_sha256": sha256(args.preregistration),
        "runner_sha256": sha256(Path(__file__)),
        "expected_roots": args.expected_roots,
        "plies": args.plies,
        "max_nodes": args.max_nodes,
        "num_plays": args.num_plays,
        "threads": args.threads,
        "minimum_nodes": args.minimum_nodes,
        "minimum_stable_checkpoints": args.minimum_stable_checkpoints,
        "near_tie_challengers": 0,
        "equal_slice_nodes": args.equal_slice_nodes,
        "checkpoint_interval": args.checkpoint_interval,
        "matched_modulus": args.matched_modulus,
        "matched_remainder": args.matched_remainder,
        "audit_modulus": args.audit_modulus,
        "audit_remainder": args.audit_remainder,
        "judge_plies": args.judge_plies,
        "judge_samples": args.judge_samples,
        "judge_risk_plays": args.judge_risk_plays,
        "uniform_floor_per_mille": args.uniform_floor_per_mille,
        "chunk_positions": args.chunk_positions,
    }
    if state_path.exists():
        state = json.loads(state_path.read_text(encoding="utf-8"))
        if state.get("protocol") != protocol:
            raise ValueError("resume protocol does not match frozen state")
    else:
        state = {
            "artifact_kind": "rule_zero_prospective_run",
            "protocol": protocol,
            "status": "running",
            "start_epoch": time.time(),
            "attempts": 0,
        }
    deadline = time.monotonic() + args.wall_seconds if args.wall_seconds else math.inf
    with log.open("a", encoding="utf-8") as stream:
        stream.write(
            f"RULE_ZERO_PANEL_RESUME epoch={time.time():.6f} "
            f"eligible={len(eligible)}\n"
        )

    while time.monotonic() < deadline:
        completed = completed_by_plies(log).get(args.plies, set())
        first_missing = validate_prefix(eligible, completed)
        if first_missing == len(eligible):
            break
        state["attempts"] = int(state.get("attempts", 0)) + 1
        attempt = int(state["attempts"])
        skip = eligible[first_missing]
        requested = min(args.chunk_positions, len(eligible) - first_missing)
        partial_log = chunks / f"attempt-{attempt:05d}-s{skip}.partial.log"
        partial_err = chunks / f"attempt-{attempt:05d}-s{skip}.partial.err"
        environment = os.environ.copy()
        environment.update(
            {
                "THINKING_CURVE_CORPUS": str(args.corpus),
                "THINKING_CURVE_SKIP_POSITIONS": str(skip),
                "THINKING_CURVE_MAX_POSITIONS": str(requested),
                "THINKING_CURVE_MIN_BAG": "5",
                "THINKING_CURVE_MAX_BAG": "100",
                "THINKING_CURVE_NUM_PLAYS": str(args.num_plays),
                "THINKING_CURVE_PLIES": str(args.plies),
                "THINKING_CURVE_THREADS": str(args.threads),
                "THINKING_CURVE_MAX_NODES": str(args.max_nodes),
                "THINKING_CURVE_TARGET_NODES": str(args.max_nodes),
                "THINKING_CURVE_UNIFORM_FLOOR_PER_MILLE": str(
                    args.uniform_floor_per_mille
                ),
                "THINKING_CURVE_JUDGE_PLIES": str(args.judge_plies),
                "THINKING_CURVE_JUDGE_SAMPLES": str(args.judge_samples),
                "THINKING_CURVE_JUDGE_RISK_PLAYS": str(args.judge_risk_plays),
                "THINKING_CURVE_REGRET_CORRELATION": "0.48",
                "THINKING_CURVE_REGRET_CALIBRATION": "1",
                "THINKING_CURVE_REGRET_CHECK_INTERVAL": "256",
                "THINKING_CURVE_REGRET_MIN_SAMPLES_PER_ARM": "32",
                "THINKING_CURVE_RULE_ZERO": "1",
                "THINKING_CURVE_RULE_ZERO_MINIMUM_NODES": str(
                    args.minimum_nodes
                ),
                "THINKING_CURVE_RULE_ZERO_MINIMUM_STABLE_CHECKPOINTS": str(
                    args.minimum_stable_checkpoints
                ),
                "THINKING_CURVE_RULE_ZERO_EQUAL_SLICE_NODES": str(
                    args.equal_slice_nodes
                ),
                "THINKING_CURVE_RULE_ZERO_CHECKPOINT_INTERVAL": str(
                    args.checkpoint_interval
                ),
                "THINKING_CURVE_RULE_ZERO_MATCHED_MODULUS": str(
                    args.matched_modulus
                ),
                "THINKING_CURVE_RULE_ZERO_MATCHED_REMAINDER": str(
                    args.matched_remainder
                ),
                "THINKING_CURVE_RULE_ZERO_AUDIT_MODULUS": str(
                    args.audit_modulus
                ),
                "THINKING_CURVE_RULE_ZERO_AUDIT_REMAINDER": str(
                    args.audit_remainder
                ),
                "THINKING_CURVE_WALL_SECONDS": "0",
            }
        )
        with partial_log.open("w", encoding="utf-8") as stdout, partial_err.open(
            "w", encoding="utf-8"
        ) as stderr:
            result = subprocess.run(
                [str(args.binary), "thinkingcurve"],
                env=environment,
                stdout=stdout,
                stderr=stderr,
                check=False,
            )
        if result.returncode != 0:
            state.update(
                {"status": "failed", "failed_attempt": attempt, "returncode": result.returncode}
            )
            write_state(state_path, state)
            raise SystemExit(result.returncode)
        accepted = validate_rule_zero_chunk_accounting(
            partial_log,
            plies=args.plies,
            max_nodes=args.max_nodes,
            minimum_nodes=args.minimum_nodes,
            minimum_stable_checkpoints=args.minimum_stable_checkpoints,
            equal_slice_nodes=args.equal_slice_nodes,
            checkpoint_interval=args.checkpoint_interval,
            matched_modulus=args.matched_modulus,
            matched_remainder=args.matched_remainder,
            audit_modulus=args.audit_modulus,
            audit_remainder=args.audit_remainder,
            judge_samples=args.judge_samples,
            judge_risk_plays=args.judge_risk_plays,
        )
        decorate_shadow_scores(partial_log, shadow_model, shadow_sha)
        accepted = validate_rule_zero_chunk_accounting(
            partial_log,
            plies=args.plies,
            max_nodes=args.max_nodes,
            minimum_nodes=args.minimum_nodes,
            minimum_stable_checkpoints=args.minimum_stable_checkpoints,
            equal_slice_nodes=args.equal_slice_nodes,
            checkpoint_interval=args.checkpoint_interval,
            matched_modulus=args.matched_modulus,
            matched_remainder=args.matched_remainder,
            audit_modulus=args.audit_modulus,
            audit_remainder=args.audit_remainder,
            judge_samples=args.judge_samples,
            judge_risk_plays=args.judge_risk_plays,
            shadow_model=shadow_model,
            shadow_model_sha256=shadow_sha,
        )
        expected = eligible[first_missing : first_missing + len(accepted)]
        if accepted != expected or len(accepted) > requested:
            raise ValueError("chunk completed unexpected source indices")
        with log.open("a", encoding="utf-8") as stream:
            stream.write(partial_log.read_text(encoding="utf-8"))
        with err.open("a", encoding="utf-8") as stream:
            stream.write(partial_err.read_text(encoding="utf-8"))
        partial_log.replace(
            partial_log.with_name(partial_log.name.replace(".partial", ".accepted"))
        )
        partial_err.replace(
            partial_err.with_name(partial_err.name.replace(".partial", ".accepted"))
        )
        completed_now = completed_by_plies(log).get(args.plies, set())
        state.update(
            {
                "status": "running",
                "last_update_epoch": time.time(),
                "eligible_positions": len(eligible),
                "completed_positions": len(completed_now),
            }
        )
        write_state(state_path, state)

    completed_final = completed_by_plies(log).get(args.plies, set())
    complete = validate_prefix(eligible, completed_final) == len(eligible)
    state.update(
        {
            "status": "complete" if complete else "wall_limit",
            "end_epoch": time.time(),
            "eligible_positions": len(eligible),
            "completed_positions": len(completed_final),
        }
    )
    write_state(state_path, state)
    print(json.dumps(state, sort_keys=True))


if __name__ == "__main__":
    main()
