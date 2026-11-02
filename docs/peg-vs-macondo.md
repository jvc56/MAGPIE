# MAGPIE vs Macondo: PEG solves when behind by 20 to 40

The 41 positions from "Depth, search and nesting when behind by 20 to 40" in
`better-defaults-plan.md`, solved by both engines with their PEG defaults,
on 10 threads, at most 15 minutes each. The question: where MAGPIE is much
slower than Macondo, or gives a different answer, and why.

This is the second pass. The first found MAGPIE much slower on two
positions and traced it to how the complete solve spreads its threads; that
fix is now MAGPIE's solver, and every position was solved again with it. It
also found six plays whose win% differed and left the cause open; it is now
traced.

## Summary

- **Answers.** On the 26 positions where both engines finished the same
  depth (2 or 3 plies), they picked the same play, or plays with the same
  win%, every time. The best play's win% matched on 20 and differed on 6.
- **Why they differ.** Both engines finish a line still going at the search
  depth with a greedy playout. Macondo's playout picks each move by score
  alone, keeping the first of equal scores, so it can pass up a play of the
  same score that goes out. MAGPIE's ranks a play that goes out by its score
  plus twice the opponent's rack. On QI(S) and S(WO)WND, traced to one
  draw each, Macondo's 3-ply value rests on a playout that does exactly
  this, with two plays of equal score. With MAGPIE's rule added to its
  playout, Macondo gives MAGPIE's win% on 5 of the 6 plays and comes within
  3 orderings of 720 on the sixth (8.3% against 7.9%, from 16.7%). In every
  case MAGPIE's value was the closer to a deeper search, and on four it
  equaled it.
- **Speed, now.** On the 34 positions that both versions of
  MAGPIE finished, the solve now takes 2,689 s in all, against 6,558 s
  before. Every one was faster, and no count of a candidate solved in full
  changed (289 compared). MAGPIE was faster than Macondo on all 27
  positions where Macondo ran and either finished, often by several times;
  at 3 and 4 in the bag Macondo finished 3 plies on none. Three more
  positions now finish in 15 minutes; four, all at 3 or 4 in the bag, still
  do not.
- **The fix.** With early cutoff, the complete solve used to take the
  candidates one at a time and spread each one's draws over the threads.
  When a play wins 100% early, every other candidate needs only one losing
  draw to be cut. One thread finds it after about one cheap endgame; ten
  threads start all the draws at once, and the draws the mover wins are the
  expensive ones. g111545 took 655 s on 10 threads and 88 s on one. Now the
  seed's best candidate is solved first, every thread on its draws, to set
  the bar, and then the rest at once, one per thread, each its draws in turn
  against the best finished so far, read live. g111545 now takes 22 s and
  g110360 2 s, against Macondo's 104 and 7 s.
- **Why Macondo is slow at 3 and 4 in the bag.** It solves every ordered
  draw separately (720 at 3 in the bag, 7,920 at 4), where MAGPIE solves
  each multiset split once, weighted (31 to 196 at 3, 58 to 948 at 4). It
  also reruns every candidate at 1, 2 and 3 plies, and its spread tiebreak
  (full-window endgames over the top 20 tied plays, after every depth) took
  up to 773 s on its own. On lost positions, where every play is 0% and
  ties, the tiebreak is most of the time.

## Method

- **Positions.** The 41 deficit positions, CSW24, from 20,000 static
  self-play games: the player on turn trails by 20 to 40, with 1 to 4 tiles
  in the bag. The opponent's rack is hidden.
- **MAGPIE.** The complete solve (`-pegplies 3`, the default), `-threads
  10`, `-pegoutcomes false`. "Before" is the deficit evaluation's run (Oct
  28, the working tree at c2fad53f, one candidate at a time). "Now" is this
  change on top of cdff09e3 (Oct 29), the 41 positions in order of bag
  size, one at a time.
- **Macondo.** 67fa64a4 (Oct 28), `peg -threads 10 -maxtime 900
  -early-cutoff true`, everything else default: iterative deepening 1, 2, 3
  plies (`-endgameplies 3`), one nested level (`-max-nested-depth 1`), plays
  leaving at most one tile (`-max-tiles-left 1`), spread tiebreak on. Early
  cutoff is on to match MAGPIE and Macondo's bot and analyzer; the shell's
  default is off, which only makes Macondo slower. When the time limit
  hits, Macondo returns the last depth it completed; the times and win%
  below are from its log, per depth. Macondo did not change, so its runs
  from the first pass stand.
