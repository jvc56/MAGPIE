# Better defaults: plan and status

Source: the `magpie_todo.txt` list. This document records what the
`better-defaults` branch changed, what the code does now, and what is left.
It describes the code after c2fad53f, with the work listed under each
phase. Macondo is as of 461e13e8; its PEG and
analyzer defaults have not changed since c1647df2.

## Status

| Phase | Todo items | Status | Commits |
|---|---|---|---|
| Settings files (cross-cutting) | | Done | 99ff5749 |
| 1. PEG: complete, untimed, results-only solves | 1, 4 | Done, with the endgame depth capped (1d); depth measured and validated | 74c9f02f, 39a57e49, ad35bb97, c2fad53f |
| 2. Help text and comments | 5 | Done | 81c4039c |
| 3a. One time limit per solver | 9 | Done | bea4a4d8 |
| 3b. Analyze PEG bag range | 8 | Done; default 1, measured | 74c9f02f, 7e1d15f5 |
| 4. Endgame | 4, 9 | 4b and 4c done; 4a measured and not changed | 74c9f02f, 7e1d15f5 |
| 5. PEG positional args | 3 | Done, with `snoprune` | 74c9f02f |
| 6. PEG `leaves<N>` | 2 | Done | 74c9f02f |
| 7. WMP, WIT and RIT on by default | 4, 5 | Done, with R4 and R6; R8 open | df2172eb, 863f8d42, 8b97360f, fdb9314a |
| 8. Other strength and speed defaults | 4, 7 | First-win cascade stages on; the rest measured, defaults unchanged | 7e1d15f5 |

