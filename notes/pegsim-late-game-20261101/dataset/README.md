# Late-game data (NWL23)

Gzipped CSVs from the pegsim studies at 7 tiles in the bag, plus the 2PEG
label set at 2 in the bag and the nets the harness uses. All positions come
from static self-play games and are given as CGP. `SHA256SUMS` lists the
checksums of the uncompressed files.

**Hidden information.** Each CGP holds both racks, and the mover is the
`seat` column. A player choosing a move may see only the board, the scores,
its own rack, and the unseen tile counts (the bag plus the opponent's rack).
It must not use the opponent's rack or the bag order.

## `bag7/`: 512 boards at 7 in the bag (`pegnet:pegsim`, see `../REPORT.md`)

- `positions.csv.gz`: the 512 roots (positions seed 202610169000).
- `main.sim.csv.gz` and `main.cands.csv.gz`: the main run (seed 202610169000).
  - `sim` has one row per root and arm: the pick, its static `rank`, and its
    utility, win and equity means.
  - `cands` has every candidate's `samples`, `utility`, `win` and `equity`
    means for every arm.
  - Arms:

    | Arm | Rollouts |
    |---|---|
    | 0 (A) | static, 1,000 per candidate |
    | 1 (B) | the nets choose plays at 1-2 in the bag |
    | 2 (C) | static, for B's wall time |
    | 3 (D) | B plus a 2-ply endgame search once the bag is empty, no transposition table |
  - `rank` is the index in the root's static top 16 placements, sorted by
    equity. `main.cands` has no move text; join on (root, rank) with
    `sampling.cands` to get it.
- `sampling.sim.csv.gz` and `sampling.cands.csv.gz`: arms 0 and 3 again, with
  move text. This run used seed 202610169100, a shared endgame transposition
  table and a later binary. Its picks equal the main run's on 511 of 512 boards
  for arm 0 and 509 of 512 for arm 3.
- Units in `sim` and `cands`:
  - `win` is the mean win rate, from 0 to 1.
  - `equity` is the mean final spread.
  - `utility` per rollout is `(win + 0.5 * sigmoid(spread / 100)) / 1.5`.
  - Every rollout's result comes from its own world. D's values are its own
    estimates, not exact.
- `main.judge.oracle.csv.gz` and `main.judge.worlds.csv.gz`: every distinct
  pick of arms 0-3 on all 512 boards, judged by `pegnet:peg3oracle`.
  - Deals: 32 uniform random deals of the 14 unseen tiles (seed
    202610169200), shared by every pick on a board. The actual deal is also
    there, at weight 0.
  - Playout: static plays at 3+ in the bag, the nets at 1-2, then an exact
    endgame (25 plies, 15 s cap).
  - `oracle` columns: `codes` lists the arms that picked the move, `win` is
    the win% (ties count half), `mean` is the mean final spread, and `capped`
    is the number of capped endgames.
  - `worlds` has one row per (root, pick, deal): the bag order and the final
    spread for the mover. Its `pick` column matches `oracle`'s.

## `bag2/`: 4,096 boards at 2 in the bag (`pegnet:peg2data`)

- `positions.csv.gz`: the roots (positions seed 202610151101).
- `labels.csv.gz`: up to 64 candidates per board, 261,611 rows (the 256 label
  shards joined in order).
  - Each candidate is played in every hidden world: which 2 of the 9 unseen
    tiles are in the bag, and, for a one-tile play, which one the mover draws,
    weighted by multiplicity.
  - After a one-tile play, the opponent's 1-in-bag move is picked by the 1PEG
    net from the opponent's own information. The bag-empty endgame is then
    searched 2 plies deep, which is not exact.
  - Columns:

    | Column | Meaning |
    |---|---|
    | `candidate` | the wide rank: index in the equity-sorted move list without the pass, so 0-63 is the static top 64 |
    | `candidates` | number of placements |
    | `tiles` | tiles played |
    | `after` | the mover's spread right after the move |
    | `static_equity` | MAGPIE's static equity |
    | `win`, `margin` | the mover's mean win rate (0 to 1) and final spread over the worlds |
    | `worlds` | number of worlds |
    | `net_calls`, `net_ms`, `solve_ms` | timing only |

## `nets/`: the 1PEG and 2PEG choosers (`gzip -d` before use)

`pegsim` and `peg3oracle` take them as one argument:
`peg1-fast-u.bin:peg1-strong-u.bin:4:0.5:100:peg2-blend2-v1.bin:peg2-cheap-opp-v1.bin:16:0`,
with each file given as a full path.

The harness loads `set -path ./collection-runtime-data:./data -lex NWL23 -wmp
true -wit true -rit false`, so run it from a directory whose data holds NWL23.

Not included: every rollout of arms 0 and 3 (`PEG_SIM_SAMPLES`, 459 MB), the
equal-time and endgame-plies judge runs, and the autoplay run's 1,024 boards
(ready tonight).
