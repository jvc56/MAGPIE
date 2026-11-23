#!/usr/bin/env python3
"""Analyzes simhorizon output of the three-stage design (screen, main,
oracle; test/sim_horizon_test.c in the horizon-eval worktree).

Usage: analyze_horizon_v2.py [--base N] FILE [FILE...]

--base is the variant the others are compared with: 3 (the default) is the
sim as it is on main, whose horizon values both players' last leaves as the
rollout's plies do, shift included; 0 is those leaves without the shift.

Each variant's pick is the kept candidate with the best mean utility over
the main stage's rollouts. Where the variants' picks differ, the oracle
stage gave every pick fresh rollouts played to the end of the game. A
variant's gain at a position is its pick's mean result there less the
base variant's pick's, 0 where they agree. Standard errors are clustered
by game.
"""
import math
import re
import sys
from collections import defaultdict

# A variant is a leave mode and a share of the last play's PAT term (see
# SIM_LAB_NUM_LEAVE_MODES in src/impl/sim_lab.h). The rollout's leaves are the
# stock leaves under a pool shift weight and the leaves of the sim's pool
# under -poolshift exact. Runs made before the last two leave modes existed
# have nine variants.
LEAVE_MODES = ["rollout leaves, no shift", "sim as it is",
               "shift on own leave only", "opponent's leave at stock",
               "both leaves at stock"]
PAT_SHARES = ["", " + half PAT", " + PAT"]
VARIANTS = [mode + share for mode in LEAVE_MODES for share in PAT_SHARES]
NUM_PARTS = 4
METRICS = ["fspread", "fwin", "futil"]

POS_RE = re.compile(r"^SIMH pos idx=(\d+) game=(\d+) bag=(\d+) plays=(\d+) "
                    r"kept=(\d+)")
MAIN_RE = re.compile(r"^SIMH main pos=(\d+) play=(\d+) move=(.*?) half=(\d+) "
                     r"n=(\d+)")
PICKS_RE = re.compile(r"^SIMH picks pos=(\d+) measure=(\w+) picks=(\S+)")
ORACLE_RE = re.compile(
    r"^SIMH oracle pos=(\d+) play=(\d+) move=(.*?) half=(\d+) n=(\d+) "
    r"cont=(\d+) fspread=(\S+) fwin=(\S+) futil=(\S+)")
TIME_RE = re.compile(r"^SIMH time pos=(\d+) screen_secs=(\S+) "
                     r"main_secs=(\S+) oracle_secs=(\S+)")


def read(paths):
    positions = {}
    notes = []
    for path in paths:
        for line in open(path, errors="replace"):
            match = POS_RE.match(line)
            if match:
                positions[int(match.group(1))] = {
                    "game": int(match.group(2)), "bag": int(match.group(3)),
                    "kept": int(match.group(5)), "moves": {}, "picks": {},
                    "oracle": defaultdict(dict), "done": False,
                    "main_rollouts": 0, "seconds": None}
                continue
            match = MAIN_RE.match(line)
            if match:
                position = positions[int(match.group(1))]
                position["moves"][int(match.group(2))] = match.group(3)
                if int(match.group(2)) == 0:
                    position["main_rollouts"] += int(match.group(5))
                continue
            match = PICKS_RE.match(line)
            if match:
                positions[int(match.group(1))]["picks"][match.group(2)] = [
                    int(pick) for pick in match.group(3).split(",")]
                continue
            match = ORACLE_RE.match(line)
            if match:
                positions[int(match.group(1))]["oracle"][match.group(3)][
                    int(match.group(4))] = {
                    "cont": int(match.group(6)),
                    "fspread": float(match.group(7)),
                    "fwin": float(match.group(8)),
                    "futil": float(match.group(9))}
                continue
            match = TIME_RE.match(line)
            if match:
                position = positions[int(match.group(1))]
                position["done"] = True
                position["seconds"] = (float(match.group(2)),
                                       float(match.group(3)),
                                       float(match.group(4)))
                continue
            if line.startswith("SIMH checks") or \
                    line.startswith("SIMH mean_mismatch"):
                notes.append(line.strip())
    return positions, notes


def clustered(values_by_game, total):
    mean = sum(sum(values) for values in values_by_game.values()) / total
    games = len(values_by_game)
    if games < 2:
        return mean, float("nan")
    squares = 0.0
    for values in values_by_game.values():
        squares += (sum(values) - mean * len(values)) ** 2
    return mean, math.sqrt(games / (games - 1) * squares) / total


def oracle_value(parts, metric, only_part=None):
    chosen = [parts[part] for part in parts
              if only_part is None or part == only_part]
    weight = sum(part["cont"] for part in chosen)
    return sum(part["cont"] * part[metric] for part in chosen) / weight