The open work is listed in [What is left](#what-is-left).

## Settings files

`-savesettings` used to write every setting, so a changed default never
reached anyone with a `settings.txt`. Settings files now record
`-fileversion 2`. A line from an older file (no `-fileversion`) drops the
settings in `config_stale_settings`, so their new defaults apply: `-pegpess`,
`-w1`/`-w2`, `-rit1`/`-rit2`, `-wit1`/`-wit2` and `-ritmmap`. A later change
of default adds its setting to that list and bumps the version.

---

## Phase 1: PEG: complete, untimed, results-only solves

The target user is analyzing their games after a tournament. They have no
clock, and only the result counts: a win by 1 is as good as a win by 100. The
old defaults served an engine on a clock: a 60 s limit, a cascade that cut on
shallow estimates, and spread in the ranking key.

### What `peg` does now

**The time limit picks the strategy (1a).** With no `-pegtlim` (the default,
Phase 3a) and no `-pegtopk`, `peg` runs the complete solve. The greedy seed
only orders the candidates. Then one stage solves every candidate, with:
- first-win endgames (1c), `-pegplies` deep (3 by default; see below);
- exact early cutoff (1d);
- the pessimistic opponent and complete nesting (1e);
- known opponent tiles (1f).

A time limit, or an explicit `-pegtopk`, runs the staged cascade, which
always has an answer from its last completed stage. `config_fill_peg_args`
makes the choice.

**Progress against a known total (1a).** While a solve runs, `status` shows
the candidate in progress and how long it has run. It also shows the
ranking's label (which stage and depth it comes from), a row for each draw in
progress (its endgame depth, root moves done, nodes and helpers), and the
finished draws' results. In interactive mode, `stop`, `status` and `shgame`
(or `s`) are the only commands accepted while a command runs.

**Ranking by results (1b).** Candidates rank by wins + ½ draws, counted
exactly in labeled orderings: a win is worth 2, a draw 1 and a loss 0.
Spread breaks only exact ties, and only when both spreads are known. Results
show wins, draws and losses as separate columns. A first-win solve has no
spreads, so they show as "-".

**First-win endgames (1c).** `PegArgs.first_win` solves every PEG endgame
leaf, outer and nested, first-win. Nested values become -1/0/+1, so a nested
player ranks its plays by wins + ½ draws. The standalone `endgame` command
keeps its full window (Phase 4).

**Sound pruning over plays that leave at most 1 tile (1d).**
- The default candidate set is `leaves1`: plays that leave at most 1 tile in
  the bag, as in Macondo (`-max-tiles-left 1`). `peg all` solves every move,
  and moves in the second positional are always added (Phase 5).
- `PegArgs.early_cutoff` stops solving a candidate once its losses and draws
  prove it can't reach the best finished candidate. Cut candidates show "<="
  their best possible win%. This is Macondo's early cutoff, made exact by the
  first-win verdicts.
- Within a candidate, the draws most likely to lose are solved first, so a
  losing candidate is cut sooner.
- Scenarios are fully enumerated (`-pegstride` 0).

**The endgame depth: `-pegplies`, default 3.** The plan called for solving
every scenario to the end of the game. That proved too slow, so the complete
solve searches its endgames `-pegplies` deep. A line still running at that
depth is finished with a greedy playout, so the result is an estimate.
`-pegplies end` searches to the end of the game, the most the endgame
supports (`PEG_EXHAUSTIVE_PLIES` = `MAX_SEARCH_DEPTH` = 25 plies). The depth
also applies to `-pegtopk all`, to the nested inner endgames, and to
analyze's PEG turns.

The default comes from the saved games in `example_pegs/`, solved on 10
threads.

At 1 in the bag, ten games, each depth capped at 3 minutes and `end` at 10:

| Depth | Finished | Time when finished | Best play against `end` |
|---|---|---|---|
| 1 ply | 10 of 10 | 0.5-59 s | Wrong in 2 of the 3 games `end` finished |
| 2 plies | 10 of 10 | 0.6-151 s, half under 30 s | Same in all 3 |
| 3 plies | 4 of 10 | 0.7-130 s | Same in all 3 |
| 4 plies | 3 of 10 | 1.4-11 s | Same in all 3 |
| `end` | 3 of 10 | 11-13 s | |

- In the three games `end` finished, 2, 3 and 4 plies gave every fully
  solved candidate the same wins, draws and losses as `end`. Two of them
  have close top plays: six plays tied at 12.5%, and two at 50% over others
  at 12.5%.
- 1 ply is too hopeful. It rated (Z)IP 75% where every deeper search gives
  12.5%, and it tied (E)L with the 50% plays when (E)L wins 12.5%.
- In the other seven games, every depth from 2 up chose the same best play
  as far as it got. Six of them win every draw.

At 2 to 4 in the bag, eleven games that 2 plies finished in 5 minutes, with
1 and 3 plies capped at 400 s. `end` can't finish here, so there is no exact
answer to compare against:
- 1 ply took 2-32 s and chose a different best play than 2 plies in 4 of
  the 11, always with a higher win%.
- 3 plies took 38-399 s and stopped at 400 s in 3 more. Where it finished,
  it chose the same best play as 2 plies in 6 of 8. The two it changed were
  a 2-in-the-bag game (NU 11.1% over UM 12.5%) and a 4-in-the-bag game
  (JE(tO)N 36.6% over (S)DEIN 28.5%).

2 plies was the first default: it is the shallowest depth that matched
`end` wherever `end` finished, and the only one that finished every
1-in-the-bag game in 3 minutes. The default is now 3 plies, after the
positions behind by 20 to 40 below showed 2 plies overstating the trailing
player's win%. On these ten games 3 plies costs more: of the six it did not
finish in 3 minutes, one took 330 s and one ran past 15 minutes (2 plies:
39 s and 40 s).

**The opponent model (1e).**
- `-pegpess` defaults to true, and with nesting it defines the opponent at
  every stage (`PegArgs.nested_pessimistic`), not just stage 0. After a play
  that doesn't empty the bag, every opponent reply is tried on the scenario,
  as if the opponent knew the racks and the bag order. The scenario takes
  the worst result for the mover, and stops at the first reply that wins for
  the opponent. This is Macondo's model, and it makes a win% a lower bound.
- The mover's next turn, when the bag still isn't empty, is a nested PEG
  from the mover's view. Its choice is scored on the scenario's actual racks
  (`nested_score_actual`).
- The complete solve nests to depth 24 (`PEG_MAX_NEST_DEPTH`), over every
  inner candidate (up to `PEG_NEST_FIELD_MAX`, 64) and every inner scenario
  (stride 1). Its inner endgames are `-pegplies` deep. The cascade keeps the
  cheaper schedule: depth 1, caps 8/4/2, and inner strides by bag size.
- `-pegndepth`, `-pegncaps` and `-pegnstrides` apply to `peg` and `analyze`
  as well as autoplay. Once given, they override either schedule.
- `-pegpess false` gives a rational opponent. It chooses its reply from what
  it can see, and the choice is scored on the actual racks. In the complete
  solve it chooses by wins + ½ draws, since nested values are -1/0/+1.
- Autoplay's `-pegpess1/2` keep their own default (false).

**Known opponent tiles (1f).** `PegArgs.opp_rack_known` treats the tiles on
the opponent's rack in the position as known: every scenario's opponent
holds them, and only the rest are dealt. `peg` and `analyze` turn it on.
Analyze uses the same opponent rack as its sims. Autoplay and the play
chooser leave it off.

### MAGPIE's default PEG vs Macondo's

This compares the `peg` command's defaults in each engine.

**The same**
- No time limit.
- Candidates: plays that leave at most 1 tile (`leaves1`;
  `-max-tiles-left 1`).
- First-win endgames, a fixed shallow depth, and a greedy finish past it.
  Macondo's endgame now plays greedily past its ply limit instead of scoring
  the spread as it stands.
- The opponent after a play that doesn't empty the bag: omniscient, and the
  worst case for the mover over all its replies.
- The mover's later turn: a nested PEG from its own view, scored on the
  actual racks.
- Ranking by wins + ½ draws.
- Known opponent tiles as an input.
- One list that restricts the solve to given moves, and one that protects
  moves from pruning.

**What differs**

| | MAGPIE | Macondo |
|---|---|---|
| Endgame depth | 3 plies (`-pegplies`), in one stage; `end` = 25 plies | 3 plies (`-endgameplies`), by iterative deepening from 1 |
| Search | One stage over every candidate; the greedy seed only orders them | Iterative deepening over the endgame depth, every candidate at each depth |
| Early cutoff | On | Off from the shell; on in the bot and analyzer |
| Scenarios | Multisets of draws, weighted, fully enumerated | Every ordered draw, solved separately |
| Nesting | Up to depth 24, every inner candidate and scenario | One nested sub-PEG; deeper lines skipped (`-max-nested-depth 1`); no passes inside (`-skip-deep-pass true`) |
| Spread | Breaks exact ties only when known; the complete solve computes none | A full-window tiebreak pass over the top 20 tied plays that empty the bag |
| Bag sizes | 1 to 4; 5 to 7 when every candidate leaves at most 4 | 1 to 6, with any play |
| Opponent model choice | `-pegpess false` gives a rational opponent | Fixed |
| Known opponent tiles | The opponent's rack in the position, always | `-opprack` |
| Move lists | A set keyword (`leaves1` by default, `all`, `empty`, `leaves<N>`, `only`), then moves added and protected | `-only-solve` restricts and `-avoid-prune` protects, both repeatable |
| Results shown | Every candidate, with W/D/L columns and per-draw outcomes | The top 30 (`-maxsolutions`) |
| With a time limit | The staged cascade | The same algorithm, stopped at the deadline |
| In game analysis | PEG at 1 in the bag (`-apegmaxbag 1`), unlimited, 3 plies, early cutoff on | PEG at 1 in the bag only, 4 plies, early cutoff on, 5-minute timeout |

### Depth, search and nesting when behind by 20 to 40

The endgame depth, search and nesting rows above, measured on positions
where the player on turn trails by 20 to 40 points. 20,000 static
(top-equity) self-play games in CSW24 had 1,807 such turns with 1 to 4 in
the bag, in 1,791 of the games. One turn per game was kept, and positions were taken in
rounds of four: one from each deficit bin (20-24, 25-29, 30-34, 35-40),
with the bag sizes rotated so every round also covers 1 to 4 in the bag.
The opponent's rack was taken out. The run covered 41 positions in 6 hours
36 minutes:

| In the bag | Positions | Deficits |
|---|---|---|
| 1 | 10 | 21-38 |
| 2 | 10 | 20-36 |
| 3 | 11 | 22-38 |
| 4 | 10 | 24-39 |

Each position ran the complete solve at 1, 2 and 3 plies. At 2 or more in
the bag it also ran with one nested level (`-pegndepth 1`), Macondo's
`-max-nested-depth 1`. Every run was capped at 15 minutes, on 10 threads.
3 plies was skipped where 2 plies hit the cap. Where 2 and 3 plies picked
different plays, a solve to the end of the game, or else at 4 plies,
decided. The best play is contested (strictly between 0% and 100% at 2
plies) in 33 of the 39 that 2 plies finished.

**Endgame depth: 2 plies against 3.**

| | 2 plies | 3 plies |
|---|---|---|
| Did not finish in 15 minutes | 2 of 41 | 7 of 41: 5 stopped, 2 skipped after 2 plies stopped |
| Median time when finished, 1/2/3/4 in the bag | 11/24/44/59 s | 16/79/275/162 s |
| Time against 2 plies | | 1.4-11.8 times, median 2.7 |

- Both finished on 34 positions. They chose the same best play, or one tied
  with it, on 33. On the other (3 in the bag, down 24), 2 plies chose
  SKE(E)F 89.6% over FILES 88.3%. 3 and 4 plies both chose FILES at 84.2%,
  with SKE(E)F at most 83.8%.
- 2 plies is too hopeful for the trailing player. Its best play's win% was
  higher than at 3 plies on 15 of the 34, and never lower: by 2.1 points on
  average, and by up to 12.5. A solve to the end of the game checked three
  of these at 1 in the bag, and agreed with 3 plies each time: 87.5% where
  2 plies gave 100%, and 0% (GO(E), OE) where 2 plies gave 12.5%.

**Search: one stage against iterative deepening.** Macondo's iterative
deepening to its 3 plies, under a 15-minute limit, was emulated from the
1-, 2- and 3-ply runs: it returns the deepest depth whose runs, added up,
finish in 15 minutes. This leaves out Macondo's reuse of the previous
depth's ordering, so its times are an upper bound.
- It reached 3 plies on 33 positions, 2 on 6, 1 on 1, and nothing on 1.
  Where the one-stage 2-ply solve also finished, it chose the same play,
  or one tied with it, on all but the SKE(E)F position above, where its
  choice was the better play. On the position where
  only 1 ply finished, the 2-ply solve stopped too, with K12 (A)RB, the
  1-ply choice, its leader.
- Reaching 3 plies took 1.1-1.9 times (median 1.46) as long as a single
  3-ply stage.
- The 1-ply stage is a poor fallback: its best play was worse at the
  deepest finished depth on 12 of 39 positions, and its win% was too high
  by 14.7 points on average (median 4.4). It rated BR(U)NT 84.2% where 2
  plies chose O(NE)R at 23.5% and gave BR(U)NT at most 13.8%.

**Nesting: complete against one nested level**, at 2 to 4 in the bag
(31 positions):
- Both finished on 29. They chose the same best play, at the same win%,
  on all 29.
- 729 plays were solved in both runs; 2 differ. At 4 in the bag, LIR(A)
  wins 20.3% with complete nesting and 18.2% with one level, and
  W(E)E(V)I(L) 1.2% against 1.4%.
- Times were about the same where both finished: one level took 0.48-1.01
  times as long, median 0.99.
  Both stopped at 15 minutes on the two slowest positions. One level was
  faster there: 409 candidates solved against 27 at 2 in the bag, and
  LIR(A) solved in 343 s against 569 s at 4.

**Conclusions.**
- The default is now 3 plies. 2 plies chose the best play in 33 of 34,
  but it overstates the trailing player's chances, and 3 plies gives the
  exact win% more often, at a median 2.7 times the cost. `-pegplies 2`
  gives the quicker estimate.
- Iterative deepening only adds a fallback when time runs out. Its 1-ply
  answer is often wrong, and the complete solve has no time limit. A
  time limit picks the staged cascade, which already keeps its last
  completed stage.
- Complete nesting changed no best play and cost nothing in 29 of 31
  positions. On the two slowest, `-pegndepth 1` solved many more
  candidates in the same time.

### Validation

- **Wall time.** See the depth tables above, and 3b for 2 to 4 in the bag.
  Before `-pegplies`, a solve to the end of the game took 27-48 s at 1 in
  the bag (over 10 minutes on one lost position) and over 20 minutes at 2,
  3 and 4 (7e1d15f5).
- **The on-demand test `pegvalidate`** solves committed fixtures
  (`notes/peg_positions`), with the opponent's rack taken out, in variants
  of the complete solve at 2 plies (it sets `-pegplies 2`, below the
  default, to keep its run time down). It ran two random
  positions with 2 in the bag and ten with 1, on 12 threads, in 1 hour 41
  minutes. A third 2-in-the-bag fixture is left out: its rational solve ran
  over 95 minutes.
  Every check passed:
  - **Early cutoff** on and off give the same best play, or one tied with
    it, with the same win%: 12 of 12 positions.
  - **First-win against full window.** Every play that empties the bag has
    the same wins, draws and losses in a first-win solve as in a
    full-window one (the old `-pegtopk all`): over 25,000 plays at 1 in the
    bag. The 2-in-the-bag positions skip this check: those plays lead to the
    same endgames at any bag size, and the full-window solve's nested PEGs
    ran over 45 minutes on one position.
  - **Pessimism.** No play that leaves tiles has a higher win% against the
    pessimistic opponent than against the rational one: 108 plays at 2
    in the bag, and at 1 the pass, the only such play. `test_peg_main_opp_models` also checks
    the spreads on a 4-in-the-bag position.
  - Times: early cutoff made the solve 1.1-8.3 times as fast. A
    full-window solve took 1.4-5.3 times as long as a first-win one without
    cutoff, up to 734 s at 1 in the bag. The rational opponent took 43-65
    times as long as the default at 2 in the bag, with no cutoff.

---

## Phase 2: Help text and comments (done)

- `-wmp`: on by default only in the command line program; off for
  `magpie_create()`, the browser build and browser games. Phase 7 has since
  made it `auto`.
- `-pegoutcomes`: the default is true.
- `snoprune`: the help examples parse (the rack is followed by a comma).
- `peg`: documents `resume` and when checkpoints are kept.
- `analyze`: covers static, PEG, endgame and sim turns.
- The `peg empty` error no longer uses the old name `pegonly`.
- `PegArgs.scenario_stride`: 0 is full enumeration.
- `endgame_time.c/h` deleted: `endgame_compute_turn_limits` had no callers.
- README: a Defaults section for WMP, RIT, WIT and PAT per entry point.

---

## Phase 3: Time limits and the analyze PEG range

### 3a. One independent time limit per solver (done)

| Solver | Flag | Default |
|---|---|---|
| Sim | `-tlim` (per player in autoplay: `-tlim1`, `-tlim2`) | 60 s |
| Endgame | `-etlim` | 0: unlimited |
| PEG | `-pegtlim` | 0: unlimited |

- No limit falls back to another. `peg` no longer falls back from
  `-pegtlim` to `-tlim`. `analyze`'s endgame turns no longer fall back from
  `-etlim` to `-tlim`, and its PEG turns follow `-pegtlim`.
- Inference has no time limit. When a sim runs inference first, `-tlim`
  starts after it.
- For PEG, a time limit also selects the cascade (1a).
- The browser client sends `-pegtlim` and `-etlim` itself
  (`wasmentry/analysis-model.mjs`).
- Help and README say 0 is unlimited.

### 3b. The analyze PEG bag range (done)

`-apegmaxbag` (0 to 4) is the largest bag analyze solves with PEG; 0
disables PEG in analyze. Bags above it go to sim.

**The default stays 1.** These figures are at 2 plies; the default is now
3 (Phase 1), which takes longer. On 10 threads, the 2-ply complete solve took
0.6-151 s at 1 in the bag in all ten saved games. At 2 to 4 in the bag, ten
games each, it took 5-288 s when it finished, and ran past 5 minutes in 13
of the 30: 4 at 2 in the bag, 4 at 3 and 5 at 4. Analyze solves every
pre-endgame turn in a game, so one 2-in-the-bag turn could hold it up for
many minutes. Its help and the comment beside the default give these
figures.

---

## Phase 4: Endgame

The standalone `endgame` command keeps its full window and never becomes
first-win by default: a user who runs it wants the best move and its exact
spread. First-win is used only where the result alone is needed: inside PEG
(1c) and in analyze's endgame turns (4c).

### 4a. The standalone `endgame` command's depth (measured; not changed)

`-eplies` stays 6. On full-rack endgames, a 6-ply full-window solve took
5-80 s or more, and 25 plies over 2 minutes (7e1d15f5). Macondo's `endgame`
defaults to 4 plies.

### 4b, 4c. Analyze's endgame turns (done)

- They search to the end of the game: 25 plies (`MAX_SEARCH_DEPTH`), unless
  `-eplies` is given.
- They are solved first-win, which took under a second to 25 plies, and
  graded on the result alone: a win turned into a draw or a loss, or a draw
  turned into a loss.
- `-aegspread` restores the full-window solve, with spread lost shown.
- A first-win root decided by a stored bound keeps the previous depth's
  line. When there is still no line and the played move was not best,
  analyze falls back to a full-window solve.

The endgame never searches deeper than `MAX_SEARCH_DEPTH`: a deeper request
is clamped to it (c2fad53f). Its per-ply arrays hold that many entries, and
a 40-ply request from PEG used to write past them.

---

## Phase 5: PEG positional args (done)

`peg` takes up to two positionals:
1. **The candidate set**: `all`, `empty`, `leaves<N>` (Phase 6), `only` or
   `resume`. Without it, the set is `leaves1`.
2. **A move list**, optional and comma-separated: moves added to the set and
   protected through every stage. With `only`, the list is the set.

```
peg                          plays that leave at most 1 tile (= leaves1)
peg all                      every move
peg empty                    every play that empties the bag (= leaves0)
peg empty 8D.WORD            those, plus 8D.WORD, protected
peg leaves2 8D.WORD,11J.MEH  plays that leave at most 2 tiles, plus these two
peg only 8D.WORD,11J.MEH     exactly these two moves
peg resume                   resume a checkpointed solve
```

- Errors: a move list in the first position (`peg 8D.WORD`), `peg only`
  with no moves, `peg resume` with moves, and exchanges in the list.
- `-pnoprune` is removed. Giving it is a parse error that points to the
  second positional.
- The browser client sends `peg leaves1 <moves>`.

`snoprune` accepts the period format too: `snoprune 8G.QI,8H.QI`, with no
rack, or after the opponent's rack as before (`snoprune ABC,8G.QI`).

---

## Phase 6: PEG `leaves<N>` (done)

- `leaves<N>` selects moves that leave at most N tiles in the bag, the same
  rule as Macondo's `-max-tiles-left N`. `empty` is `leaves0`.
- A pass leaves the whole bag, so it qualifies when N is at least the bag
  size: at 1 in the bag, `leaves1` includes it.
- A bag of 5 to 7 is accepted when every candidate leaves at most 4
  (`PEG_MAX_BAG`), so each candidate leads to a supported inner PEG.

---

## Phase 7: WMP, WIT and RIT on by default when the machine has room

All three are speed structures: none changes which move is chosen.

| Table | Size (CSW24) | Build peak | Gain (`tablebench`, CSW24, 4 threads) |
|---|---|---|---|
| WMP | 179 MB | ~710 MB | 3-4x movegens/s on its own |
| WIT | 122 MB | ~310 MB | +9% simmer with a WMP; nothing without one |
| RIT | 1.9 GB | ~2.4 GB | +17% simmer, +6% static with a WMP; all three together +49% simmer over WMP alone |

### Done

- **`auto`.** `-wmp`, `-wit` and `-rit` (and the per-player forms) take
  `auto`, `true` or `false`. `auto` is the default for the command line
  program and `magpie_create()`. It uses the table when the machine has room
  for it, building it first if needed. `true` requires the table, and
  `false` never uses it.
- **Who decides.** `contribute_choose_tables` decides, in value-per-byte
  order (wordmaps, then word info tables, then rack info tables), with this
  budget:
  - disk: free space on the data directory's filesystem, less the larger of
    10 GB and 1% of it;
  - memory: available memory, less a 1 GB reserve and the search table
    (`-ttfraction` of physical memory), capped at a quarter of physical
    memory. Where available memory can't be read, the quarter alone.
- **macOS** reports available memory (free, inactive and purgeable pages),
  in src/compat.
- **First-use builds.** A missing wordmap, word info table or rack info table
  that `auto` chose is built on first use, for data from `data fetch`. A
  missing wordmap is built first and charged to the RIT's budget. Before
  each build, MAGPIE prints a line naming the table, the disk it takes and
  its peak build memory. The RIT size is exact; the wordmap is estimated at
  700 bytes a word and the WIT at 21 times the `.kwg`.
- **The report.** Each lexicon load prints one line, e.g. "CSW24: using
  wordmap, using word info table, using rack info table". A table left out
  gives its reason: "skipped: X needed, Y free", or "not built (it requires
  a wordmap)". A missing table names the convert command that builds it.
