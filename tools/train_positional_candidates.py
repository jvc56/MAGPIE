#!/usr/bin/env python3
"""Fit a cheap positional reranker from paired candidate rollouts.

The positional oracle emits the same top-N static candidates under shared
unseen-tile scenarios.  This trainer centers both labels and features within a
position, so it learns candidate differences rather than absolute game
outcomes.  The existing static equity is retained with coefficient one; ridge
regression learns only an additive positional correction.

Evaluation is five-fold cross-fitting by source game.  Its primary statistic
is the paired oracle-spread gain from the cross-fitted model's selected move
over ordinary static evaluation's selected move.
"""

from __future__ import annotations

import argparse
import json
import math
from dataclasses import dataclass
from pathlib import Path

import numpy as np

from train_positional_adjustment import (
    BOARD_DIM,
    Fit,
    fit_ridge,
    global_features,
    local_features,
    read_distribution,
    read_layout,
)


EQUITY_RESOLUTION = 1000.0
BLANK_MASK = 0x80
UNBLANK_MASK = 0x7F
PLACEMENT_MOVE = 1


@dataclass
class Candidate:
    position: int
    game: int
    turn: int
    bag: int
    score_diff: int
    rank: int
    base_equity: float
    move_score: float
    oracle_spread: float
    oracle_spread_sem: float
    oracle_utility: float
    oracle_win: float
    features: dict[str, float]


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--oracle-log", required=True, type=Path)
    parser.add_argument(
        "--test-oracle-log",
        type=Path,
        help="optional untouched corpus for final train-on-all evaluation",
    )
    parser.add_argument(
        "--hook-feature-log",
        type=Path,
        help="optional C-extracted post-play cross-set/leave feature sidecar",
    )
    parser.add_argument(
        "--test-hook-feature-log",
        type=Path,
        help="hook-feature sidecar paired with --test-oracle-log",
    )
    parser.add_argument("--letter-distribution", required=True, type=Path)
    parser.add_argument("--layout", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--seed", type=int, default=620)
    parser.add_argument("--folds", type=int, default=5)
    return parser.parse_args()


def parse_board(encoded: str) -> list[list[str | None]]:
    board: list[list[str | None]] = []
    for encoded_row in encoded.split("/"):
        row: list[str | None] = []
        digits = ""
        for char in encoded_row:
            if char.isdigit():
                digits += char
                continue
            if digits:
                row.extend([None] * int(digits))
                digits = ""
            row.append(char)
        if digits:
            row.extend([None] * int(digits))
        if len(row) != BOARD_DIM:
            raise ValueError(f"invalid CGP row {encoded_row!r}: {len(row)} cells")
        board.append(row)
    if len(board) != BOARD_DIM:
        raise ValueError(f"invalid CGP board: {len(board)} rows")
    return board


def parse_fields(prefix: str) -> dict[str, str]:
    result: dict[str, str] = {}
    for token in prefix.split()[1:]:
        name, value = token.split("=", 1)
        result[name] = value
    return result


def add_phase_interactions(
    features: dict[str, float], bag: int, score_diff: int
) -> dict[str, float]:
    result = dict(features)
    bag_bucket = (
        "bag_61plus"
        if bag >= 61
        else "bag_41_60"
        if bag >= 41
        else "bag_21_40"
        if bag >= 21
        else "bag_0_20"
    )
    score_bucket = (
        "ahead" if score_diff >= 25 else "behind" if score_diff <= -25 else "close"
    )
    # Phase-varying coefficients are cheap to deploy as a table selection.
    for name, value in features.items():
        result[f"{bag_bucket}__{name}"] = value
        if name.startswith("compact_"):
            result[f"{score_bucket}__{name}"] = value
    return result


def candidate_features(
    cgp: str,
    raw_move: str,
    bag: int,
    layout: list[list[str]],
    scores: dict[str, int],
    machine_letters: list[str],
) -> dict[str, float]:
    cgp_fields = cgp.split()
    if len(cgp_fields) < 4:
        raise ValueError(f"invalid CGP: {cgp!r}")
    board = parse_board(cgp_fields[0])
    score_parts = [int(value) for value in cgp_fields[2].split("/")]
    score_diff = score_parts[0] - score_parts[1]
    raw = [int(value) for value in raw_move.split(",")]
    if len(raw) < 8:
        raise ValueError(f"invalid raw move: {raw_move!r}")
    move_type, row, col, direction, tiles_played, tiles_length = raw[:6]
    encoded_score, encoded_equity = raw[6:8]
    strip = raw[8:]
    if len(strip) != tiles_length:
        raise ValueError(
            f"move length {tiles_length} has {len(strip)} encoded tiles"
        )

    result: dict[str, float] = {
        "move_tiles_played": float(tiles_played),
        "move_bingo": float(tiles_played == 7),
        "move_exchange": float(move_type != PLACEMENT_MOVE),
        "move_score": encoded_score / EQUITY_RESOLUTION,
        "move_leave": (encoded_equity - encoded_score) / EQUITY_RESOLUTION,
    }
    if move_type != PLACEMENT_MOVE:
        return add_phase_interactions(result, bag, score_diff)

    post_board = [board_row.copy() for board_row in board]
    dr, dc = ((1, 0) if direction else (0, 1))
    new_tiles: list[tuple[int, int, str, bool]] = []
    for offset, machine_letter in enumerate(strip):
        tile_row, tile_col = row + offset * dr, col + offset * dc
        if machine_letter == 0:
            if post_board[tile_row][tile_col] is None:
                raise ValueError("played-through marker crosses an empty square")
            continue
        blank = bool(machine_letter & BLANK_MASK)
        letter_index = machine_letter & UNBLANK_MASK
        letter = machine_letters[letter_index]
        if letter == "?":
            raise ValueError("placed blank did not encode its represented letter")
        if post_board[tile_row][tile_col] is not None:
            raise ValueError("candidate placement overlaps an occupied square")
        post_board[tile_row][tile_col] = letter.lower() if blank else letter
        new_tiles.append((tile_row, tile_col, letter, blank))

    local = local_features(post_board, new_tiles, dr, dc, layout, scores)
    global_before = global_features(board, layout, scores)
    global_after = global_features(post_board, layout, scores)
    for name, value in local.items():
        result[name.removeprefix("local_")] = value
    for name in global_before.keys() | global_after.keys():
        delta = global_after.get(name, 0.0) - global_before.get(name, 0.0)
        if delta:
            result[f"delta_{name.removeprefix('global_')}"] = delta
    return add_phase_interactions(result, bag, score_diff)


def parse_log(
    path: Path,
    layout: list[list[str]],
    scores: dict[str, int],
    machine_letters: list[str],
) -> list[Candidate]:
    candidates: list[Candidate] = []
    for line in path.read_text(encoding="utf-8").splitlines():
        if not line.startswith("POSITIONAL_CANDIDATE "):
            continue
        before_cgp, cgp = line.split(" cgp=", 1)
        before_raw, raw_move = before_cgp.split(" move_raw=", 1)
        # The human-readable move may contain no spaces, but removing it here
        # keeps parsing robust if notation changes.
        fields_part, _move = before_raw.rsplit(" move=", 1)
        fields = parse_fields(fields_part)
        bag = int(fields["bag"])
        features = candidate_features(
            cgp, raw_move, bag, layout, scores, machine_letters
        )
        candidates.append(
            Candidate(
                position=int(fields["position"]),
                game=int(fields["game"]),
                turn=int(fields["turn"]),
                bag=bag,
                score_diff=(
                    int(cgp.split()[2].split("/")[0])
                    - int(cgp.split()[2].split("/")[1])
                ),
                rank=int(fields["rank"]),
                base_equity=float(fields["base_equity"]),
                move_score=float(fields["move_score"]),
                oracle_spread=float(fields["oracle_spread"]),
                oracle_spread_sem=float(fields["oracle_spread_sem"]),
                oracle_utility=float(fields["oracle_utility"]),
                oracle_win=float(fields["oracle_win"]),
                features=features,
            )
        )
    if not candidates:
        raise ValueError(f"{path}: no POSITIONAL_CANDIDATE rows")
    return candidates


def merge_hook_features(
    candidates: list[Candidate], path: Path
) -> None:
    by_key: dict[tuple[int, int], dict[str, float]] = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        if not line.startswith("POSITIONAL_HOOK_FEATURE "):
            continue
        fields = parse_fields(line)
        key = (int(fields.pop("position")), int(fields.pop("rank")))
        if key in by_key:
            raise ValueError(f"{path}: duplicate hook feature key {key}")
        by_key[key] = {
            f"compact_{name}": float(value)
            for name, value in fields.items()
            if float(value) != 0.0
        }
    candidate_keys = {(candidate.position, candidate.rank) for candidate in candidates}
    if set(by_key) != candidate_keys:
        missing = sorted(candidate_keys - set(by_key))
        extra = sorted(set(by_key) - candidate_keys)
        raise ValueError(
            f"{path}: hook feature key mismatch; "
            f"missing={missing[:5]} extra={extra[:5]}"
        )
    for candidate in candidates:
        candidate.features.update(
            add_phase_interactions(
                by_key[(candidate.position, candidate.rank)],
                candidate.bag,
                candidate.score_diff,
            )
        )


def feature_matrix(
    candidates: list[Candidate], model: str
) -> tuple[np.ndarray, list[str]]:
    def selected(name: str) -> bool:
        if model == "compact_base":
            return (
                (name.startswith("compact_") or "__compact_" in name)
                and "compact_hook_" not in name
            )
        if model == "compact_hooks":
            if not (name.startswith("compact_") or "__compact_" in name):
                return False
            canonical_name = name.rsplit("__", 1)[-1]
            # These are useful diagnostics but are exact sums/partitions of
            # the held and live counts. Including both sides makes rare dead
            # hooks nearly collinear and produces unstable raw coefficients.
            # The additive held reward plus live risk already represents all
            # four cases: safe-held, contested, opponent-only, and dead.
            return (
                "compact_hook_letter_" not in canonical_name
                and canonical_name
                not in {
                    "compact_hook_total",
                    "compact_hook_dead",
                    "compact_hook_held_safe",
                    "compact_hook_held_contested",
                    "compact_hook_opponent_only",
                }
            )
        if model == "compact_hook_letters":
            return name.startswith("compact_") or "__compact_" in name
        if model == "full":
            return True
        if model == "compact":
            return name.startswith("compact_") or "__compact_" in name
        if model == "local":
            return not (
                name.startswith("delta_")
                or "__delta_" in name
                or name.startswith("move_")
                or "__move_" in name
            )
        raise ValueError(f"unknown model {model}")

    names = sorted(
        {
            name
            for candidate in candidates
            for name in candidate.features
            if selected(name)
        }
    )
    index = {name: column for column, name in enumerate(names)}
    values = np.zeros((len(candidates), len(names)), dtype=np.float64)
    for row, candidate in enumerate(candidates):
        for name, value in candidate.features.items():
            column = index.get(name)
            if column is not None:
                values[row, column] = value
    return values, names


def center_by_position(
    candidates: list[Candidate], values: np.ndarray
) -> np.ndarray:
    positions = np.asarray([candidate.position for candidate in candidates])
    _unique_positions, inverse = np.unique(positions, return_inverse=True)
    group_count = int(inverse.max()) + 1
    group_sums = np.zeros((group_count, values.shape[1]), dtype=np.float64)
    np.add.at(group_sums, inverse, values)
    group_sizes = np.bincount(inverse, minlength=group_count)
    group_means = group_sums / group_sizes[:, None]
    centered = values - group_means[inverse]
    return centered


def choose_ridge(
    train_values: np.ndarray,
    train_target: np.ndarray,
    valid_values: np.ndarray,
    valid_target: np.ndarray,
) -> Fit:
    best: tuple[float, Fit] | None = None
    for alpha in (
        0.01,
        0.1,
        1.0,
        10.0,
        100.0,
        1_000.0,
        10_000.0,
        100_000.0,
        1_000_000.0,
    ):
        fitted = fit_ridge(train_values, train_target, alpha)
        error = valid_target - fitted.predict(valid_values)
        mse = float(np.mean(error * error))
        if best is None or mse < best[0]:
            best = (mse, fitted)
    assert best is not None
    return best[1]


def mean_sem_ci_p(values: np.ndarray) -> dict[str, float]:
    mean = float(values.mean())
    if len(values) < 2:
        return {
            "mean": mean,
            "sem": math.nan,
            "ci95_low": math.nan,
            "ci95_high": math.nan,
            "p_two_sided_normal": math.nan,
        }
    sem = float(values.std(ddof=1) / math.sqrt(len(values)))
    z = mean / sem if sem > 0 else 0.0
    return {
        "mean": mean,
        "sem": sem,
        "ci95_low": mean - 1.96 * sem,
        "ci95_high": mean + 1.96 * sem,
        "p_two_sided_normal": math.erfc(abs(z) / math.sqrt(2.0)),
    }


def selection_metrics(
    candidates: list[Candidate], adjustment: np.ndarray
) -> dict[str, object]:
    positions = np.asarray([candidate.position for candidate in candidates])
    games = np.asarray([candidate.game for candidate in candidates])
    base_equity = np.asarray(
        [candidate.base_equity for candidate in candidates], dtype=np.float64
    )
    oracle_spread = np.asarray(
        [candidate.oracle_spread for candidate in candidates], dtype=np.float64
    )
    position_gains: list[float] = []
    position_regret_base: list[float] = []
    position_regret_model: list[float] = []
    changed = 0
    model_wins = 0
    baseline_wins = 0
    game_gains: dict[int, list[float]] = {}
    for position in np.unique(positions):
        indices = np.flatnonzero(positions == position)
        baseline_index = indices[np.argmax(base_equity[indices])]
        model_index = indices[np.argmax(base_equity[indices] + adjustment[indices])]
        oracle_best = float(np.max(oracle_spread[indices]))
        gain = float(oracle_spread[model_index] - oracle_spread[baseline_index])
        position_gains.append(gain)
        position_regret_base.append(oracle_best - oracle_spread[baseline_index])
        position_regret_model.append(oracle_best - oracle_spread[model_index])
        game_gains.setdefault(int(games[indices[0]]), []).append(gain)
        if model_index != baseline_index:
            changed += 1
            if gain > 0:
                model_wins += 1
            elif gain < 0:
                baseline_wins += 1

    gains = np.asarray(position_gains)
    clustered = np.asarray(
        [float(np.mean(per_game)) for per_game in game_gains.values()]
    )
    return {
        "positions": len(position_gains),
        "changed_positions": changed,
        "changed_rate": changed / len(position_gains),
        "model_wins_on_changes": model_wins,
        "baseline_wins_on_changes": baseline_wins,
        "ties_on_changes": changed - model_wins - baseline_wins,
        "oracle_spread_gain_per_position": mean_sem_ci_p(gains),
        "oracle_spread_gain_clustered_by_game": mean_sem_ci_p(clustered),
        "baseline_mean_regret": float(np.mean(position_regret_base)),
        "model_mean_regret": float(np.mean(position_regret_model)),
        "mean_regret_reduction": float(
            np.mean(position_regret_base) - np.mean(position_regret_model)
        ),
        "adjustment_stddev": float(np.std(adjustment)),
        "adjustment_p95_abs": float(np.quantile(np.abs(adjustment), 0.95)),
    }


def evaluate_model(
    candidates: list[Candidate],
    values: np.ndarray,
    names: list[str],
    game_folds: dict[int, int],
    folds: int,
) -> dict[str, object]:
    positions = np.asarray([candidate.position for candidate in candidates])
    games = np.asarray([candidate.game for candidate in candidates])
    base_equity = np.asarray(
        [candidate.base_equity for candidate in candidates], dtype=np.float64
    )
    oracle_spread = np.asarray(
        [candidate.oracle_spread for candidate in candidates], dtype=np.float64
    )
    centered_values = center_by_position(candidates, values)
    centered_oracle = center_by_position(candidates, oracle_spread[:, None])[:, 0]
    centered_base = center_by_position(candidates, base_equity[:, None])[:, 0]
    residual_target = centered_oracle - centered_base
    adjustment = np.full(len(candidates), np.nan, dtype=np.float64)
    selected_alphas: list[float] = []

    for test_fold in range(folds):
        valid_fold = (test_fold + 1) % folds
        test_mask = np.asarray([game_folds[int(game)] == test_fold for game in games])
        valid_mask = np.asarray(
            [game_folds[int(game)] == valid_fold for game in games]
        )
        train_mask = ~(test_mask | valid_mask)
        selected = choose_ridge(
            centered_values[train_mask],
            residual_target[train_mask],
            centered_values[valid_mask],
            residual_target[valid_mask],
        )
        fitted = fit_ridge(
            centered_values[train_mask | valid_mask],
            residual_target[train_mask | valid_mask],
            selected.alpha,
        )
        adjustment[test_mask] = fitted.predict(centered_values[test_mask])
        selected_alphas.append(selected.alpha)
    if not np.isfinite(adjustment).all():
        raise FloatingPointError("cross-fitting left invalid predictions")

    final_fit = choose_ridge(
        centered_values[np.asarray([game_folds[int(game)] != 0 for game in games])],
        residual_target[
            np.asarray([game_folds[int(game)] != 0 for game in games])
        ],
        centered_values[np.asarray([game_folds[int(game)] == 0 for game in games])],
        residual_target[
            np.asarray([game_folds[int(game)] == 0 for game in games])
        ],
    )
    coefficients = sorted(
        zip(names, final_fit.coef),
        key=lambda item: abs(float(item[1])),
        reverse=True,
    )
    return {
        "features": len(names),
        "selected_alphas": selected_alphas,
        **selection_metrics(candidates, adjustment),
        "top_standardized_coefficients": [
            {"feature": name, "coefficient": float(coefficient)}
            for name, coefficient in coefficients[:30]
        ],
        "raw_coefficients": {
            name: float(coefficient / final_fit.scale[index])
            for index, (name, coefficient) in enumerate(
                zip(names, final_fit.coef)
            )
        },
    }


def independent_test(
    train_candidates: list[Candidate],
    test_candidates: list[Candidate],
    model: str,
    train_game_folds: dict[int, int],
    folds: int,
) -> dict[str, object]:
    train_values, names = feature_matrix(train_candidates, model)
    name_index = {name: index for index, name in enumerate(names)}
    test_values = np.zeros((len(test_candidates), len(names)), dtype=np.float64)
    for row, candidate in enumerate(test_candidates):
        for name, value in candidate.features.items():
            column = name_index.get(name)
            if column is not None:
                test_values[row, column] = value

    train_centered = center_by_position(train_candidates, train_values)
    test_centered = center_by_position(test_candidates, test_values)
    train_oracle = np.asarray(
        [candidate.oracle_spread for candidate in train_candidates]
    )
    train_base = np.asarray(
        [candidate.base_equity for candidate in train_candidates]
    )
    target = (
        center_by_position(train_candidates, train_oracle[:, None])[:, 0]
        - center_by_position(train_candidates, train_base[:, None])[:, 0]
    )
    train_games = np.asarray([candidate.game for candidate in train_candidates])
    best: tuple[float, float] | None = None
    for alpha in (
        0.01,
        0.1,
        1.0,
        10.0,
        100.0,
        1_000.0,
        10_000.0,
        100_000.0,
        1_000_000.0,
    ):
        prediction = np.full(len(train_candidates), np.nan)
        for fold in range(folds):
            valid = np.asarray(
                [train_game_folds[int(game)] == fold for game in train_games]
            )
            fitted = fit_ridge(train_centered[~valid], target[~valid], alpha)
            prediction[valid] = fitted.predict(train_centered[valid])
        mse = float(np.mean((target - prediction) ** 2))
        if best is None or mse < best[0]:
            best = (mse, alpha)
    assert best is not None
    fitted = fit_ridge(train_centered, target, best[1])
    test_adjustment = fitted.predict(test_centered)
    coefficients = sorted(
        zip(names, fitted.coef),
        key=lambda item: abs(float(item[1])),
        reverse=True,
    )
    return {
        "model": model,
        "features": len(names),
        "selected_alpha_from_training_crossfit": best[1],
        **selection_metrics(test_candidates, test_adjustment),
        "top_standardized_coefficients": [
            {"feature": name, "coefficient": float(coefficient)}
            for name, coefficient in coefficients[:30]
        ],
        "raw_coefficients": {
            name: float(coefficient / fitted.scale[index])
            for index, (name, coefficient) in enumerate(zip(names, fitted.coef))
        },
    }


def main() -> None:
    args = parse_args()
    machine_letters, scores = read_distribution(args.letter_distribution)
    layout = read_layout(args.layout)
    candidates = parse_log(
        args.oracle_log, layout, scores, machine_letters
    )
    if args.hook_feature_log is not None:
        merge_hook_features(candidates, args.hook_feature_log)
    by_position: dict[int, list[Candidate]] = {}
    for candidate in candidates:
        by_position.setdefault(candidate.position, []).append(candidate)
    malformed = [
        position
        for position, rows in by_position.items()
        if sorted(row.rank for row in rows) != list(range(len(rows)))
    ]
    if malformed:
        raise ValueError(f"incomplete candidate groups: {malformed[:10]}")
    games = sorted({candidate.game for candidate in candidates})
    if len(games) < args.folds:
        raise ValueError(f"need at least {args.folds} games, found {len(games)}")
    rng = np.random.default_rng(args.seed)
    shuffled_games = np.asarray(games)
    rng.shuffle(shuffled_games)
    game_folds = {
        int(game): index % args.folds
        for index, game in enumerate(shuffled_games)
    }

    models = (
        ("compact_base", "compact_hooks", "compact_hook_letters")
        if args.hook_feature_log is not None
        else ("compact", "local", "full")
    )
    results: dict[str, object] = {
        "oracle_log": str(args.oracle_log),
        "games": len(games),
        "positions": len(by_position),
        "candidates": len(candidates),
        "mean_candidates_per_position": len(candidates) / len(by_position),
        "protocol": {
            "target": "candidate oracle spread, centered within position",
            "baseline": "static equity with coefficient fixed at one",
            "validation": f"{args.folds}-fold cross-fit by source game",
        },
        "models": {},
    }
    for model in models:
        values, names = feature_matrix(candidates, model)
        results["models"][model] = evaluate_model(
            candidates, values, names, game_folds, args.folds
        )
    if args.test_oracle_log is not None:
        test_candidates = parse_log(
            args.test_oracle_log, layout, scores, machine_letters
        )
        if args.hook_feature_log is not None:
            if args.test_hook_feature_log is None:
                raise ValueError(
                    "--test-hook-feature-log is required with hook features"
                )
            merge_hook_features(test_candidates, args.test_hook_feature_log)
        results["independent_test"] = {
            "oracle_log": str(args.test_oracle_log),
            "games": len({candidate.game for candidate in test_candidates}),
            "positions": len({candidate.position for candidate in test_candidates}),
            "models": {
                model: independent_test(
                    candidates, test_candidates, model, game_folds, args.folds
                )
                for model in models
            },
        }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(results, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(results, indent=2))


if __name__ == "__main__":
    main()
