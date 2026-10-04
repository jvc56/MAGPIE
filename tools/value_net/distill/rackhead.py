"""A head that predicts the leave the opponent kept from their last move,
trained on a frozen student's hidden vector.

Data: records written by `valuenet:distill ... opp=1` (<out>.bin.t<k> and
<out>.bin.t<k>.opp). Each decision has one opponent record (the opponent's
rack and kept leave, joined to the records by order of each decision's
first row); the decision's chosen candidate row is the net's input.

Model: p_l is proportional to u_l * exp(theta_l(x)) over letters l (the
blank is 0, then A-Z), where u_l is the count of letter l unseen to the
mover before its move (scalars[27:54] times bag + 7, exact) and theta is a
linear (or MLP) head on the hidden vector, zero-initialised so it starts as
the uniform draw from the unseen pool. The leave's tiles are modelled as
independent draws from p (the multinomial approximation to drawing without
replacement); the loss is -sum_l n_l log p_l over leave tiles, so the
baseline (theta = 0) is the cost of guessing the leave at random from the
unseen tiles. exp(theta_l) are the odds multipliers a sampler applies to
the unseen counts. Decisions where the opponent has not moved, passed or
bingoed (no leave) are left out.

    python rackhead.py --student runs/t6-so --data "opp5m/*.bin.t*" \
        --out runs/t6-so-rack
"""

import argparse
import glob
import json
import os
import time

import mlx.core as mx
import mlx.optimizers as optim
import numpy as np
from mlx.utils import tree_flatten, tree_map

from data import open_record_file, unpack_board  # noqa: F401
from train import forward, teacher_params, unpack

OPP = np.dtype([("game_id", "<u4"), ("turn", "<u2"), ("flags", "<u2"),
                ("rack", "u1", 28), ("leave", "u1", 28),
                ("last_score", "<i2"), ("last_tiles", "<u2")])
LETTERS = 27
MOVED, BINGO, EXCHANGE, PASS = 1, 2, 4, 8
LETTER_NAMES = "?ABCDEFGHIJKLMNOPQRSTUVWXYZ"


def features(student_dir, files, cache, batch=256):
    """Hidden vectors and labels of every decision's chosen row."""
    if os.path.exists(cache):
        cached = np.load(cache)
        return {k: cached[k] for k in cached.files}
    layers = json.load(open(f"{student_dir}/manifest.json"))["hparams"]["layers"]
    params, hparams = teacher_params(student_dir, list(range(layers)))
    out = {k: [] for k in ["hidden", "unseen", "leave", "rack", "flags", "bag",
                           "group", "game", "last_score"]}
    start = time.time()
    for path in files:
        records = open_record_file(path)
        opp = np.fromfile(path + ".opp", dtype=OPP)
        chosen = np.flatnonzero(np.asarray(records["chosen"]) == 1)
        firsts = np.flatnonzero(np.asarray(records["candidate"]) == 0)
        assert len(chosen) == len(firsts) == len(opp), (path, len(chosen),
                                                        len(firsts), len(opp))
        assert (np.asarray(records["game_id"])[firsts] == opp["game_id"]).all()
        assert (np.asarray(records["turn"])[firsts] == opp["turn"]).all()
        group = 1 if "/ane." in path else 0
        for lo in range(0, len(chosen), batch):
            rows = records[chosen[lo:lo + batch]]
            hidden = forward(params, unpack(mx.array(rows["board_bits"])),
                             mx.array(rows["scalars"]), hparams["heads"],
                             return_hidden=True)
            mx.eval(hidden)
            out["hidden"].append(np.array(hidden))
            bag = rows["bag"].astype(np.int64)
            unseen = rows["scalars"][:, 27:54].astype(np.float64) \
                * (bag + 7)[:, None]
            assert np.abs(unseen - np.round(unseen)).max() < 0.05
            out["unseen"].append(np.round(unseen).astype(np.int16))
            out["bag"].append(bag.astype(np.int16))
        out["leave"].append(opp["leave"][:, :LETTERS])
        out["rack"].append(opp["rack"][:, :LETTERS])
        out["flags"].append(opp["flags"])
        out["group"].append(np.full(len(opp), group, np.int8))
        out["game"].append(opp["game_id"])
        out["last_score"].append(opp["last_score"])
        print(f"{path}: {len(opp)} decisions "
              f"({time.time() - start:.0f}s)", flush=True)
    out = {k: np.concatenate(v) for k, v in out.items()}
    np.savez(cache, **out)
    return out


