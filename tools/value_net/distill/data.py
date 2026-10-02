"""Reads the distillation records valuenet:distill writes (VntDistillRecord
in test/value_net_test.c): one candidate move per record, with its input
row, the teacher's value and spread, and the decision it belongs to."""

import glob
import os

import numpy as np

from model import BOARD_FLOATS, SCALARS

BOARD_BYTES = ((BOARD_FLOATS + 63) // 64) * 8

RECORD = np.dtype([
    ("board_bits", np.uint8, BOARD_BYTES),
    ("scalars", "<f4", SCALARS),
    ("value", "<f4"),
    ("spread", "<f4"),
    ("spread_after", "<f4"),
    ("equity", "<f4"),
    ("game_id", "<u4"),
    ("turn", "<u2"),
    ("candidate", "<u2"),
    ("candidates", "<u2"),
    ("bag", "<u2"),
    ("tiles_played", "<u2"),
    ("chosen", "<u2"),
])
assert RECORD.itemsize == 2712, RECORD.itemsize


def open_records(pattern):
    """Memory-maps every file matching pattern; returns a list of arrays. A
    trailing partial record (a file still being written) is left out."""
    files = sorted(glob.glob(pattern))
    if not files:
        raise FileNotFoundError(pattern)
    parts = []
    for path in files:
        count = os.path.getsize(path) // RECORD.itemsize
        parts.append(np.memmap(path, dtype=RECORD, mode="r", shape=(count,)))
    return parts


def unpack_board(bits):
    """(N, BOARD_BYTES) uint8 -> (N, 85 * 225) float32 of 0/1."""
    return np.unpackbits(bits, axis=1, count=BOARD_FLOATS).astype(np.float32)


if __name__ == "__main__":
    # Checks a records file: decodes a sample and reruns the teacher on it.
    import sys

    import torch

    from model import load_blob

    pattern, model_dir = sys.argv[1], sys.argv[2]
    parts = open_records(pattern)
    records = np.concatenate([np.asarray(part) for part in parts])
    print(f"{len(records)} records, {len(np.unique(records['game_id']))} games,"
          f" {len(np.unique(records[['game_id', 'turn']]))} decisions")
    print("candidates per decision: mean",
          records["candidates"].mean().round(1), "chosen per decision:",
          records["chosen"].sum() / len(np.unique(records[["game_id", "turn"]])))
    sample = records[np.random.default_rng(0).choice(len(records), 2048,
                                                      replace=False)]
    net = load_blob(model_dir).eval()
    with torch.no_grad():
        value, spread, _ = net(torch.from_numpy(unpack_board(sample["board_bits"])),
                               torch.from_numpy(sample["scalars"].copy()))
    print("teacher fp32 vs recorded (fp16 Metal): max |value diff|",
          np.abs(value.numpy() - sample["value"]).max(),
          "max |spread diff|", np.abs(spread.numpy() - sample["spread"]).max())
