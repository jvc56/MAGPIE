"""How long MAGPIE's inference takes per decision, by the size of the leave
the opponent kept and for exchanges, from `valuenet:distill ... opp=1
inferq=1` (.opp2, .inf and .inft files); single-threaded runs.

    python infertiming.py "ceiling_time/c.bin.t*"
"""

import glob
import sys

import numpy as np

OPP2 = np.dtype([("game_id", "<u4"), ("turn", "<u2"), ("flags", "<u2"),
                 ("rack", "u1", 28), ("leave", "u1", 28),
                 ("last_score", "<i2"), ("last_tiles", "<u2"),
                 ("played", "u1", 28)])
INF = np.dtype([("game_id", "<u4"), ("turn", "<u2"), ("flags", "<u2"),
                ("log_p", "<f4", 4), ("leaves", "<u4", 4)])
TIME = np.dtype([("game_id", "<u4"), ("turn", "<u2"), ("reserved", "<u2"),
                 ("micros", "<u4", 4)])
MARGINS = [0, 10, 25, 60]


def main():
    files = sorted(p for p in glob.glob(sys.argv[1])
                   if p.endswith(tuple(f".t{k}" for k in range(32))))
    opp = np.concatenate([np.fromfile(p + ".opp2", dtype=OPP2) for p in files])
    inf = np.concatenate([np.fromfile(p + ".inf", dtype=INF) for p in files])
    tim = np.concatenate([np.fromfile(p + ".inft", dtype=TIME) for p in files])
    assert len(opp) == len(inf) == len(tim)
    ran = inf["flags"] > 0
    keep = opp["leave"][:, :27].sum(1)
    exch = (opp["flags"] & 4) > 0
    print(f"{int(ran.sum())} decisions where inference ran "
          f"(of {len(opp)}); microseconds per run, single thread\n")
    print(f"{'group':10s} {'n':>5s}  " + "  ".join(
        f"m={m:<2d} mean/p90/max ms  leaves  leaves/s" for m in MARGINS[:2]))
    groups = [(f"keep {k}", ran & (keep == k) & ~exch) for k in range(1, 7)]
    groups.append(("exchange", ran & exch))
    groups.append(("all", ran))
    for label, sel in groups:
        if sel.sum() == 0:
            continue
        cells = []
        for m in range(2):
            ms = tim["micros"][sel, m] / 1000.0
            leaves = inf["leaves"][sel, m].astype(float)
            rate = leaves.sum() / max(ms.sum() / 1000.0, 1e-9)
            cells.append(f"{ms.mean():7.1f}/{np.percentile(ms, 90):7.1f}/"
                         f"{ms.max():7.1f} {leaves.mean():7.0f} {rate:9.0f}")
        print(f"{label:10s} {int(sel.sum()):5d}  " + "  ".join(cells))


if __name__ == "__main__":
    main()