def init_head(kind, d_in, hidden_units, seed):
    rng = np.random.default_rng(seed)
    if kind == "linear":
        return {"out": {"weight": mx.zeros((LETTERS, d_in)),
                        "bias": mx.zeros((LETTERS,))}}
    return {"fc": {"weight": mx.array(rng.normal(0, 1 / np.sqrt(d_in),
                                                 (hidden_units, d_in))
                                      .astype(np.float32)),
                   "bias": mx.zeros((hidden_units,))},
            "out": {"weight": mx.zeros((LETTERS, hidden_units)),
                    "bias": mx.zeros((LETTERS,))}}


def theta(head, x):
    if "fc" in head:
        x = mx.maximum(x @ head["fc"]["weight"].T + head["fc"]["bias"], 0)
    return x @ head["out"]["weight"].T + head["out"]["bias"]


def log_probs(th, unseen):
    """log p_l, p proportional to unseen_l * exp(theta_l); letters with no
    unseen tiles get -inf-like."""
    logits = th + mx.where(unseen > 0, mx.log(mx.maximum(unseen, 1)), -1e9)
    return logits - mx.logsumexp(logits, axis=-1, keepdims=True)


def nll(head, x, unseen, leave):
    lp = log_probs(theta(head, x), unseen)
    return -(leave * lp).sum() / leave.sum()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--student", required=True)
    parser.add_argument("--data", required=True)
    parser.add_argument("--out", required=True)
    parser.add_argument("--cache", default="")
    parser.add_argument("--kind", choices=["linear", "mlp"], default="linear")
    parser.add_argument("--units", type=int, default=256)
    parser.add_argument("--epochs", type=int, default=30)
    parser.add_argument("--batch", type=int, default=2048)
    parser.add_argument("--lr", type=float, default=2e-3)
    parser.add_argument("--weight-decay", type=float, default=1e-4)
    parser.add_argument("--val-mod", type=int, default=20)
    parser.add_argument("--extra", action="store_true",
                        help="also feed the opponent's last move's score, "
                        "tiles played and kind (from the opp records)")
    parser.add_argument("--seed", type=int, default=0)
    args = parser.parse_args()
    os.makedirs(args.out, exist_ok=True)
    files = sorted(p for p in glob.glob(args.data)
                   if not p.endswith((".opp", ".npy")))
    cache = args.cache or f"{args.out}/features.npz"
    d = features(args.student, files, cache)
    if args.extra:
        tiles = np.array([0] * len(d["flags"]))
        extra = np.stack([d["last_score"] / 100.0,
                          (d["flags"] & EXCHANGE > 0).astype(np.float32),
                          (d["flags"] & BINGO > 0).astype(np.float32),
                          (d["flags"] & MOVED > 0).astype(np.float32)], axis=1)
        d["hidden"] = np.concatenate([d["hidden"], extra.astype(np.float32)],
                                     axis=1)
    has_leave = ((d["flags"] & MOVED) > 0) & ((d["flags"] & (BINGO | PASS)) == 0)
    val = (d["game"] % args.val_mod == 0)
    train_idx = np.flatnonzero(has_leave & ~val)
    val_idx = np.flatnonzero(has_leave & val)
    print(f"{len(train_idx)} train / {len(val_idx)} val decisions with a leave "
          f"(of {len(has_leave)}; bingos {int(((d['flags'] & BINGO) > 0).sum())})",
          flush=True)

    def tensors(idx):
        return (mx.array(d["hidden"][idx]),
                mx.array(d["unseen"][idx].astype(np.float32)),
                mx.array(d["leave"][idx].astype(np.float32)))

    x_val, u_val, n_val = tensors(val_idx)
    head = init_head(args.kind, d["hidden"].shape[1], args.units, args.seed)
    base = float(nll(tree_map(lambda a: a * 0, head), x_val, u_val, n_val))
    print(f"baseline (uniform draw from the unseen tiles): "
          f"{base:.4f} nats/leave tile", flush=True)
    opt = optim.AdamW(learning_rate=args.lr, weight_decay=args.weight_decay)
    loss_and_grad = mx.value_and_grad(nll)
    rng = np.random.default_rng(args.seed)
    best = (base, None)
    for epoch in range(args.epochs):
        order = rng.permutation(train_idx)
        total = 0.0
        for lo in range(0, len(order), args.batch):
            x, u, n = tensors(order[lo:lo + args.batch])
            loss, grads = loss_and_grad(head, x, u, n)
            opt.update(head, grads)
            mx.eval(head, opt.state)
            total += float(loss) * len(x)
        v = float(nll(head, x_val, u_val, n_val))
        print(f"epoch {epoch + 1}: train {total / len(order):.4f} "
              f"val {v:.4f} ({base - v:+.4f} vs baseline)", flush=True)
        if v < best[0]:
            best = (v, tree_map(lambda a: a + 0, head))
    head = best[1] if best[1] is not None else head
    report(head, d, val_idx, base, best[0], args)


