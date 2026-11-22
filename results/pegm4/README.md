# Pre-endgame (PEG) solver lab, M4, 2026-11-19 to 11-22

LAB ONLY. Environment switches and test modes here never go into a pull request.
Positions: `pegclose_<bag>.txt` (close positions with 1-4 tiles in the bag, CSW24).
Run modes live in `test/benchmark_peg_test.c` (dispatch in `test/test.c`); lab switches in `src/impl/peg.c`.

## Findings (utility = (win + 0.5*sigmoid(spread/100))/1.5, x100)

- A "complete" solve runs endgames `-pegplies` deep (default 3). Only `-pegplies end` is the true reference.
- Opponent model on plays that leave tiles in the bag: the default (`-pegpess true`) lets the opponent see our rack.
  `-pegpess false` is the hidden-rack model; with first-win off it ranks the opponent's replies by mean spread.
  Lab switch `MAGPIE_PEG_WINFIRST=1` ranks them by wins, then spread.
- Fast hidden-rack judge (`MAGPIE_PEG_FASTJUDGE=1`, needs WINFIRST): win-only pass over the opponent's replies,
  spreads only for replies tied at the top, the choice cached per information set.
  57 bag-2 picks: identical values on 49 of 49 finished by both, 3,573 s vs 5,662 s (8 threads); finished 53 vs 49 in 400 s.
- First-win (win/loss window) gives the same sign as the full window: 0 mismatches on every scenario checked.
- Depth vs end-of-game values (bag 2, bag-emptying candidates): 2-ply sign right 99.5%, lopsided optimistic
  (63 too good / 5 too bad), pick loses 0.32 pp win (SE 0.20); 3-ply values differ on 13 of 102 picks, -0.49 pp on average.
- Tie order (candidates tied on win at the top, 5 s solves): no win lost in any judged tie; utility lost per position
  0.046 (bag 1), 0.043 (bag 2, 5 s), 0.020 (bag 2, 20 s). With spreads known the pick was the best in every judged tie.
- Bag-2 comparisons, hidden-rack values (10 s, 139 rows): 2-ply top 32 vs greedy +3.77 utility (0.69);
  middle stage + 2-ply vs 2-ply -0.18 (0.21); middle stage vs static greedy +0.99 (0.47).
- Stopping rule "top set unchanged between two depths" (an_settle.py): no wrong stops on 26 positions; too small to trust.
  A longer reference run (soft_end_b2_long, 25 min per position) was in progress when this was pushed.

## Files

- `an_*.py`: analysis (an_tl tie loss, an_rj hidden-rack restatement, an_settle stopping rule, an_built, an_soft*).
- `run_*.sh`: how each run was launched (paths are the M4's).
- `tl_*`, `rejudge_*`, `endcheck_b2`, `built_b2*`: outputs behind the numbers above.

Not measured: bags 3 and 4; inference of the opponent's rack inside the solver (all scenarios are weighted by draw counts only).
