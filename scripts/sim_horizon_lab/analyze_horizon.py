#!/usr/bin/env python3
"""Analyzes simhorizon output (test/sim_horizon_test.c in the horizon-eval
worktree): what the pool shift and the last play's PAT term at a sim's
horizon are worth.

Usage: analyze_horizon.py FILE [FILE...]

Each position's kept candidates have their rollouts split in four parts by
seed. For every way of taking k parts to choose a play and the other parts
to value it, each variant picks the play its horizon values best, and the
pick is scored by the mean result of the other parts' rollouts that were
played to the end of the game. A variant's gain is its pick's score less
the unchanged sim's pick's score, 0 where they agree or where the screen
left one candidate.
"""
import itertools
import math
import re
import sys
from collections import defaultdict

VARIANTS = ["base", "half PAT", "PAT", "shift", "shift + half PAT",
            "shift + PAT"]
NUM_VARIANTS = len(VARIANTS)
NUM_PARTS = 4
CRITERIA = ["util", "win", "eq"]
ORACLES = ["fspread", "fwin", "futil", "deep_plain", "deep_full"]

POS_RE = re.compile(r"^SIMH pos idx=(\d+) game=(\d+) bag=(\d+) plays=(\d+) "
                    r"kept=(\d+)")
MAIN_RE = re.compile(
    r"^SIMH main pos=(\d+) play=(\d+) move=(.*?) half=(\d+) n=(\d+) "
    r"eq=(\S+) win=(\S+) util=(\S+) shift=(\S+) pat=(\S+) cont=(\d+) "
    r"fspread=(\S+) fwin=(\S+) futil=(\S+) deep=(\S+) mom=(\S+) "
    r"tgt0=(\S+) tgt1=(\S+) tgt2=(\S+)(?: split=(\S+))?")
SCREEN_RE = re.compile(
    r"^SIMH screen pos=(\d+) play=(\d+) move=(.*?) kept=(\d+) n=(\d+) "
    r"eq=(\S+) win=(\S+) util=(\S+) shift=(\S+) pat=(\S+)")


def floats(text):
    return [float(part) for part in text.split(",")]


def read(paths):
    positions = {}
    checks = []
    for path in paths:
        for line in open(path, errors="replace"):
            match = POS_RE.match(line)
            if match:
                idx = int(match.group(1))
                positions[idx] = {
                    "game": int(match.group(2)), "bag": int(match.group(3)),
                    "plays": int(match.group(4)), "kept": int(match.group(5)),
                    "main": defaultdict(dict), "screen": {}}
                continue
            match = SCREEN_RE.match(line)
            if match:
                positions[int(match.group(1))]["screen"][
                    int(match.group(2))] = {
                    "move": match.group(3), "kept": int(match.group(4)),
                    "n": int(match.group(5)), "eq": floats(match.group(6)),
                    "win": floats(match.group(7)),
                    "util": floats(match.group(8)),
                    "shift": float(match.group(9)),
                    "pat": float(match.group(10))}
                continue
            match = MAIN_RE.match(line)
            if match:
                deep = floats(match.group(15))
                positions[int(match.group(1))]["main"][int(match.group(2))][
                    int(match.group(4))] = {
                    "move": match.group(3), "n": int(match.group(5)),
                    "eq": floats(match.group(6)),
                    "win": floats(match.group(7)),
                    "util": floats(match.group(8)),
                    "shift": float(match.group(9)),
                    "pat": float(match.group(10)),
                    "cont": int(match.group(11)),
                    "fspread": float(match.group(12)),
                    "fwin": float(match.group(13)),
                    "futil": float(match.group(14)),
                    "deep_plain": deep[0], "deep_full": deep[1],
                    "mom": floats(match.group(16)),
                    "tgt": [floats(match.group(17)), floats(match.group(18)),
                            floats(match.group(19))],
                    "split": (floats(match.group(20)) if match.group(20)
                              else None)}
                continue
            if line.startswith("SIMH checks") or line.startswith("SIMH done") \
                    or line.startswith("SIMH mean_mismatch"):
                checks.append(line.strip())
    return positions, checks


