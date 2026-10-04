"""How well each model predicts the leave the opponent kept, on the same
decisions: static inference (score + leave, and with the PAT term), net
inference (the teacher and the student as the opponent's policy), and the
opponent-leave head. From `valuenet:distill ... opp=1 inferq=1 keep=1`
records (.opp2, .inf, .ninf) and a model with a head.

Each model is scored by the natural log of the probability it gives the
actual leave (as a multiset), as the gain in nats per decision over drawing
the leave at random from the unseen tiles. Models that can give it zero
probability (static inference) are mixed with that random draw at weight
--eps, as a sim would.

    python leavemodels.py --data "leavemodels/l.bin.t*" \\
        --head-model runs/t6-so-rack --head rack/A_frozen_side
"""

import argparse
import json
from math import lgamma

import mlx.core as mx
import numpy as np

from leaveceiling import fisher_log_p, load_head, log_choose
from rackjoint import BINGO, LETTERS, MOVED, PASS, Data, head_logits, side_features
from train import forward, teacher_params, unpack

INF = np.dtype([("game_id", "<u4"), ("turn", "<u2"), ("flags", "<u2"),
                ("log_p", "<f4", 4), ("leaves", "<u4", 4)])
NINF = np.dtype([("game_id", "<u4"), ("turn", "<u2"), ("flags", "<u2"),
                 ("log_p", "<f4", 8), ("leaves", "<u4"), ("rows", "<u4"),
                 ("micros", "<u4")])
