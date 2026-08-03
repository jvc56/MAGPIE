#!/usr/bin/env python3
"""Run and audit common-RNG TimeManager-vs-equal PlayChooser game pairs."""

from __future__ import annotations

import argparse
from collections import defaultdict
import csv
import datetime as dt
import hashlib
import json
import math
import os
from pathlib import Path
import re
import statistics
import subprocess
import time


FIELD_RE = re.compile(r'(\w+)=(?:"([^"]*)"|(\S+))')
CSV_FIELDS = [
    "pair", "seed", "game_seed", "started_utc", "finished_utc", "elapsed_s",
    "game1_turns", "game2_turns", "identical_prefix_turns",
    "first_divergence_turn", "first_divergence_bag", "game1_divergence_move",
    "game2_divergence_move", "first_divergence_cgp", "fully_identical", "game1_spread",
    "game1_win", "game1_utility", "game2_spread", "game2_win",
    "game2_utility", "pair_spread_mean", "pair_win_score", "pair_utility",
    "game1_p0_policy", "game1_p0_expected_game_regret",
    "game1_p0_regret_estimated_turns", "game1_p0_regret_unknown_turns",
    "game1_p1_policy", "game1_p1_expected_game_regret",
    "game1_p1_regret_estimated_turns", "game1_p1_regret_unknown_turns",
    "game2_p0_policy", "game2_p0_expected_game_regret",
    "game2_p0_regret_estimated_turns", "game2_p0_regret_unknown_turns",
    "game2_p1_policy", "game2_p1_expected_game_regret",
    "game2_p1_regret_estimated_turns", "game2_p1_regret_unknown_turns",
    "tm_expected_game_regret", "equal_expected_game_regret",
    "tm_regret_estimated_turns", "equal_regret_estimated_turns",
    "tm_regret_unknown_turns", "equal_regret_unknown_turns",
    "tm_expected_post_divergence_regret",
    "equal_expected_post_divergence_regret",
    "tm_post_divergence_regret_estimated_turns",
    "equal_post_divergence_regret_estimated_turns",
    "tm_post_divergence_regret_unknown_turns",
    "equal_post_divergence_regret_unknown_turns",
    "shared_regret_comparisons", "shared_regret_unknown_roots",
    "predicted_shared_utility_delta", "actual_minus_predicted_utility_delta",
    "tm_time_ms", "equal_time_ms", "tm_terminal_remaining_ms",
    "equal_terminal_remaining_ms", "tm_overtime_ms", "equal_overtime_ms",
    "tm_penalty", "equal_penalty", "tm_sim_ms", "equal_sim_ms",
    "tm_peg_ms", "equal_peg_ms", "tm_endgame_ms", "equal_endgame_ms",
    "tm_static_ms", "equal_static_ms", "tm_sim_calls", "equal_sim_calls",
    "tm_sim_planned_budget_ms", "tm_sim_legacy_budget_ms",
    "tm_sim_released_budget_ms", "tm_sim_released_turns",
    "rule_zero_enabled_turns", "rule_zero_stops", "rule_zero_would_stops",
    "tm_low_bag_fallbacks", "tm_peg_reserve_shortfalls",
    "tm_peg_shadow_budget_ms", "tm_peg_legacy_budget_ms",
    "tm_peg_deposit_caps", "tm_peg_withdrawal_caps",
    "tm_peg_calls", "equal_peg_calls", "tm_endgame_calls",
    "equal_endgame_calls", "sim_calls", "sim_iters", "sim_nodes",
    "sim_candidate_rows", "peg_calls", "peg_candidate_completions",
    "peg_tm_admissions", "peg_tm_false_starts", "eg_calls", "eg_nodes",
    "eg_depth_sum",
]


def utc_now() -> str:
    return dt.datetime.now(dt.timezone.utc).isoformat(timespec="seconds")


def fields(line: str) -> dict[str, str]:
    return {
        match.group(1): match.group(2) if match.group(2) is not None else match.group(3)
        for match in FIELD_RE.finditer(line)
    }


def rows_with_prefix(lines: list[str], prefix: str) -> list[dict[str, str]]:
    return [fields(line) for line in lines if line.startswith(prefix + " ")]


def integer(row: dict[str, str], key: str) -> int:
    return int(row[key])


def turn_regret(turn: dict[str, str]) -> float | None:
    if integer(turn, "regret_valid") == 0:
        return None
    value = float(turn["expected_utility_regret"])
    assert math.isfinite(value) and value >= 0.0
    return value


def add_realized_path_rest_regret(
    turns_by_game: dict[int, list[dict[str, str]]]
) -> None:
    """Annotate each turn with later ex-ante estimates on its realized path.

    This reverse sum is useful for retrospective calibration, but it is not an
    online forecast of positions that had not yet been reached. Unknown-mode
    counts stay explicit so an uncovered PEG/endgame/static turn cannot be
    silently treated as zero regret.
    """
    for game_turns in turns_by_game.values():
        regret = [0.0, 0.0]
        estimated = [0, 0]
        unknown = [0, 0]
        for turn in reversed(game_turns):
            player = integer(turn, "player")
            value = turn_regret(turn)
            if value is None:
                unknown[player] += 1
            else:
                regret[player] += value
                estimated[player] += 1
            turn["realized_path_rest_game_expected_regret"] = f"{regret[player]:.12f}"
            turn["realized_path_rest_game_estimated_turns"] = str(estimated[player])
            turn["realized_path_rest_game_unknown_turns"] = str(unknown[player])


def atomic_write(path: Path, value: str) -> None:
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(value, encoding="utf-8")
    temporary.replace(path)


def write_rows(path: Path, rows: list[dict[str, str]]) -> None:
    if not rows:
        path.write_text("", encoding="utf-8")
        return
    keys: list[str] = []
    seen: set[str] = set()
    for row in rows:
        for key in row:
            if key not in seen:
                seen.add(key)
                keys.append(key)
    with path.open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=keys)
        writer.writeheader()
        writer.writerows(rows)


def call_to_turn(
    turns: list[dict[str, str]], family: str
) -> dict[int, dict[str, str]]:
    result: dict[int, dict[str, str]] = {}
    for turn in turns:
        first = integer(turn, f"{family}_call_first")
        count = integer(turn, f"{family}_calls")
        for call_index in range(first, first + count):
            assert call_index not in result, f"duplicate {family} call {call_index}"
            result[call_index] = turn
    return result