def position_gains(position, criterion, pick_parts_count):
    """Mean over the splits of each variant's gain and whether it flipped.

    Returns (gains[variant][oracle], flips[variant]) or None when a part
    lacks rollouts."""
    plays = position["main"]
    play_ids = sorted(plays)
    gains = [[0.0] * len(ORACLES) for _ in range(NUM_VARIANTS)]
    flips = [0.0] * NUM_VARIANTS
    splits = list(itertools.combinations(range(NUM_PARTS), pick_parts_count))
    for pick_parts in splits:
        oracle_parts = [part for part in range(NUM_PARTS)
                        if part not in pick_parts]
        oracle = {}
        for play in play_ids:
            weight = sum(plays[play][part]["cont"] for part in oracle_parts)
            if weight == 0:
                return None
            oracle[play] = [
                sum(plays[play][part]["cont"] * plays[play][part][name]
                    for part in oracle_parts) / weight for name in ORACLES]
        picks = []
        for variant in range(NUM_VARIANTS):
            best_play = None
            best_value = -math.inf
            for play in play_ids:
                weight = sum(plays[play][part]["n"] for part in pick_parts)
                if weight == 0:
                    return None
                value = sum(plays[play][part]["n"] *
                            plays[play][part][criterion][variant]
                            for part in pick_parts) / weight
                if value > best_value:
                    best_value = value
                    best_play = play
            picks.append(best_play)
        for variant in range(NUM_VARIANTS):
            if picks[variant] != picks[0]:
                flips[variant] += 1.0 / len(splits)
            for oracle_idx in range(len(ORACLES)):
                gains[variant][oracle_idx] += (
                    oracle[picks[variant]][oracle_idx] -
                    oracle[picks[0]][oracle_idx]) / len(splits)
    return gains, flips


def clustered(values_by_game, total):
    """Mean over positions and its standard error, clustered by game."""
    mean = sum(sum(values) for values in values_by_game.values()) / total
    games = len(values_by_game)
    if games < 2:
        return mean, float("nan")
    squares = 0.0
    for values in values_by_game.values():
        squares += (sum(values) - mean * len(values)) ** 2
    return mean, math.sqrt(games / (games - 1) * squares) / total


def report_gains(positions, criterion, pick_parts_count, bag_filter=None,
                 label=""):
    per_variant = [[defaultdict(list) for _ in ORACLES]
                   for _ in range(NUM_VARIANTS)]
    flips = [defaultdict(list) for _ in range(NUM_VARIANTS)]
    total = 0
    with_main = 0
    for position in positions.values():
        if bag_filter is not None and not bag_filter(position["bag"]):
            continue
        game = position["game"]
        result = None
        if len(position["main"]) > 1:
            result = position_gains(position, criterion, pick_parts_count)
        total += 1
        if result is not None:
            with_main += 1
        for variant in range(NUM_VARIANTS):
            flips[variant][game].append(result[1][variant] if result else 0.0)
            for oracle_idx in range(len(ORACLES)):
                per_variant[variant][oracle_idx][game].append(
                    result[0][variant][oracle_idx] if result else 0.0)
    if total == 0:
        return
    pick_rollouts = None
    for position in positions.values():
        if len(position["main"]) > 1:
            play = next(iter(position["main"].values()))
            pick_rollouts = sum(play[part]["n"] for part in play) * \
                pick_parts_count // NUM_PARTS
            break
    print(f"\n{label}picks by {criterion} from {pick_parts_count} of "
          f"{NUM_PARTS} parts (about {pick_rollouts} rollouts a candidate); "
          f"{total} positions, {with_main} with more than one candidate kept")
    print(f"  {'variant':18s} {'differs':>8s} {'spread/pos':>18s} "
          f"{'win pts/pos':>18s} {'utility/pos':>20s} {'4-ply plain':>16s} "
          f"{'4-ply full':>16s}")
    for variant in range(1, NUM_VARIANTS):
        flip_mean, _ = clustered(flips[variant], total)
        cells = []
        for oracle_idx, scale in enumerate([1.0, 100.0, 1.0, 1.0, 1.0]):
            mean, error = clustered(per_variant[variant][oracle_idx], total)
            cells.append((mean * scale, error * scale))
        print(f"  {VARIANTS[variant]:18s} {100 * flip_mean:7.2f}% "
              f"{cells[0][0]:+8.4f} ({cells[0][1]:.4f}) "
              f"{cells[1][0]:+8.4f} ({cells[1][1]:.4f}) "
              f"{cells[2][0]:+9.6f} ({cells[2][1]:.6f}) "
              f"{cells[3][0]:+7.4f} ({cells[3][1]:.4f}) "
              f"{cells[4][0]:+7.4f} ({cells[4][1]:.4f})")


