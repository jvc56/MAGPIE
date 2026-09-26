#!/usr/bin/env python3
"""Train and export MAGPIE's additive contextual KLV3 model.

The input is produced by:

    magpie autoplay fj <games> -lex <lexicon> ...

Like ordinary leavegen, each observed full rack supplies its best static
equity and is projected onto subleaves.  For retained leave L in full rack R,
the contextual source pool is reconstructed as the public unseen pool plus
the drawn complement R-L.  The fitted residual is:

    static_equity(R) - corpus_mean_static_equity - KLV2(L)

and the model is:

    sum_h L[h] * (bias[pool_bin, draw_count, h]
                  + sum_u weight[draw_count, h, u] * unseen[u] / unseen_total)

The exporter copies an ordinary KLV2 body and appends a compact KLV3 trailer.
"""

from __future__ import annotations

import argparse
import csv
import glob
import itertools
import json
import math
import os
import random
import struct
import sys
from dataclasses import dataclass
from pathlib import Path

import numpy as np


KLV3_MAGIC = b"MAGKLV3\0"
KLV3_VERSION = 2
KLV3_DRAW_COUNT_HEADS = 8
KLV3_CAP_QUANTILE_SCALE = 1_000_000
FJ_FIXED_COLUMNS = 16
KWG_NODE_IS_END_FLAG = 0x400000
KWG_NODE_ACCEPTS_FLAG = 0x800000
KWG_ARC_INDEX_MASK = 0x3FFFFF
KWG_TILE_BIT_OFFSET = 24


@dataclass
class Corpus:
    seed: np.ndarray
    draw_count: np.ndarray
    pool_bin: np.ndarray
    leave_index: np.ndarray
    leave_counts: np.ndarray
    unseen_counts: np.ndarray
    unseen_total: np.ndarray
    target: np.ndarray
    corpus_mean_static_equity: float
    rows_seen: int
    rows_used: int


class KLV2:
    """Minimal KLV2 reader for fast subleave value lookups."""

    def __init__(self, path: Path, alphabet_size: int):
        self.alphabet_size = alphabet_size
        with path.open("rb") as stream:
            kwg_size_bytes = stream.read(4)
            if len(kwg_size_bytes) != 4:
                raise ValueError(f"{path}: truncated KLV2 header")
            kwg_size = struct.unpack("<I", kwg_size_bytes)[0]
            self.nodes = np.frombuffer(
                stream.read(kwg_size * 4), dtype="<u4"
            ).astype(np.uint32)
            if len(self.nodes) != kwg_size:
                raise ValueError(f"{path}: truncated KLV2 KWG")
            number_of_leaves_bytes = stream.read(4)
            if len(number_of_leaves_bytes) != 4:
                raise ValueError(f"{path}: missing KLV2 leave count")
            number_of_leaves = struct.unpack(
                "<I", number_of_leaves_bytes
            )[0]
            self.values = np.frombuffer(
                stream.read(number_of_leaves * 4), dtype="<f4"
            ).astype(np.float32)
            if len(self.values) != number_of_leaves:
                raise ValueError(f"{path}: truncated KLV2 leave values")
        self.word_counts = np.full(len(self.nodes), -1, dtype=np.int64)
        for node_index in range(len(self.nodes) - 1, -1, -1):
            self._count_words_at(node_index)
        self.number_of_leaves = number_of_leaves
        self.cache: dict[bytes, tuple[int, float]] = {}

    def _count_words_at(self, node_index: int) -> int:
        cached = int(self.word_counts[node_index])
        if cached >= 0:
            return cached
        node = int(self.nodes[node_index])
        count = 1 if node & KWG_NODE_ACCEPTS_FLAG else 0
        child = node & KWG_ARC_INDEX_MASK
        if child:
            count += self._count_words_at(child)
        if not node & KWG_NODE_IS_END_FLAG:
            count += self._count_words_at(node_index + 1)
        self.word_counts[node_index] = count
        return count

    def index_and_value_for_counts(
        self, counts: np.ndarray
    ) -> tuple[int, float]:
        key = counts.tobytes()
        cached = self.cache.get(key)
        if cached is not None:
            return cached
        letters = np.repeat(
            np.arange(self.alphabet_size, dtype=np.uint8), counts
        )
        if len(letters) == 0:
            return 0.0
        node_index = int(self.nodes[0]) & KWG_ARC_INDEX_MASK
        word_index = 0
        for position, machine_letter in enumerate(letters):
            scan_word_index = word_index
            while node_index:
                node = int(self.nodes[node_index])
                if node >> KWG_TILE_BIT_OFFSET == int(machine_letter):
                    word_index = scan_word_index
                    break
                if node & KWG_NODE_IS_END_FLAG:
                    raise KeyError(
                        f"KLV2 has no leave for machine letters {letters}"
                    )
                scan_word_index += int(
                    self.word_counts[node_index]
                    - self.word_counts[node_index + 1]
                )
                node_index += 1
            if position + 1 == len(letters):
                value = float(self.values[word_index])
                result = (word_index, value)
                self.cache[key] = result
                return result
            word_index += 1
            node_index = int(self.nodes[node_index]) & KWG_ARC_INDEX_MASK
        raise KeyError(f"KLV2 has no leave for machine letters {letters}")

    def value_for_counts(self, counts: np.ndarray) -> float:
        return self.index_and_value_for_counts(counts)[1]