def add_turn_context(
    events: list[dict[str, str]], mapping: dict[int, dict[str, str]]
) -> list[dict[str, str]]:
    result = []
    for event in events:
        turn = mapping[integer(event, "call")]
        result.append(
            {
                "game": turn["game"],
                "turn": turn["turn"],
                "player": turn["player"],
                "policy": turn["policy"],
                **event,
            }
        )
    return result


def audit_trace(log_path: Path) -> dict[str, object]:
    lines = log_path.read_text(encoding="utf-8", errors="replace").splitlines()
    rng_rows = rows_with_prefix(lines, "PCRNG")
    turns = rows_with_prefix(lines, "PCTURN")
    sims = rows_with_prefix(lines, "PCSIMCAND")
    pegs = rows_with_prefix(lines, "PCPEGCAND")
    endgames = rows_with_prefix(lines, "PCEG")
    games = rows_with_prefix(lines, "PCGAME")
    benches = rows_with_prefix(lines, "PCBENCH")

    assert len(rng_rows) == 4, f"expected four PCRNG rows, got {len(rng_rows)}"
    assert len(games) == 2, f"expected two PCGAME rows, got {len(games)}"
    assert len(benches) == 1, f"expected one PCBENCH row, got {len(benches)}"
    benchmark = benches[0]
    for key in ("sim_event_drops", "peg_event_drops", "eg_event_drops"):
        assert integer(benchmark, key) == 0, f"{key} is nonzero"

    stream_seeds: dict[int, set[int]] = defaultdict(set)
    for row in rng_rows:
        expected_stream = 0 if row["player"] == row["start"] else 1
        assert integer(row, "stream") == expected_stream
        stream_seeds[expected_stream].add(integer(row, "seed"))
    assert set(stream_seeds) == {0, 1}
    assert all(len(seeds) == 1 for seeds in stream_seeds.values())
    assert next(iter(stream_seeds[0])) != next(iter(stream_seeds[1]))

    turns_by_game: dict[int, list[dict[str, str]]] = defaultdict(list)
    for turn in turns:
        regret_valid = integer(turn, "regret_valid")
        assert regret_valid in (0, 1)
        regret = turn_regret(turn)
        if regret_valid:
            assert regret is not None
            assert turn["regret_model"] in ("forced", "sim_bai")
            expected_scope = (
                "exact_forced_current_turn"
                if turn["regret_model"] == "forced"
                else "conditional_candidate_sampling"
            )
            assert turn["regret_scope"] == expected_scope
            if turn["regret_model"] == "sim_bai":
                assert integer(turn, "sim_calls") == 1
        else:
            assert regret is None
            assert turn["regret_model"] == "none"
            assert turn["regret_scope"] == "none"
            assert math.isnan(float(turn["expected_utility_regret"]))
        # There is no learned forecast of unseen future turns yet. Keep that
        # absence machine-readable so the retrospective reverse sum cannot be
        # mistaken for a pre-move value-to-go prediction.
        assert integer(turn, "value_to_go_valid") == 0
        reserve_shortfall = integer(turn, "reserve_shortfall")
        peg_shadow_budget_ms = float(turn["peg_shadow_budget_ms"])
        peg_deposit_capped = integer(turn, "peg_deposit_capped")
        peg_withdrawal_capped = integer(turn, "peg_withdrawal_capped")
        assert reserve_shortfall in (0, 1)
        assert peg_shadow_budget_ms >= 0.0
        assert peg_deposit_capped in (0, 1)
        assert peg_withdrawal_capped in (0, 1)
        assert reserve_shortfall <= peg_deposit_capped
        assert not (peg_deposit_capped and peg_withdrawal_capped)
        if turn["policy"] == "timemanager":
            # Candidate-level PEG admission is intentionally shadow-only
            # until a completed-depth policy passes held-out oracle replay.
            assert integer(turn, "peg_tm_admissions") == 0
        if reserve_shortfall:
            assert turn["policy"] == "timemanager"
            assert integer(turn, "bag") <= 4
            assert integer(turn, "peg_calls") == 1
            assert integer(turn, "fallbacks") == 0
        if peg_deposit_capped or peg_withdrawal_capped:
            assert turn["policy"] == "timemanager"
            assert integer(turn, "bag") <= 4
            assert integer(turn, "peg_calls") == 1
            # PEG calibration is deliberately shadow-only after the first
            # live oracle audit. Both proposed deposits and withdrawals must
            # leave the production budget at the proven equal slice.
            assert abs(
                float(turn["budget_ms"]) - float(turn["legacy_budget_ms"])
            ) <= 5.0
            if peg_deposit_capped:
                assert peg_shadow_budget_ms < float(turn["legacy_budget_ms"])
            else:
                assert peg_shadow_budget_ms > float(turn["legacy_budget_ms"])
        elif turn["policy"] != "timemanager" or integer(turn, "bag") > 4:
            assert peg_shadow_budget_ms == 0.0

        turns_by_game[integer(turn, "game")].append(turn)
    assert set(turns_by_game) == {1, 2}
    games_by_start = {integer(game, "start"): game for game in games}
    assert set(games_by_start) == {0, 1}
    for game_number, game_turns in turns_by_game.items():
        start = integer(game_turns[0], "start")
        game = games_by_start[start]
        assert (
            game["regret_sum_scope"]
            == "observed_path_conditional_not_forecast"
        )
        assert len(game_turns) == integer(game, "turns")
        assert [integer(turn, "turn") for turn in game_turns] == list(
            range(1, len(game_turns) + 1)
        )
        assert all(integer(turn, "game") == game_number for turn in game_turns)
        for player in (0, 1):
            player_turns = [
                turn for turn in game_turns if integer(turn, "player") == player
            ]
            known = [turn_regret(turn) for turn in player_turns]
            known_values = [value for value in known if value is not None]
            assert len(known_values) == integer(
                game, f"p{player}_regret_estimated_turns"
            )
            assert len(player_turns) - len(known_values) == integer(
                game, f"p{player}_regret_unknown_turns"
            )
            assert math.isclose(
                sum(known_values),
                float(game[f"p{player}_expected_utility_regret"]),
                rel_tol=0.0,
                abs_tol=5.0e-10,
            )

    sim_map = call_to_turn(turns, "sim")
    peg_map = call_to_turn(turns, "peg")
    eg_map = call_to_turn(turns, "eg")
    assert set(sim_map) == set(range(integer(benchmark, "sim_calls")))
    assert set(peg_map) == set(range(integer(benchmark, "peg_calls")))
    assert set(eg_map) == set(range(integer(benchmark, "eg_calls")))

    sims_by_call: dict[int, list[dict[str, str]]] = defaultdict(list)
    for event in sims:
        sims_by_call[integer(event, "call")].append(event)
    assert set(sims_by_call) == set(sim_map)
    for call_index, events in sims_by_call.items():
        assert sum(integer(event, "iterations") for event in events) == integer(
            sim_map[call_index], "sim_iters"
        )
        assert sum(integer(event, "selected") for event in events) == 1

    pegs_by_call: dict[int, list[dict[str, str]]] = defaultdict(list)
    for event in pegs:
        pegs_by_call[integer(event, "call")].append(event)
    for call_index, turn in peg_map.items():
        assert len(pegs_by_call[call_index]) == integer(
            turn, "peg_candidate_completions"
        )

    eg_by_call: dict[int, list[dict[str, str]]] = defaultdict(list)
    for event in endgames:
        eg_by_call[integer(event, "call")].append(event)
    assert set(eg_by_call) == set(eg_map)
    assert all(len(events) == 1 for events in eg_by_call.values())
    # A turn normally has one direct endgame call, but keep the audit correct if
    # PlayChooser retries or adds a second window. PCTURN stores per-turn sums;
    # PCEG stores one row per call.
    for turn in turns:
        first = integer(turn, "eg_call_first")
        count = integer(turn, "eg_calls")
        events = [eg_by_call[index][0] for index in range(first, first + count)]
        assert sum(integer(event, "nodes") for event in events) == integer(
            turn, "eg_nodes"
        )
        assert sum(integer(event, "depth") for event in events) == integer(
            turn, "eg_depth_sum"
        )

    paired_turns = min(len(turns_by_game[1]), len(turns_by_game[2]))
    first_divergence: int | None = None
    for index in range(paired_turns):
        first, second = turns_by_game[1][index], turns_by_game[2][index]
        # PCTURN captures the root before either policy chooses. Mirrored games
        # must therefore have the exact same replayable position through and
        # including their first move disagreement.
        assert first["cgp"] == second["cgp"]
        if first["move"] != second["move"]:
            first_divergence = index + 1
            break
        assert first["bag"] == second["bag"]
        assert first["spread_before"] == second["spread_before"]
    if first_divergence is None and len(turns_by_game[1]) != len(turns_by_game[2]):
        first_divergence = paired_turns + 1

    add_realized_path_rest_regret(turns_by_game)
    stem = log_path.with_suffix("")
    write_rows(Path(str(stem) + ".turns.csv"), turns)
    write_rows(Path(str(stem) + ".sim_candidates.csv"), add_turn_context(sims, sim_map))
    write_rows(Path(str(stem) + ".peg_candidates.csv"), add_turn_context(pegs, peg_map))
    write_rows(Path(str(stem) + ".endgames.csv"), add_turn_context(endgames, eg_map))
    return {
        "turns": turns,
        "turns_by_game": turns_by_game,
        "games": games,
        "benchmark": benchmark,
        "sim_rows": len(sims),
        "first_divergence": first_divergence,
    }