- **RIT naming.** A RIT is named for its (lexicon, leaves) pair:
  `<lexicon>.<leaves>.rit`, as contribute names it, or `<lexicon>.rit` for
  the lexicon's own leaves. So `-k1 custom` no longer picks up the stock
  leaves' table.
- **`-ritmmap`** defaults to true on little-endian machines.
- **Sticky WMP-off fixed.** A lexicon with no wordmap no longer turns the
  wordmap off for later lexicons.
- **Saved settings.** `-savesettings` writes the setting chosen (`auto`,
  `true` or `false`), not the loaded state.
- **Leavegen** still forces the RIT off, since its KLV changes every
  generation.
- **R4: automatic builds are managed.** Each table the CLI builds on first
  use is recorded in `auto_tables.txt`, a manifest in the data directory
  kept apart from contribute's, so neither evicts the other's tables. Every
  load touches the tables it uses. Before a build, the tables built on first
  use and unused the longest are deleted until the build fits on the disk
  with the same reserve the chooser keeps. After a build, they are deleted
  until the recorded tables total at most 8 GB. A table the current load
  uses is never deleted, and each deletion prints a line naming the table.
  `contribute_tables_trim_to` does the trimming, on the same
  least-recently-used order as contribute's eviction.
- **R6: a RIT records its inputs.** Version 13 ends with the hashes of the
  lexicon (KWG) and leaves (KLV) it was built from, after the entries, so
  the body is unchanged. A load checks them: under `auto`, a mismatched
  table is left out with a line saying to delete it, and under `true` the
  load fails (`ERROR_STATUS_RIT_INPUTS_MISMATCH`). Older tables have no
  hashes and still load. `RIT_BUILDER_VERSION` is 2, so the pinned RIT
  hashes for contribute must be pinned again before contributors on this
  version run RIT jobs.