def checked_matmul(left: np.ndarray, right: np.ndarray) -> np.ndarray:
    # Accelerate-backed NumPy on some Apple Silicon versions can leave IEEE
    # status flags set after a finite float32 GEMM, producing spurious
    # divide/overflow warnings on the matmul source line. Suppress those flags
    # locally, then perform the stronger check that matters to training.
    with np.errstate(divide="ignore", over="ignore", invalid="ignore"):
        result = left @ right
    if not np.isfinite(result).all():
        raise FloatingPointError("matrix multiplication produced NaN or inf")
    return result


def read_letter_distribution(path: Path) -> tuple[list[str], np.ndarray]:
    tiles: list[str] = []
    counts: list[int] = []
    with path.open(newline="", encoding="utf-8") as stream:
        for row in csv.reader(stream):
            if not row:
                continue
            tiles.append(row[0])
            counts.append(int(row[2]))
    if not tiles or len(tiles) > 32:
        raise ValueError(f"expected 1..32 machine letters, found {len(tiles)}")
    if any(len(tile) != 1 for tile in tiles):
        raise ValueError(
            "this trainer currently requires one Unicode codepoint per tile"
        )
    return tiles, np.asarray(counts, dtype=np.float32)


def pool_bin_for(unseen_total: int, upper_bounds: list[int]) -> int:
    for index, upper_bound in enumerate(upper_bounds):
        if unseen_total <= upper_bound:
            return index
    raise ValueError(
        f"unseen total {unseen_total} exceeds final pool bound "
        f"{upper_bounds[-1]}"
    )


def count_rows(paths: list[str]) -> int:
    total = 0
    for path in paths:
        with open(path, "rb") as stream:
            total += sum(1 for _ in stream)
    return total