- **Order.** Macondo ran on all 20 positions with 1 or 2 in the bag, then 8
  of the 21 with 3 or 4. It timed out on all 8, so the remaining 13 were
  not run. Three positions were rerun with `-skip-tiebreaker true`.
- **Machine.** Intel i7-1250U, 2 performance and 8 efficiency cores, 12
  threads, 15 GB. One solve at a time. Two Macondo runs overlapped short
  side runs of 1 or 2 threads (g105869 and g110360) or a build at low
  priority (g105295 without tiebreak); the effect is small next to the
  differences reported.

## Per position

Times in seconds. "Macondo" is "-" where it was not run. Win% is the
best play's at 3 plies, from MAGPIE now. The last column compares the
engines at the deepest depth Macondo completed; MAGPIE's 2-ply values are
from the deficit evaluation. Where the engines' best plays differ, they
have the same win% (on lost positions every play is 0%).

| Position | Bag | Down | MAGPIE before | MAGPIE now | Macondo | of which tiebreak | Best play, 3 plies | Win% | MAGPIE / Macondo at Macondo's depth |
|---|---|---|---|---|---|---|---|---|---|
| g100371_t17 | 1 | 21 | 25 | 2 | 13 | 0 | D1 S(WO)WND | 87.5 | 87.5 / 75.0 (3 plies) |
| g101353_t16 | 1 | 28 | 16 | 4 | 25 | 0 | 4A JETE | 93.8 | 93.8 / 93.8 (3 plies) |
| g101884_t20 | 1 | 31 | 6 | 1 | 2 | 0 | J1 AZAL(EA) | 87.5 | 87.5 / 87.5 (3 plies) |
| g104331_t19 | 1 | 37 | 4 | 1 | 6 | 0 | 14A BRIEF | 25.0 | 25.0 / 25.0 (3 plies) |
| g106642_t22 | 1 | 26 | 17 | 3 | 8 | 0 | 7M O(E) | 37.5 | 37.5 / 37.5 (3 plies) |
| g107424_t17 | 1 | 23 | 61 | 16 | 91 | 3 | 2F UN(M)UTE | 43.8 | 43.8 / 43.8 (3 plies) |
| g108230_t17 | 1 | 32 | 4 | 2 | 14 | 9 | N12 B(R)E(E) | 0.0 | 0.0 / 0.0 (3 plies) |
| g111545_t21 | 1 | 22 | 655 | 22 | 104 | 9 | 15H cLEFTS | 100.0 | 100.0 / 100.0 (3 plies) |
| g111791_t20 | 1 | 38 | 53 | 11 | 64 | 15 | 4A (PO)T | 62.5 | 62.5 / 62.5 (3 plies) |
| g112315_t22 | 1 | 38 | 10 | 5 | 16 | 10 | A12 OE | 0.0 | 0.0 / 0.0 (3 plies) |
| g100746_t20 | 2 | 34 | 10 | 6 | 436 (70 without) | 368 | 11I V(AS) | 0.0 | 0.0 / 0.0 (3 plies) |
| g104730_t18 | 2 | 23 | 78 | 22 | 223 | 0 | 11C YITES | 26.4 | 26.4 / 26.4 (3 plies) |
| g105295_t16 | 2 | 20 | 36 | 26 | >900 (134 without) | 773 | 1H (V)UG | 0.0 | 0.0 / 0.0 (2 plies) |
| g109781_t20 | 2 | 35 | 63 | 27 | 103 | 1 | 1D (F)ERLIER | 31.9 | 31.9 / 31.9 (3 plies) |
| g110360_t20 | 2 | 28 | 41 | 2 | 7 | 0 | A3 O(R)IENTAL | 100.0 | 100.0 / 100.0 (3 plies) |
| g113383_t19 | 2 | 29 | 373 | 52 | 593 | 0 | 14G B(Y)E | 27.8 | 27.8 / 27.8 (3 plies) |
| g114993_t17 | 2 | 27 | not run | 688 | >900 | 0 | K12 (A)RB | 22.2 | - |
| g116234_t17 | 2 | 31 | 105 | 57 | 541 | 220 | B6 QI | 5.6 | 5.6 / 5.6 (3 plies) |
| g117253_t18 | 2 | 23 | 275 | 50 | 224 | 0 | F10 QI(S) | 58.3 | 58.3 / 55.6 (3 plies) |
| g117714_t18 | 2 | 36 | 599 | 270 | >900 | 0 | B5 UnROUGE(D) | 58.3 | 59.7 / 59.7 (2 plies) |
| g102681_t18 | 3 | 24 | 355 | 130 | - | - | 15K FILES | 84.2 | - |
| g102951_t19 | 3 | 27 | 272 | 178 | >900 | 0 | 8A F(L)IP | 7.9 | 7.9 / 16.7 (2 plies) |
| g104654_t20 | 3 | 27 | 162 | 22 | >900 | 0 | 4B OP(S)ONISE | 95.8 | 96.7 / 96.7 (2 plies) |
| g105659_t23 | 3 | 31 | 274 | 187 | - | - | C9 AEO(N) | 0.8 | - |
| g105869_t19 | 3 | 29 | 108 | 79 | >900 | 38 | B13 ULU | 0.0 | 1.7 / 2.5 (2 plies) |
| g106693_t17 | 3 | 38 | 596 | 50 | - | - | 11G SPUEING | 97.5 | - |
| g107700_t17 | 3 | 33 | >900 | >900 | - | - | - | - | - |
| g108686_t17 | 3 | 35 | 326 | 221 | - | - | N4 FIX | 91.2 | - |
| g110722_t19 | 3 | 31 | 87 | 38 | >900 | 0 | 12G WERGELD | 83.3 | 83.3 / 84.2 (2 plies) |
| g113200_t16 | 3 | 22 | >900 | 647 | - | - | 4B O(NE)R | 26.1 | - |
| g116923_t19 | 3 | 23 | 694 | 314 | - | - | N1 SHISO | 50.8 | - |
| g100532_t16 | 4 | 27 | >900 | >900 | - | - | - | - | - |
| g100738_t16 | 4 | 37 | >900 | >900 | >900 | 0 | - | - | - |
| g107448_t17 | 4 | 24 | 213 | 197 | - | - | 14D MIXY | 96.4 | - |
| g107691_t18 | 4 | 24 | 139 | 47 | >900 | 0 | 14E SPE(I)LING | 95.3 | 95.6 / 95.8 (2 plies) |
| g107749_t20 | 4 | 29 | not run | >900 | - | - | - | - | - |
| g112339_t17 | 4 | 32 | 86 | 56 | >900 | 0 | 3C INBOXES | 93.2 | 93.2 / 93.2 (2 plies) |
| g112931_t17 | 4 | 34 | 65 | 49 | >900 (263 without) | 637 | N7 TOLT(S) | 0.0 | 0.0 / 0.0 (2 plies) |
| g114130_t20 | 4 | 37 | 564 | 428 | - | - | H10 FO(G)IE | 12.3 | - |
| g118912_t17 | 4 | 39 | >900 | 797 | - | - | H1 GL(I)T | 1.2 | - |
| g119959_t19 | 4 | 33 | 184 | 112 | - | - | 4E DRILLIN(G) | 54.1 | - |

