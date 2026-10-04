"""How well a trained opponent-leave head (rackjoint.py) predicts statistics
of the leave it implies, on held-out games: the vowel fraction (A E I O U of
the leave's tiles) and the leave's mean tile score, each against the same
guess from the uniform draw of the unseen tiles. The leave size is known to
a sim (the opponent's last move fixes it), so predictions are per tile.

    python rackderived.py --student runs/t6-so --data "opp500k/*.bin.t*" \
        --head rack/A_frozen_side
"""

import argparse
import json

import mlx.core as mx
import numpy as np

from rackjoint import (BINGO, LETTERS, MOVED, PASS, Data, head_logits,
                       rack_log_probs, side_features)
from train import forward, teacher_params, unpack

# English tile scores by machine letter: the blank, then A-Z.
SCORES = np.array([0, 1, 3, 3, 2, 1, 4, 2, 4, 1, 8, 5, 1, 3, 1, 1, 3, 10, 1,
                   1, 1, 1, 4, 4, 8, 4, 10], np.float64)
VOWELS = np.zeros(LETTERS)
VOWELS[[1, 5, 9, 15, 21]] = 1.0  # A E I O U


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--student", required=True)
    parser.add_argument("--data", required=True)
    parser.add_argument("--head", required=True, help="a rackjoint output dir")
    parser.add_argument("--val-mod", type=int, default=20)
    parser.add_argument("--max", type=int, default=20000)
    args = parser.parse_args()
    summary = json.load(open(f"{args.head}/summary.json"))["args"]
    use_side = summary["side"]
    head = {}
    for key, value in np.load(f"{args.head}/head.npz").items():
        a, b = key.split("/")
        head.setdefault(a, {})[b] = mx.array(value)
    layers = json.load(open(f"{args.student}/manifest.json"))["hparams"]["layers"]
    trunk, hparams = teacher_params(args.student, list(range(layers)))
    data = Data(args.data, args.val_mod)
    refs = data.val[data.val[:, 3] == 1][:args.max]
    out = {k: [] for k in ["pred_vowel", "base_vowel", "true_vowel",
                           "pred_score", "base_score", "true_score", "bag"]}
    for lo in range(0, len(refs), 256):
        chunk = refs[lo:lo + 256]
        bits, scalars, bag, opps = [], [], [], []
        for p, d, _, _ in chunk:
            row = data.starts[p][d] + data.played[p][d]
            r = data.parts[p][row]
            bits.append(r["board_bits"])
            scalars.append(r["scalars"])
            bag.append(int(r["bag"]))
            opps.append(data.opp[p][d])
        opps = np.array(opps)
        bag = np.array(bag)
        hidden = forward(trunk, unpack(mx.array(np.stack(bits))),
                         mx.array(np.stack(scalars)), hparams["heads"],
                         return_hidden=True)
        x = np.array(hidden)
        if use_side:
            x = np.concatenate([x, side_features(opps)], axis=1)
        unseen = np.round(np.stack(scalars)[:, 27:54].astype(np.float64)
                          * (bag + 7)[:, None])
        theta = head_logits(head, mx.array(x))
        p_model = np.exp(np.array(rack_log_probs(
            theta, mx.array(unseen.astype(np.float32)))))
        p_base = unseen / unseen.sum(1, keepdims=True)
        leave = opps["leave"][:, :LETTERS].astype(np.float64)
        k = leave.sum(1)
        keep = k > 0  # an empty leave (all 7 tiles exchanged) has no tiles
        leave, k = leave[keep], k[keep]
        p_model, p_base, bag = p_model[keep], p_base[keep], bag[keep]
        out["pred_vowel"].append(p_model @ VOWELS)
        out["base_vowel"].append(p_base @ VOWELS)
        out["true_vowel"].append(leave @ VOWELS / k)
        out["pred_score"].append(p_model @ SCORES)
        out["base_score"].append(p_base @ SCORES)
        out["true_score"].append(leave @ SCORES / k)
        out["bag"].append(bag)
    out = {k: np.concatenate(v) for k, v in out.items()}
    print(f"{len(out['bag'])} held-out decisions with a leave")
    for name in ["vowel", "score"]:
        t = out[f"true_{name}"]
        for kind in ["base", "pred"]:
            e = out[f"{kind}_{name}"]
            print(f"{name:6s} {kind:5s} mse {np.mean((e - t) ** 2):.5f}  "
                  f"corr {np.corrcoef(e, t)[0, 1]:.3f}  "
                  f"mean pred {e.mean():.3f} vs true {t.mean():.3f}")
        mb = np.mean((out[f"base_{name}"] - t) ** 2)
        mp = np.mean((out[f"pred_{name}"] - t) ** 2)
        print(f"{name:6s} model mse is {100 * (mb - mp) / mb:.2f}% lower than "
              f"the unseen-pool guess; the leave-to-leave variance of the "
              f"truth is {t.var():.5f}")
        for lo, hi, label in [(60, 999, "bag 60+"), (30, 60, "bag 30-59"),
                              (7, 30, "bag 7-29"), (0, 7, "bag 0-6")]:
            m = (out["bag"] >= lo) & (out["bag"] < hi)
            b = np.mean((out[f"base_{name}"][m] - t[m]) ** 2)
            f = np.mean((out[f"pred_{name}"][m] - t[m]) ** 2)
            print(f"   {label:10s} n={int(m.sum()):6d} base {b:.5f} model "
                  f"{f:.5f} ({100 * (b - f) / b:+.2f}%)")


if __name__ == "__main__":
    main()
