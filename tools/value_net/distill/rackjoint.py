"""Fine-tunes a student with a head that predicts the leave the opponent
kept, jointly with its value and spread heads (see rackhead.py for the
frozen-trunk version and the head's definition).

Data: records written by `valuenet:distill ... opp=1 keep=<n>` (<out>.bin.t<k>
and <out>.bin.t<k>.opp2). Each step takes --decisions decisions and --rows
of their recorded candidate rows, the played one first. Every row trains the
value and spread heads on the teacher's labels (the distillation loss of
train.py); the played row also trains the rack head, whose input is the
trunk's hidden vector and, with --side, the opponent's last move's tiles,
score and kind (known to a sim from the game). With --freeze-trunk only the
rack head learns.

Held-out games (game_id mod --val-mod) report the rack loss against the
uniform-draw baseline, and the value and spread error and the regret among
each decision's rows against the teacher, so drift of the value heads from
the starting student shows.

    python rackjoint.py --student runs/t6-so --data "opp500k/*.bin.t*" \
        --out runs/t6-so-rackjoint --steps 10000
"""

import argparse
import glob
import json
import os
import queue
import threading
import time

import mlx.core as mx
import mlx.optimizers as optim
import numpy as np
from mlx.utils import tree_flatten, tree_map

from data import open_record_file
from train import (MAX_SPREAD_OUTPUT, _linear, decision_index, forward, save,
                   teacher_params, unpack, utility)

OPP2 = np.dtype([("game_id", "<u4"), ("turn", "<u2"), ("flags", "<u2"),
                 ("rack", "u1", 28), ("leave", "u1", 28),
                 ("last_score", "<i2"), ("last_tiles", "<u2"),
                 ("played", "u1", 28)])
LETTERS = 27
MOVED, BINGO, EXCHANGE, PASS = 1, 2, 4, 8
SIDE = LETTERS + 4


class Data:
    """Every file's decisions: where its rows start, how many, which row was
    played, and the opponent record."""

    def __init__(self, pattern, val_mod, partial=False):
        self.parts, self.starts, self.counts, self.played = [], [], [], []
        self.paths = []
        self.opp, self.group = [], []
        for path in sorted(p for p in glob.glob(pattern)
                           if not p.endswith((".opp2", ".inf", ".inft", ".ninf", ".npy"))):
            part = open_record_file(path)
            try:
                idx = decision_index(part)
            except ValueError:
                # keep=1: one row per decision, which need not be rank 0.
                if not (np.asarray(part["candidates"]) == 1).all():
                    raise
                idx = np.stack([np.arange(len(part)), np.ones(len(part), int),
                                np.asarray(part["game_id"]).astype(int)],
                               axis=1)
            opp = np.fromfile(path + ".opp2", dtype=OPP2)
            if partial:
                # Files still being written: the decisions in both.
                common = min(len(idx), len(opp))
                idx, opp = idx[:common], opp[:common]
            assert len(idx) == len(opp), (path, len(idx), len(opp))
            assert (np.asarray(part["game_id"])[idx[:, 0]]
                    == opp["game_id"]).all()
            self.parts.append(part)
            self.paths.append(path)
            self.starts.append(idx[:, 0])
            self.counts.append(idx[:, 1])
            self.played.append(played_offsets(part, idx, path))
            self.opp.append(opp)
            self.group.append(1 if "/ane." in path else 0)
        refs = []
        for p, opp in enumerate(self.opp):
            usable = ((opp["flags"] & MOVED) > 0) & \
                ((opp["flags"] & (BINGO | PASS)) == 0)
            for d in range(len(opp)):
                refs.append((p, d, int(opp["game_id"][d] % val_mod == 0),
                             int(usable[d])))
        refs = np.array(refs, np.int64)
        self.train = refs[refs[:, 2] == 0]
        self.val = refs[refs[:, 2] == 1]
        print(f"{len(self.train)} train / {len(self.val)} val decisions "
              f"({int(self.train[:, 3].sum())} / {int(self.val[:, 3].sum())} "
              f"with a leave)", flush=True)

    def batch(self, refs, rows, rng):
        """Rows of the decisions refs: for each, the played row then rows - 1
        others at random. Returns arrays (decisions * rows, ...) with each
        decision's rows together, and the decisions' opponent records."""
        picks = []
        opps = []
        for p, d, _, _ in refs:
            start, count = self.starts[p][d], self.counts[p][d]
            played = self.played[p][d]
            others = np.delete(np.arange(count), played)
            take = np.concatenate([[played], rng.choice(
                others, min(rows - 1, len(others)), replace=False)])
            picks.append((p, start + take))
            opps.append(self.opp[p][d])
        out = {"bits": [], "scalars": [], "value": [], "spread": [],
               "after": [], "bag": []}
        for p, rows_idx in picks:
            r = self.parts[p][rows_idx]
            out["bits"].append(r["board_bits"])
            out["scalars"].append(r["scalars"])
            out["value"].append(r["value"])
            out["spread"].append(r["spread"])
            out["after"].append(r["spread_after"])
            out["bag"].append(r["bag"])
        out = {k: np.concatenate(v) for k, v in out.items()}
        return out, np.array(opps, OPP2)


