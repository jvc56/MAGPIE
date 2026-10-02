"""Quantizes a value net's Neural Engine build (build_ane.py's
<model_dir>/ane.mlpackage) to int8 with coremltools: W8 (int8 weights,
per channel), A8 (int8 activations, calibrated on distillation records) and
W8A8 (both, for the Neural Engine's int8 compute path). Each variant goes to
<model_dir>-<variant>/ (the net's weights.f32, manifest.json and parity/
linked, so MAGPIE's ane backend and anebench load it), and is compared with
the float16 build on held-out decisions: how often it picks the same move,
and its regret against the teacher's values in the records.

    nn-venv/bin/python quantize_ane.py <model_dir> '<records glob>'
"""

import argparse
import os

import coremltools as ct
import coremltools.optimize as cto
import numpy as np

from data import open_records, unpack_board
from train import Records, utility

BATCH = 8


def predict(model, board, scalars):
    """value and spread for rows rows, in fixed batches of BATCH."""
    rows = len(board)
    values, spreads = np.empty(rows, np.float32), np.empty(rows, np.float32)
    for start in range(0, rows, BATCH):
        count = min(BATCH, rows - start)
        b = np.zeros((BATCH, 85, 225), np.float32)
        s = np.zeros((BATCH, 72), np.float32)
        b[:count] = board[start:start + count].reshape(count, 85, 225)
        s[:count] = scalars[start:start + count]
        b[count:], s[count:] = b[0], s[0]
        out = model.predict({"board": b, "scalars": s})
        values[start:start + count] = out["value"][:count]
        spreads[start:start + count] = out["spread"][:count]
    return values, spreads


def calibration_data(records, batches, seed):
    rng = np.random.default_rng(seed)
    samples = []
    for _ in range(batches):
        picks = records.train[rng.integers(0, len(records.train), BATCH)]
        rows = [records.rows(int(part), int(start) + int(rng.integers(count)))
                for part, start, count in picks]
        samples.append({
            "board": unpack_board(np.stack([r["board_bits"] for r in rows]))
            .reshape(BATCH, 85, 225),
            "scalars": np.stack([r["scalars"] for r in rows]).astype(np.float32)})
    return samples


def compare(models, records, decisions):
    """Per variant: top-1 agreement with the float16 build and with the
    teacher, and regret against the teacher, by value and by utility."""
    stats = {name: {"same_v": 0, "same_u": 0, "top1_v": 0, "top1_u": 0,
                    "regret_v": 0.0, "regret_u": 0.0} for name in models}
    picks = records.val[:decisions]
    for part, start, count in picks:
        rows = records.rows(int(part), np.arange(start, start + count))
        board = unpack_board(rows["board_bits"])
        teacher_u = utility(rows["value"], rows["spread"], rows["spread_after"])
        best_v, best_u = int(np.argmax(rows["value"])), int(np.argmax(teacher_u))
        reference = None
        for name, model in models.items():
            value, spread = predict(model, board, rows["scalars"])
            pick_v = int(np.argmax(value))
            pick_u = int(np.argmax(utility(value, spread, rows["spread_after"])))
            if reference is None:
                reference = (pick_v, pick_u)
            st = stats[name]
            st["same_v"] += pick_v == reference[0]
            st["same_u"] += pick_u == reference[1]
            st["top1_v"] += pick_v == best_v
            st["top1_u"] += pick_u == best_u
            st["regret_v"] += rows["value"][best_v] - rows["value"][pick_v]
            st["regret_u"] += teacher_u[best_u] - teacher_u[pick_u]
    n = len(picks)
    for name, st in stats.items():
        print(f"{name:6s} same move as fp16: value {st['same_v'] / n:.3f} "
              f"utility {st['same_u'] / n:.3f} | top1 vs teacher: value "
              f"{st['top1_v'] / n:.3f} utility {st['top1_u'] / n:.3f} | "
              f"regret value {st['regret_v'] / n:.4f} utility "
              f"{st['regret_u'] / n:.4f}", flush=True)


def variant_dir(model_dir, name, model):
    out = f"{model_dir.rstrip('/')}-{name}"
    os.makedirs(out, exist_ok=True)
    for item in ["weights.f32", "manifest.json", "parity"]:
        link = f"{out}/{item}"
        if not os.path.lexists(link):
            os.symlink(os.path.abspath(f"{model_dir}/{item}"), link)
    model.save(f"{out}/ane.mlpackage")
    return out


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("model_dir")
    parser.add_argument("records")
    parser.add_argument("--calibration", type=int, default=128,
                        help="batches of 8 rows for activation calibration")
    parser.add_argument("--decisions", type=int, default=500)
    args = parser.parse_args()
    records = Records(args.records, 50, 1)
    units = ct.ComputeUnit.CPU_AND_NE
    fp16 = ct.models.MLModel(f"{args.model_dir}/ane.mlpackage",
                             compute_units=units)
    weight_config = cto.coreml.OptimizationConfig(
        global_config=cto.coreml.OpLinearQuantizerConfig(
            mode="linear_symmetric", dtype="int8", granularity="per_channel",
            weight_threshold=0))
    activation_config = cto.coreml.OptimizationConfig(
        global_config=cto.coreml.OpLinearQuantizerConfig(
            mode="linear_symmetric"))
    samples = calibration_data(records, args.calibration, 0)
    w8 = cto.coreml.linear_quantize_weights(fp16, weight_config)
    a8 = cto.coreml.linear_quantize_activations(fp16, activation_config,
                                                samples)
    w8a8 = cto.coreml.linear_quantize_weights(a8, weight_config)
    models = {"fp16": fp16}
    for name, model in [("w8", w8), ("a8", a8), ("w8a8", w8a8)]:
        out = variant_dir(args.model_dir, name, model)
        print("saved", out, flush=True)
        models[name] = ct.models.MLModel(f"{out}/ane.mlpackage",
                                         compute_units=units)
    compare(models, records, args.decisions)


if __name__ == "__main__":
    main()