def load_corpus(
    paths: list[str],
    tiles: list[str],
    pool_bounds: list[int],
    base_klv2: Path,
    rack_row_modulus: int,
    subleaves_per_rack: int,
) -> Corpus:
    alphabet_size = len(tiles)
    tile_to_index = {tile: index for index, tile in enumerate(tiles)}
    rows_seen = count_rows(paths)
    if rows_seen == 0:
        raise ValueError("the FJ input files contain no rows")

    # First pass: estimate the same corpus-wide full-rack equity baseline used
    # by ordinary leave generation.
    static_equity_sum = 0.0
    static_equity_count = 0
    selected_rack_rows = 0
    for path in paths:
        with open(path, newline="", encoding="utf-8") as stream:
            for row in csv.reader(stream):
                if len(row) != FJ_FIXED_COLUMNS + alphabet_size:
                    raise ValueError(
                        f"{path}: expected {FJ_FIXED_COLUMNS + alphabet_size} "
                        f"columns, found {len(row)}"
                    )
                if int(row[4]):
                    static_equity_sum += float(row[5])
                    static_equity_count += 1
                    full_rack = row[6]
                    seed = int(row[0])
                    turn = int(row[1])
                    if (
                        len(full_rack) == 7
                        and int(row[15]) > 0
                        and (
                            (
                                seed
                                ^ (
                                    turn
                                    * 0x9E3779B97F4A7C15
                                )
                            )
                            % rack_row_modulus
                            == 0
                        )
                    ):
                        selected_rack_rows += 1
    if static_equity_count == 0:
        raise ValueError("the FJ input contains no valid static equities")
    corpus_mean = static_equity_sum / static_equity_count

    max_examples = selected_rack_rows * subleaves_per_rack
    seeds = np.empty(max_examples, dtype=np.uint64)
    draws = np.empty(max_examples, dtype=np.uint8)
    bins = np.empty(max_examples, dtype=np.uint8)
    leave_indices = np.empty(max_examples, dtype=np.uint32)
    leaves = np.empty((max_examples, alphabet_size), dtype=np.uint8)
    unseen_counts = np.empty(
        (max_examples, alphabet_size), dtype=np.uint8
    )
    unseen_totals = np.empty(max_examples, dtype=np.uint8)
    targets = np.empty(max_examples, dtype=np.float32)
    klv2 = KLV2(base_klv2, alphabet_size)
    masks_by_retained_count = {
        count: [
            sum(1 << position for position in positions)
            for positions in itertools.combinations(range(7), count)
        ]
        for count in range(1, 7)
    }
    used = 0

    for path in paths:
        with open(path, newline="", encoding="utf-8") as stream:
            for row in csv.reader(stream):
                if not int(row[4]):
                    continue
                seed = int(row[0])
                turn = int(row[1])
                full_rack = row[6]
                current_unseen_total = int(row[15])
                if (
                    len(full_rack) != 7
                    or current_unseen_total <= 0
                    or (
                        (
                            seed
                            ^ (
                                turn
                                * 0x9E3779B97F4A7C15
                            )
                        )
                        % rack_row_modulus
                        != 0
                    )
                ):
                    continue

                try:
                    rack_letters = [
                        tile_to_index[tile] for tile in full_rack
                    ]
                except KeyError as exc:
                    raise ValueError(
                        f"{path}: unknown tile {exc.args[0]!r} in "
                        f"rack {full_rack!r}"
                    ) from exc

                current_unseen = np.asarray(
                    [int(value) for value in row[FJ_FIXED_COLUMNS:]],
                    dtype=np.uint8,
                )
                if int(current_unseen.sum()) != current_unseen_total:
                    raise ValueError(
                        f"{path}: unseen counts sum to "
                        f"{int(current_unseen.sum())}, row says "
                        f"{current_unseen_total}"
                    )
                hash_value = (
                    seed
                    ^ (turn * 0xD1B54A32D192ED03)
                ) & ((1 << 64) - 1)
                masks: list[int] = []
                # Guarantee every draw-count head gets one example per rack.
                for retained_count in range(1, 7):
                    candidates = masks_by_retained_count[retained_count]
                    hash_value = (
                        hash_value * 6364136223846793005 + 1
                    ) & ((1 << 64) - 1)
                    masks.append(candidates[hash_value % len(candidates)])
                while len(masks) < subleaves_per_rack:
                    hash_value = (
                        hash_value * 6364136223846793005 + 1
                    ) & ((1 << 64) - 1)
                    masks.append(1 + hash_value % 126)

                static_equity = float(row[5])
                for mask in masks:
                    leave_counts = np.zeros(
                        alphabet_size, dtype=np.uint8
                    )
                    drawn_counts = np.zeros(
                        alphabet_size, dtype=np.uint8
                    )
                    for position, machine_letter in enumerate(rack_letters):
                        if mask & (1 << position):
                            leave_counts[machine_letter] += 1
                        else:
                            drawn_counts[machine_letter] += 1
                    draw_count = int(drawn_counts.sum())
                    source_unseen = current_unseen + drawn_counts
                    source_unseen_total = current_unseen_total + draw_count
                    leave_index, klv2_value = (
                        klv2.index_and_value_for_counts(leave_counts)
                    )
                    target = (
                        static_equity
                        - corpus_mean
                        - klv2_value
                    )
                    seeds[used] = seed
                    draws[used] = draw_count
                    bins[used] = pool_bin_for(
                        source_unseen_total, pool_bounds
                    )
                    leave_indices[used] = leave_index
                    leaves[used] = leave_counts
                    unseen_counts[used] = source_unseen
                    unseen_totals[used] = source_unseen_total
                    targets[used] = target
                    used += 1

    if used == 0:
        raise ValueError("no usable KLV3 training rows were found")
    return Corpus(
        seed=seeds[:used],
        draw_count=draws[:used],
        pool_bin=bins[:used],
        leave_index=leave_indices[:used],
        leave_counts=leaves[:used],
        unseen_counts=unseen_counts[:used],
        unseen_total=unseen_totals[:used],
        target=targets[:used],
        corpus_mean_static_equity=corpus_mean,
        rows_seen=rows_seen,
        rows_used=used,
    )


