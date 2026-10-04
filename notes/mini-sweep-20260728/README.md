# Depth crossover "Mini sweep" (M4 Mini, 2026-07-28) — archived 2026-10-04

The 2/4/6(/8)-ply sweep at 300K, 1M and 3M nodes per position that PR #633 (10M nodes, M5 Max) refers to as "the earlier Mini sweep". It was run by Codex in `~/sources/jul21-magpie/MAGPIE-positional` at commit 5a1dc19e. Its outputs lived only in the ignored `obj/` directory and were never in a PR description.

- **Setup:** the same 644-position (bag >= 50), 60-candidate CSW24 panel as #633, and a 10-ply oracle with 100,000 samples per distinct nominee.
- **Oracle labels:** in the oracle logs, `klv2` / `hybrid` / `klv3` mean 2 / 4 / 6 ply. `tools/build_depth_crossover_oracle_corpus.py` writes each disagreement's moves in the order p2, p4, p6.

Mean oracle utility difference over disagreement positions (SE in parentheses):

| budget | disagreements | 4 vs 2 | 6 vs 2 | 6 vs 4 |
|---|---:|---:|---:|---:|
| 300K | 91 / 644 | +0.00078 (0.00028) | +0.00058 (0.00031) | −0.00020 (0.00021) |
| 1M | 69 / 644 | +0.00136 (0.00032) | +0.00150 (0.00037) | +0.00015 (0.00023) |
| 3M | 71 / 644 | +0.00148 (0.00029) | +0.00165 (0.00031) | +0.00017 (0.00015) |

The 8-ply arms were run but not judged by the oracle.

## Contents

- `key-results/`: the depth-crossover and depth-replication logs, disagreement TSVs, the oracle logs, and the runner log, uncompressed.
- `all-obj-results/`: every result file from `obj/`, 466 files (617 MB uncompressed). This includes the earlier positional-candidate and combined-sweep experiments of 2026-07-27. To restore:
  `cat positional-obj-results.tar.gz.part-* > positional-obj-results.tar.gz`, verify against `SHA256`, then `tar xzf`.
- `../../tools/`: eight analysis and training scripts that were never committed (`analyze_*`, `select_positional_signal_corpus.py`, `sweep_positional_deployment.py`, `train_positional_*`).

The later "thinking curves" run (50K–10M nodes, 2/4/6 ply, 2026-07-29) was in `MAGPIE-time-value-trace`, which has since been deleted, and is not here.
