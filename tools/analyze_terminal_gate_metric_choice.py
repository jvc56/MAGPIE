#!/usr/bin/env python3
"""Monte-Carlo: per-decision-clustered vs per-game(pair)-sum JMV at ~260 pairs.
Uses the panel's empirical per-decision JMV distribution (316 zeros + the four
mismatch magnitudes). Answers: (a) pass margin under the observed law,
(b) type-I calibration when the true mean sits exactly on the threshold."""
import json, math, random, statistics

random.seed(20260806)
PANEL = json.load(open("/Users/olaugh/sources/aug04-magpie/MAGPIE/obj/panel-jmv-per-root.json"))
N_PAIRS = 260
STOPS_PER_PAIR = 17.5
PER_DECISION_MARGIN = 0.001
PER_PAIR_MARGIN = PER_DECISION_MARGIN * STOPS_PER_PAIR  # consistent scaling
SIMS = 3000
Z95 = 1.6448536269514722


def t_upper(vals, margin_df=None):
    n = len(vals)
    m = statistics.fmean(vals)
    sd = statistics.pstdev(vals) * math.sqrt(n / (n - 1)) if n > 1 else 0.0
    # normal approx to the one-sided upper (n is large here)
    return m + Z95 * sd / math.sqrt(n)


def boot_upper(vals, reps=1000):
    n = len(vals)
    means = []
    for _ in range(reps):
        s = 0.0
        for _ in range(n):
            s += vals[random.randrange(n)]
        means.append(s / n)
    means.sort()
    return means[int(0.95 * reps)]  # one-sided 95% percentile upper


def cluster_upper(pair_sums, pair_counts):
    # cluster-robust mean of per-decision JMV, clusters = pairs
    total = sum(pair_sums)
    ndec = sum(pair_counts)
    mean = total / ndec
    G = len(pair_sums)
    # cluster-robust variance of the mean (each pair's decision-mean deviation)
    num = 0.0
    for s, c in zip(pair_sums, pair_counts):
        num += (s - mean * c) ** 2
    var = (G / (G - 1)) * num / (ndec ** 2)
    return mean + Z95 * math.sqrt(var)


def simulate(pop, sims=SIMS):
    """pop = per-decision JMV population to sample. Returns pass rates + upper stats."""
    res = {k: [] for k in ("dec_cluster", "pair_t", "pair_boot")}
    npop = len(pop)
    for _ in range(sims):
        pair_sums, pair_counts, pair_avgs = [], [], []
        for _ in range(N_PAIRS):
            k = max(1, int(random.gauss(STOPS_PER_PAIR, 2)))
            s = sum(pop[random.randrange(npop)] for _ in range(k))
            pair_sums.append(s)
            pair_counts.append(k)
            pair_avgs.append(s / k)
        res["dec_cluster"].append(cluster_upper(pair_sums, pair_counts))
        res["pair_t"].append(t_upper(pair_sums))
        res["pair_boot"].append(boot_upper(pair_sums, reps=400))
    return res


def report(label, res, dec_margin, pair_margin):
    def stats(x):
        x = sorted(x)
        return x[len(x) // 2], x[int(0.05 * len(x))], x[int(0.95 * len(x))]
    print(f"\n=== {label} ===")
    for k, margin, unit in (
        ("dec_cluster", dec_margin, "per-decision (cluster-robust)"),
        ("pair_t", pair_margin, "per-pair-sum (Student-t)"),
        ("pair_boot", pair_margin, "per-pair-sum (bootstrap)"),
    ):
        med, lo, hi = stats(res[k])
        passrate = sum(1 for u in res[k] if u <= margin) / len(res[k])
        print(f"  {unit:34s} upper med={med:+.5f} [p5 {lo:+.5f}, p95 {hi:+.5f}]  "
              f"margin={margin:.4f}  pass={100*passrate:.1f}%")


# (a) observed law
report("A. OBSERVED panel law (true mean ~ -8e-5, should pass easily)",
       simulate(PANEL), PER_DECISION_MARGIN, PER_PAIR_MARGIN)

# (b) type-I calibration: shift the population so its true per-decision mean == margin.
# Worst case for the tail: flip the helpful -0.048 to +0.048 then re-center to exactly
# the margin, so the gate SHOULD reject ~95% of the time (false-pass = type-I error).
flipped = [abs(x) for x in PANEL]  # make the big event a positive (dangerous) tail
shift = PER_DECISION_MARGIN - statistics.fmean(flipped)
null_pop = [x + shift for x in flipped]  # true mean is exactly the margin
print(f"\n[null population: true per-decision mean = {statistics.fmean(null_pop):.5f} "
      f"= margin; heavy +tail from |{min(PANEL):.3f}| event]")
report("B. TYPE-I at the margin (pass here = FALSE PASS; want <= 5%)",
       simulate(null_pop), PER_DECISION_MARGIN, PER_PAIR_MARGIN)