def predictions(
    corpus: Corpus,
    indices: np.ndarray,
    biases: np.ndarray,
    weights: np.ndarray,
) -> np.ndarray:
    result = np.empty(len(indices), dtype=np.float32)
    for draw_count in range(1, KLV3_DRAW_COUNT_HEADS):
        local = indices[corpus.draw_count[indices] == draw_count]
        if len(local) == 0:
            continue
        leave = corpus.leave_counts[local].astype(np.float32)
        unseen = corpus.unseen_counts[local].astype(np.float32)
        unseen /= corpus.unseen_total[local, None]
        interaction = np.sum(
            checked_matmul(leave, weights[draw_count]) * unseen, axis=1
        )
        bias = np.sum(
            leave * biases[corpus.pool_bin[local], draw_count], axis=1
        )
        positions = np.flatnonzero(corpus.draw_count[indices] == draw_count)
        result[positions] = interaction + bias
    return result


def metrics(target: np.ndarray, predicted: np.ndarray) -> dict[str, float]:
    residual = predicted - target
    rmse = float(np.sqrt(np.mean(residual * residual)))
    mae = float(np.mean(np.abs(residual)))
    if len(target) > 1 and np.std(target) > 0 and np.std(predicted) > 0:
        correlation = float(np.corrcoef(target, predicted)[0, 1])
    else:
        correlation = 0.0
    return {"rmse": rmse, "mae": mae, "correlation": correlation}


def quantile_higher(values: np.ndarray, quantile: float) -> float:
    return float(np.quantile(values, quantile, method="higher"))


def compute_context_leave_caps(
    corpus: Corpus,
    number_of_leaves: int,
    biases: np.ndarray,
    weights: np.ndarray,
    quantile: float,
    min_samples: int,
) -> tuple[np.ndarray, dict[str, object]]:
    """Calibrate an upper contextual residual independently for each leave.

    Common leaves use their own empirical quantile. Sparse and unseen leaves
    fall back to the corresponding draw-head quantile (or the model-wide
    quantile when the leave was never observed). This is deliberately lossy:
    the RIT later takes max(KLV2 + cap) over a rack's subleaves.
    """

    all_rows = np.arange(len(corpus.seed), dtype=np.int64)
    residuals = predictions(corpus, all_rows, biases, weights)
    global_cap = quantile_higher(residuals, quantile)
    draw_caps = np.full(KLV3_DRAW_COUNT_HEADS, global_cap, dtype=np.float32)
    for draw_count in range(1, KLV3_DRAW_COUNT_HEADS):
        local = residuals[corpus.draw_count == draw_count]
        if len(local):
            draw_caps[draw_count] = quantile_higher(local, quantile)

    caps = np.full(number_of_leaves, global_cap, dtype=np.float32)
    order = np.argsort(corpus.leave_index, kind="stable")
    sorted_indices = corpus.leave_index[order]
    start = 0
    individually_calibrated = 0
    sparse_fallback = 0
    while start < len(order):
        end = start + 1
        leave_index = int(sorted_indices[start])
        while end < len(order) and int(sorted_indices[end]) == leave_index:
            end += 1
        rows = order[start:end]
        if len(rows) >= min_samples:
            caps[leave_index] = quantile_higher(residuals[rows], quantile)
            individually_calibrated += 1
        else:
            draw_count = int(corpus.draw_count[rows[0]])
            caps[leave_index] = draw_caps[draw_count]
            sparse_fallback += 1
        start = end

    covered = residuals <= caps[corpus.leave_index]
    report = {
        "quantile": quantile,
        "min_samples": min_samples,
        "global_fallback": global_cap,
        "draw_fallbacks": {
            str(draw_count): float(draw_caps[draw_count])
            for draw_count in range(1, KLV3_DRAW_COUNT_HEADS)
        },
        "observed_leaves": individually_calibrated + sparse_fallback,
        "individually_calibrated_leaves": individually_calibrated,
        "sparse_fallback_leaves": sparse_fallback,
        "unseen_fallback_leaves": (
            number_of_leaves - individually_calibrated - sparse_fallback
        ),
        "empirical_row_coverage": float(np.mean(covered)),
        "cap_min": float(np.min(caps)),
        "cap_median": float(np.median(caps)),
        "cap_max": float(np.max(caps)),
    }
    return caps, report


