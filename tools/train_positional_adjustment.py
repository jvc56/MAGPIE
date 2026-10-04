#!/usr/bin/env python3
"""Smoke-train a compact positional adjustment from autoplay GCGs.

This is deliberately an offline feasibility experiment, not a production
trainer.  It asks whether cheap board-opportunity features improve held-out
prediction of the *net remaining spread swing* after controlling for score,
phase, move score, rack composition, and KLV2 leave value.

Games, rather than positions, are assigned to train/validation/test splits.
That prevents near-identical consecutive positions from leaking across splits.
"""

from __future__ import annotations

import argparse
import csv
import glob
import json
import math
import re
from dataclasses import dataclass
from pathlib import Path

import numpy as np

from train_klv3 import KLV2


BOARD_DIM = 15
RACK_SIZE = 7
PLACEMENT_RE = re.compile(
    r"^>([^:]+):\s+(\S+)\s+([A-O]\d+|\d+[A-O])\s+(\S+)"
    r"\s+\+(-?\d+)\s+(-?\d+)\s*$"
)
OTHER_EVENT_RE = re.compile(r"^>([^:]+):\s+.*\s+(-?\d+)\s*$")
MOVE_EVENT_RE = re.compile(
    r"^>([^:]+):\s+(\S+)\s+(.+?)\s+\+(-?\d+)\s+(-?\d+)\s*$"
)
PREMIUM_NAMES = {"=": "tw", "-": "dw", '"': "tl", "'": "dl", " ": "plain"}
PREMIUM_WEIGHTS = {"=": 6.0, "-": 3.0, '"': 3.0, "'": 1.0, " ": 0.0}
VOWELS = frozenset("AEIOU")


@dataclass
class Row:
    game: int
    player: int
    event_index: int
    target: float
    baseline: dict[str, float]
    local: dict[str, float]
    global_: dict[str, float]


