# Nested vs Flat Outer Sim: Rollout Divergence on Closed-Board Midgame Positions

Date: 2026-05-28
Source data: `/tmp/gamepairbai/inneragree_perroot_v2.csv` (20-game per-root run)
+ 30-min augment JSONLs: `inneragree_aug_g4t21_30m.jsonl`, `inneragree_aug_g4t7_30m.jsonl`
Tooling: `test_inner_agreement` in `test/inner_agreement_test.c`
Renderer: `/tmp/gamepairbai/render_inneragree.py`

## TL;DR

For two cherry-picked spicy positions we ran flat-outer vs nested-outer sims at
30 minutes per variant on K=100 outer arms.

- **Endgame (bag=5)**: flat and nested rollouts are statistically
  indistinguishable. Per-ply scores match within 0.1, heatmap percentage
  diffs are ~0.5pp total across the whole board. The production inner sim
  falls through to `endgame_ply_solve` (top-equity) once the bag is empty,
  so nested literally executes flat's code path at deeper plies.
- **Midgame closed board (bag=57)**: massive divergence. Same top arm
  (`12L VE(HM)`), but flat values it at win pct 0.588 vs nested 0.670.
  Flat hallucinates a fantasy where bingos keep landing (18–25% per
  later ply); nested correctly sees the board has closed and bingo rate
  drops to 2–8% per later ply.

The midgame closed-board case is exactly the position class where nested
earns its compute. Endgame doesn't benefit at all.

## Position 1: g4 t21 p1 (CHERU(P)) — bag near empty, hypothesis NOT supported

```
   A B C D E F G H I J K L M N O   > Player 1  CEHIRTU  335
                                     Player 2  DEEEINS  345
 1| . . . . . . . . . . . . . . U |   Unseen: (5)  A B O R T
 2| . . . . . . . . . . . . . . P |
 3| . . . . . . . . W . . . . . S |
 4| . . . . . . . G A N g p L O W |
 5| . . . . . . . . T . . . . A A |
 6| . . . . . . . B E . . . . K Y |
 7| . . . . . . . R . . . . . . . |
 8| . . . . . . J I Z . . S A F T |
 9| . . . . . . . D A R I O L E . |
10| . . . . Q . V E G O . P E E L |
11| . . . . U . . . . U . . . . A |
12| . . C O I S T R E L . V E H M |
13| . G I . M . . I . . . . . O I |
14| F E T . . . O N Y X . . . I N |
15| . D O N N A R D . . . . . . A |
```

CGP: `14U/14P/8W5S/7GANgpLOW/8T4AA/7BE4KY/7R7/6JIZ2SAFT/7DARIOLE1/4Q1VEGO1PEEL/4U4U4A/2COISTREL1VEHM/1GI1M2I5OI/FET3ONYX3IN/1DONNARD6A CEHIRTU/DEEEINS 335/345 0`

- On-turn rack: CEHIRTU, scores 335-345, bag = 5 tiles (A, B, O, R, T)
- Nested top arm = `2J CHERU(P)` wpct=0.7816 (n=438,469)
- Flat top arm = `2J CHERU(P)` wpct=0.7811 (n=35,879,831)
- Win-pct diff: +0.0005 (noise)

| ply | nested score | flat score | nested bingo | flat bingo |
|---|---|---|---|---|
| 0 | 26.2 | 26.2 | 1.5% | 1.5% |
| 1 (opp) | 30.1 | 30.0 | 10.4% | 10.3% |
| 2 (us)  | 13.9 | 13.9 | 0.0% | 0.0% |
| 3 (opp) | 10.9 | 10.9 | 0.0% | 0.0% |

Heatmap total absolute pct diff per ply: 0.30 / 0.47 / 0.49 / 0.92.

Why no divergence: the bag drains in 1 ply. After ply 0 the bag is empty,
which triggers `bag_is_empty(...)` in the inner sim and falls through to
`endgame_ply_solve` = top-equity. Nested at plies 2/3 literally executes
flat's code path.

## Position 2: g4 t7 p1 (VE(HM)) — midgame closed board, hypothesis CONFIRMED

```
   A B C D E F G H I J K L M N O   > Player 1  EEIORTV  132
                                     Player 2  LOORTUU  121
 1| . . . . . . . . . . . . . . . |   Unseen: (57)
 2| . . . . . . . . . . . . . . . |
 3| . . . . . . . . . . . . . . . |
 4| . . . . . . . . . . . . . . . |
 5| . . . . . . . . . . . . . . . |
 6| . . . . . . . . . . . . . . . |
 7| . . . . . . . . . . . . . . . |
 8| . . . . . . J I Z . . S A F T |
 9| . . . . . . . D A R I O L E . |
10| . . . . . . V E G . . P E E L |
11| . . . . . . . . . . . . . . A |
12| . . . . . . . . . . . . H M . |
13| . . . . . . . . . . . . O I . |
14| . . . . . . . . . . . . I N . |
15| . . . . . . . . . . . . . . A |
```

