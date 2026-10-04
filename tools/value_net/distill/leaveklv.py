"""Does the kept leave's value (KLV) sharpen the opponent-leave head, as
Macondo found for its inference of long leaves? For decisions with a leave,
models of the leave L the opponent kept (k tiles drawn without replacement
from the tiles unseen to the mover, n_l of letter l):

  head          P(L) ~ prod C(u_l, n_l) exp(theta_l n_l)   (Fisher's
                noncentral multivariate hypergeometric, theta from the head)
  klv           P(L) ~ prod C(u_l, n_l) exp(beta KLV(L))
  head + klv    P(L) ~ prod C(u_l, n_l) exp(alpha theta.n + beta KLV(L))

KLV does not factor over letters, so the normalizer of the klv models is
estimated by importance sampling: leaves drawn exactly from the head's
distribution (alpha = 1, beta = 0) or from the uniform draw (theta = 0),
reweighted for each (alpha, beta), the same draws for every grid point. A
leave's KLV is found by a random 64-bit hash of its counts. alpha and beta
are fitted per kept-leave size on even games and scored on odd games and
the reverse (two-fold, by game), so the gains are out of sample. Scores are
the log probability of the actual leave as a multiset, as gains over the
exact uniform draw, in nats and bits per decision.

    python leaveklv.py --data "ceiling/c.bin.t*" --klv data/lexica/NWL23.csv \\
        --student rack/D_joint_gentle/student --head rack/D_joint_gentle
"""

import argparse
import json
from math import lgamma, log

import mlx.core as mx
import numpy as np

from leaveceiling import load_head
from rackjoint import (BINGO, EXCHANGE, LETTERS, MOVED, PASS, Data,
                       head_logits, side_features)
from train import forward, teacher_params, unpack

NAMES = "?ABCDEFGHIJKLMNOPQRSTUVWXYZ"
ALPHAS = np.round(np.arange(0.0, 1.51, 0.1), 2)
BETAS = np.round(np.arange(0.0, 0.401, 0.01), 3)
LGAMMA = np.vectorize(lgamma)


def log_choose(n, k):
    return LGAMMA(n + 1.0) - LGAMMA(k + 1.0) - LGAMMA(n - k + 1.0)


def klv_table(path, zobrist):
    """Sorted hashes of every leave in the KLV CSV and their values."""
    hashes, values = [np.uint64(0)], [0.0]
    for line in open(path):
        leave, value = line.rsplit(",", 1)
        h = np.uint64(0)
        for ch in leave:
            h = h + zobrist[NAMES.index(ch)]
        hashes.append(h)
        values.append(float(value))
    hashes = np.array(hashes, dtype=np.uint64)
    values = np.array(values)
    order = np.argsort(hashes)
    assert len(np.unique(hashes)) == len(hashes), "hash collision"
    return hashes[order], values[order]


def lookup(hashes, values, counts, zobrist):
    """KLV of each row of counts (n x LETTERS)."""
    with np.errstate(over="ignore"):
        h = (counts.astype(np.uint64) * zobrist[None, :]).sum(
            1, dtype=np.uint64)
    idx = np.searchsorted(hashes, h)
    idx = np.minimum(idx, len(hashes) - 1)
    ok = hashes[idx] == h
    assert ok.all(), f"{(~ok).sum()} leaves missing from the KLV table"
    return values[idx]


def fisher_tables(unseen, theta, k):
    """weight[l][n] = C(u_l, n) exp(theta_l n) and suffix[l][r] = total
    weight of taking r tiles from letters l onward; log of the total."""
    weight = np.zeros((LETTERS, k + 1))
    for l in range(LETTERS):
        top = min(int(unseen[l]), k)
        n = np.arange(top + 1)
        weight[l, :top + 1] = np.exp(log_choose(unseen[l], n) + theta[l] * n)
    suffix = np.zeros((LETTERS + 1, k + 1))
    suffix[LETTERS, 0] = 1.0
    for l in range(LETTERS - 1, -1, -1):
        for r in range(k + 1):
            suffix[l, r] = (weight[l, :r + 1] * suffix[l + 1, r::-1]).sum()
    return weight, suffix