def report_regression(positions):
    """Pooled within-candidate regressions of what followed the horizon on
    the horizon's two terms, over the rollouts played to the end."""
    target_names = ["next ply's score", "next two plies and leaves",
                    "rest of the game"]
    sums = [dict(xx=[[0.0, 0.0], [0.0, 0.0]], xy=[0.0, 0.0], yy=0.0)
            for _ in target_names]
    rollouts = 0
    cells = 0
    for position in positions.values():
        for play in position["main"].values():
            for part in play.values():
                count = part["cont"]
                if count < 2:
                    continue
                pat_sum, shift_sum, pat_sq, shift_sq, pat_shift = part["mom"]
                rollouts += count
                cells += 1
                centered = [[pat_sq - pat_sum * pat_sum / count,
                             pat_shift - pat_sum * shift_sum / count],
                            [pat_shift - pat_sum * shift_sum / count,
                             shift_sq - shift_sum * shift_sum / count]]
                for target in range(len(target_names)):
                    y_sum, y_sq, y_pat, y_shift = part["tgt"][target]
                    for row in range(2):
                        for col in range(2):
                            sums[target]["xx"][row][col] += centered[row][col]
                    sums[target]["xy"][0] += y_pat - y_sum * pat_sum / count
                    sums[target]["xy"][1] += y_shift - y_sum * shift_sum / count
                    sums[target]["yy"] += y_sq - y_sum * y_sum / count
    if rollouts == 0:
        return
    print(f"\nWhat followed the horizon, regressed on the horizon's terms "
          f"within each candidate ({rollouts} rollouts played to the end; "
          f"standard errors ignore that rollouts of one bag cycle share a "
          f"shuffle)")
    for target, name in enumerate(target_names):
        xx = sums[target]["xx"]
        xy = sums[target]["xy"]
        det = xx[0][0] * xx[1][1] - xx[0][1] * xx[1][0]
        if det == 0:
            continue
        inverse = [[xx[1][1] / det, -xx[0][1] / det],
                   [-xx[1][0] / det, xx[0][0] / det]]
        slopes = [inverse[0][0] * xy[0] + inverse[0][1] * xy[1],
                  inverse[1][0] * xy[0] + inverse[1][1] * xy[1]]
        residual = sums[target]["yy"] - slopes[0] * xy[0] - slopes[1] * xy[1]
        variance = residual / max(rollouts - cells - 2, 1)
        errors = [math.sqrt(variance * inverse[0][0]),
                  math.sqrt(variance * inverse[1][1])]
        alone = [xy[0] / xx[0][0] if xx[0][0] else float("nan"),
                 xy[1] / xx[1][1] if xx[1][1] else float("nan")]
        print(f"  {name:28s} PAT term {slopes[0]:+.3f} ({errors[0]:.3f}), "
              f"alone {alone[0]:+.3f};  shift {slopes[1]:+.3f} "
              f"({errors[1]:.3f}), alone {alone[1]:+.3f};  residual sd "
              f"{math.sqrt(variance):.1f}")
    print(f"  sd of the PAT term within a candidate "
          f"{math.sqrt(sums[0]['xx'][0][0] / rollouts):.2f}, of the shift "
          f"{math.sqrt(sums[0]['xx'][1][1] / rollouts):.2f} points")


def solve(matrix, vector):
    """Solves a small linear system by Gaussian elimination."""
    size = len(vector)
    rows = [list(matrix[row]) + [vector[row]] for row in range(size)]
    for col in range(size):
        pivot = max(range(col, size), key=lambda row: abs(rows[row][col]))
        if abs(rows[pivot][col]) < 1e-12:
            return None
        rows[col], rows[pivot] = rows[pivot], rows[col]
        for row in range(size):
            if row != col:
                factor = rows[row][col] / rows[col][col]
                for idx in range(col, size + 1):
                    rows[row][idx] -= factor * rows[col][idx]
    return [rows[row][size] / rows[row][row] for row in range(size)]


