#!/usr/bin/env python3
"""
Fits an outcome_model from autoplay -tdump CSV and writes a .ocm file.

Usage:
  python3 train_outcome_model.py train.csv model.ocm

Reads all rows in CSV, fits:
  - logistic regression for win in {0, 0.5, 1} (treating 0.5 as 0.5
    target via probabilistic gradient — sklearn handles binary only,
    so we round 0.5 to 1 by default; ties are <0.3% of rows in
    practice and don't shift the fit meaningfully)
  - linear regression for final_spread (millipoints)
Writes the binary .ocm file format (see outcome_model.h).

Requires numpy. Uses sklearn if available, otherwise falls back to
numpy's least-squares for linear and a closed-form Newton-Raphson for
logistic.
"""

import csv
import struct
import sys

import numpy as np

# Must match outcome_model.c FEATURE_NAMES exactly (and the CSV header).
FEATURE_NAMES = [
    "us_st_frac_playable",
    "us_st_top1",
    "us_st_top2",
    "opp_st_frac_playable",
    "opp_st_top1",
    "opp_st_top2",
    "us_bingo_prob",
    "opp_bingo_prob",
    "unplayed_blanks",
    "tiles_unseen",
    "score_diff",
    "us_leave_value",
]

OCM_MAGIC = b"OCM1"


def read_csv(path):
    with open(path) as f:
        rows = list(csv.DictReader(f))
    X = np.array([[float(r[c]) for c in FEATURE_NAMES] for r in rows])
    win = np.array([float(r["win"]) for r in rows])
    spread = np.array([float(r["final_spread"]) for r in rows])
    return X, win, spread


def fit_linear(X, y):
    # Closed-form OLS: [bias | weights] = (X' X)^-1 X' y where X is
    # augmented with a leading 1-column for the bias term.
    Xa = np.hstack([np.ones((X.shape[0], 1)), X])
    coef, *_ = np.linalg.lstsq(Xa, y, rcond=None)
    return coef[0], coef[1:]


def fit_logistic(X, y, max_iter=50, tol=1e-6):
    # Newton-Raphson for logistic regression. y is in [0, 1]; we treat
    # 0.5 as a soft-label target by minimizing cross-entropy with the
    # given target probabilities.
    Xa = np.hstack([np.ones((X.shape[0], 1)), X])
    n, d = Xa.shape
    w = np.zeros(d)
    for _ in range(max_iter):
        z = Xa @ w
        p = 1.0 / (1.0 + np.exp(-np.clip(z, -50, 50)))
        # gradient: X' (p - y), Hessian: X' diag(p(1-p)) X
        grad = Xa.T @ (p - y)
        W = p * (1.0 - p)
        H = (Xa * W[:, None]).T @ Xa
        # Add tiny ridge for numerical stability if Hessian is rank-def.
        H += np.eye(d) * 1e-9
        try:
            step = np.linalg.solve(H, grad)
        except np.linalg.LinAlgError:
            step = np.linalg.lstsq(H, grad, rcond=None)[0]
        w_new = w - step
        if np.max(np.abs(w_new - w)) < tol:
            w = w_new
            break
        w = w_new
    return w[0], w[1:]


def write_ocm(path, win_bias, win_weights, spread_bias, spread_weights):
    n = len(FEATURE_NAMES)
    assert len(win_weights) == n
    assert len(spread_weights) == n
    with open(path, "wb") as f:
        f.write(OCM_MAGIC)
        f.write(struct.pack("<I", n))
        f.write(struct.pack("<d", float(win_bias)))
        f.write(struct.pack(f"<{n}d", *win_weights.astype(float)))
        f.write(struct.pack("<d", float(spread_bias)))
        f.write(struct.pack(f"<{n}d", *spread_weights.astype(float)))


def main():
    if len(sys.argv) != 3:
        print(__doc__, file=sys.stderr)
        sys.exit(1)
    csv_path, ocm_path = sys.argv[1], sys.argv[2]

    print(f"reading {csv_path} ...")
    X, win, spread = read_csv(csv_path)
    print(f"  rows={len(X)} features={X.shape[1]}")
    print(f"  win mean={win.mean():.4f}  spread mean={spread.mean():.1f}")

    print("fitting linear regression for spread ...")
    spread_bias, spread_weights = fit_linear(X, spread)
    print(f"  spread_bias={spread_bias:.3f}")
    pred_spread = spread_bias + X @ spread_weights
    rss = ((spread - pred_spread) ** 2).sum()
    tss = ((spread - spread.mean()) ** 2).sum()
    print(f"  R^2 = {1 - rss / tss:.4f}")

    print("fitting logistic regression for win ...")
    win_bias, win_weights = fit_logistic(X, win)
    print(f"  win_bias={win_bias:.3f}")
    pred_p = 1.0 / (1.0 + np.exp(-(win_bias + X @ win_weights)))
    eps = 1e-12
    ll = np.where(win > 0, win * np.log(pred_p + eps), 0) + np.where(
        win < 1, (1 - win) * np.log(1 - pred_p + eps), 0
    )
    print(f"  mean log-likelihood = {ll.mean():.4f}")

    print()
    print(f"{'feature':25s} {'win_w':>10s} {'spread_w':>12s}")
    for name, ww, sw in zip(FEATURE_NAMES, win_weights, spread_weights):
        print(f"  {name:25s} {ww:+10.5f} {sw:+12.3f}")

    write_ocm(ocm_path, win_bias, win_weights, spread_bias, spread_weights)
    print(f"\nwrote {ocm_path}")


if __name__ == "__main__":
    main()