### Per entry point

| Entry point | Default |
|---|---|
| CLI | `auto` for all three |
| libmagpie `magpie_create()` | `auto` |
| WASM, browser_game.c | WMP installed by the JS. RIT and WIT off: 32-bit address space, no available-memory figure, and the browser stopped building WITs (#829) |
| `contribute` | Unchanged: the task decides, and `settings.txt` is ignored |
| Tests | What each test states |

### Open

- **R8.** Benchmark each table on and off. The off state must measure ~0
  against main.

---

## Phase 8: Other strength and speed defaults

The first three rows were measured on 12 threads with CSW24 fixtures
(7e1d15f5); the rest are described below the table.

| Change | Result | Default now |
|---|---|---|
| First-win at the timed cascade's stages before the last, full window at the last, candidates tied at a cut line kept | Untimed at 2 in the bag: 98 s and 311 s against 189 s and 754 s, same best move | On for the cascade (`first_win_intermediate`) |
| Skip cascade stages that cut nothing | 186 s against 189 s | Off (`skip_noncutting_stages`) |
| Early cutoff in the cascade's last stage | 192 s against 189 s | Off (`final_stage_cutoff`) |
| `-pegstride` by bag size in the cascade | With `-pegtlim 60`: same best play in 6 of 10; better in the 2 contested ones that differed (below) | 0 (full enumeration); stride 5 at 3 in the bag and 7 at 4 is worth a larger test |
| Bots solve endgames | +10.2 spread per pair (95% CI 8.1-12.2), 52.6% of games (200 pairs); single endgames took hours | `-eplies1/2` 0 (off) |
| Bots also solve PEGs at 1 in the bag | Over endgames alone: changed play in 13 of 100 pairs; +1.9 spread per pair (95% CI 0.0-3.9), 50.75% of games; +14.9 per pair in the 13 | `-pegbag1/2` 0 (off). Macondo's elite bot: PEG at 1 in the bag only |
| `-ttfraction` 0.5 | 2-ply complete solves at 1 in the bag: 373 s against 364 s, same results | 0.25 (0.125 in WASM). Macondo: 0.20 |
| `-smargin` on | 49.25% of games, +5.5 spread per pair (95% CI -5.4 to 16.3; 200 pairs) | Off |
| PEG `inner_top_k` in the CLI | Only if nested PEG becomes a bottleneck | 0 (API only) |

How these were measured (10 threads, CSW24 example games for PEG, CSW21
for game pairs, seed 42):
- **`-pegstride`.** Timed cascades of 60 s on six 3-in-the-bag games
  (stride 5 against 0) and four 4-in-the-bag games (stride 7 against 0),
  all ones the 2-ply complete solve finished. Both reached the same stage in
  9 of 10; most finished only stage 0. Four best plays differed. Two were
  between plays that lose every draw. In the other two, the stride's pick
  ranked higher in the complete solve: EAT (the complete solve's best,
  95.4%) over (V)I(L)E (at most 93.9%), and SE(Y) (at most 6.9%) over
  STE(Y) (at most 4.2%).
- **Bots.** Equity players (`-numplays 1`). First player 1 with
  `-eplies1 6` against none; then both with 6-ply endgames and player 1
  also with `-pegbag1 1`, which runs the bot's cascade (`-pegtopk1`
  32,16,8,4,2). With no time limit, a bot's endgame can stall a game: one
  took about 2 hours on its thread, and one in the PEG run over 3.