def between_by_game(positions, target, regressors, needs_split=0):
    """Per game, the normal equations of the finished games' mean result in
    two parts of the rollouts on the horizon's values in the other two
    parts, across a position's kept candidates, with a constant for each
    position. needs_split is how many split values a position must have."""
    size = len(regressors)
    by_game = defaultdict(lambda: ([[0.0] * size for _ in range(size)],
                                   [0.0] * size))
    points = 0
    for position in positions.values():
        plays = position["main"]
        if len(plays) < 2:
            continue
        if needs_split and any(part["split"] is None or
                               len(part["split"]) < needs_split
                               for play in plays.values()
                               for part in play.values()):
            continue
        xtx, xty = by_game[position["game"]]
        for value_parts in itertools.combinations(range(NUM_PARTS), 2):
            result_parts = [part for part in range(NUM_PARTS)
                            if part not in value_parts]
            rows = []
            for play in plays.values():
                weight = sum(play[part]["n"] for part in value_parts)
                finished = sum(play[part]["cont"] for part in result_parts)
                if weight == 0 or finished == 0:
                    rows = []
                    break
                values = [sum(play[part]["n"] * regressor(play[part])
                              for part in value_parts) / weight
                          for regressor in regressors]
                result = sum(play[part]["cont"] * play[part][target]
                             for part in result_parts) / finished
                rows.append((values, result))
            if len(rows) < 2:
                continue
            means = [sum(row[0][idx] for row in rows) / len(rows)
                     for idx in range(size)]
            result_mean = sum(row[1] for row in rows) / len(rows)
            for values, result in rows:
                centered = [values[idx] - means[idx] for idx in range(size)]
                for row_idx in range(size):
                    xty[row_idx] += centered[row_idx] * (result - result_mean)
                    for col_idx in range(size):
                        xtx[row_idx][col_idx] += (centered[row_idx] *
                                                  centered[col_idx])
                points += 1
    return by_game, points


def fit_between(by_game, size, skip=None):
    xtx = [[0.0] * size for _ in range(size)]
    xty = [0.0] * size
    for game, (game_xtx, game_xty) in by_game.items():
        if game == skip:
            continue
        for row_idx in range(size):
            xty[row_idx] += game_xty[row_idx]
            for col_idx in range(size):
                xtx[row_idx][col_idx] += game_xtx[row_idx][col_idx]
    return solve(xtx, xty)


def jackknife_error(values):
    count = len(values)
    mean = sum(values) / count
    return math.sqrt((count - 1) / count *
                     sum((value - mean) ** 2 for value in values))


def report_between(positions, label, target, regressors,
                   names=("the sim's value", "PAT term", "shift"),
                   needs_split=0):
    """Prints the fit of between_by_game, with standard errors from leaving
    out one game at a time."""
    size = len(regressors)
    by_game, points = between_by_game(positions, target, regressors,
                                      needs_split)
    if not by_game:
        return
    slopes = fit_between(by_game, size)
    if slopes is None:
        return
    games = len(by_game)
    errors = [float("nan")] * size
    if games > 2:
        left_out = [fit_between(by_game, size, game) for game in by_game]
        left_out = [fitted for fitted in left_out if fitted is not None]
        errors = [jackknife_error([fitted[idx] for fitted in left_out])
                  for idx in range(size)]
    print(f"  {label}: " + ";  ".join(
        f"{name} {slopes[idx]:+.3f} ({errors[idx]:.3f})"
        for idx, name in enumerate(names))
        + f"   [{games} games, {points} candidate values]")