def played_offsets(part, idx, path):
    """Offset of the played row within each decision, cached."""
    cache = path + ".played.npy"
    if os.path.exists(cache) and \
            os.path.getmtime(cache) >= os.path.getmtime(path):
        return np.load(cache)
    chosen = np.asarray(part["chosen"])
    offsets = np.zeros(len(idx), np.int64)
    for d, (start, count) in enumerate(idx[:, :2]):
        offsets[d] = int(np.argmax(chosen[start:start + count]))
    np.save(cache, offsets)
    return offsets


def side_features(opp):
    """(N, SIDE): the opponent's last move's tiles played / 7, score / 100,
    and whether it was an exchange, a bingo or a pass."""
    return np.concatenate([
        opp["played"][:, :LETTERS].astype(np.float32) / 7.0,
        (opp["last_score"].astype(np.float32) / 100.0)[:, None],
        ((opp["flags"] & EXCHANGE) > 0).astype(np.float32)[:, None],
        ((opp["flags"] & BINGO) > 0).astype(np.float32)[:, None],
        ((opp["flags"] & MOVED) == 0).astype(np.float32)[:, None]], axis=1)


def init_head(kind, d_in, units, seed):
    rng = np.random.default_rng(seed)
    if kind == "linear":
        return {"out": {"weight": mx.zeros((LETTERS, d_in)),
                        "bias": mx.zeros((LETTERS,))}}
    return {"fc": {"weight": mx.array(rng.normal(
        0, 1 / np.sqrt(d_in), (units, d_in)).astype(np.float32)),
        "bias": mx.zeros((units,))},
        "out": {"weight": mx.zeros((LETTERS, units)),
                "bias": mx.zeros((LETTERS,))}}


def head_logits(head, x):
    if "fc" in head:
        x = mx.maximum(x @ head["fc"]["weight"].T + head["fc"]["bias"], 0)
    return x @ head["out"]["weight"].T + head["out"]["bias"]


def rack_log_probs(theta, unseen):
    logits = theta + mx.where(unseen > 0, mx.log(mx.maximum(unseen, 1)), -1e9)
    return logits - mx.logsumexp(logits, axis=-1, keepdims=True)