- **`-ttfraction`.** The table is the largest power of two that fits, so on
  this 16 GB machine 0.25 gives 2 GB and 0.5 gives 4 GB. Ten 2-ply complete
  solves at 1 in the bag gave the same answers, 2% slower with the larger
  table.
- **`-smargin`.** Both players simulate 2 plies, 10 plays, 300 iterations;
  player 1 with `-sm1 true`.

These stay on unless data says otherwise: `-sinfer`, `-sbagcycle`,
`-pegnested`, `-ebestreply`, and `-threads` = all cores.

Quality changes are validated with game pairs:

```
set -lex CSW21 -wmp true <variant flags> ...
autoplay games N -gp true -threads 12 -seed 42
```

The game pairs above ran 200 pairs, or 100 for PEG. That shows spread
differences of a few points, but a win% difference under about 5 points is
within noise.

New on-demand tests: `pegcascade` (the cascade refinements), `egdepth`
(first-win endgames to 25 plies) and `pegvalidate` (Phase 1's checks).

---

## What is left

1. **R8 (Phase 7).** Benchmark each table on and off; the off state must
   measure ~0 against main.
2. **`-pegstride` by bag size.** Stride 5 at 3 in the bag and 7 at 4 gave
   an equal or better answer than full enumeration in 10 timed cascades.
   A larger set, or longer limits, would show whether to make it the
   cascade's default.
3. **Bot endgames have no time limit.** A 6-ply endgame held one autoplay
   game for 2 hours, and another for over 3, each on one thread. A depth
   or time cap is worth adding before bots solve endgames by default.
4. **The table report leaves out forced tables.** With `-wmp true` and the
   other tables `auto`, the load line says "no tables" although the wordmap
   is loaded: the line lists only the tables `auto` chose.