def report_model_gains(positions, criterion, pick_parts_count):
    """Each variant's gain in final spread as the between-candidate fit
    predicts it: where a variant's pick differs from the unchanged sim's, the
    fit's value of the difference in the sim's value, the PAT term and the
    shift (each from all of the rollouts). Far less noisy than the finished
    games of the two picks, and only as good as the fit. Standard errors
    from leaving out one game at a time, refitting each time."""
    regressors = [lambda part: part["eq"][0], lambda part: part["pat"],
                  lambda part: part["shift"]]
    size = len(regressors)
    by_game, _ = between_by_game(positions, "fspread", regressors)
    differences = defaultdict(
        lambda: [[0.0] * size for _ in range(NUM_VARIANTS)])
    counts = defaultdict(int)
    splits = list(itertools.combinations(range(NUM_PARTS), pick_parts_count))
    for position in positions.values():
        game = position["game"]
        counts[game] += 1
        plays = position["main"]
        if len(plays) < 2:
            continue
        play_ids = sorted(plays)
        overall = {}
        for play in play_ids:
            weight = sum(plays[play][part]["n"] for part in plays[play])
            overall[play] = [
                sum(plays[play][part]["n"] * regressor(plays[play][part])
                    for part in plays[play]) / weight
                for regressor in regressors]
        for pick_parts in splits:
            picks = []
            for variant in range(NUM_VARIANTS):
                picks.append(max(play_ids, key=lambda play: sum(
                    plays[play][part]["n"] *
                    plays[play][part][criterion][variant]
                    for part in pick_parts) / sum(
                        plays[play][part]["n"] for part in pick_parts)))
            for variant in range(NUM_VARIANTS):
                for idx in range(size):
                    differences[game][variant][idx] += (
                        overall[picks[variant]][idx] -
                        overall[picks[0]][idx]) / len(splits)

    def estimate(skip=None):
        slopes = fit_between(by_game, size, skip)
        total = sum(count for game, count in counts.items() if game != skip)
        gains = []
        for variant in range(NUM_VARIANTS):
            gain = 0.0
            for game in counts:
                if game == skip or game not in differences:
                    continue
                gain += sum(slopes[idx] * differences[game][variant][idx]
                            for idx in range(size))
            gains.append(gain / total)
        return gains

    gains = estimate()
    left_out = [estimate(game) for game in counts]
    print(f"\nGain in final spread a position as the between-candidate fit "
          f"predicts it (picks by {criterion} from {pick_parts_count} of "
          f"{NUM_PARTS} parts)")
    for variant in range(1, NUM_VARIANTS):
        error = jackknife_error([fitted[variant] for fitted in left_out])
        print(f"  {VARIANTS[variant]:18s} {gains[variant]:+.4f} ({error:.4f})")


LINEAR_VARIANTS = [
    ("PAT", (1.0, 0.0, 0.0)),
    ("shift, both leaves", (0.0, 1.0, 1.0)),
    ("shift, own leave only", (0.0, 1.0, 0.0)),
    ("PAT + shift, both leaves", (1.0, 1.0, 1.0)),
    ("PAT + shift, own leave only", (1.0, 1.0, 0.0)),
    ("0.65 PAT + 0.8 own shift", (0.65, 0.8, 0.0)),
]