def report(head, d, val_idx, base, final, args):
    """Held-out results by game phase and the head's largest odds."""
    x = mx.array(d["hidden"][val_idx])
    u = mx.array(d["unseen"][val_idx].astype(np.float32))
    n = mx.array(d["leave"][val_idx].astype(np.float32))
    th = np.array(theta(head, x))
    lp_model = np.array(log_probs(mx.array(th), u))
    lp_base = np.array(log_probs(mx.zeros_like(mx.array(th)), u))
    leave = d["leave"][val_idx].astype(np.float64)
    bag = d["bag"][val_idx]
    print(f"\nheld-out: {base:.4f} -> {final:.4f} nats/leave tile "
          f"({100 * (base - final) / base:.2f}% lower)")
    for lo, hi, name in [(60, 999, "bag 60+"), (30, 60, "bag 30-59"),
                         (7, 30, "bag 7-29"), (1, 7, "bag 1-6"), (0, 1, "bag 0")]:
        m = (bag >= lo) & (bag < hi)
        if m.sum() == 0:
            continue
        tiles = leave[m].sum()
        b = -(leave[m] * lp_base[m]).sum() / tiles
        f = -(leave[m] * lp_model[m]).sum() / tiles
        print(f"  {name:10s} n={int(m.sum()):6d} baseline {b:.4f} model {f:.4f} "
              f"({100 * (b - f) / b:+.2f}%)")
    # Mean odds multiplier by letter, over decisions where the letter is unseen.
    pred = np.exp(lp_model)
    expected = pred * leave.sum(1, keepdims=True)
    print("  letter: mean predicted vs actual leave count per decision")
    rows = []
    for l in range(LETTERS):
        rows.append((LETTER_NAMES[l], expected[:, l].mean(), leave[:, l].mean(),
                     float(np.exp(th[:, l]).mean())))
    print("  " + "  ".join(f"{c}:{p:.2f}/{a:.2f}" for c, p, a, _ in rows))
    np.savez(f"{args.out}/head.npz",
             **{k: np.array(v) for k, v in tree_flatten(head)})
    json.dump({"kind": args.kind, "units": args.units, "baseline": base,
               "final": final, "student": args.student},
              open(f"{args.out}/summary.json", "w"), indent=1)


if __name__ == "__main__":
    main()