Over the 34 positions both versions finished: 6,558 s before, 2,689 s now,
faster on every one, and the same counts on all 289 candidates solved in
full by both. g113200, g114993 and g118912 now finish (647, 688 and 797 s);
g100532, g100738, g107700 and g107749 do not, and Macondo, where it ran
(g100738), did not finish 3 plies either.

## Where MAGPIE was slower, and the fix

Both slow positions have hundreds or thousands of candidates, and a play
that wins 100% found first: g111545 (8,997 candidates, 1 in the bag) and
g110360 (796, 2 in the bag). After the first candidate, every other one is
cut as soon as one draw is not a win.

With early cutoff on (`PegArgs.early_cutoff`, always on in the complete
solve), the stage loop in `peg_solve` evaluated the candidates one at a time
and called `peg_eval_candidates_scenario` for each, which spreads the
candidate's draws over the pool, after ordering them losses first by a
greedy playout. The cut (`PegCutoff`) was checked when a draw's job starts,
and not seen by an endgame already running.

**Threads made it slower.** The old solver, at 3 plies:

| Position | Bag | 1 thread | 2 threads | 10 threads |
|---|---|---|---|---|
| g110360 | 2 | 4.6 s (4.3 s CPU) | 10.3 s (17 s CPU) | 38-41 s (166-180 s CPU) |
| g111545 | 1 | 88 s | 310 s (644 s CPU) | 655 s |
| g107424 | 1 | 62 s | | 61 s |
| g104730 | 2 | 74 s | | 78 s |
| g109781 | 2 | 113 s | | 63 s |
| g110722 | 3 | 155 s | | 87 s |