def sigmoid_spread(spread: float, scale: float) -> float:
    scaled = spread / scale
    if scaled >= 0:
        return 1.0 / (1.0 + math.exp(-scaled))
    exponential = math.exp(scaled)
    return exponential / (1.0 + exponential)


def terminal_utility(win: float, spread: float) -> float:
    return (win + 0.5 * sigmoid_spread(spread, 100.0)) / 1.5


def mode_for_turn(turn: dict[str, str]) -> str:
    if integer(turn, "sim_calls"):
        return "sim"
    if integer(turn, "peg_calls"):
        return "peg"
    if integer(turn, "eg_calls"):
        return "endgame"
    return "static"


def parse_pair(
    audit: dict[str, object], pair_number: int, seed: int, clock_ms: int,
    started_utc: str, finished_utc: str, elapsed_s: float,
) -> dict[str, object]:
    turns = audit["turns"]
    turns_by_game = audit["turns_by_game"]
    games = audit["games"]
    benchmark = audit["benchmark"]
    assert isinstance(turns, list) and isinstance(turns_by_game, dict)
    assert isinstance(games, list) and isinstance(benchmark, dict)
    first_divergence = audit["first_divergence"]
    paired_turns = min(len(turns_by_game[1]), len(turns_by_game[2]))
    identical_prefix = paired_turns if first_divergence is None else first_divergence - 1
    divergence_bag = ""
    divergence_cgp = ""
    moves = ["", ""]
    if first_divergence is not None:
        first = turns_by_game[1][first_divergence - 1]
        second = turns_by_game[2][first_divergence - 1]
        divergence_bag = first["bag"]
        divergence_cgp = first["cgp"]
        moves = [first["move"], second["move"]]

    spreads = [float(game["p0_score"]) - float(game["p1_score"]) for game in games]
    wins = [1.0 if spread > 0 else 0.0 if spread < 0 else 0.5 for spread in spreads]
    utilities = [terminal_utility(win, spread) for win, spread in zip(wins, spreads)]
    game_seeds = {integer(game, "seed") for game in games}
    assert len(game_seeds) == 1

    mode_ms = defaultdict(float)
    mode_calls = defaultdict(int)
    player_regret_fields: dict[str, object] = {}
    for game_number in (1, 2):
        for player in (0, 1):
            player_turns = [
                turn for turn in turns_by_game[game_number]
                if integer(turn, "player") == player
            ]
            policies = {turn["policy"] for turn in player_turns}
            assert len(policies) == 1
            known_values = [
                value for value in (turn_regret(turn) for turn in player_turns)
                if value is not None
            ]
            prefix = f"game{game_number}_p{player}"
            player_regret_fields[f"{prefix}_policy"] = next(iter(policies))
            player_regret_fields[f"{prefix}_expected_game_regret"] = (
                f"{sum(known_values):.12f}"
            )
            player_regret_fields[f"{prefix}_regret_estimated_turns"] = len(
                known_values
            )
            player_regret_fields[f"{prefix}_regret_unknown_turns"] = (
                len(player_turns) - len(known_values)
            )
    expected_regret = defaultdict(float)
    regret_estimated_turns = defaultdict(int)
    regret_unknown_turns = defaultdict(int)
    expected_post_divergence_regret = defaultdict(float)
    post_divergence_regret_estimated_turns = defaultdict(int)
    post_divergence_regret_unknown_turns = defaultdict(int)
    tm_sim_planned_budget_ms = 0.0
    tm_sim_legacy_budget_ms = 0.0
    tm_sim_released_turns = 0
    rule_zero_enabled_turns = 0
    rule_zero_stops = 0
    rule_zero_would_stops = 0
    tm_low_bag_fallbacks = 0
    tm_peg_reserve_shortfalls = 0
    tm_peg_shadow_budget_ms = 0.0
    tm_peg_legacy_budget_ms = 0.0
    tm_peg_deposit_caps = 0
    tm_peg_withdrawal_caps = 0
    for turn in turns:
        policy = turn["policy"]
        # Older traces predate the Rule-of-Zero fields; when present they must
        # be internally consistent: a stop implies enablement and a recorded
        # would-stop, and a shadow turn must never stop the search.
        if "rule_zero_enabled" in turn:
            rz_enabled = integer(turn, "rule_zero_enabled")
            rz_shadow = integer(turn, "rule_zero_shadow")
            rz_stopped = integer(turn, "rule_zero_stopped")
            rz_would_stop = integer(turn, "rule_zero_would_stop")
            assert rz_enabled in (0, 1) and rz_shadow in (0, 1)
            assert rz_stopped in (0, 1) and rz_would_stop in (0, 1)
            if rz_stopped:
                assert rz_enabled == 1 and rz_would_stop == 1 and \
                    rz_shadow == 0, (
                        f"inconsistent Rule-of-Zero stop at game "
                        f"{turn['game']} turn {turn['turn']}"
                    )
            if rz_would_stop:
                assert rz_enabled == 1
            rule_zero_enabled_turns += rz_enabled
            rule_zero_stops += rz_stopped
            rule_zero_would_stops += rz_would_stop
        regret = turn_regret(turn)
        if regret is None:
            regret_unknown_turns[policy] += 1
        else:
            expected_regret[policy] += regret
            regret_estimated_turns[policy] += 1
        if first_divergence is not None and integer(turn, "turn") >= first_divergence:
            if regret is None:
                post_divergence_regret_unknown_turns[policy] += 1
            else:
                expected_post_divergence_regret[policy] += regret
                post_divergence_regret_estimated_turns[policy] += 1
        mode = mode_for_turn(turn)
        mode_ms[(policy, mode)] += float(turn["elapsed_ms"])
        if policy == "timemanager" and mode == "sim":
            planned = float(turn["budget_ms"])
            legacy = float(turn["legacy_budget_ms"])
            assert planned + 0.001 >= legacy, (
                f"TimeManager shortened sim budget at game {turn['game']} "
                f"turn {turn['turn']}: {planned} < {legacy}"
            )
            tm_sim_planned_budget_ms += planned
            tm_sim_legacy_budget_ms += legacy
            if planned > legacy + 0.001:
                tm_sim_released_turns += 1
        if (
            policy == "timemanager"
            and integer(turn, "bag") <= 4
            and integer(turn, "fallbacks") > 0
        ):
            tm_low_bag_fallbacks += integer(turn, "fallbacks")
        if policy == "timemanager":
            tm_peg_reserve_shortfalls += integer(turn, "reserve_shortfall")
            tm_peg_deposit_caps += integer(turn, "peg_deposit_capped")
            tm_peg_withdrawal_caps += integer(turn, "peg_withdrawal_capped")
            if mode == "peg":
                tm_peg_shadow_budget_ms += float(turn["peg_shadow_budget_ms"])
                tm_peg_legacy_budget_ms += float(turn["legacy_budget_ms"])
        for family, label in (("sim", "sim"), ("peg", "peg"), ("eg", "endgame")):
            mode_calls[(policy, label)] += integer(turn, f"{family}_calls")

    # Before and including the first differing move, each normalized turn is
    # the same root with one policy in each mirrored game. The difference in
    # their residual-regret estimates is therefore the model's causal utility
    # prediction for the extra/withheld computation. Later roots are different
    # positions and are intentionally excluded from this paired prediction.
    shared_regret_comparisons = 0
    shared_regret_unknown_roots = 0
    predicted_shared_utility_delta = 0.0
    shared_limit = (
        paired_turns if first_divergence is None else min(first_divergence, paired_turns)
    )
    for index in range(shared_limit):
        root_turns = [turns_by_game[1][index], turns_by_game[2][index]]
        by_policy = {turn["policy"]: turn for turn in root_turns}
        assert set(by_policy) == {"timemanager", "equal"}
        tm_regret = turn_regret(by_policy["timemanager"])
        equal_regret = turn_regret(by_policy["equal"])
        if tm_regret is None or equal_regret is None:
            shared_regret_unknown_roots += 1
            continue
        shared_regret_comparisons += 1
        predicted_shared_utility_delta += equal_regret - tm_regret

    tm_time = sum(float(game["p0_time_ms"]) for game in games)
    equal_time = sum(float(game["p1_time_ms"]) for game in games)
    return {
        "pair": pair_number,
        "seed": seed,
        "game_seed": next(iter(game_seeds)),
        "started_utc": started_utc,
        "finished_utc": finished_utc,
        "elapsed_s": f"{elapsed_s:.3f}",
        "game1_turns": len(turns_by_game[1]),
        "game2_turns": len(turns_by_game[2]),
        "identical_prefix_turns": identical_prefix,
        "first_divergence_turn": "" if first_divergence is None else first_divergence,
        "first_divergence_bag": divergence_bag,
        "game1_divergence_move": moves[0],
        "game2_divergence_move": moves[1],
        "first_divergence_cgp": divergence_cgp,
        "fully_identical": int(first_divergence is None),
        "game1_spread": f"{spreads[0]:.6f}",
        "game1_win": f"{wins[0]:.6f}",
        "game1_utility": f"{utilities[0]:.12f}",
        "game2_spread": f"{spreads[1]:.6f}",
        "game2_win": f"{wins[1]:.6f}",
        "game2_utility": f"{utilities[1]:.12f}",
        "pair_spread_mean": f"{statistics.fmean(spreads):.6f}",
        "pair_win_score": f"{statistics.fmean(wins):.12f}",
        "pair_utility": f"{statistics.fmean(utilities):.12f}",
        **player_regret_fields,
        "tm_expected_game_regret":
            f"{expected_regret['timemanager']:.12f}",
        "equal_expected_game_regret": f"{expected_regret['equal']:.12f}",
        "tm_regret_estimated_turns": regret_estimated_turns["timemanager"],
        "equal_regret_estimated_turns": regret_estimated_turns["equal"],
        "tm_regret_unknown_turns": regret_unknown_turns["timemanager"],
        "equal_regret_unknown_turns": regret_unknown_turns["equal"],
        "tm_expected_post_divergence_regret":
            f"{expected_post_divergence_regret['timemanager']:.12f}",
        "equal_expected_post_divergence_regret":
            f"{expected_post_divergence_regret['equal']:.12f}",
        "tm_post_divergence_regret_estimated_turns":
            post_divergence_regret_estimated_turns["timemanager"],
        "equal_post_divergence_regret_estimated_turns":
            post_divergence_regret_estimated_turns["equal"],
        "tm_post_divergence_regret_unknown_turns":
            post_divergence_regret_unknown_turns["timemanager"],
        "equal_post_divergence_regret_unknown_turns":
            post_divergence_regret_unknown_turns["equal"],
        "shared_regret_comparisons": shared_regret_comparisons,
        "shared_regret_unknown_roots": shared_regret_unknown_roots,
        "predicted_shared_utility_delta":
            f"{predicted_shared_utility_delta:.12f}",
        "actual_minus_predicted_utility_delta":
            f"{statistics.fmean(utilities) - 0.5 - predicted_shared_utility_delta:.12f}",
        "tm_time_ms": f"{tm_time:.3f}",
        "equal_time_ms": f"{equal_time:.3f}",
        "tm_terminal_remaining_ms": f"{2 * clock_ms - tm_time:.3f}",
        "equal_terminal_remaining_ms": f"{2 * clock_ms - equal_time:.3f}",
        "tm_overtime_ms": f"{sum(float(game['p0_overtime_ms']) for game in games):.3f}",
        "equal_overtime_ms": f"{sum(float(game['p1_overtime_ms']) for game in games):.3f}",
        "tm_penalty": sum(integer(game, "p0_penalty") for game in games),
        "equal_penalty": sum(integer(game, "p1_penalty") for game in games),
        **{
            f"{policy}_{mode}_ms": f"{mode_ms[(name, mode)]:.3f}"
            for policy, name in (("tm", "timemanager"), ("equal", "equal"))
            for mode in ("sim", "peg", "endgame", "static")
        },
        **{
            f"{policy}_{mode}_calls": mode_calls[(name, mode)]
            for policy, name in (("tm", "timemanager"), ("equal", "equal"))
            for mode in ("sim", "peg", "endgame")
        },
        "tm_sim_planned_budget_ms": f"{tm_sim_planned_budget_ms:.3f}",
        "tm_sim_legacy_budget_ms": f"{tm_sim_legacy_budget_ms:.3f}",
        "tm_sim_released_budget_ms":
            f"{tm_sim_planned_budget_ms - tm_sim_legacy_budget_ms:.3f}",
        "tm_sim_released_turns": tm_sim_released_turns,
        "rule_zero_enabled_turns": rule_zero_enabled_turns,
        "rule_zero_stops": rule_zero_stops,
        "rule_zero_would_stops": rule_zero_would_stops,
        "tm_low_bag_fallbacks": tm_low_bag_fallbacks,
        "tm_peg_reserve_shortfalls": tm_peg_reserve_shortfalls,
        "tm_peg_shadow_budget_ms": f"{tm_peg_shadow_budget_ms:.3f}",
        "tm_peg_legacy_budget_ms": f"{tm_peg_legacy_budget_ms:.3f}",
        "tm_peg_deposit_caps": tm_peg_deposit_caps,
        "tm_peg_withdrawal_caps": tm_peg_withdrawal_caps,
        "sim_calls": benchmark["sim_calls"],
        "sim_iters": benchmark["sim_iters"],
        "sim_nodes": benchmark["sim_nodes"],
        "sim_candidate_rows": audit["sim_rows"],
        "peg_calls": benchmark["peg_calls"],
        "peg_candidate_completions": benchmark["peg_candidate_completions"],
        "peg_tm_admissions": benchmark["peg_tm_admissions"],
        "peg_tm_false_starts": benchmark["peg_tm_false_starts"],
        "eg_calls": benchmark["eg_calls"],
        "eg_nodes": benchmark["eg_nodes"],
        "eg_depth_sum": benchmark["eg_depth"],
    }


