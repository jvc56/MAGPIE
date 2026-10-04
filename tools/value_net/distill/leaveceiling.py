"""How well does exact inference (MAGPIE's infer) predict the leave the
opponent kept, against the opponent-leave heads, on the same decisions?

Data: `valuenet:distill ... opp=1 inferq=1 keep=1` (records, .opp2 and .inf
per file). Every model is scored by the natural log of the probability it
gives the actual kept leave as a multiset of tiles, in nats per decision:

  uniform (iid)   the tiles drawn independently from the unseen pool; the
                  head's model class, so the fair baseline for the heads
  uniform (exact) drawn without replacement (the hypergeometric)
  head            a head's odds multipliers applied to the unseen counts,
                  independent tile draws
  inference m     MAGPIE's inferred leaves consistent with the move within
                  m points, weighted by their draw counts, mixed with the
                  exact uniform (weight eps) because the leave the opponent
                  kept is sometimes not among them

    python leaveceiling.py --data "ceiling/c.bin.t*" \
        --head A:runs/t6-so:rack/A_frozen_side \
        --head D:rack/D_joint_gentle/student:rack/D_joint_gentle
"""

import argparse
import glob
import json
from math import lgamma

import mlx.core as mx
import numpy as np

from rackjoint import (BINGO, LETTERS, MOVED, PASS, Data, head_logits,
                       rack_log_probs, side_features)
from train import forward, teacher_params, unpack

INF = np.dtype([("game_id", "<u4"), ("turn", "<u2"), ("flags", "<u2"),
                ("log_p", "<f4", 4), ("leaves", "<u4", 4)])
MARGINS = [0, 10, 25, 60]
LGAMMA = np.vectorize(lgamma)


def log_choose(n, k):
    return LGAMMA(n + 1.0) - LGAMMA(k + 1.0) - LGAMMA(n - k + 1.0)


def fisher_log_p(unseen, theta, leave):
    """Log probability of each decision's leave (a multiset of k tiles drawn
    without replacement from unseen, letter l carrying odds exp(theta_l)):
    prod C(u_l, n_l) exp(theta_l n_l) over the sum of the same over every
    multiset of k tiles (Fisher's noncentral multivariate hypergeometric),
    the sum by a polynomial product over letters."""
    out = np.zeros(len(unseen))
    for i in range(len(unseen)):
        k = int(leave[i].sum())
        poly = np.zeros(k + 1)
        poly[0] = 1.0
        for l in range(LETTERS):
            u = int(unseen[i, l])
            if u == 0:
                continue
            top = min(u, k)
            coef = np.array([np.exp(log_choose(u, n) + theta[i, l] * n)
                             for n in range(top + 1)])
            poly = np.convolve(poly, coef)[:k + 1]
        n = leave[i]
        numerator = (log_choose(unseen[i], n) + theta[i] * n).sum()
        out[i] = numerator - np.log(poly[k])
    return out