**What the threads were doing.** Stack samples of g110360 on 10 threads (30
samples of 14 threads, taken with gdb; `perf` is not installed for this
kernel): 64% waiting at the candidate barrier, 22% in endgame search, 6%
building the candidate's pruned leaf KWG, 7% the stdin reader.

**Why the extra endgames cost so much.** Losses first puts the draw the
opponent wins at the front. That endgame is cheap: the opponent's first-win
search stops at its first winning reply. Run alone, it cuts the candidate,
and the other draws are never started. Run on 10 threads, they all start,
and those the mover wins are expensive: the opponent's search must show
that no reply wins. They run to the end, and the candidate's time is the
slowest of them. It was not the injection monitor (off, g110360 took 38.9
s) or the per-candidate pruned leaf KWG (skipped, 37.7 s against 38.4 s).

**Macondo's layout.** With more plays than threads, Macondo gives each
thread one play and runs that play's draws serially, hardest for the mover
first (`processJobPerPlay`), stopping as soon as the play has more losses
than the best play's (`minPotentialLosses`). Each thread usually pays one
cheap endgame per refuted play, and the 10 threads refute 10 plays at once.

**The fix** (`peg_live_solve_cand` and `peg_live_solve_at_once` in
`peg.c`). With a cutoff and more than one thread:

- The seed's best candidate is solved first, alone, every thread on its
  draws, as before. It sets the bar.
- The rest are pool jobs, one candidate each. A job solves its candidate's
  draws in turn on its own thread, losses first.
- The cutoff reads the best finished score live (`PegCutoff.best_score`, an
  atomic the jobs raise as they finish), when a draw is added and before
  each draw and ordering starts. A stale bar only prunes less, so the cut
  stays exact.
- Each finished candidate is published as before, to the live
  leaderboard, the checkpoint and `-pegoutcomes`, under a lock. `status`
  shows how many candidates are being solved at once.
- Idle threads at the end of the stage still join the long endgames that
  remain (the injection monitor).

The answer is the same: the best play, its win%, and every count solved in
full. Which candidates are cut, and the bounds they show, can vary between
runs, since that depends on which better candidate finished first. A unit
test (`test_peg_complete_threads_match`) checks a complete solve on 4
threads against 1.

**The saved games.** The ten 1-in-the-bag games in `example_pegs/`, on 10
threads at 3 plies, before (cdff09e3) and now, capped at 15 minutes. Each
gave the same best play and win% both times.

| Game | Best play | Win% | Before | Now |
|---|---|---|---|---|
| anno61211 | G13 N(U) | 100.0 | >900 | >900 |
| anno61214 | 7H D(A) | 100.0 | 118 | 81 |
| anno61217 | B9 IN(D)UCE | 12.5 | 2 | 1 |
| anno61220 | 2J GUANI(N) | 100.0 | 337 | 46 |
| anno61232 | B1 (L)I | 100.0 | 667 | 416 |
| anno61495 | 4I VU(M) | 0.0 | 1 | 0 |
| anno61521 | C7 AL | 100.0 | 673 | 366 |
| anno61532 | 11J (O)X | 100.0 | 501 | 339 |
| anno61536 | M7 ANTILOG | 50.0 | 9 | 2 |
| anno62192 | 1H (G)RAiNERS | 100.0 | 857 | 60 |

The slow ones have many plays that win every draw. A play tied with the
best is never cut, so each of them is solved in full, its draws all wins,
the expensive kind. On anno61211 every one of the 168 candidates wins every
draw: it finished 134 of them in 15 minutes, against 119 before.

Two variants were tried first, behind environment variables in a scratch
build:
- Candidates at once without the first one alone has no bar for the
  candidates that start together, and a slow best play (WERGELD on g110722
  takes 69 s on one thread) leaves the others to be solved in full: g110722
  took 234 s, g112339 over 300 s.
- Keeping one candidate at a time and stopping its running endgames when
  it is cut helped less: g111545 219 s, g110360 12 s, g117253 148 s. A job
  that does nested solves keeps going after its current endgame is stopped,
  and the threads still sit idle at each candidate's barrier.

## Where Macondo is slower

- **Ordered draws.** Macondo enumerates every ordered draw of the bag
  tiles, with a count for repeated letters (`generatePermutations`): 72 at
  2 in the bag, 720 at 3, 7,920 at 4. MAGPIE enumerates splits, the
  multiset the mover draws and the multiset left in the bag, each weighted
  by its number of orderings (`peg_enum_splits`). The deficit runs had 23
  to 43 unique splits at 2 in the bag, 31 to 196 at 3 and 58 to 948 at 4.
  When the mover's play empties the bag, every order of its draw is the same
  endgame, and Macondo keeps an outcome per ordered draw (`outcomeIndex`,
  `HasLoss`), so the orders are not merged. To sort the draws it also
  generates the opponent's moves once per ordered draw, for every play.