def report_linear_variants(positions, pick_parts_count):
    """Variants that add weighted terms to the sim's equity, for picks by
    equity (which is linear in the terms, so no rollout needs replaying):
    the last play's PAT term, the simming player's own last leave's shift
    and the opponent's. Only positions from runs that recorded the shift in
    parts. Gains by the finished games of the other parts, and by the
    between-candidate fit."""
    regressors = [lambda part: part["eq"][0], lambda part: part["pat"],
                  lambda part: part["split"][0],
                  lambda part: part["split"][1]]
    size = len(regressors)
    usable = {idx: position for idx, position in positions.items()
              if not any(part["split"] is None
                         for play in position["main"].values()
                         for part in play.values()) and
              position.get("has_split", True)}
    # A position with one candidate kept has no main lines to say which run
    # it came from: keep those of games that have split data elsewhere.
    split_games = {position["game"] for position in usable.values()
                   if len(position["main"]) > 1}
    usable = {idx: position for idx, position in usable.items()
              if position["game"] in split_games}
    if not usable:
        return
    by_game, _ = between_by_game(usable, "fspread", regressors, 4)
    splits = list(itertools.combinations(range(NUM_PARTS), pick_parts_count))
    direct = [[defaultdict(list) for _ in range(2)] for _ in LINEAR_VARIANTS]
    flips = [defaultdict(list) for _ in LINEAR_VARIANTS]
    differences = defaultdict(
        lambda: [[0.0] * size for _ in LINEAR_VARIANTS])
    counts = defaultdict(int)
    for position in usable.values():
        game = position["game"]
        counts[game] += 1
        plays = position["main"]
        gains = [[0.0, 0.0] for _ in LINEAR_VARIANTS]
        flipped = [0.0] * len(LINEAR_VARIANTS)
        if len(plays) > 1:
            play_ids = sorted(plays)
            overall = {}
            for play in play_ids:
                weight = sum(plays[play][part]["n"] for part in plays[play])
                overall[play] = [
                    sum(plays[play][part]["n"] * regressor(plays[play][part])
                        for part in plays[play]) / weight
                    for regressor in regressors]
            for pick_parts in splits:
                oracle_parts = [part for part in range(NUM_PARTS)
                                if part not in pick_parts]
                terms = {}
                oracle = {}
                for play in play_ids:
                    weight = sum(plays[play][part]["n"]
                                 for part in pick_parts)
                    terms[play] = [
                        sum(plays[play][part]["n"] *
                            regressor(plays[play][part])
                            for part in pick_parts) / weight
                        for regressor in regressors]
                    finished = sum(plays[play][part]["cont"]
                                   for part in oracle_parts)
                    oracle[play] = [
                        sum(plays[play][part]["cont"] *
                            plays[play][part][name]
                            for part in oracle_parts) / finished
                        for name in ("fspread", "fwin")]
                base_pick = max(play_ids, key=lambda play: terms[play][0])
                for variant_idx, (_, weights) in enumerate(LINEAR_VARIANTS):
                    pick = max(play_ids, key=lambda play: terms[play][0] + sum(
                        weights[idx] * terms[play][idx + 1]
                        for idx in range(3)))
                    if pick != base_pick:
                        flipped[variant_idx] += 1.0 / len(splits)
                    for oracle_idx in range(2):
                        gains[variant_idx][oracle_idx] += (
                            oracle[pick][oracle_idx] -
                            oracle[base_pick][oracle_idx]) / len(splits)
                    for idx in range(size):
                        differences[game][variant_idx][idx] += (
                            overall[pick][idx] -
                            overall[base_pick][idx]) / len(splits)
        for variant_idx in range(len(LINEAR_VARIANTS)):
            flips[variant_idx][game].append(flipped[variant_idx])
            for oracle_idx in range(2):
                direct[variant_idx][oracle_idx][game].append(
                    gains[variant_idx][oracle_idx])
    total = sum(counts.values())

    def fitted_gains(skip=None):
        slopes = fit_between(by_game, size, skip)
        kept = sum(count for game, count in counts.items() if game != skip)
        return [sum(slopes[idx] * differences[game][variant_idx][idx]
                    for game in counts if game != skip
                    for idx in range(size)) / kept
                for variant_idx in range(len(LINEAR_VARIANTS))]

    fitted = fitted_gains()
    left_out = [fitted_gains(game) for game in counts]
    print(f"\nVariants that are linear in the terms, picks by equity from "
          f"{pick_parts_count} of {NUM_PARTS} parts; {total} positions from "
          f"{len(counts)} games that recorded the shift in parts")
    print(f"  {'variant':28s} {'differs':>8s} {'spread/pos':>18s} "
          f"{'win pts/pos':>18s} {'spread/pos by fit':>20s}")
    for variant_idx, (name, _) in enumerate(LINEAR_VARIANTS):
        flip_mean, _ = clustered(flips[variant_idx], total)
        spread, spread_error = clustered(direct[variant_idx][0], total)
        win, win_error = clustered(direct[variant_idx][1], total)
        fit_error = jackknife_error([values[variant_idx]
                                     for values in left_out])
        print(f"  {name:28s} {100 * flip_mean:7.2f}% "
              f"{spread:+8.4f} ({spread_error:.4f}) "
              f"{100 * win:+8.4f} ({100 * win_error:.4f}) "
              f"{fitted[variant_idx]:+9.4f} ({fit_error:.4f})")