def beta_fraction(a: float, b: float, x: float) -> float:
    qab, qap, qam = a + b, a + 1.0, a - 1.0
    tiny, c = 1.0e-300, 1.0
    d = 1.0 - qab * x / qap
    d = 1.0 / (tiny if abs(d) < tiny else d)
    result = d
    for iteration in range(1, 301):
        twice = 2 * iteration
        aa = iteration * (b - iteration) * x / ((qam + twice) * (a + twice))
        d = 1.0 + aa * d
        d = tiny if abs(d) < tiny else d
        c = 1.0 + aa / c
        c = tiny if abs(c) < tiny else c
        d = 1.0 / d
        result *= d * c
        aa = -(a + iteration) * (qab + iteration) * x / ((a + twice) * (qap + twice))
        d = 1.0 + aa * d
        d = tiny if abs(d) < tiny else d
        c = 1.0 + aa / c
        c = tiny if abs(c) < tiny else c
        d = 1.0 / d
        delta = d * c
        result *= delta
        if abs(delta - 1.0) <= 3.0e-14:
            return result
    raise ArithmeticError("incomplete beta did not converge")


def regularized_beta(x: float, a: float, b: float) -> float:
    if x <= 0.0:
        return 0.0
    if x >= 1.0:
        return 1.0
    front = math.exp(
        math.lgamma(a + b) - math.lgamma(a) - math.lgamma(b)
        + a * math.log(x) + b * math.log1p(-x)
    )
    if x < (a + 1.0) / (a + b + 2.0):
        return front * beta_fraction(a, b, x) / a
    return 1.0 - front * beta_fraction(b, a, 1.0 - x) / b