@dataclass
class Fit:
    alpha: float
    mean: np.ndarray
    scale: np.ndarray
    coef: np.ndarray

    def predict(self, matrix: np.ndarray) -> np.ndarray:
        with np.errstate(divide="ignore", over="ignore", invalid="ignore"):
            prediction = ((matrix - self.mean) / self.scale) @ self.coef
        if not np.isfinite(prediction).all():
            raise FloatingPointError("ridge prediction contains NaN or inf")
        return prediction


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--gcg-glob", required=True)
    parser.add_argument("--letter-distribution", required=True, type=Path)
    parser.add_argument("--layout", required=True, type=Path)
    parser.add_argument("--base-klv2", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument(
        "--target",
        choices=("terminal", "two_ply", "four_ply"),
        default="terminal",
        help="net outcome label to fit (default: %(default)s)",
    )
    parser.add_argument("--seed", type=int, default=620)
    parser.add_argument(
        "--max-board-tiles",
        type=int,
        default=80,
        help="exclude near/post-bag states (default: %(default)s)",
    )
    return parser.parse_args()


def read_distribution(path: Path) -> tuple[list[str], dict[str, int]]:
    tiles: list[str] = []
    scores: dict[str, int] = {}
    with path.open(newline="", encoding="utf-8") as stream:
        for row in csv.reader(stream):
            if not row:
                continue
            tile = row[0]
            tiles.append(tile)
            scores[tile] = int(row[3])
    return tiles, scores


def read_layout(path: Path) -> list[list[str]]:
    lines = path.read_text(encoding="utf-8").splitlines()[1:]
    if len(lines) != BOARD_DIM or any(len(line) != BOARD_DIM for line in lines):
        raise ValueError(f"{path}: expected a {BOARD_DIM}x{BOARD_DIM} layout")
    return [list(line) for line in lines]


def coordinate(coord: str) -> tuple[int, int, int, int]:
    if coord[0].isalpha():
        return int(coord[1:]) - 1, ord(coord[0]) - ord("A"), 1, 0
    return int(coord[:-1]) - 1, ord(coord[-1]) - ord("A"), 0, 1


def add(feature: dict[str, float], name: str, value: float = 1.0) -> None:
    feature[name] = feature.get(name, 0.0) + value


def empty_run(
    board: list[list[str | None]], row: int, col: int, dr: int, dc: int
) -> int:
    count = 0
    row += dr
    col += dc
    while (
        0 <= row < BOARD_DIM
        and 0 <= col < BOARD_DIM
        and board[row][col] is None
    ):
        count += 1
        row += dr
        col += dc
    return count


def distance_bucket(distance: int) -> str:
    return str(distance) if distance <= 3 else "4plus"


def local_features(
    board: list[list[str | None]],
    new_tiles: list[tuple[int, int, str, bool]],
    dr: int,
    dc: int,
    layout: list[list[str]],
    scores: dict[str, int],
) -> dict[str, float]:
    result: dict[str, float] = {}
    if not new_tiles:
        return result
    add(result, "local_new_tiles", len(new_tiles))
    for row, col, letter, blank in new_tiles:
        score = 0 if blank else scores[letter]
        kind = "vowel" if letter in VOWELS else "consonant"
        add(result, f"local_{kind}")
        add(result, "local_tile_score", score)
        add(result, "local_high_tile", score >= 4)

        # Newly exposed perpendicular sides are the direct generalization of
        # the opening hotspot heuristic: a tile floated into an open lane can
        # help the opponent reach premiums or form hooks.
        perp_dr, perp_dc = dc, dr
        open_sides = 0
        for side in (-1, 1):
            near_row = row + side * perp_dr
            near_col = col + side * perp_dc
            if (
                0 <= near_row < BOARD_DIM
                and 0 <= near_col < BOARD_DIM
                and board[near_row][near_col] is None
            ):
                open_sides += 1
                premium = PREMIUM_NAMES[layout[near_row][near_col]]
                add(result, f"local_exposed_{premium}")
                add(result, f"local_exposed_{kind}")
        add(result, "local_exposed_sides", open_sides)
        add(result, "local_exposed_score", open_sides * score)
        add(result, "local_compact_exposure", open_sides)
        add(result, "local_compact_exposure_score", open_sides * score)
        perp_span = empty_run(board, row, col, perp_dr, perp_dc) + empty_run(
            board, row, col, -perp_dr, -perp_dc
        )
        add(result, f"local_perp_span_{min(perp_span, 7)}")

        # Premium squares visible through an otherwise empty lane.  This is
        # cheap to evaluate for a candidate move and captures both floaters
        # and premium-square access without generating an opponent response.
        for sight_dr, sight_dc in ((1, 0), (-1, 0), (0, 1), (0, -1)):
            for distance in range(1, RACK_SIZE + 1):
                sight_row = row + distance * sight_dr
                sight_col = col + distance * sight_dc
                if not (
                    0 <= sight_row < BOARD_DIM
                    and 0 <= sight_col < BOARD_DIM
                ):
                    break
                if board[sight_row][sight_col] is not None:
                    break
                premium_char = layout[sight_row][sight_col]
                if premium_char != " ":
                    premium = PREMIUM_NAMES[premium_char]
                    bucket = distance_bucket(distance)
                    add(result, f"local_sight_{premium}_{bucket}")
                    add(result, f"local_sight_{premium}_{kind}")
                    add(result, f"local_sight_{premium}_score", score)
                    weighted_access = PREMIUM_WEIGHTS[premium_char] * (
                        RACK_SIZE + 1 - distance
                    )
                    add(
                        result,
                        "local_compact_premium_access",
                        weighted_access,
                    )
                    add(
                        result,
                        "local_compact_premium_access_score",
                        weighted_access * score,
                    )

    # The two main-word endpoints are particularly cheap, stable opportunity
    # features because they do not require inspecting every square touched.
    start_row, start_col, *_ = new_tiles[0]
    end_row, end_col, *_ = new_tiles[-1]
    for label, row, col in (
        ("before", start_row - dr, start_col - dc),
        ("after", end_row + dr, end_col + dc),
    ):
        if (
            0 <= row < BOARD_DIM
            and 0 <= col < BOARD_DIM
            and board[row][col] is None
        ):
            add(result, f"local_word_end_{label}")
            add(result, f"local_word_end_{PREMIUM_NAMES[layout[row][col]]}")
            add(
                result,
                "local_compact_word_end",
                PREMIUM_WEIGHTS[layout[row][col]],
            )
    return result


def global_features(
    board: list[list[str | None]],
    layout: list[list[str]],
    scores: dict[str, int],
) -> dict[str, float]:
    result: dict[str, float] = {}
    for row in range(BOARD_DIM):
        for col in range(BOARD_DIM):
            if board[row][col] is not None:
                continue
            neighbors: list[str] = []
            for dr, dc in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                adj_row, adj_col = row + dr, col + dc
                if (
                    0 <= adj_row < BOARD_DIM
                    and 0 <= adj_col < BOARD_DIM
                    and board[adj_row][adj_col] is not None
                ):
                    neighbors.append(board[adj_row][adj_col] or "")
            if neighbors:
                premium = PREMIUM_NAMES[layout[row][col]]
                add(result, "global_anchors")
                add(result, f"global_anchor_{premium}")
                add(result, "global_anchor_neighbors", len(neighbors))
                add(
                    result,
                    "global_anchor_score",
                    sum(
                        0 if letter.islower() else scores[letter.upper()]
                        for letter in neighbors
                    ),
                )
                add(
                    result,
                    "global_anchor_vowels",
                    sum(letter.upper() in VOWELS for letter in neighbors),
                )
                horizontal_span = empty_run(board, row, col, 0, -1) + empty_run(
                    board, row, col, 0, 1
                )
                vertical_span = empty_run(board, row, col, -1, 0) + empty_run(
                    board, row, col, 1, 0
                )
                add(result, f"global_anchor_hspan_{min(horizontal_span, 7)}")
                add(result, f"global_anchor_vspan_{min(vertical_span, 7)}")

            premium_char = layout[row][col]
            if premium_char == " ":
                continue
            premium = PREMIUM_NAMES[premium_char]
            for sight_dr, sight_dc in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                for distance in range(1, RACK_SIZE + 1):
                    sight_row = row + distance * sight_dr
                    sight_col = col + distance * sight_dc
                    if not (
                        0 <= sight_row < BOARD_DIM
                        and 0 <= sight_col < BOARD_DIM
                    ):
                        break
                    letter = board[sight_row][sight_col]
                    if letter is not None:
                        bucket = distance_bucket(distance)
                        add(result, f"global_reach_{premium}_{bucket}")
                        add(
                            result,
                            f"global_reach_{premium}_"
                            f"{'vowel' if letter.upper() in VOWELS else 'consonant'}",
                        )
                        add(
                            result,
                            f"global_reach_{premium}_score",
                            0 if letter.islower() else scores[letter.upper()],
                        )
                        break
    return result


def subtract_rack(
    rack: str, new_tiles: list[tuple[int, int, str, bool]], tile_index: dict[str, int]
) -> np.ndarray:
    counts = np.zeros(len(tile_index), dtype=np.int16)
    for tile in rack:
        counts[tile_index[tile.upper() if tile != "?" else "?"]] += 1
    for _, _, letter, blank in new_tiles:
        tile = "?" if blank else letter
        counts[tile_index[tile]] -= 1
    if np.any(counts < 0):
        raise ValueError(f"move tiles are not a subset of rack {rack}")
    return counts


def parse_game(
    path: Path,
    game_index: int,
    layout: list[list[str]],
    scores: dict[str, int],
    tile_index: dict[str, int],
    klv2: KLV2,
    max_board_tiles: int,
    target_mode: str,
) -> list[Row]:
    lines = path.read_text(encoding="utf-8").splitlines()
    nick_to_player: dict[str, int] = {}
    final_scores = [0, 0]
    for line in lines:
        if line.startswith("#player"):
            player = int(line[7]) - 1
            nick_to_player[line.split(maxsplit=2)[1]] = player
        elif line.startswith(">"):
            match = OTHER_EVENT_RE.match(line)
            if match and match.group(1) in nick_to_player:
                final_scores[nick_to_player[match.group(1)]] = int(match.group(2))

    board: list[list[str | None]] = [
        [None for _ in range(BOARD_DIM)] for _ in range(BOARD_DIM)
    ]
    current_scores = [0, 0]
    rows: list[Row] = []
    move_events: list[tuple[int, int]] = []
    turn = 0
    for line in lines:
        match = PLACEMENT_RE.match(line)
        if not match:
            move_event = MOVE_EVENT_RE.match(line)
            if move_event and move_event.group(1) in nick_to_player:
                player = nick_to_player[move_event.group(1)]
                move_score = int(move_event.group(4))
                current_scores[player] = int(move_event.group(5))
                move_events.append((player, move_score))
                turn += 1
                continue
            other = OTHER_EVENT_RE.match(line)
            if other and other.group(1) in nick_to_player:
                current_scores[nick_to_player[other.group(1)]] = int(other.group(2))
            continue
        nick, rack, coord, word, move_score_str, cumulative_str = match.groups()
        player = nick_to_player[nick]
        move_score = int(move_score_str)
        cumulative = int(cumulative_str)
        current_scores[player] = cumulative
        event_index = len(move_events)
        move_events.append((player, move_score))
        row, col, dr, dc = coordinate(coord)
        new_tiles: list[tuple[int, int, str, bool]] = []
        for offset, char in enumerate(word):
            tile_row, tile_col = row + offset * dr, col + offset * dc
            if char == ".":
                if board[tile_row][tile_col] is None:
                    raise ValueError(f"{path}: playthrough crosses an empty square")
                continue
            blank = char.islower()
            letter = char.upper()
            if board[tile_row][tile_col] is not None:
                raise ValueError(f"{path}: placement overlaps an occupied square")
            board[tile_row][tile_col] = char
            new_tiles.append((tile_row, tile_col, letter, blank))

        turn += 1
        board_tiles = sum(cell is not None for board_row in board for cell in board_row)
        if board_tiles > max_board_tiles:
            continue
        leave_counts = subtract_rack(rack, new_tiles, tile_index)
        leave_value = (
            0.0
            if int(leave_counts.sum()) == 0
            else klv2.value_for_counts(leave_counts)
        )
        leave_vowels = sum(
            int(leave_counts[tile_index[vowel]])
            for vowel in VOWELS
            if vowel in tile_index
        )
        leave_blanks = int(leave_counts[tile_index["?"]])
        leave_high = sum(
            int(count)
            for tile, count in zip(tile_index, leave_counts)
            if tile != "?" and scores[tile] >= 4
        )
        score_diff = current_scores[player] - current_scores[1 - player]
        final_spread = final_scores[player] - final_scores[1 - player]
        target = float(final_spread - score_diff)
        phase = min(board_tiles // 10, 8)
        baseline = {
            "base_intercept": 1.0,
            "base_board_tiles": float(board_tiles),
            "base_board_tiles_sq": float(board_tiles * board_tiles) / 100.0,
            "base_score_diff": float(score_diff),
            "base_score_diff_sq": math.copysign(score_diff * score_diff, score_diff)
            / 100.0,
            "base_move_score": float(move_score),
            "base_leave_value": float(leave_value),
            "base_leave_size": float(leave_counts.sum()),
            "base_leave_vowels": float(leave_vowels),
            "base_leave_blanks": float(leave_blanks),
            "base_leave_high": float(leave_high),
            "base_tiles_played": float(len(new_tiles)),
            "base_bingo": float(len(new_tiles) == RACK_SIZE),
            "base_turn": float(turn),
            f"base_phase_{phase}": 1.0,
            f"base_phase_score_{phase}": float(score_diff),
            f"base_phase_leave_{phase}": float(leave_value),
        }
        rows.append(
            Row(
                game=game_index,
                player=player,
                event_index=event_index,
                target=target,
                baseline=baseline,
                local=local_features(
                    board, new_tiles, dr, dc, layout, scores
                ),
                global_=global_features(board, layout, scores),
            )
        )
    if target_mode == "terminal":
        return rows
    horizon = 2 if target_mode == "two_ply" else 4
    retained: list[Row] = []
    for row in rows:
        future = move_events[row.event_index + 1 : row.event_index + 1 + horizon]
        if len(future) != horizon:
            continue
        row.target = float(
            sum(
                score if player == row.player else -score
                for player, score in future
            )
        )
        retained.append(row)
    return retained


def matrix(
    rows: list[Row], groups: tuple[str, ...]
) -> tuple[np.ndarray, list[str]]:
    names = sorted(
        {
            name
            for row in rows
            for group in groups
            for name in getattr(row, group).keys()
        }
    )
    index = {name: column for column, name in enumerate(names)}
    values = np.zeros((len(rows), len(names)), dtype=np.float64)
    for row_index, row in enumerate(rows):
        for group in groups:
            for name, value in getattr(row, group).items():
                values[row_index, index[name]] = value
    return values, names


def matrix_prefix(
    rows: list[Row], group: str, prefix: str
) -> tuple[np.ndarray, list[str]]:
    names = sorted(
        {
            name
            for row in rows
            for name in getattr(row, group).keys()
            if name.startswith(prefix)
        }
    )
    index = {name: column for column, name in enumerate(names)}
    values = np.zeros((len(rows), len(names)), dtype=np.float64)
    for row_index, row in enumerate(rows):
        for name, value in getattr(row, group).items():
            if name in index:
                values[row_index, index[name]] = value
    return values, names


def fit_ridge(matrix_: np.ndarray, target: np.ndarray, alpha: float) -> Fit:
    mean = matrix_.mean(axis=0)
    scale = matrix_.std(axis=0)
    scale[scale < 1e-9] = 1.0
    normalized = (matrix_ - mean) / scale
    # Constant columns are centered away.  Recover an intercept by leaving the
    # explicit base_intercept column uncentered and unscaled.
    constant = np.std(matrix_, axis=0) < 1e-9
    normalized[:, constant] = matrix_[:, constant]
    mean[constant] = 0.0
    scale[constant] = 1.0
    with np.errstate(divide="ignore", over="ignore", invalid="ignore"):
        gram = normalized.T @ normalized
        rhs = normalized.T @ target
    if not np.isfinite(gram).all() or not np.isfinite(rhs).all():
        raise FloatingPointError("ridge normal equations contain NaN or inf")
    penalty = np.eye(gram.shape[0]) * alpha
    penalty[constant, constant] = 0.0
    coef = np.linalg.solve(
        gram + penalty + np.eye(gram.shape[0]) * 1e-10, rhs
    )
    return Fit(alpha=alpha, mean=mean, scale=scale, coef=coef)


def choose_fit(
    train_matrix: np.ndarray,
    train_target: np.ndarray,
    valid_matrix: np.ndarray,
    valid_target: np.ndarray,
) -> Fit:
    best: tuple[float, Fit] | None = None
    for alpha in (
        0.01,
        0.1,
        1.0,
        10.0,
        100.0,
        1000.0,
        10_000.0,
        100_000.0,
        1_000_000.0,
    ):
        candidate = fit_ridge(train_matrix, train_target, alpha)
        rmse = float(
            np.sqrt(np.mean((valid_target - candidate.predict(valid_matrix)) ** 2))
        )
        if best is None or rmse < best[0]:
            best = (rmse, candidate)
    assert best is not None
    return best[1]


def metric(target: np.ndarray, prediction: np.ndarray) -> dict[str, float]:
    residual = target - prediction
    return {
        "rmse": float(np.sqrt(np.mean(residual * residual))),
        "mae": float(np.mean(np.abs(residual))),
        "r2": float(1.0 - np.sum(residual * residual) / np.sum((target - target.mean()) ** 2)),
    }


def paired_game_test(
    rows: list[Row],
    target: np.ndarray,
    baseline_prediction: np.ndarray,
    candidate_prediction: np.ndarray,
) -> dict[str, float]:
    game_ids = np.asarray([row.game for row in rows])
    deltas: list[float] = []
    for game in sorted(set(game_ids)):
        mask = game_ids == game
        baseline_mse = np.mean((target[mask] - baseline_prediction[mask]) ** 2)
        candidate_mse = np.mean((target[mask] - candidate_prediction[mask]) ** 2)
        deltas.append(float(baseline_mse - candidate_mse))
    values = np.asarray(deltas)
    mean = float(values.mean())
    sem = float(values.std(ddof=1) / math.sqrt(len(values)))
    z = mean / sem if sem > 0.0 else 0.0
    return {
        "games": len(values),
        "mean_mse_reduction": mean,
        "sem": sem,
        "ci95_low": mean - 1.96 * sem,
        "ci95_high": mean + 1.96 * sem,
        "p_two_sided_normal": math.erfc(abs(z) / math.sqrt(2.0)),
    }


def crossfit(
    rows: list[Row],
    target: np.ndarray,
    values: np.ndarray,
    game_folds: np.ndarray,
    folds: int,
) -> tuple[np.ndarray, list[float]]:
    prediction = np.full(len(rows), np.nan, dtype=np.float64)
    selected_alphas: list[float] = []
    row_games = np.asarray([row.game for row in rows])
    for test_fold in range(folds):
        valid_fold = (test_fold + 1) % folds
        test_mask = game_folds[row_games] == test_fold
        valid_mask = game_folds[row_games] == valid_fold
        train_mask = ~(test_mask | valid_mask)
        selected = choose_fit(
            values[train_mask],
            target[train_mask],
            values[valid_mask],
            target[valid_mask],
        )
        fitted = fit_ridge(
            values[train_mask | valid_mask],
            target[train_mask | valid_mask],
            selected.alpha,
        )
        prediction[test_mask] = fitted.predict(values[test_mask])
        selected_alphas.append(selected.alpha)
    if not np.isfinite(prediction).all():
        raise FloatingPointError("cross-fit left missing or invalid predictions")
    return prediction, selected_alphas


def crossfit_incremental(
    rows: list[Row],
    target: np.ndarray,
    baseline_values: np.ndarray,
    positional_values: np.ndarray,
    game_folds: np.ndarray,
    folds: int,
) -> tuple[np.ndarray, np.ndarray, list[float], list[float]]:
    baseline_prediction = np.full(len(rows), np.nan, dtype=np.float64)
    combined_prediction = np.full(len(rows), np.nan, dtype=np.float64)
    baseline_alphas: list[float] = []
    positional_alphas: list[float] = []
    row_games = np.asarray([row.game for row in rows])
    alpha_grid = (
        0.01,
        0.1,
        1.0,
        10.0,
        100.0,
        1000.0,
        10_000.0,
        100_000.0,
        1_000_000.0,
    )
    for test_fold in range(folds):
        valid_fold = (test_fold + 1) % folds
        test_mask = game_folds[row_games] == test_fold
        valid_mask = game_folds[row_games] == valid_fold
        train_mask = ~(test_mask | valid_mask)

        baseline_selected = choose_fit(
            baseline_values[train_mask],
            target[train_mask],
            baseline_values[valid_mask],
            target[valid_mask],
        )
        baseline_fit = fit_ridge(
            baseline_values[train_mask],
            target[train_mask],
            baseline_selected.alpha,
        )
        baseline_train = baseline_fit.predict(baseline_values[train_mask])
        baseline_valid = baseline_fit.predict(baseline_values[valid_mask])
        baseline_test = baseline_fit.predict(baseline_values[test_mask])
        residual_train = target[train_mask] - baseline_train

        best: tuple[float, Fit, float] | None = None
        for alpha in alpha_grid:
            positional_fit = fit_ridge(
                positional_values[train_mask], residual_train, alpha
            )
            valid_prediction = baseline_valid + positional_fit.predict(
                positional_values[valid_mask]
            )
            rmse = float(
                np.sqrt(
                    np.mean((target[valid_mask] - valid_prediction) ** 2)
                )
            )
            if best is None or rmse < best[0]:
                best = (rmse, positional_fit, alpha)
        assert best is not None
        baseline_prediction[test_mask] = baseline_test
        combined_prediction[test_mask] = baseline_test + best[1].predict(
            positional_values[test_mask]
        )
        baseline_alphas.append(baseline_selected.alpha)
        positional_alphas.append(best[2])
    if not np.isfinite(combined_prediction).all():
        raise FloatingPointError("incremental cross-fit produced invalid predictions")
    return (
        baseline_prediction,
        combined_prediction,
        baseline_alphas,
        positional_alphas,
    )


def main() -> None:
    args = parse_args()
    paths = [Path(path) for path in sorted(glob.glob(args.gcg_glob))]
    if len(paths) < 20:
        raise ValueError(f"need at least 20 GCGs, found {len(paths)}")
    tiles, scores = read_distribution(args.letter_distribution)
    tile_index = {tile: index for index, tile in enumerate(tiles)}
    layout = read_layout(args.layout)
    klv2 = KLV2(args.base_klv2, len(tiles))
    rows: list[Row] = []
    for game_index, path in enumerate(paths):
        rows.extend(
            parse_game(
                path,
                game_index,
                layout,
                scores,
                tile_index,
                klv2,
                args.max_board_tiles,
                args.target,
            )
        )

    rng = np.random.default_rng(args.seed)
    game_order = np.arange(len(paths))
    rng.shuffle(game_order)
    train_end = int(len(game_order) * 0.70)
    valid_end = int(len(game_order) * 0.85)
    game_splits = {
        "train": set(int(game) for game in game_order[:train_end]),
        "valid": set(int(game) for game in game_order[train_end:valid_end]),
        "test": set(int(game) for game in game_order[valid_end:]),
    }
    target = np.asarray([row.target for row in rows], dtype=np.float64)
    row_masks = {
        split: np.asarray([row.game in games for row in rows])
        for split, games in game_splits.items()
    }

    model_groups = {
        "baseline": ("baseline",),
        "baseline_local": ("baseline", "local"),
        "baseline_global": ("baseline", "global_"),
        "full": ("baseline", "local", "global_"),
    }
    results: dict[str, object] = {
        "games": len(paths),
        "positions": len(rows),
        "target": args.target,
        "split_games": {name: len(games) for name, games in game_splits.items()},
        "models": {},
        "crossfit_models": {},
        "incremental_crossfit": {},
    }
    predictions: dict[str, np.ndarray] = {}
    crossfit_predictions: dict[str, np.ndarray] = {}
    coefficients: dict[str, list[dict[str, float | str]]] = {}
    game_folds = np.empty(len(paths), dtype=np.int16)
    for order_index, game in enumerate(game_order):
        game_folds[game] = order_index % 5
    for model_name, groups in model_groups.items():
        values, names = matrix(rows, groups)
        train_mask, valid_mask, test_mask = (
            row_masks["train"],
            row_masks["valid"],
            row_masks["test"],
        )
        selected = choose_fit(
            values[train_mask],
            target[train_mask],
            values[valid_mask],
            target[valid_mask],
        )
        # Refit the selected regularization on train + validation games.
        fit_mask = train_mask | valid_mask
        fitted = fit_ridge(values[fit_mask], target[fit_mask], selected.alpha)
        prediction = fitted.predict(values)
        predictions[model_name] = prediction
        crossfit_prediction, crossfit_alphas = crossfit(
            rows, target, values, game_folds, 5
        )
        crossfit_predictions[model_name] = crossfit_prediction
        test_metric = metric(target[test_mask], prediction[test_mask])
        results["models"][model_name] = {
            "alpha": fitted.alpha,
            **test_metric,
        }
        results["crossfit_models"][model_name] = {
            "selected_alphas": crossfit_alphas,
            **metric(target, crossfit_prediction),
        }
        standardized = sorted(
            zip(names, fitted.coef),
            key=lambda item: abs(float(item[1])),
            reverse=True,
        )
        coefficients[model_name] = [
            {"feature": name, "standardized_coefficient": float(coef)}
            for name, coef in standardized[:20]
        ]

    test_rows = [row for row, use in zip(rows, row_masks["test"]) if use]
    test_target = target[row_masks["test"]]
    baseline_prediction = predictions["baseline"][row_masks["test"]]
    for candidate in ("baseline_local", "baseline_global", "full"):
        candidate_prediction = predictions[candidate][row_masks["test"]]
        results["models"][candidate]["paired_game_test"] = paired_game_test(
            test_rows, test_target, baseline_prediction, candidate_prediction
        )
        results["models"][candidate]["rmse_reduction_vs_baseline"] = (
            results["models"]["baseline"]["rmse"]
            - results["models"][candidate]["rmse"]
        )
        results["crossfit_models"][candidate]["paired_game_test"] = (
            paired_game_test(
                rows,
                target,
                crossfit_predictions["baseline"],
                crossfit_predictions[candidate],
            )
        )
        results["crossfit_models"][candidate][
            "rmse_reduction_vs_baseline"
        ] = (
            results["crossfit_models"]["baseline"]["rmse"]
            - results["crossfit_models"][candidate]["rmse"]
        )

    full_delta = (
        predictions["full"][row_masks["test"]]
        - predictions["baseline"][row_masks["test"]]
    )
    baseline_residual = test_target - baseline_prediction
    results["full_increment"] = {
        "stddev_points": float(np.std(full_delta)),
        "p95_abs_points": float(np.quantile(np.abs(full_delta), 0.95)),
        "correlation_with_baseline_residual": float(
            np.corrcoef(full_delta, baseline_residual)[0, 1]
        ),
    }
    baseline_values, _ = matrix(rows, ("baseline",))
    for candidate, groups in {
        "local": ("local",),
        "global": ("global_",),
        "full": ("local", "global_"),
    }.items():
        positional_values, _ = matrix(rows, groups)
        (
            fair_baseline_prediction,
            fair_candidate_prediction,
            fair_baseline_alphas,
            fair_positional_alphas,
        ) = crossfit_incremental(
            rows,
            target,
            baseline_values,
            positional_values,
            game_folds,
            5,
        )
        baseline_metric = metric(target, fair_baseline_prediction)
        candidate_metric = metric(target, fair_candidate_prediction)
        results["incremental_crossfit"][candidate] = {
            "baseline_alphas": fair_baseline_alphas,
            "positional_alphas": fair_positional_alphas,
            "baseline": baseline_metric,
            "candidate": candidate_metric,
            "rmse_reduction_vs_baseline": (
                baseline_metric["rmse"] - candidate_metric["rmse"]
            ),
            "adjustment_stddev_points": float(
                np.std(
                    fair_candidate_prediction - fair_baseline_prediction
                )
            ),
            "adjustment_p95_abs_points": float(
                np.quantile(
                    np.abs(
                        fair_candidate_prediction
                        - fair_baseline_prediction
                    ),
                    0.95,
                )
            ),
            "paired_game_test": paired_game_test(
                rows,
                target,
                fair_baseline_prediction,
                fair_candidate_prediction,
            ),
        }
    compact_values, compact_names = matrix_prefix(
        rows, "local", "local_compact_"
    )
    (
        compact_baseline_prediction,
        compact_prediction,
        compact_baseline_alphas,
        compact_alphas,
    ) = crossfit_incremental(
        rows,
        target,
        baseline_values,
        compact_values,
        game_folds,
        5,
    )
    compact_baseline_metric = metric(target, compact_baseline_prediction)
    compact_metric = metric(target, compact_prediction)
    results["incremental_crossfit"]["compact"] = {
        "features": compact_names,
        "baseline_alphas": compact_baseline_alphas,
        "positional_alphas": compact_alphas,
        "baseline": compact_baseline_metric,
        "candidate": compact_metric,
        "rmse_reduction_vs_baseline": (
            compact_baseline_metric["rmse"] - compact_metric["rmse"]
        ),
        "adjustment_stddev_points": float(
            np.std(compact_prediction - compact_baseline_prediction)
        ),
        "adjustment_p95_abs_points": float(
            np.quantile(
                np.abs(compact_prediction - compact_baseline_prediction),
                0.95,
            )
        ),
        "paired_game_test": paired_game_test(
            rows,
            target,
            compact_baseline_prediction,
            compact_prediction,
        ),
    }
    results["top_coefficients"] = coefficients
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(results, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(results, indent=2))


if __name__ == "__main__":
    main()