def outputs(trunk, bits, scalars, heads):
    """hidden, value, spread logit."""
    hidden = forward(trunk, unpack(bits), scalars, heads, return_hidden=True)
    p = trunk["heads"]
    probs = mx.softmax(_linear(p["wdl"], hidden), axis=-1)
    return hidden, probs[:, 2] - probs[:, 0], _linear(p["spread"], hidden)[:, 0]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--student", required=True)
    parser.add_argument("--data", required=True)
    parser.add_argument("--out", required=True)
    parser.add_argument("--kind", choices=["linear", "mlp"], default="mlp")
    parser.add_argument("--units", type=int, default=256)
    parser.add_argument("--side", action=argparse.BooleanOptionalAction,
                        default=True)
    parser.add_argument("--freeze-trunk", action="store_true")
    parser.add_argument("--steps", type=int, default=10000)
    parser.add_argument("--decisions", type=int, default=256)
    parser.add_argument("--rows", type=int, default=4)
    parser.add_argument("--lr-trunk", type=float, default=2e-4)
    parser.add_argument("--lr-head", type=float, default=2e-3)
    parser.add_argument("--warmup", type=int, default=200)
    parser.add_argument("--weight-decay", type=float, default=0.01)
    parser.add_argument("--w-value", type=float, default=1.0)
    parser.add_argument("--w-centered", type=float, default=5.0)
    parser.add_argument("--w-spread", type=float, default=0.5)
    parser.add_argument("--w-rack", type=float, default=0.1)
    parser.add_argument("--val-mod", type=int, default=20)
    parser.add_argument("--val-decisions", type=int, default=3000)
    parser.add_argument("--eval-every", type=int, default=1000)
    parser.add_argument("--seed", type=int, default=0)
    args = parser.parse_args()
    os.makedirs(args.out, exist_ok=True)
    data = Data(args.data, args.val_mod)
    layers = json.load(open(f"{args.student}/manifest.json"))["hparams"]["layers"]
    trunk, hparams = teacher_params(args.student, list(range(layers)))
    heads = hparams["heads"]
    d_in = hparams["hidden"] + (SIDE if args.side else 0)
    head = init_head(args.kind, d_in, args.units, args.seed)
    params = {"trunk": trunk, "head": head}
    rows = args.rows

    def loss_fn(params, bits, scalars, value_t, spread_t, unseen, leave,
                has_leave, side):
        hidden, value, logit = outputs(params["trunk"], bits, scalars, heads)
        shape = (args.decisions, rows)
        value_g, target_g = value.reshape(shape), value_t.reshape(shape)
        centered = ((value_g - value_g.mean(axis=1, keepdims=True))
                    - (target_g - target_g.mean(axis=1, keepdims=True)))
        distill = (args.w_value * mx.mean((value - value_t) ** 2)
                   + args.w_centered * mx.mean(centered ** 2)
                   + args.w_spread * mx.mean((mx.tanh(logit) - spread_t) ** 2))
        # The rack head reads the played row (the first of each decision).
        x = hidden.reshape(args.decisions, rows, -1)[:, 0]
        if args.freeze_trunk:
            x = mx.stop_gradient(x)
        if args.side:
            x = mx.concatenate([x, side], axis=1)
        log_p = rack_log_probs(head_logits(params["head"], x), unseen)
        tiles = (leave * has_leave[:, None]).sum()
        rack = -((leave * has_leave[:, None]) * log_p).sum() \
            / mx.maximum(tiles, 1.0)
        return distill + args.w_rack * rack, (distill, rack)

    grad_fn = mx.value_and_grad(loss_fn)
    opt_trunk = optim.AdamW(learning_rate=args.lr_trunk,
                            weight_decay=args.weight_decay)
    opt_head = optim.AdamW(learning_rate=args.lr_head,
                           weight_decay=args.weight_decay)

    def arrays(batch, opps):
        unseen = (batch["scalars"].reshape(len(opps), rows, -1)[:, 0, 27:54]
                  .astype(np.float64)
                  * (batch["bag"].reshape(len(opps), rows)[:, 0] + 7)[:, None])
        has_leave = (((opps["flags"] & MOVED) > 0)
                     & ((opps["flags"] & (BINGO | PASS)) == 0))
        return (mx.array(batch["bits"]), mx.array(batch["scalars"]),
                mx.array(batch["value"]), mx.array(batch["spread"]),
                mx.array(np.round(unseen).astype(np.float32)),
                mx.array(opps["leave"][:, :LETTERS].astype(np.float32)),
                mx.array(has_leave.astype(np.float32)),
                mx.array(side_features(opps)))

    batches = queue.Queue(maxsize=6)
    stop = threading.Event()

    def loader(seed):
        rng = np.random.default_rng(seed)
        while not stop.is_set():
            refs = data.train[rng.integers(0, len(data.train), args.decisions)]
            batches.put(data.batch(refs, rows, rng))

    for k in range(3):
        threading.Thread(target=loader, args=(args.seed + k,),
                         daemon=True).start()

    val_rng = np.random.default_rng(1234)
    val_refs = data.val[val_rng.choice(len(data.val), min(
        args.val_decisions, len(data.val)), replace=False)]

    def evaluate(params):
        """Rack loss against the uniform baseline, and value/spread error and
        regret over each decision's recorded rows."""
        nll_m = nll_b = tiles = 0.0
        mse_v = mse_s = 0.0
        regret = []
        n_rows = 0
        agree = []
        chunk = min(128, len(val_refs))
        for lo in range(0, len(val_refs) - chunk + 1, chunk):
            refs = val_refs[lo:lo + chunk]
            rng = np.random.default_rng(lo)
            batch, opps = data.batch(refs, 8, rng)  # up to 8 rows (keep=8)
            n = len(refs)
            r = len(batch["value"]) // n
            hidden, value, logit = outputs(params["trunk"],
                                           mx.array(batch["bits"]),
                                           mx.array(batch["scalars"]), heads)
            value, logit = np.array(value), np.array(logit)
            mse_v += float(((value - batch["value"]) ** 2).sum())
            mse_s += float(((np.tanh(logit) - batch["spread"]) ** 2).sum())
            n_rows += len(value)
            teacher_u = utility(batch["value"], batch["spread"],
                                batch["after"]).reshape(n, r)
            student_u = utility(value, np.tanh(logit),
                                batch["after"]).reshape(n, r)
            pick = student_u.argmax(1)
            regret.append(teacher_u.max(1) - teacher_u[np.arange(n), pick])
            agree.append(pick == teacher_u.argmax(1))
            x = np.array(hidden).reshape(n, r, -1)[:, 0]
            if args.side:
                x = np.concatenate([x, side_features(opps)], axis=1)
            theta = head_logits(params["head"], mx.array(x))
            unseen = (batch["scalars"].reshape(n, r, -1)[:, 0, 27:54]
                      .astype(np.float64)
                      * (batch["bag"].reshape(n, r)[:, 0] + 7)[:, None])
            unseen = mx.array(np.round(unseen).astype(np.float32))
            leave = opps["leave"][:, :LETTERS].astype(np.float32)
            ok = (((opps["flags"] & MOVED) > 0)
                  & ((opps["flags"] & (BINGO | PASS)) == 0))
            leave = leave * ok[:, None]
            lp_m = np.array(rack_log_probs(theta, unseen))
            lp_b = np.array(rack_log_probs(mx.zeros_like(theta), unseen))
            nll_m += float(-(leave * lp_m).sum())
            nll_b += float(-(leave * lp_b).sum())
            tiles += float(leave.sum())
        regret = np.concatenate(regret)
        return {"rack_baseline": nll_b / tiles, "rack_model": nll_m / tiles,
                "rack_gain_pct": 100 * (nll_b - nll_m) / nll_b,
                "value_mse": mse_v / n_rows, "spread_mse": mse_s / n_rows,
                "regret": float(regret.mean()),
                "top_agree": float(np.concatenate(agree).mean())}

    log = open(f"{args.out}/log.jsonl", "a")
    metrics = evaluate(params)
    metrics.update(step=0)
    print(json.dumps(metrics), flush=True)
    log.write(json.dumps(metrics) + "\n")
    start = time.time()
    loss_sum = rack_sum = 0.0
    for step in range(args.steps):
        scale = min((step + 1) / args.warmup,
                    0.5 * (1 + np.cos(np.pi * step / args.steps)))
        opt_trunk.learning_rate = args.lr_trunk * scale
        opt_head.learning_rate = args.lr_head * scale
        batch, opps = batches.get()
        (loss, (distill, rack)), grads = grad_fn(params, *arrays(batch, opps))
        if not args.freeze_trunk:
            params["trunk"] = opt_trunk.apply_gradients(grads["trunk"],
                                                        params["trunk"])
        params["head"] = opt_head.apply_gradients(grads["head"],
                                                  params["head"])
        mx.eval(params, opt_trunk.state, opt_head.state)
        loss_sum += float(distill)
        rack_sum += float(rack)
        if (step + 1) % 100 == 0:
            print(f"step {step + 1} distill {loss_sum / 100:.5f} rack "
                  f"{rack_sum / 100:.4f} ({(step + 1) / (time.time() - start):.2f}"
                  f" steps/s)", flush=True)
            loss_sum = rack_sum = 0.0
        if (step + 1) % args.eval_every == 0 or step + 1 == args.steps:
            metrics = evaluate(params)
            metrics.update(step=step + 1)
            print(json.dumps(metrics), flush=True)
            log.write(json.dumps(metrics) + "\n")
            log.flush()
    stop.set()
    save(params["trunk"], hparams, f"{args.out}/student",
         {"rackjoint": {"side": args.side, "kind": args.kind}})
    np.savez(f"{args.out}/head.npz",
             **{k.replace(".", "/"): np.array(v)
                for k, v in tree_flatten(params["head"])})
    json.dump({"args": vars(args), "final": metrics},
              open(f"{args.out}/summary.json", "w"), indent=1)


if __name__ == "__main__":
    main()