- **Every depth from 1.** Iterative deepening reruns every candidate at 1,
  2 and 3 plies. The earlier depths reorder the candidates, and on g111545
  they cost 74 of Macondo's 104 s. MAGPIE's complete solve runs one stage
  at 3 plies after a greedy seed that takes under a second.
- **The spread tiebreak.** When plays tie for the best win%, Macondo solves
  the top 20 tied plays that empty the bag again with a full window, for
  spread (`maybeTiebreak`), after every depth. On lost positions every play
  ties at 0%. The tiebreak took 368 of 436 s on g100746, 773 s on g105295
  (where the 3-ply PEG itself took 75 s, and then the time ran out), and
  637 s on g112931. Without it (`-skip-tiebreaker true`) Macondo finished
  those in 70, 134 and 263 s. MAGPIE's complete solve computes no spread.
- **Nothing to cut.** On g100746 every candidate loses every draw, so
  neither engine can cut anything. Without the tiebreak, Macondo's 3-ply
  pass solved 11,775 endgames in 49 s; MAGPIE's whole solve took 10 s
  before the fix.

A Go CPU profile of Macondo on g112339 (4 in the bag, 158 s on 10 threads)
puts 81% of the time in endgame search (`negamax`), and 79% in move
generation, mostly within it. The greedy leaf playout alone is 41%: it
generates the full move list at every move it plays. Sorting the draws by
the opponent's best reply (`TopPlayOnlyRecorder`) is 2%, and looking up a
draw's outcome (`HasLoss`) does not show.

## Where the answers differ

The best play agreed on all 26 positions with a result at the same depth
from both. Plays with equal win% count as the same (on lost positions,
every play is 0%). The win% differed on 6. "Macondo, going out" is a copy
of Macondo 67fa64a4 whose playout values going out as MAGPIE's does (below);
"Deeper search" is the value at more plies.

| Position | Bag | Play | Depth | MAGPIE | Macondo | Macondo, going out | Deeper search |
|---|---|---|---|---|---|---|---|
| g100371 | 1 | D1 S(WO)WND | 3 | 87.5 | 75.0 | 87.5 | 87.5 (MAGPIE to the end; Macondo at 6 and 10 plies) |
| g117253 | 2 | F10 QI(S) | 3 | 58.3 | 55.6 | 58.3 | 58.3 (MAGPIE to the end; Macondo at 5 plies) |
| g102951 | 3 | 8A F(L)IP | 2 | 7.9 | 16.7 | 8.3 | 7.9 (both at 3 plies) |
| g110722 | 3 | 12G WERGELD | 2 | 83.3 | 84.2 | 83.3 | 83.3 (MAGPIE at 3 plies) |
| g105869 | 3 | F12 URB | 2 | 1.7 | 2.5 | 1.7 | 0.0 (MAGPIE at 3 plies) |
| g107691 | 4 | 14E SPE(I)LING | 2 | 95.6 | 95.8 | 95.6 | 95.3 (MAGPIE at 3 plies) |

All six plays empty the bag, so each draw is one endgame, with no nesting
or opponent model involved: the difference is in the endgames.

### QI(S) with FU drawn

g117253: the mover plays F10 QI(S) for 32 and draws FU, which empties the
bag. The opponent, on turn, holds EEIIIMZ and trails 378 to 387; the mover
holds EFOORTU. Both engines search 3 plies and finish a line still going at
that depth with a greedy playout.

| Engine | Opponent's best | Value for the opponent | Final spread for the opponent |
|---|---|---|---|
| MAGPIE, 3 plies | F1 (J)IZ | +1 | -8 |
| MAGPIE, to the end (25 plies) | L13 (F)IZ | 0 | -9 |
| Macondo, 3 plies | 14L EI(D)E | +11 | +2 |
| Macondo, going out, 3 plies | F1 (J)IZ | +1 | -8 |

Macondo's line is 14L EI(D)E, 15J ROUT, 2A (A)I, and then the playout. At
that leaf the mover holds EFO against IMZ, and two plays score 23: 2F OF,
and 14H FOE, which goes out and adds twice IMZ's 14 points. Macondo's
playout picks by score alone and keeps the first of equal scores, OF, after
which the opponent goes out with 8L ZIM(B): -6 for the mover where FOE
gives +51. That makes (A)I a strong reply to ROUT, and EI(D)E the
opponent's best play. MAGPIE's playout plays FOE, (A)I is refuted, and the
opponent's best play loses.