def train(
    corpus: Corpus,
    distribution: np.ndarray,
    num_pool_bins: int,
    epochs: int,
    batch_size: int,
    learning_rate: float,
    l2: float,
    huber_delta: float,
    validation_modulus: int,
    rng_seed: int,
) -> tuple[np.ndarray, np.ndarray, dict[str, object]]:
    alphabet_size = corpus.leave_counts.shape[1]
    validation_mask = corpus.seed % validation_modulus == 0
    if not validation_mask.any() or validation_mask.all():
        raise ValueError("the deterministic game-level split is empty")
    train_indices = np.flatnonzero(~validation_mask)
    validation_indices = np.flatnonzero(validation_mask)

    biases = np.zeros(
        (num_pool_bins, KLV3_DRAW_COUNT_HEADS, alphabet_size),
        dtype=np.float32,
    )
    weights = np.zeros(
        (KLV3_DRAW_COUNT_HEADS, alphabet_size, alphabet_size),
        dtype=np.float32,
    )
    bias_m = np.zeros_like(biases)
    bias_v = np.zeros_like(biases)
    weight_m = np.zeros_like(weights)
    weight_v = np.zeros_like(weights)
    beta1 = 0.9
    beta2 = 0.999
    epsilon = 1e-7
    step = 0
    rng = np.random.default_rng(rng_seed)

    history: list[dict[str, object]] = []
    for epoch in range(epochs):
        shuffled = train_indices.copy()
        rng.shuffle(shuffled)
        for start in range(0, len(shuffled), batch_size):
            batch = shuffled[start : start + batch_size]
            step += 1
            for draw_count in range(1, KLV3_DRAW_COUNT_HEADS):
                local = batch[corpus.draw_count[batch] == draw_count]
                if len(local) == 0:
                    continue
                leave = corpus.leave_counts[local].astype(np.float32)
                unseen = corpus.unseen_counts[local].astype(np.float32)
                unseen /= corpus.unseen_total[local, None]
                pool_bin = corpus.pool_bin[local]
                target = corpus.target[local]

                predicted = np.sum(
                    checked_matmul(leave, weights[draw_count]) * unseen,
                    axis=1,
                )
                predicted += np.sum(
                    leave * biases[pool_bin, draw_count], axis=1
                )
                residual = predicted - target
                gradient = np.clip(
                    residual, -huber_delta, huber_delta
                ).astype(np.float32)
                normalization = float(len(local))

                weight_gradient = checked_matmul(
                    leave.T, gradient[:, None] * unseen
                ) / normalization
                weight_gradient += l2 * weights[draw_count]
                weight_m[draw_count] = (
                    beta1 * weight_m[draw_count]
                    + (1.0 - beta1) * weight_gradient
                )
                weight_v[draw_count] = (
                    beta2 * weight_v[draw_count]
                    + (1.0 - beta2) * weight_gradient * weight_gradient
                )
                corrected_m = weight_m[draw_count] / (1.0 - beta1**step)
                corrected_v = weight_v[draw_count] / (1.0 - beta2**step)
                weights[draw_count] -= (
                    learning_rate
                    * corrected_m
                    / (np.sqrt(corrected_v) + epsilon)
                )

                for bin_index in np.unique(pool_bin):
                    in_bin = pool_bin == bin_index
                    bias_gradient = checked_matmul(
                        leave[in_bin].T, gradient[in_bin]
                    ) / normalization
                    bias_gradient += l2 * biases[
                        bin_index, draw_count
                    ]
                    bias_m[bin_index, draw_count] = (
                        beta1 * bias_m[bin_index, draw_count]
                        + (1.0 - beta1) * bias_gradient
                    )
                    bias_v[bin_index, draw_count] = (
                        beta2 * bias_v[bin_index, draw_count]
                        + (1.0 - beta2)
                        * bias_gradient
                        * bias_gradient
                    )
                    corrected_m = bias_m[
                        bin_index, draw_count
                    ] / (1.0 - beta1**step)
                    corrected_v = bias_v[
                        bin_index, draw_count
                    ] / (1.0 - beta2**step)
                    biases[bin_index, draw_count] -= (
                        learning_rate
                        * corrected_m
                        / (np.sqrt(corrected_v) + epsilon)
                    )

        train_prediction = predictions(
            corpus, train_indices, biases, weights
        )
        validation_prediction = predictions(
            corpus, validation_indices, biases, weights
        )
        epoch_metrics = {
            "epoch": epoch + 1,
            "train": metrics(corpus.target[train_indices], train_prediction),
            "validation": metrics(
                corpus.target[validation_indices], validation_prediction
            ),
        }
        history.append(epoch_metrics)
        validation = epoch_metrics["validation"]
        print(
            f"epoch {epoch + 1:02d}: "
            f"validation rmse={validation['rmse']:.4f} "
            f"mae={validation['mae']:.4f} "
            f"corr={validation['correlation']:.4f}",
            flush=True,
        )

    # The unseen frequencies sum to one, so each held tile's mean interaction
    # is not separately identifiable from its bias. Center the matrix under
    # the original tile distribution and transfer that constant into every
    # pool-bin bias. Predictions are unchanged, but interaction signs become
    # interpretable as abundance relative to the ordinary distribution.
    distribution_freq = distribution / distribution.sum()
    for draw_count in range(1, KLV3_DRAW_COUNT_HEADS):
        interaction_mean = weights[draw_count] @ distribution_freq
        weights[draw_count] -= interaction_mean[:, None]
        biases[:, draw_count] += interaction_mean[None, :]

    train_prediction = predictions(corpus, train_indices, biases, weights)
    validation_prediction = predictions(
        corpus, validation_indices, biases, weights
    )
    report: dict[str, object] = {
        "rows_seen": corpus.rows_seen,
        "rows_used": corpus.rows_used,
        "training_rows": int(len(train_indices)),
        "validation_rows": int(len(validation_indices)),
        "corpus_mean_static_equity": corpus.corpus_mean_static_equity,
        "zero_model_validation": metrics(
            corpus.target[validation_indices],
            np.zeros(len(validation_indices), dtype=np.float32),
        ),
        "fitted_validation": metrics(
            corpus.target[validation_indices], validation_prediction
        ),
        "fitted_training": metrics(
            corpus.target[train_indices], train_prediction
        ),
        "history": history,
        "draw_counts": {},
    }
    for draw_count in range(1, KLV3_DRAW_COUNT_HEADS):
        draw_validation = validation_indices[
            corpus.draw_count[validation_indices] == draw_count
        ]
        if len(draw_validation) == 0:
            continue
        report["draw_counts"][str(draw_count)] = {
            "validation_rows": int(len(draw_validation)),
            "zero_model": metrics(
                corpus.target[draw_validation],
                np.zeros(len(draw_validation), dtype=np.float32),
            ),
            "fitted": metrics(
                corpus.target[draw_validation],
                predictions(corpus, draw_validation, biases, weights),
            ),
        }
    return biases, weights, report