def fisher_sample(weight, suffix, k, n_samples, rng):
    """n_samples leaves (counts) drawn exactly from the Fisher distribution."""
    counts = np.zeros((n_samples, LETTERS), dtype=np.int64)
    remaining = np.full(n_samples, k)
    for l in range(LETTERS):
        if not remaining.any():
            break
        # P(n | r) = weight[l][n] suffix[l+1][r-n] / suffix[l][r].
        n = np.arange(k + 1)
        r = remaining[:, None]
        valid = n[None, :] <= r
        tail = suffix[l + 1][np.clip(r - n[None, :], 0, k)]
        p = np.where(valid, weight[l][None, :] * tail, 0.0)
        p /= p.sum(1, keepdims=True)
        u = rng.random(n_samples)[:, None]
        pick = (np.cumsum(p, 1) < u).sum(1)
        counts[:, l] = pick
        remaining -= pick
    return counts


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--data", required=True)
    parser.add_argument("--klv", required=True)
    parser.add_argument("--student", required=True)
    parser.add_argument("--head", required=True)
    parser.add_argument("--samples", type=int, default=2000)
    parser.add_argument("--max", type=int, default=0)
    args = parser.parse_args()
    rng = np.random.default_rng(0)
    zobrist = rng.integers(1, 2 ** 63, LETTERS, dtype=np.uint64) * 2 + 1
    hashes, values = klv_table(args.klv, zobrist)
    data = Data(args.data, 10 ** 9)
    rows = []
    for p, part in enumerate(data.parts):
        for d in range(len(data.opp[p])):
            opp = data.opp[p][d]
            k = int(opp["leave"][:LETTERS].sum())
            if (opp["flags"] & MOVED) and not (opp["flags"] & (BINGO | PASS)) \
                    and k > 0:
                rows.append((p, d))
    if args.max:
        rows = rows[:args.max]
    n = len(rows)
    print(f"{n} decisions with a leave", flush=True)
    unseen = np.zeros((n, LETTERS))
    leave = np.zeros((n, LETTERS), dtype=np.int64)
    bag = np.zeros(n)
    game = np.zeros(n, dtype=np.int64)
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
        game[i] = data.opp[p][d]["game_id"]
    bits, scalars = np.stack(bits), np.stack(scalars)
    head, side = load_head(args.head)
    layers = json.load(open(f"{args.student}/manifest.json"))["hparams"][
        "layers"]
    trunk, hp = teacher_params(args.student, list(range(layers)))
    theta = np.zeros((n, LETTERS))
    for lo in range(0, n, 256):
        sl = slice(lo, lo + 256)
        hidden = np.array(forward(trunk, unpack(mx.array(bits[sl])),
                                  mx.array(scalars[sl]), hp["heads"],
                                  return_hidden=True))
        x = np.concatenate([hidden, side_features(opps[sl])], axis=1) \
            if side else hidden
        theta[sl] = np.array(head_logits(head, mx.array(x)))
    print("head odds computed", flush=True)
    k_all = leave.sum(1)
    total = unseen.sum(1)
    exch = (opps["flags"] & EXCHANGE) > 0
    log_hyper = log_choose(unseen, leave).sum(1)
    log_exact = log_hyper - log_choose(total, k_all)
    actual_klv = lookup(hashes, values, leave, zobrist)
    # log P(actual) for each (alpha, beta), under the head proposal, and for
    # each beta under the uniform proposal (alpha = 0).
    head_grid = np.zeros((n, len(ALPHAS), len(BETAS)))
    for i in range(n):
        k = int(k_all[i])
        w1, s1 = fisher_tables(unseen[i], theta[i], k)
        log_z_head = log(s1[0, k])
        draws = fisher_sample(w1, s1, k, args.samples, rng)
        draw_klv = lookup(hashes, values, draws, zobrist)
        draw_tn = draws @ theta[i]
        tn = float(leave[i] @ theta[i])
        # log Z(alpha, beta) = log Z_head + log mean exp((alpha-1) theta.n +
        # beta KLV) over the head's draws.
        expo = ((ALPHAS[:, None, None] - 1.0) * draw_tn[None, None, :]
                + BETAS[None, :, None] * draw_klv[None, None, :])
        top = expo.max(2, keepdims=True)
        log_z = log_z_head + top[:, :, 0] + np.log(
            np.exp(expo - top).mean(2))
        head_grid[i] = (log_hyper[i] + ALPHAS[:, None] * tn
                        + BETAS[None, :] * actual_klv[i] - log_z)
        if i % 1000 == 0:
            print(f"  {i}/{n}", flush=True)
    # The head alone, exact (alpha = 1, beta = 0 has log_z = log_z_head).
    a1 = int(np.where(ALPHAS == 1.0)[0][0])
    b0 = int(np.where(BETAS == 0.0)[0][0])
    head_exact = head_grid[:, a1, b0]
    groups = [(f"keep {s}", (k_all == s) & ~exch) for s in range(1, 7)]
    groups.append(("exchange", exch))
    folds = (game % 2).astype(bool)
    print(f"\ngain over the exact uniform draw, per decision, out of sample "
          f"(alpha, beta fitted on the other half of the games)")
    print(f"{'group':10s} {'n':>6s}  {'head':>14s}  {'klv only':>14s} "
          f"{'beta':>5s}  {'head + klv':>14s} {'alpha':>5s} {'beta':>5s}")
    for label, sel in groups:
        if sel.sum() < 20:
            continue
        gains = {"head": head_exact[sel] - log_exact[sel]}
        oos_klv = np.zeros(int(sel.sum()))
        oos_both = np.zeros(int(sel.sum()))
        idx = np.where(sel)[0]
        chosen_klv, chosen_both = [], []
        for fold in (False, True):
            train = idx[folds[idx] != fold]
            test_pos = np.where(folds[idx] == fold)[0]
            test = idx[test_pos]
            # klv only: alpha 0.
            a0 = int(np.where(ALPHAS == 0.0)[0][0])
            b = int(np.argmax(head_grid[train, a0, :].mean(0)))
            oos_klv[test_pos] = head_grid[test, a0, b] - log_exact[test]
            chosen_klv.append(BETAS[b])
            ab = head_grid[train].mean(0)
            a, b2 = np.unravel_index(np.argmax(ab), ab.shape)
            oos_both[test_pos] = head_grid[test, a, b2] - log_exact[test]
            chosen_both.append((ALPHAS[a], BETAS[b2]))
        gains["klv"] = oos_klv
        gains["both"] = oos_both

        def fmt(x):
            return f"{x.mean():+.3f}/{x.mean() / log(2):+.2f}b"
        print(f"{label:10s} {int(sel.sum()):6d}  {fmt(gains['head']):>14s}  "
              f"{fmt(gains['klv']):>14s} {np.mean(chosen_klv):5.2f}  "
              f"{fmt(gains['both']):>14s} "
              f"{np.mean([c[0] for c in chosen_both]):5.2f} "
              f"{np.mean([c[1] for c in chosen_both]):5.2f}")
    print("\n(nats/bits per decision; beta per KLV point; alpha multiplies the "
          "head's log odds)")


if __name__ == "__main__":
    main()