STATIC = ["score+leave m0", "score+leave m10", "PAT m0", "PAT m10"]
STATIC_SLOT = [2, 3, 0, 1]  # .inf slots: PAT m0, PAT m10, plain m0, plain m10
NET = ["teacher PAT t0.001", "teacher PAT t0.003", "teacher PAT t0.01",
       "teacher no-PAT t0.003", "student PAT t0.001", "student PAT t0.003",
       "student PAT t0.01"]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--data", required=True)
    parser.add_argument("--head-model", required=True)
    parser.add_argument("--head", required=True)
    parser.add_argument("--eps", type=float, default=0.05)
    parser.add_argument("--partial", action="store_true",
                        help="files still being written: their common decisions")
    args = parser.parse_args()
    data = Data(args.data, 10 ** 9, partial=args.partial)
    rows = []
    for p in range(len(data.parts)):
        inf = np.fromfile(data.paths[p] + ".inf", dtype=INF)
        ninf = np.fromfile(data.paths[p] + ".ninf", dtype=NINF)
        if args.partial:
            common = min(len(inf), len(ninf), len(data.opp[p]))
            inf, ninf = inf[:common], ninf[:common]
            data.opp[p] = data.opp[p][:common]
        assert len(inf) == len(ninf) == len(data.opp[p]), data.paths[p]
        assert (ninf["game_id"] == data.opp[p]["game_id"]).all()
        for d in range(len(data.opp[p])):
            opp = data.opp[p][d]
            k = int(opp["leave"][:LETTERS].sum())
            if (opp["flags"] & MOVED) and not (opp["flags"] & (BINGO | PASS)) \
                    and not (opp["flags"] & 4) and 1 <= k:
                rows.append((p, d, inf[d], ninf[d], k))
    n = len(rows)
    print(f"{n} tile-placement decisions with a leave")
    leave = np.zeros((n, LETTERS)); unseen = np.zeros((n, LETTERS))
    k = np.zeros(n, int); bag = np.zeros(n)
    inf = np.zeros(n, INF); ninf = np.zeros(n, NINF)
    opps = np.zeros(n, data.opp[0].dtype); bits = []; scalars = []
    for i, (p, d, ir, nr, kk) in enumerate(rows):
        r = data.parts[p][data.starts[p][d] + data.played[p][d]]
        bits.append(r["board_bits"]); scalars.append(r["scalars"])
        unseen[i] = np.round(r["scalars"][27:54].astype(np.float64) * (r["bag"] + 7))
        leave[i] = data.opp[p][d]["leave"][:LETTERS]; opps[i] = data.opp[p][d]
        inf[i] = ir; ninf[i] = nr; k[i] = kk; bag[i] = r["bag"]
    bits = np.stack(bits); scalars = np.stack(scalars)
    log_uniform = (log_choose(unseen, leave).sum(1)
                   - log_choose(unseen.sum(1), k))
    uniform = np.exp(log_uniform)
    scores = {}
    for name, slot in zip(STATIC, STATIC_SLOT):
        ran = ((inf["flags"] >> slot) & 1) > 0
        hit = ran & (inf["log_p"][:, slot] > -1e29)
        p_model = np.where(hit, np.exp(np.minimum(inf["log_p"][:, slot], 0.0)), 0.0)
        mixed = np.log((1 - args.eps) * p_model + args.eps * uniform)
        scores[name] = (np.where(ran, mixed, np.nan), hit, ran)
    for slot, name in enumerate(NET):
        ran = ninf["log_p"][:, slot] > -1e29
        scores[name] = (np.where(ran, ninf["log_p"][:, slot], np.nan), ran, ran)
    # The head (without replacement).
    head, side = load_head(args.head)
    layers = json.load(open(f"{args.head_model}/manifest.json"))["hparams"]["layers"]
    trunk, hp = teacher_params(args.head_model, list(range(layers)))
    theta = np.zeros((n, LETTERS))
    for lo in range(0, n, 256):
        sl = slice(lo, lo + 256)
        hidden = np.array(forward(trunk, unpack(mx.array(bits[sl])),
                                  mx.array(scalars[sl]), hp["heads"],
                                  return_hidden=True))
        x = np.concatenate([hidden, side_features(opps[sl])], axis=1)
        theta[sl] = np.array(head_logits(head, mx.array(x)))
    head_log = fisher_log_p(unseen, theta, leave)
    scores["head"] = (head_log, np.ones(n, bool), np.ones(n, bool))
    for label, sel in [("opponent kept 1 tile (6 played)", k == 1),
                       ("opponent kept 2 tiles (5 played)", k == 2),
                       ("opponent kept 1-2 tiles", k <= 2),
                       ("opponent kept 3 tiles", k == 3),
                       ("opponent kept 4 tiles", k == 4)]:
        base = sel & ~np.isnan(scores["teacher PAT t0.003"][0]) if k[sel].max(initial=0) <= 2 else sel
        print(f"\n{label}: n={int(base.sum())}  (gain in nats/decision over a random draw; "
              f"coverage = actual leave given nonzero probability)")
        for name, (value, hit, ran) in scores.items():
            ok = base & ~np.isnan(value)
            if ok.sum() < base.sum() * 0.98 or ok.sum() == 0:
                continue
            gain = value[ok] - log_uniform[ok]
            se = gain.std() / np.sqrt(len(gain))
            print(f"  {name:24s} {gain.mean():+.3f} +/- {se:.3f}   coverage {hit[ok].mean():6.1%}")
    print("\nmixtures with the head (gain over a random draw, nats/decision):")
    for base_name in ["teacher PAT t0.01", "teacher PAT t0.003",
                      "student PAT t0.01", "score+leave m10", "PAT m10"]:
        base_value = scores[base_name][0]
        for label, sel in [("1-2 tiles", k <= 2), ("3 tiles", k == 3),
                           ("4 tiles", k == 4)]:
            ok = sel & ~np.isnan(base_value)
            if ok.sum() < 50:
                continue
            cells = []
            for weight in [0.0, 0.1, 0.2, 0.5]:
                mixed = np.log((1 - weight) * np.exp(base_value[ok])
                               + weight * np.exp(head_log[ok]))
                cells.append(f"head {weight:.1f}: {np.mean(mixed - log_uniform[ok]):+.3f}")
            print(f"  {base_name:20s} [{label}]  " + "  ".join(cells))
    print("\npaired differences (same decisions), nats/decision:")
    for a, b in [("PAT m10", "score+leave m10"), ("PAT m0", "score+leave m0"),
                 ("teacher PAT t0.003", "teacher no-PAT t0.003"),
                 ("teacher PAT t0.01", "student PAT t0.01"),
                 ("teacher PAT t0.01", "score+leave m10"),
                 ("student PAT t0.01", "score+leave m10")]:
        for label, sel in [("1-2 tiles", k <= 2), ("3 tiles", k == 3),
                           ("4 tiles", k == 4)]:
            va, vb = scores[a][0], scores[b][0]
            ok = sel & ~np.isnan(va) & ~np.isnan(vb)
            if ok.sum() < 50:
                continue
            d = va[ok] - vb[ok]
            print(f"  {a} - {b} [{label}]: {d.mean():+.3f} +/- "
                  f"{d.std() / np.sqrt(len(d)):.3f} (n={int(ok.sum())})")
    t = ninf["micros"][ninf["log_p"][:, 1] > -1e29] / 1000
    print(f"\nnet inference time (teacher PAT pass, 16 threads sharing the GPU): "
          f"mean {t.mean():.0f} ms, p90 {np.percentile(t, 90):.0f} ms; "
          f"rows/decision {ninf['rows'][ninf['log_p'][:, 1] > -1e29].mean():.0f}")


if __name__ == "__main__":
    main()