def export_klv3(
    base_klv2: Path,
    output: Path,
    pool_bounds: list[int],
    biases: np.ndarray,
    weights: np.ndarray,
    leave_caps: np.ndarray,
    cap_quantile: float,
    cap_min_samples: int,
    scale: float,
) -> None:
    output.parent.mkdir(parents=True, exist_ok=True)
    header = struct.pack(
        "<8sIIII",
        KLV3_MAGIC,
        KLV3_VERSION,
        weights.shape[1],
        KLV3_DRAW_COUNT_HEADS,
        len(pool_bounds),
    )
    bounds = struct.pack(f"<{len(pool_bounds)}H", *pool_bounds)
    with base_klv2.open("rb") as source, output.open("wb") as destination:
        while chunk := source.read(1024 * 1024):
            destination.write(chunk)
        destination.write(header)
        destination.write(bounds)
        destination.write(
            np.asarray(biases * scale, dtype="<f4").tobytes(order="C")
        )
        destination.write(
            np.asarray(weights * scale, dtype="<f4").tobytes(order="C")
        )
        destination.write(
            struct.pack(
                "<II",
                round(cap_quantile * KLV3_CAP_QUANTILE_SCALE),
                cap_min_samples,
            )
        )
        destination.write(
            np.asarray(leave_caps, dtype="<f4").tobytes(order="C")
        )


