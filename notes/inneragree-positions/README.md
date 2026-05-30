# Inner-pick agreement / loss — augmented position reports

Single-page HTML reports for individual root positions from the
nested-vs-flat outer-sim analysis. Each report includes:

- Position diagram (board + tile racks + scores + bag)
- Per-arm score table (sortable, searchable) showing both nested and flat
  per-ply average scores and bingo rates
- Per-ply tile-placement heatmaps with toggles:
  - `bingos only` — restricts to bingo placements; cell values are
    pct-of-all-plays (so heatmap sums to bingo rate, not 100%)
  - `diff (nested − flat)` — divergent red/blue scale showing where the
    two sims send tiles differently
- Per-ply exchange/pass strip (`PASS | EX1 | EX2 | ... | EX7`) showing
  fraction of plays at that ply that were exchanges of each size or pass.

## Reports

| file | position | rack | nested pick | flat pick | notes |
|---|---|---|---|---|---|
| `inneragree_g4t7_15m_v3.html` | g4 t7 p1 | EEIORTV | `12L VE(HM)` | `12L VE(HM)` | same pick, big rollout divergence (bag=57, closed midgame) |
| `inneragree_g4t21_30m.html` | g4 t21 p1 | CEHIRTU | `2J CHERU(P)` | `2J CHERU(P)` | same pick, near-zero divergence (bag=5, endgame) |
| `inneragree_g12t11p1.html` | g12 t11 p1 | DFGMNS? | `13I F(A)ND` | `13I F(A)NGS` | nested-favors-its-pick: gap +0.134 (largest) |
| `inneragree_g17t12p0.html` | g17 t12 p0 | AEKLNOT | `O8 TONKE(D)` | `12L KETO` | nested-favors gap +0.092 |
| `inneragree_g13t1p1.html` | g13 t1 p1 | AAAIIIZ | `F8 (R)IZA` | `7G AIA` | nested-favors gap +0.092 |
| `inneragree_g16t6p0.html` | g16 t6 p0 | AELNNOS | `D1 NEONS` | `12B LOAN` | nested-favors gap +0.087 |
| `inneragree_g0t2p0.html` | g0 t2 p0 | ADMNOTU | `D2 OUTNAM(E)D` | `D2 AMOUNT(E)D` | suspected epigon collision; treat with caution |
| `inneragree_g4t13p1.html` | g4 t13 p1 | AADNNOR | `15B DONNAR(D)` | `C9 NAR(C)O` | nested-favors gap +0.068 |

## Caveats

- "Gap" numbers come from a 30s nested screening pass. They are
  upward-biased: when nested decides flat's pick is bad early, BAI prunes
  it and the wpct estimate for it is noisy (typically 10–100 samples). The
  augment-derived wpcts (in the JSONLs / HTMLs) are more reliable but
  still subject to BAI similarity-key pruning (the "epigon" issue) for
  same-square different-anagram move pairs.
- Per-game average utility lost from using flat picks instead of nested
  picks is approximately +0.07 win-pct, but this is also upward-biased
  by the same small-sample issue. True value probably 0.03–0.05/game.

## Subdirectories

- `jsonl/` — raw per-position augment JSONL files (one position per file).
  Each file contains both flat and nested per-arm per-ply data including
  225-cell heatmaps (all + bingo), pass counts, and exchange-by-size
  counts.
- `csv/` — source CSVs:
  - `inneragree_perroot_v2.csv` — 20-game per-root inner-sim agreement
    metrics (mean_loss, agree_rate, etc.)
  - `inneragree_screen30s.csv` — flat-sim screening of 241 bag≥36
    positions (flat pick, flat wpct, flat wpct for nested's pick)
  - `inneragree_nscreen30s.csv` — nested-sim screening of the 92
    pick-disagreement positions

## Regenerating reports

The renderer is `render_inneragree.py` in this directory. To regenerate a
report:

```bash
INNERAGREE_CSV=csv/inneragree_perroot_v2.csv \
INNERAGREE_JSONL=jsonl/batch_g12t11p1.jsonl \
INNERAGREE_HTML=inneragree_g12t11p1.html \
python3 render_inneragree.py
```

## Generating new augments

```bash
# 1. Build single-row input CSV with the target position's row from the
#    full per-root CSV.
head -1 csv/inneragree_perroot_v2.csv > /tmp/single.csv
grep "^GAME,TURN,PLAYER," csv/inneragree_perroot_v2.csv >> /tmp/single.csv

# 2. Run augment (15-min budget × flat + nested = ~30 min wall).
INNERAGREE_AUGMENT_CSV=/tmp/single.csv \
  INNERAGREE_AUGMENT_OUT=jsonl/new.jsonl \
  INNERAGREE_TLIM=900 \
  ./bin/magpie_test inneragree

# 3. Render.
INNERAGREE_CSV=/tmp/single.csv \
  INNERAGREE_JSONL=jsonl/new.jsonl \
  INNERAGREE_HTML=new.html \
  python3 render_inneragree.py
```