CGP: `15/15/15/15/15/15/15/6JIZ2SAFT/7DARIOLE1/6VEG2PEEL/14A/13HM/13OI/13IN/14A EEIORTV/LOORTUU 132/121 0`

- On-turn rack: EEIORTV, scores 132-121, bag = 57 tiles
- Nested top arm = `12L VE(HM)` wpct=0.6695 (n=42,519)
- Flat top arm = `12L VE(HM)` wpct=0.5881 (n=28,156,351)
- Win-pct diff: **+0.0814** (real, large)

Both sims pick the SAME move. They disagree on its valuation.

### Per-ply rollout outcomes for the same `12L VE(HM)` arm

| ply | nested score | flat score | score diff | nested bingo | flat bingo | bingo diff |
|---|---|---|---|---|---|---|
| 0 | 16.6 | 20.9 | -4.3 | 3.3% | 4.7% | -1.4pp |
| 1 (opp) | 12.6 | **26.6** | **-14.0** | 2.0% | **18.5%** | **-16.6pp** |
| 2 (us)  | 18.1 | **35.4** | **-17.4** | 4.9% | **21.5%** | **-16.6pp** |
| 3 (opp) | 20.1 | **36.3** | **-16.2** | 8.2% | **25.4%** | **-17.3pp** |

Flat keeps rolling out bingos at 18–25% per later ply. Nested correctly
sees the board has closed and bingos drop to 2–8%.

### Heatmap divergence

Total absolute pct-point diff per ply (vs 0.3–0.9 for CHERU(P)):

| ply | total abs diff (pp) |
|---|---|
| 0 (opp response) | 29.1 |
| 1 (our follow-up) | 48.9 |
| 2 (opp) | 35.1 |
| 3 (our) | 30.8 |

Where they disagree:

- Flat ply 0 places more at **14M, 13M, 15M** (column-M scoring lines that
  *would* work if opp got an anchor to play through) and **14J–15N** (a
  perceived big-scoring lane). Nested instead plays smaller and more
  locally — **7O, 6O, 5O**, **12J, 13L**.
- Flat ply 1 likes **10J, 11J** for bingos through the existing words
  (8–9% of plays); nested only sees 6%.
- Flat keeps trying to bingo at **1O–3O** at later plies; nested correctly
  says no.

## What this means

For positions like g4 t7 p1 — substantial bag (50+) plus a tight,
closed-up board — flat-sim's static eval keeps imagining bingo-rich
futures. Nested's BAI-driven leaf eval correctly recognizes the board is
closed and projects a much more constrained scoring environment.

The actionable practical point: at this position, **both sims still pick
the same top arm (`VE(HM)`)**. The difference is only in the *valuation*.
The places where nested actually changes the move played are positions
where the valuation flip crosses two arms — that's the population we'd
need to filter for to find the highest-leverage nested-pays-off cases.

## Reproduce

```
# 1. Generate per-root inner-agreement metrics (20 games × 20s/turn ≈ 2.5h)
INNERAGREE_GAMES=20 INNERAGREE_TLIM=20 \
  INNERAGREE_CSV=/tmp/gamepairbai/inneragree_perroot_v2.csv \
  ./bin/magpie_test inneragree

# 2. Augment a specific position with full flat+nested sim at 30 min each
head -1 /tmp/gamepairbai/inneragree_perroot_v2.csv > /tmp/single.csv
grep "^4,7,1," /tmp/gamepairbai/inneragree_perroot_v2.csv >> /tmp/single.csv
INNERAGREE_AUGMENT_CSV=/tmp/single.csv \
  INNERAGREE_AUGMENT_OUT=/tmp/single_aug.jsonl \
  INNERAGREE_TLIM=1800 ./bin/magpie_test inneragree

# 3. Render the inspector HTML
INNERAGREE_JSONL=/tmp/single_aug.jsonl \
  INNERAGREE_HTML=/tmp/single.html \
  python3 /tmp/gamepairbai/render_inneragree.py
open /tmp/single.html

# Quick board dump from CGP without running sims
INNERAGREE_DUMP_CGP='<cgp>' ./bin/magpie_test inneragree
```