def two_sided_t_pvalue(value: float, degrees_freedom: int) -> float:
    if not math.isfinite(value):
        return 0.0
    x = degrees_freedom / (degrees_freedom + value * value)
    return min(1.0, max(0.0, regularized_beta(x, degrees_freedom / 2.0, 0.5)))


def metric_summary(values: list[float], null: float = 0.0) -> str:
    mean = statistics.fmean(values)
    if len(values) < 2:
        return f"{mean:+.6f} ci=NA p=NA"
    sem = statistics.stdev(values) / math.sqrt(len(values))
    if sem == 0.0:
        p_value = 1.0 if mean == null else 0.0
        return f"{mean:+.6f} ci=[{mean:+.6f},{mean:+.6f}] p={p_value:.6f}"
    low, high = 0.0, 100.0
    for _ in range(100):
        middle = (low + high) / 2.0
        if two_sided_t_pvalue(middle, len(values) - 1) > 0.05:
            low = middle
        else:
            high = middle
    critical = (low + high) / 2.0
    p_value = two_sided_t_pvalue((mean - null) / sem, len(values) - 1)
    return (
        f"{mean:+.6f} ci=[{mean - critical * sem:+.6f},"
        f"{mean + critical * sem:+.6f}] p={p_value:.6f}"
    )