def report(positions, base, measure="util", bag_filter=None, label=""):
    number_of_variants = min(
        [len(VARIANTS)] +
        [len(position["picks"][measure]) for position in positions.values()
         if measure in position["picks"]])
    gains = [[defaultdict(list) for _ in METRICS]
             for _ in range(number_of_variants)]
    flips = [defaultdict(list) for _ in range(number_of_variants)]
    noise_squares = [[0.0 for _ in METRICS] for _ in range(number_of_variants)]
    noise_counts = [0] * number_of_variants
    total = 0
    with_main = 0
    with_oracle = 0
    missing_oracle = 0
    rollouts = 0
    for position in positions.values():
        if not position["done"]:
            continue
        if bag_filter is not None and not bag_filter(position["bag"]):
            continue
        picks = position["picks"].get(measure)
        position_gains = [[0.0] * len(METRICS)
                          for _ in range(number_of_variants)]
        position_flips = [0.0] * number_of_variants
        usable = True
        if picks is not None:
            with_main += 1
            rollouts = max(rollouts, position["main_rollouts"])
            if len(set(picks)) > 1:
                moves = [position["moves"][pick] for pick in picks]
                if any(move not in position["oracle"] or
                       len(position["oracle"][move]) != NUM_PARTS
                       for move in moves):
                    usable = False
                    missing_oracle += 1
                else:
                    with_oracle += 1
                    for variant in range(number_of_variants):
                        if picks[variant] == picks[base]:
                            continue
                        position_flips[variant] = 1.0
                        noise_counts[variant] += 1
                        for metric_idx, metric in enumerate(METRICS):
                            position_gains[variant][metric_idx] = (
                                oracle_value(position["oracle"][moves[variant]],
                                             metric) -
                                oracle_value(position["oracle"][moves[base]],
                                             metric))
                            by_part = [
                                oracle_value(
                                    position["oracle"][moves[variant]], metric,
                                    part) -
                                oracle_value(position["oracle"][moves[base]],
                                             metric, part)
                                for part in range(NUM_PARTS)]
                            part_mean = sum(by_part) / NUM_PARTS
                            noise_squares[variant][metric_idx] += sum(
                                (value - part_mean) ** 2
                                for value in by_part) / (
                                    NUM_PARTS * (NUM_PARTS - 1))
        if not usable:
            continue
        total += 1
        game = position["game"]
        for variant in range(number_of_variants):
            flips[variant][game].append(position_flips[variant])
            for metric_idx in range(len(METRICS)):
                gains[variant][metric_idx][game].append(
                    position_gains[variant][metric_idx])
    if total == 0:
        return
    games = len(flips[0])
    print(f"\n{label}against '{VARIANTS[base]}': picks by {measure} from "
          f"about {rollouts} rollouts a "
          f"candidate; {total} positions from {games} games, {with_main} "
          f"with more than one candidate kept, {with_oracle} with differing "
          f"picks" + (f"; {missing_oracle} dropped for lack of oracle lines"
                      if missing_oracle else ""))
    print(f"  {'variant':36s} {'differs':>8s} {'spread/pos':>18s} "
          f"{'win pts/pos':>18s} {'utility/pos x 1e4':>20s}   "
          f"oracle noise per differing pick (spread, win pts)")
    for variant in range(number_of_variants):
        if variant == base:
            continue
        flip_mean, _ = clustered(flips[variant], total)
        cells = []
        for metric_idx, scale in enumerate([1.0, 100.0, 1e4]):
            mean, error = clustered(gains[variant][metric_idx], total)
            cells.append((mean * scale, error * scale))
        count = max(noise_counts[variant], 1)
        noise = [math.sqrt(noise_squares[variant][metric_idx] / count)
                 for metric_idx in range(len(METRICS))]
        print(f"  {VARIANTS[variant]:36s} {100 * flip_mean:7.2f}% "
              f"{cells[0][0]:+8.4f} ({cells[0][1]:.4f}) "
              f"{cells[1][0]:+8.4f} ({cells[1][1]:.4f}) "
              f"{cells[2][0]:+9.4f} ({cells[2][1]:.4f})   "
              f"{noise[0]:.2f}, {100 * noise[1]:.2f}")


def main():
    arguments = sys.argv[1:]
    base = 3
    if arguments and arguments[0] == "--base":
        base = int(arguments[1])
        arguments = arguments[2:]
    positions, notes = read(arguments)
    done = [position for position in positions.values() if position["done"]]
    games = len({position["game"] for position in done})
    print(f"{len(done)} finished positions from {games} games")
    for line in notes:
        print(" ", line[:230])
    seconds = [position["seconds"] for position in done]
    if seconds:
        count = len(seconds)
        print(f"  mean seconds a position: screen "
              f"{sum(part[0] for part in seconds) / count:.2f}, main "
              f"{sum(part[1] for part in seconds) / count:.2f}, oracle "
              f"{sum(part[2] for part in seconds) / count:.2f}")
    report(positions, base)
    report(positions, base, bag_filter=lambda bag: bag >= 21,
           label="bag of 21 or more: ")
    report(positions, base, bag_filter=lambda bag: bag < 21,
           label="bag under 21: ")


if __name__ == "__main__":
    main()