def report_terms(positions):
    pat_means = []
    shift_means = []
    pat_spreads = []
    shift_spreads = []
    for position in positions.values():
        screen = position["screen"]
        if not screen:
            continue
        pats = [play["pat"] for play in screen.values() if play["n"] > 0]
        shifts = [play["shift"] for play in screen.values() if play["n"] > 0]
        if len(pats) < 2:
            continue
        pat_means.append(sum(pats) / len(pats))
        shift_means.append(sum(shifts) / len(shifts))
        pat_spreads.append(max(pats) - min(pats))
        shift_spreads.append(max(shifts) - min(shifts))
    if not pat_means:
        return
    count = len(pat_means)

    def median(values):
        ordered = sorted(values)
        return ordered[len(ordered) // 2]

    print(f"\nThe terms over each position's screened candidates ({count} "
          f"positions), in points")
    print(f"  PAT term: mean {sum(pat_means) / count:+.3f}; range across a "
          f"position's candidates: mean {sum(pat_spreads) / count:.3f}, "
          f"median {median(pat_spreads):.3f}")
    print(f"  shift:    mean {sum(shift_means) / count:+.3f}; range across a "
          f"position's candidates: mean {sum(shift_spreads) / count:.3f}, "
          f"median {median(shift_spreads):.3f}")


def main():
    positions, checks = read(sys.argv[1:])
    games = len({position["game"] for position in positions.values()})
    print(f"{len(positions)} positions from {games} games")
    for line in checks:
        print(" ", line)
    # A run stopped part-way leaves its last position without all of its
    # lines.
    bad = [idx for idx, position in positions.items()
           if (position["kept"] > 1 and
               (len(position["main"]) != position["kept"] or
                any(len(play) != NUM_PARTS
                    for play in position["main"].values()))) or
           len(position["screen"]) != position["plays"]]
    for idx in bad:
        del positions[idx]
    if bad:
        print(f"  dropped {len(bad)} incomplete positions: {bad[:5]}")
    report_terms(positions)
    for pick_parts_count in (1, 2, 3):
        report_gains(positions, "util", pick_parts_count)
    report_gains(positions, "win", 3)
    report_gains(positions, "eq", 3)
    report_gains(positions, "util", 3, lambda bag: bag >= 21,
                 "bag of 21 or more: ")
    report_gains(positions, "util", 3, lambda bag: bag < 21,
                 "bag under 21: ")
    report_regression(positions)
    print("\nAcross a position's kept candidates: finished games' result "
          "on the horizon's values (1 = worth its face value)")
    report_between(positions, "final spread", "fspread",
                   [lambda part: part["eq"][0], lambda part: part["pat"],
                    lambda part: part["shift"]])
    report_between(positions, "final win  ", "fwin",
                   [lambda part: part["win"][0],
                    lambda part: part["win"][2] - part["win"][0],
                    lambda part: part["win"][3] - part["win"][0]])
    report_between(positions, "4-ply value ", "deep_plain",
                   [lambda part: part["eq"][0], lambda part: part["pat"],
                    lambda part: part["shift"]])
    print("\nThe same with the shift in parts (later runs only): each "
          "player's last leave, as its mover sees the unseen tiles and "
          "from the bag alone")
    split_names = ("the sim's value", "PAT term", "own leave's shift",
                   "opponent's leave's shift")
    for label, target in (("final spread", "fspread"),
                          ("4-ply value ", "deep_plain")):
        report_between(positions, label + ", mover's view", target,
                       [lambda part: part["eq"][0], lambda part: part["pat"],
                        lambda part: part["split"][0],
                        lambda part: part["split"][1]],
                       split_names, 4)
        report_between(positions, label + ", from the bag", target,
                       [lambda part: part["eq"][0], lambda part: part["pat"],
                        lambda part: part["split"][2],
                        lambda part: part["split"][3]],
                       split_names, 4)
    print("\nThe same with the leave values in parts as well (last runs "
          "only)")
    report_between(
        positions, "final spread", "fspread",
        [lambda part: part["eq"][0] - part["split"][4] - part["split"][5],
         lambda part: part["split"][4], lambda part: part["split"][5],
         lambda part: part["pat"], lambda part: part["split"][0],
         lambda part: part["split"][1]],
        ("the spread", "own leave value", "opponent's leave value",
         "PAT term", "own leave's shift", "opponent's leave's shift"), 6)
    report_model_gains(positions, "util", 3)
    report_model_gains(positions, "eq", 3)
    report_linear_variants(positions, 3)


if __name__ == "__main__":
    main()