def read_klv3_context(
    path: Path, expected_alphabet_size: int
) -> tuple[list[int], np.ndarray, np.ndarray]:
    with path.open("rb") as stream:
        kwg_size_data = stream.read(4)
        if len(kwg_size_data) != 4:
            raise ValueError(f"{path}: truncated KLV header")
        kwg_size = struct.unpack("<I", kwg_size_data)[0]
        stream.seek(4 * kwg_size, os.SEEK_CUR)
        leave_count_data = stream.read(4)
        if len(leave_count_data) != 4:
            raise ValueError(f"{path}: truncated KLV leave count")
        leave_count = struct.unpack("<I", leave_count_data)[0]
        stream.seek(4 * leave_count, os.SEEK_CUR)
        header = stream.read(struct.calcsize("<8sIIII"))
        if len(header) != struct.calcsize("<8sIIII"):
            raise ValueError(f"{path}: missing KLV3 trailer")
        magic, version, alphabet_size, draw_heads, num_pool_bins = (
            struct.unpack("<8sIIII", header)
        )
        if (
            magic != KLV3_MAGIC
            or version not in (1, KLV3_VERSION)
            or alphabet_size != expected_alphabet_size
            or draw_heads != KLV3_DRAW_COUNT_HEADS
        ):
            raise ValueError(f"{path}: incompatible KLV3 model")
        bounds = list(
            struct.unpack(
                f"<{num_pool_bins}H", stream.read(2 * num_pool_bins)
            )
        )
        num_biases = num_pool_bins * draw_heads * alphabet_size
        num_weights = draw_heads * alphabet_size * alphabet_size
        biases = np.frombuffer(
            stream.read(4 * num_biases), dtype="<f4"
        ).astype(np.float32)
        weights = np.frombuffer(
            stream.read(4 * num_weights), dtype="<f4"
        ).astype(np.float32)
        if len(biases) != num_biases or len(weights) != num_weights:
            raise ValueError(f"{path}: truncated KLV3 tensors")
        return (
            bounds,
            biases.reshape(
                num_pool_bins, draw_heads, alphabet_size
            ),
            weights.reshape(draw_heads, alphabet_size, alphabet_size),
        )