def build_report(rows: list[dict[str, str]], target: str) -> str:
    prefixes = [integer(row, "identical_prefix_turns") for row in rows]
    divergences = [row["first_divergence_turn"] or "none" for row in rows]
    spreads = [float(row["pair_spread_mean"]) for row in rows]
    wins = [float(row["pair_win_score"]) - 0.5 for row in rows]
    utilities = [float(row["pair_utility"]) - 0.5 for row in rows]
    predicted_utilities = [
        float(row["predicted_shared_utility_delta"]) for row in rows
    ]
    prediction_residuals = [
        float(row["actual_minus_predicted_utility_delta"]) for row in rows
    ]
    expected_game_regret = {
        policy: [float(row[f"{policy}_expected_game_regret"]) for row in rows]
        for policy in ("tm", "equal")
    }
    regret_estimated_turns = {
        policy: sum(integer(row, f"{policy}_regret_estimated_turns") for row in rows)
        for policy in ("tm", "equal")
    }
    regret_unknown_turns = {
        policy: sum(integer(row, f"{policy}_regret_unknown_turns") for row in rows)
        for policy in ("tm", "equal")
    }
    tm_reserve = [float(row["tm_terminal_remaining_ms"]) / 2000.0 for row in rows]
    equal_reserve = [float(row["equal_terminal_remaining_ms"]) / 2000.0 for row in rows]
    released_budget_seconds = sum(
        float(row["tm_sim_released_budget_ms"]) for row in rows
    ) / 1000.0
    peg_shadow_budget_delta_seconds = sum(
        float(row["tm_peg_shadow_budget_ms"])
        - float(row["tm_peg_legacy_budget_ms"])
        for row in rows
    ) / 1000.0
    released_turns = sum(integer(row, "tm_sim_released_turns") for row in rows)
    low_bag_fallbacks = sum(integer(row, "tm_low_bag_fallbacks") for row in rows)
    peg_false_starts = sum(integer(row, "peg_tm_false_starts") for row in rows)
    penalties = sum(
        integer(row, policy + "_penalty")
        for row in rows
        for policy in ("tm", "equal")
    )
    mode_seconds = {
        (policy, mode): sum(float(row[f"{policy}_{mode}_ms"]) for row in rows)
        / 1000.0
        for policy in ("tm", "equal")
        for mode in ("sim", "peg", "endgame", "static")
    }
    peg_seconds_saved = (
        mode_seconds[("equal", "peg")] - mode_seconds[("tm", "peg")]
    )
    analysis_seconds_reinvested = (
        mode_seconds[("tm", "sim")]
        + mode_seconds[("tm", "endgame")]
        - mode_seconds[("equal", "sim")]
        - mode_seconds[("equal", "endgame")]
    )
    reinvestment_fraction = (
        analysis_seconds_reinvested / peg_seconds_saved
        if peg_seconds_saved > 0.0
        else math.nan
    )
    gate_ready = len(rows) >= 5
    gate_passes = (
        penalties == 0
        and low_bag_fallbacks == 0
        and peg_false_starts == 0
        and released_turns > 0
        and released_budget_seconds > 0.0
    )
    latest_player_regret_lines = []
    for game_number in (1, 2):
        player_parts = []
        for player in (0, 1):
            prefix = f"game{game_number}_p{player}"
            estimated = integer(rows[-1], f"{prefix}_regret_estimated_turns")
            unknown = integer(rows[-1], f"{prefix}_regret_unknown_turns")
            player_parts.append(
                f"p{player}/{rows[-1][f'{prefix}_policy']}="
                f"{float(rows[-1][f'{prefix}_expected_game_regret']):.6f} "
                f"coverage={estimated}/{estimated + unknown}"
            )
        latest_player_regret_lines.append(
            f"latest_pair_observed_path_conditional_regret_sum game={game_number} "
            + " ".join(player_parts)
        )
    lines = [
        f"MATCH_UPDATE pairs={len(rows)} target={target}",
        f"first_divergence={','.join(divergences)} fully_identical="
        f"{sum(integer(row, 'fully_identical') for row in rows)}",
        f"identical_prefix mean={statistics.fmean(prefixes):.3f} "
        f"median={statistics.median(prefixes):.3f}",
        "spread_delta " + metric_summary(spreads),
        "win_score_delta " + metric_summary(wins),
        "utility_delta " + metric_summary(utilities),
        "observed_path_conditional_regret_sum mean_per_player_game tm/equal="
        f"{statistics.fmean(expected_game_regret['tm']) / 2.0:.6f}/"
        f"{statistics.fmean(expected_game_regret['equal']) / 2.0:.6f} "
        "coverage_turns="
        f"{regret_estimated_turns['tm']}/"
        f"{regret_estimated_turns['tm'] + regret_unknown_turns['tm']} vs "
        f"{regret_estimated_turns['equal']}/"
        f"{regret_estimated_turns['equal'] + regret_unknown_turns['equal']}",
        *latest_player_regret_lines,
        "latest_post_divergence_conditional_regret_sum tm/equal="
        f"{float(rows[-1]['tm_expected_post_divergence_regret']):.6f}/"
        f"{float(rows[-1]['equal_expected_post_divergence_regret']):.6f} "
        "estimated_unknown_turns="
        f"{rows[-1]['tm_post_divergence_regret_estimated_turns']}/"
        f"{rows[-1]['tm_post_divergence_regret_unknown_turns']} vs "
        f"{rows[-1]['equal_post_divergence_regret_estimated_turns']}/"
        f"{rows[-1]['equal_post_divergence_regret_unknown_turns']}",
        "shared_root_conditional_bai_predicted_utility_delta "
        + metric_summary(predicted_utilities)
        + " comparisons="
        + str(sum(integer(row, "shared_regret_comparisons") for row in rows))
        + " unknown_roots="
        + str(sum(integer(row, "shared_regret_unknown_roots") for row in rows)),
        "utility_prediction_residual actual_minus_predicted "
        + metric_summary(prediction_residuals),
        f"mean_terminal_seconds tm/equal={statistics.fmean(tm_reserve):.3f}/"
        f"{statistics.fmean(equal_reserve):.3f}",
        "mode_seconds tm/equal "
        + " ".join(
            f"{mode}={mode_seconds[('tm', mode)]:.3f}/"
            f"{mode_seconds[('equal', mode)]:.3f}"
            for mode in ("sim", "peg", "endgame", "static")
        ),
        f"descriptive_mode_delta peg_saved_seconds={peg_seconds_saved:.3f} "
        f"extra_sim_plus_endgame_seconds={analysis_seconds_reinvested:.3f} "
        f"ratio={reinvestment_fraction:.3f} noncausal_after_divergence=1",
        f"planned_sim_release seconds={released_budget_seconds:.3f} "
        f"turns={released_turns}",
        "shadow_peg_budget_delta seconds="
        f"{peg_shadow_budget_delta_seconds:+.3f}",
        f"timemanager_low_bag_fallbacks={low_bag_fallbacks}",
        "timemanager_peg_reserve_shortfalls="
        f"{sum(integer(row, 'tm_peg_reserve_shortfalls') for row in rows)}",
        "timemanager_peg_deposit_caps="
        f"{sum(integer(row, 'tm_peg_deposit_caps') for row in rows)}",
        "timemanager_peg_withdrawal_caps="
        f"{sum(integer(row, 'tm_peg_withdrawal_caps') for row in rows)}",
        f"timemanager_peg_false_starts={peg_false_starts}",
        f"penalties tm/equal={sum(integer(row, 'tm_penalty') for row in rows)}/"
        f"{sum(integer(row, 'equal_penalty') for row in rows)}",
        "operational_gate "
        + ("PENDING" if not gate_ready else "PASS" if gate_passes else "FAIL")
        + f" pairs={len(rows)}/5 trace_audit=pass",
        "method mirrored seat RNG; game pair is inference unit; regret values "
        "are BAI utility estimates; PEG/endgame/static remain explicit unknowns; "
        "later moves after first divergence are incomparable",
    ]
    return "\n".join(lines)


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--repo", type=Path, default=Path(__file__).resolve().parent.parent
    )
    parser.add_argument("--run-dir", type=Path, required=True)
    parser.add_argument("--binary", type=Path)
    parser.add_argument("--pairs", type=int)
    parser.add_argument("--hours", type=float)
    parser.add_argument("--clock-ms", type=int, default=180_000)
    parser.add_argument("--threads", type=int, default=os.cpu_count() or 1)
    parser.add_argument("--base-seed", type=int, default=25_001)
    args = parser.parse_args()
    if args.pairs is None and args.hours is None:
        args.pairs = 5
    if args.pairs is not None and args.pairs <= 0:
        parser.error("--pairs must be positive")
    if args.hours is not None and args.hours <= 0:
        parser.error("--hours must be positive")

    repo = args.repo.resolve()
    run_dir = args.run_dir.resolve()
    binary = (args.binary or repo / "bin" / "magpie_test").resolve()
    if not binary.is_file():
        raise FileNotFoundError(binary)
    pair_dir = run_dir / "pairs"
    pair_dir.mkdir(parents=True, exist_ok=True)
    csv_path = run_dir / "pairs.csv"
    state_path = run_dir / "state.json"
    latest_path = run_dir / "latest.txt"
    progress_path = run_dir / "progress.log"

    rows: list[dict[str, str]] = []
    if csv_path.exists():
        with csv_path.open(newline="", encoding="utf-8") as stream:
            rows = list(csv.DictReader(stream))
    assert [integer(row, "pair") for row in rows] == list(range(1, len(rows) + 1))
    state = (
        json.loads(state_path.read_text(encoding="utf-8"))
        if state_path.exists()
        else {
            "status": "running",
            "start_utc": utc_now(),
            "start_epoch": time.time(),
        }
    )
    # Older development runs used a misleading start_monotonic key whose
    # value was nevertheless wall-clock epoch time. Preserve resumability if
    # one of those state files is supplied.
    if "start_epoch" not in state and "start_monotonic" in state:
        state["start_epoch"] = state.pop("start_monotonic")
    state.setdefault("start_epoch", time.time())
    deadline_epoch = None
    if args.hours is not None:
        deadline_epoch = float(state["start_epoch"]) + args.hours * 3600.0
    target = (
        f"{args.pairs} pairs"
        if args.pairs is not None
        else f"{args.hours:g} hours"
    )
    state.update(
        {
            "status": "running",
            "target": target,
            "pairs": len(rows),
            "updated_utc": utc_now(),
        }
    )
    protocol_path = run_dir / "protocol.json"
    protocol = {
        "players": {
            "p0": "TimeManager with PEG allocation shadowed",
            "p1": "equal slicing",
        },
        "target": target,
        "base_seed": args.base_seed,
        "seed_schedule": "base_seed + pair_number - 1",
        "clock_ms_per_player_per_game": args.clock_ms,
        "threads": args.threads,
        "lexicon": "CSW24",
        "lookup_configuration": "WMP + RIT mmap + WIT",
        "common_rng": "stream 0 starting seat; stream 1 replying seat; mirrored",
        "trace": (
            "every turn with normalized root CGP and budget-decision flags, "
            "conditional current-candidate BAI regret, an explicit invalid "
            "value-to-go marker, retrospective realized-path sums, "
            "every sim arm, PEG candidate completion, endgame call"
        ),
        "regret_model": (
            "BAI expected sampling regret conditional on the sim candidate set "
            "and rollout policy, and zero only for a forced single candidate; "
            "this is not oracle turn regret or rest-of-game value; "
            "PEG/endgame/static are unknown, never zero-filled"
        ),
        "overtime_penalty": "1 point per 1000 ms",
        "terminal_utility": "(win_score + 0.5 * logistic(spread / 100)) / 1.5",
        "binary": str(binary),
        "binary_sha256": sha256(binary),
        "runner_sha256": sha256(Path(__file__).resolve()),
        "git_head": subprocess.check_output(
            ["git", "rev-parse", "HEAD"], cwd=repo, text=True
        ).strip(),
        "git_dirty": bool(
            subprocess.check_output(
                ["git", "status", "--porcelain"], cwd=repo, text=True
            ).strip()
        ),
    }
    if protocol_path.exists():
        existing_protocol = json.loads(protocol_path.read_text(encoding="utf-8"))
        if existing_protocol != protocol:
            raise RuntimeError(
                f"refusing to mix protocols in existing run directory: {protocol_path}"
            )
    else:
        atomic_write(
            protocol_path, json.dumps(protocol, indent=2, sort_keys=True) + "\n"
        )
    # Persist the original epoch before starting a pair so an interruption
    # cannot silently turn a resumed 24-hour match into another full 24 hours.
    atomic_write(state_path, json.dumps(state, indent=2, sort_keys=True) + "\n")

    while True:
        if args.pairs is not None and len(rows) >= args.pairs:
            break
        if deadline_epoch is not None and time.time() >= deadline_epoch:
            break
        pair_number = len(rows) + 1
        seed = args.base_seed + pair_number - 1
        log_path = pair_dir / f"pair-{pair_number:04d}-seed-{seed}.log"
        err_path = pair_dir / f"pair-{pair_number:04d}-seed-{seed}.err"
        metadata_path = pair_dir / f"pair-{pair_number:04d}-seed-{seed}.meta.json"
        started_utc = utc_now()
        started = time.monotonic()
        state.update(
            {
                "status": "running",
                "pairs": len(rows),
                "current_pair": pair_number,
                "current_seed": seed,
                "current_pair_started_utc": started_utc,
                "updated_utc": started_utc,
            }
        )
        atomic_write(state_path, json.dumps(state, indent=2, sort_keys=True) + "\n")
        environment = os.environ.copy()
        environment.update(
            {
                "PCBENCH_GAMES": "1",
                "PCBENCH_CLOCK_MS": str(args.clock_ms),
                "PCBENCH_THREADS": str(args.threads),
                "PCBENCH_WIT": "true",
                "PCBENCH_SEED": str(seed),
                "PCBENCH_GAME_PAIR": "true",
                "PCBENCH_COMMON_RNG": "true",
                "PCBENCH_TM_P0_ONLY": "true",
                "PCBENCH_TURN_ROWS": "true",
                "PCBENCH_GAME_ROWS": "true",
                "PCBENCH_DETAIL_EVENTS": "true",
            }
        )
        partial_log = log_path.with_suffix(".log.part")
        partial_err = err_path.with_suffix(".err.part")
        with partial_log.open("w", encoding="utf-8") as stdout_stream, partial_err.open(
            "w", encoding="utf-8"
        ) as stderr_stream:
            completed = subprocess.run(
                [str(binary), "pcbench"], cwd=repo, env=environment,
                stdout=stdout_stream, stderr=stderr_stream,
                timeout=max(1800, int(args.clock_ms * 8 / 1000)), check=False,
            )
        elapsed_s = time.monotonic() - started
        finished_utc = utc_now()
        if completed.returncode != 0:
            raise RuntimeError(f"pair {pair_number} exited {completed.returncode}")
        if partial_err.stat().st_size:
            raise RuntimeError(f"pair {pair_number} wrote stderr: {partial_err}")
        partial_log.replace(log_path)
        partial_err.replace(err_path)
        atomic_write(
            metadata_path,
            json.dumps(
                {
                    "started_utc": started_utc,
                    "finished_utc": finished_utc,
                    "elapsed_s": elapsed_s,
                },
                indent=2, sort_keys=True,
            ) + "\n",
        )
        audit = audit_trace(log_path)
        row = parse_pair(
            audit, pair_number, seed, args.clock_ms, started_utc, finished_utc,
            elapsed_s,
        )
        needs_header = not csv_path.exists() or csv_path.stat().st_size == 0
        with csv_path.open("a", newline="", encoding="utf-8") as stream:
            writer = csv.DictWriter(stream, fieldnames=CSV_FIELDS)
            if needs_header:
                writer.writeheader()
            writer.writerow(row)
            stream.flush()
            os.fsync(stream.fileno())
        rows.append({key: str(value) for key, value in row.items()})
        state.update(
            {
                "pairs": len(rows),
                "updated_utc": finished_utc,
            }
        )
        state.pop("current_pair", None)
        state.pop("current_seed", None)
        state.pop("current_pair_started_utc", None)
        atomic_write(state_path, json.dumps(state, indent=2, sort_keys=True) + "\n")
        report = build_report(rows, target)
        atomic_write(latest_path, report + "\n")
        with progress_path.open("a", encoding="utf-8") as stream:
            stream.write(f"PAIR_DONE {pair_number} {finished_utc}\n{report}\n")
        print(report, flush=True)

    state.update({"status": "complete", "completed_utc": utc_now(), "pairs": len(rows)})
    atomic_write(state_path, json.dumps(state, indent=2, sort_keys=True) + "\n")
    print("MATCH_DONE", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