The two engines already disagree at 1 ply on the position after EI(D)E,
but there it means nothing: at iterative-deepening depth 1, MAGPIE's leaf
plays one greedy move and then counts both racks twice
(`leaf_playout_is_capped`), by design, where Macondo plays out to the end.
From 2 plies both play out, and the first difference on this line is the
leaf above.

### S(WO)WND with L drawn

g100371: the opponent holds IILOTUV and trails 430 to 437. MAGPIE's 3-ply
search and its search to the end agree: 2J TI, and the opponent loses by 3.
Macondo's 3-ply search plays 2J TIL for a tie. MAGPIE's answer to TIL is
13D EN(D). Macondo gives the opponent 12I IO in reply, which leaves the
mover BL against UV, and two plays score 11: N3 L(O)B, which goes out, and
J10 B(IO), which does not. Macondo's playout takes B(IO), the opponent goes
out with 12L UV(EA), and EN(D) looks like a loss by 6 instead of a win.

### The test

The copy of Macondo adds, in `greedyPlayout`, twice the opponent's rack to
the score of a play that uses every tile, except where the conservation
rule applies, as MAGPIE's playout does (d95eaf85, `go_out_bonus` in
`negamax_greedy_leaf_playout`). With it:
- the endgame above gives MAGPIE's 3-ply result exactly: F1 (J)IZ, +1, the
  same line; S(WO)WND's L draw likewise gives 2J TI, opponent losing by 3;
- the 3-ply PEG gives QI(S) 42 of 72 orderings (58.3%) and S(WO)WND 7 of 8
  (87.5%);
- the 2-ply PEG gives WERGELD 83.3%, URB 1.7%, SPE(I)LING 95.6% and
  F(L)IP 8.3% (60 of 720; MAGPIE 57).

The first pass reported that turning the bonus off in MAGPIE changed
nothing. That test switched it off in only one of the three places it
applies, which does nothing on the path most playouts take, where the move
generator applies the bonus itself (`best_small_out_bonus`). Switched off
in all three, MAGPIE's 3-ply endgame here changes (EI(D)E becomes its best
play, though still a loss for the opponent), but the PEG still gives QI(S)
58.3% and S(WO)WND 87.5%. Without the bonus, MAGPIE still breaks equal
scores differently from Macondo, by its move encoding rather than by
generation order, and on QI(S)'s tie that happens to take FOE, the play
that goes out.

## Smaller observations

- **Macondo's table after a timeout.** When `-maxtime` stops a depth
  partway, the "Winner:" line comes from the last completed depth, but the
  table under it is the unfinished depth's partial counts. On g110722 the
  table gave WERGELD 72.4%; the completed 2-ply result was 84.2%.
- **Macondo's shell default.** `peg` from the shell has early cutoff off
  (`-early-cutoff` is a plain bool option, false when not given), so every
  candidate is solved in full at each depth. Its bot and analyzer turn it
  on.
- **Nesting.** No difference showed. With plays limited to those leaving at
  most one tile, the mover's next turn with tiles still in the bag comes
  only after an opponent pass, so Macondo's one nested level covers almost
  every line. All six win% differences are on plays that empty the bag.

## Files

Scratch, not in the repo, under the session's scratchpad:
- `mvm/`: the Macondo driver (`mdrive.py`, `mdrive2.py`), its log parser
  (`mparse.py`), MAGPIE side runs (`mrun.sh`, `side/`), gdb sampling
  (`pmp.sh`, `stk.py`), and Macondo's output (`out/`: `ec` default, `nt`
  without tiebreak, `gb` and `gb2` going out).
- `rerun/`: this pass's MAGPIE runs and table (`rerun.py`, `newtable.py`)
  and the saved games' runs (`ex.py`, `ex_out/`).
- `qi/`: the endgame traces (`meg.py`, `mag.sh`, `magk.sh`).
- `macx/`: the copy of Macondo (`MACONDO_GO_OUT_BONUS`).

## Next steps

- Find where the time goes on the four positions that still do not finish
  in 15 minutes (3 and 4 in the bag), and on games like anno61211 where
  many plays win every draw and none can be cut.
- Macondo's playout could value going out, or at least prefer a play that
  goes out among equal scores; that is a change to Macondo, not MAGPIE.