def scale_suffix(scale: float) -> str:
    return f"{round(scale * 100):03d}"


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--fj-glob",
        required=True,
        help="glob matching nonempty autoplay_record_fj_* files",
    )
    parser.add_argument(
        "--letter-distribution",
        required=True,
        type=Path,
        help="letter distribution CSV used by the autoplay corpus",
    )
    parser.add_argument(
        "--base-klv2",
        required=True,
        type=Path,
        help="ordinary KLV2 whose body and leave values KLV3 extends",
    )
    parser.add_argument(
        "--output-prefix",
        required=True,
        type=Path,
        help="output prefix; scale suffix and .klv3 are appended",
    )
    parser.add_argument(
        "--pool-bounds",
        default="21,35,55,100",
        help="comma-separated inclusive upper bounds (default: %(default)s)",
    )
    parser.add_argument(
        "--scales",
        default="4.0",
        help="coefficient scales to export (default: %(default)s)",
    )
    parser.add_argument(
        "--component",
        choices=("interaction", "full", "bias"),
        default="interaction",
        help=(
            "model component to export; interaction keeps KLV2 as the "
            "rack-only baseline and exports only pool-dependent deviations "
            "(default: %(default)s)"
        ),
    )
    parser.add_argument(
        "--rack-row-modulus",
        type=int,
        default=10,
        help=(
            "deterministically use about one in N full-rack observations "
            "(default: %(default)s)"
        ),
    )
    parser.add_argument(
        "--subleaves-per-rack",
        type=int,
        default=16,
        help=(
            "tile-subset samples projected from each selected full rack; "
            "must be at least 6 (default: %(default)s)"
        ),
    )
    parser.add_argument("--epochs", type=int, default=12)
    parser.add_argument("--batch-size", type=int, default=4096)
    parser.add_argument("--learning-rate", type=float, default=0.015)
    parser.add_argument("--l2", type=float, default=0.002)
    parser.add_argument("--huber-delta", type=float, default=1000.0)
    parser.add_argument("--validation-modulus", type=int, default=8)
    parser.add_argument("--seed", type=int, default=60724)
    parser.add_argument(
        "--cap-quantile",
        type=float,
        default=0.99,
        help="per-leave contextual upper quantile (default: %(default)s)",
    )
    parser.add_argument(
        "--cap-min-samples",
        type=int,
        default=32,
        help=(
            "minimum observations before using a leave-specific cap "
            "(default: %(default)s)"
        ),
    )
    parser.add_argument(
        "--reuse-model",
        type=Path,
        help="reuse tensors from an existing KLV3 instead of retraining",
    )
    parser.add_argument(
        "--report",
        type=Path,
        help="optional JSON training report (default: <prefix>_report.json)",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    random.seed(args.seed)
    np.random.seed(args.seed)
    paths = sorted(
        path for path in glob.glob(args.fj_glob) if os.path.getsize(path) > 0
    )
    if not paths:
        print(f"no nonempty files matched {args.fj_glob!r}", file=sys.stderr)
        return 2
    pool_bounds = [int(value) for value in args.pool_bounds.split(",")]
    if (
        not pool_bounds
        or pool_bounds != sorted(set(pool_bounds))
        or pool_bounds[-1] < 100
    ):
        raise ValueError(
            "pool bounds must be strictly increasing and cover at least 100"
        )
    scales = [float(value) for value in args.scales.split(",")]
    if any(not math.isfinite(scale) or scale < 0 for scale in scales):
        raise ValueError("all scales must be finite and nonnegative")
    if args.rack_row_modulus <= 0:
        raise ValueError("rack row modulus must be positive")
    if args.subleaves_per_rack < 6:
        raise ValueError("subleaves per rack must be at least 6")
    if not 0.5 <= args.cap_quantile <= 1.0:
        raise ValueError("cap quantile must be in [0.5, 1.0]")
    if args.cap_min_samples <= 0:
        raise ValueError("cap minimum sample count must be positive")

    tiles, distribution = read_letter_distribution(args.letter_distribution)
    if args.reuse_model:
        model_pool_bounds, reused_biases, reused_weights = read_klv3_context(
            args.reuse_model, len(tiles)
        )
        if model_pool_bounds != pool_bounds:
            raise ValueError(
                "reuse-model pool bounds do not match --pool-bounds"
            )
    print(
        f"loading {len(paths)} FJ files for {len(tiles)} machine letters",
        flush=True,
    )
    corpus = load_corpus(
        paths,
        tiles,
        pool_bounds,
        args.base_klv2,
        args.rack_row_modulus,
        args.subleaves_per_rack,
    )
    print(
        f"loaded {corpus.rows_used:,} usable rows out of "
        f"{corpus.rows_seen:,}; mean static equity "
        f"{corpus.corpus_mean_static_equity:.4f}",
        flush=True,
    )
    if args.reuse_model:
        biases = reused_biases
        weights = reused_weights
        report = {
            "reused_model": str(args.reuse_model),
            "rows_seen": corpus.rows_seen,
            "rows_used": corpus.rows_used,
        }
    else:
        biases, weights, report = train(
            corpus=corpus,
            distribution=distribution,
            num_pool_bins=len(pool_bounds),
            epochs=args.epochs,
            batch_size=args.batch_size,
            learning_rate=args.learning_rate,
            l2=args.l2,
            huber_delta=args.huber_delta,
            validation_modulus=args.validation_modulus,
            rng_seed=args.seed,
        )
    report.update(
        {
            "format_version": KLV3_VERSION,
            "tiles": tiles,
            "pool_bounds": pool_bounds,
            "epochs": args.epochs,
            "batch_size": args.batch_size,
            "learning_rate": args.learning_rate,
            "l2": args.l2,
            "huber_delta": args.huber_delta,
            "validation_modulus": args.validation_modulus,
            "seed": args.seed,
            "scales": scales,
            "export_component": args.component,
            "rack_row_modulus": args.rack_row_modulus,
            "subleaves_per_rack": args.subleaves_per_rack,
            "cap_quantile": args.cap_quantile,
            "cap_min_samples": args.cap_min_samples,
        }
    )
    export_biases = biases
    export_weights = weights
    if not args.reuse_model and args.component == "interaction":
        export_biases = np.zeros_like(biases)
    elif not args.reuse_model and args.component == "bias":
        export_weights = np.zeros_like(weights)
    validation_indices = np.flatnonzero(
        corpus.seed % args.validation_modulus == 0
    )
    report["scaled_validation"] = {}
    report["context_caps"] = {}
    klv2 = KLV2(args.base_klv2, len(tiles))
    for scale in scales:
        report["scaled_validation"][f"{scale:g}"] = metrics(
            corpus.target[validation_indices],
            predictions(
                corpus,
                validation_indices,
                export_biases * scale,
                export_weights * scale,
            ),
        )
        leave_caps, cap_report = compute_context_leave_caps(
            corpus,
            klv2.number_of_leaves,
            export_biases * scale,
            export_weights * scale,
            args.cap_quantile,
            args.cap_min_samples,
        )
        report["context_caps"][f"{scale:g}"] = cap_report
        output = Path(f"{args.output_prefix}{scale_suffix(scale)}.klv3")
        export_klv3(
            args.base_klv2,
            output,
            pool_bounds,
            export_biases,
            export_weights,
            leave_caps,
            args.cap_quantile,
            args.cap_min_samples,
            scale,
        )
        print(f"wrote {output} (scale {scale:g})", flush=True)
    report_path = (
        args.report
        if args.report
        else Path(f"{args.output_prefix}_report.json")
    )
    report_path.parent.mkdir(parents=True, exist_ok=True)
    with report_path.open("w", encoding="utf-8") as stream:
        json.dump(report, stream, indent=2, sort_keys=True)
        stream.write("\n")
    print(f"wrote {report_path}", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