def load_head(path):
    head = {}
    for key, value in np.load(f"{path}/head.npz").items():
        a, b = key.split("/")
        head.setdefault(a, {})[b] = mx.array(value)
    return head, json.load(open(f"{path}/summary.json"))["args"]["side"]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--data", required=True)
    parser.add_argument("--head", action="append", default=[],
                        help="name:student_dir:head_dir")
    parser.add_argument("--eps", default="0.01,0.05,0.2")
    args = parser.parse_args()
    data = Data(args.data, 10 ** 9)
    infs = [np.fromfile(path + ".inf", dtype=INF) for path in data.paths]
    rows = []
    for p, part in enumerate(data.parts):
        assert len(infs[p]) == len(data.opp[p]), data.paths[p]
        assert (infs[p]["game_id"] == data.opp[p]["game_id"]).all()
        for d in range(len(data.opp[p])):
            opp = data.opp[p][d]
            k = int(opp["leave"][:LETTERS].sum())
            if (opp["flags"] & MOVED) and not (opp["flags"] & (BINGO | PASS)) \
                    and k > 0:
                rows.append((p, d))
    print(f"{len(rows)} decisions with a leave", flush=True)
    n = len(rows)
    unseen = np.zeros((n, LETTERS))
    leave = np.zeros((n, LETTERS))
    bag = np.zeros(n)
    inf = np.zeros(n, INF)
    opps = np.zeros(n, data.opp[0].dtype)
    bits, scalars = [], []
    for i, (p, d) in enumerate(rows):
        row = data.parts[p][data.starts[p][d] + data.played[p][d]]
        bits.append(row["board_bits"])
        scalars.append(row["scalars"])
        bag[i] = row["bag"]
        unseen[i] = np.round(row["scalars"][27:54].astype(np.float64)
                             * (row["bag"] + 7))
        opps[i] = data.opp[p][d]
        leave[i] = data.opp[p][d]["leave"][:LETTERS]
        inf[i] = infs[p][d]
    bits, scalars = np.stack(bits), np.stack(scalars)
    k = leave.sum(1)
    total = unseen.sum(1)
    # Multiset coefficient and the two uniform baselines.
    log_multi = LGAMMA(k + 1.0) - LGAMMA(leave + 1.0).sum(1)
    p_unif = unseen / total[:, None]
    with np.errstate(divide="ignore"):
        log_unif_iid = log_multi + (leave * np.where(
            leave > 0, np.log(p_unif), 0.0)).sum(1)
    log_unif_exact = (log_choose(unseen, leave).sum(1)
                      - log_choose(total, k))
    results = {"uniform (iid)": log_unif_iid,
               "uniform (exact)": log_unif_exact}
    for spec in args.head:
        name, student, head_dir = spec.split(":", 2)
        head, side = load_head(head_dir)
        layers = json.load(open(f"{student}/manifest.json"))["hparams"]["layers"]
        trunk, hp = teacher_params(student, list(range(layers)))
        probs = np.zeros((n, LETTERS))
        thetas = np.zeros((n, LETTERS))
        for lo in range(0, n, 256):
            sl = slice(lo, lo + 256)
            hidden = np.array(forward(trunk, unpack(mx.array(bits[sl])),
                                      mx.array(scalars[sl]), hp["heads"],
                                      return_hidden=True))
            x = np.concatenate([hidden, side_features(opps[sl])], axis=1) \
                if side else hidden
            theta = head_logits(head, mx.array(x))
            thetas[sl] = np.array(theta)
            probs[sl] = np.exp(np.array(rack_log_probs(
                theta, mx.array(unseen[sl].astype(np.float32)))))
        results[f"head {name} (iid)"] = log_multi + (leave * np.log(
            np.maximum(probs, 1e-30))).sum(1)
        results[f"head {name} (without replacement)"] = fisher_log_p(
            unseen, thetas, leave)
    eps_list = [float(e) for e in args.eps.split(",")]
    covered = {}
    for m, margin in enumerate(MARGINS):
        ran = ((inf["flags"] >> m) & 1) > 0
        hit = ran & (inf["log_p"][:, m] > -1e29)
        covered[margin] = (ran.mean(), hit.sum() / max(ran.sum(), 1))
        for eps in eps_list:
            p_inf = np.where(hit, np.exp(np.minimum(inf["log_p"][:, m], 0.0)),
                             0.0)
            mixed = np.log((1 - eps) * p_inf + eps * np.exp(log_unif_exact))
            # No inference (nothing to infer or it failed): the exact uniform.
            mixed = np.where(ran, mixed, log_unif_exact)
            results[f"inference m={margin} eps={eps}"] = mixed
    # Inference with the last head (without replacement) as the fallback and
    # as the other half of the mixture, instead of the exact uniform.
    if args.head:
        head_log = results[[k for k in results
                            if k.endswith("(without replacement)")][-1]]
        for m, margin in enumerate(MARGINS):
            if margin not in (0, 10):
                continue
            ran = ((inf["flags"] >> m) & 1) > 0
            hit = ran & (inf["log_p"][:, m] > -1e29)
            p_inf = np.where(hit, np.exp(np.minimum(inf["log_p"][:, m], 0.0)),
                             0.0)
            for eps in eps_list:
                mixed = np.log((1 - eps) * p_inf + eps * np.exp(head_log))
                results[f"inference m={margin} + head, eps={eps}"] = np.where(
                    ran, mixed, head_log)
    base = results["uniform (iid)"]
    print(f"\n{'model':32s} {'nats/decision':>14s} {'vs uniform iid':>15s} "
          f"{'nats/tile':>10s}")
    for name, value in results.items():
        print(f"{name:32s} {-value.mean():14.4f} "
              f"{value.mean() - base.mean():+15.4f} "
              f"{-(value.sum() / k.sum()):10.4f}")
    print("\ninference coverage (ran, actual leave among those inferred):")
    for margin, (ran, hit) in covered.items():
        print(f"  margin {margin:3d}: ran on {ran:.1%} of decisions, covered "
              f"{hit:.1%}")
    # By the size of the leave kept (7 less the tiles played; an exchange is
    # its own row): the gain of each model over the exact uniform, and how
    # many leaves inference enumerated.
    exch = (opps["flags"] & 4) > 0
    exact = results["uniform (exact)"]
    print("\ngain over the exact uniform, nats/decision, by leave kept "
          "(n, leaves enumerated at margin 0 / 10):")
    keys = [k_ for k_ in results if k_.endswith("(without replacement)")
            or k_ == "inference m=10 eps=0.05"
            or k_.startswith("inference m=0 + head, eps=0.5")]
    print(f"{'group':14s} {'n':>6s} {'leaves0':>8s} {'leaves10':>9s}  "
          + "  ".join(k_[:26] for k_ in keys))
    groups = [(f"keep {size}", (k == size) & ~exch) for size in range(1, 7)]
    groups.append(("exchange", exch))
    for label, sel in groups:
        if sel.sum() == 0:
            continue
        print(f"{label:14s} {int(sel.sum()):6d} "
              f"{inf['leaves'][sel, 0].mean():8.0f} "
              f"{inf['leaves'][sel, 1].mean():9.0f}  " + "  ".join(
                  f"{(results[k_][sel] - exact[sel]).mean():+26.3f}"
                  for k_ in keys))
    # Subset where inference ran: every model on the same decisions.
    ran = ((inf["flags"] >> 1) & 1) > 0
    print(f"\non the {int(ran.sum())} decisions where inference ran:")
    for name, value in results.items():
        print(f"  {name:32s} {-value[ran].mean():10.4f} nats/decision "
              f"({value[ran].mean() - base[ran].mean():+.4f} vs uniform iid)")
    for lo, hi, label in [(60, 999, "bag 60+"), (30, 60, "bag 30-59"),
                          (7, 30, "bag 7-29"), (0, 7, "bag 0-6")]:
        sel = ran & (bag >= lo) & (bag < hi)
        if sel.sum():
            print(f"  {label} n={int(sel.sum())}: " + "  ".join(
                f"{name.split(' eps')[0][:22]} "
                f"{value[sel].mean() - base[sel].mean():+.3f}"
                for name, value in results.items()
                if "uniform (exact)" in name or "without replacement" in name
                or "m=10 eps=0.05" in name))


if __name__ == "__main__":
    main()
