# MAGPIE PEG vs Macondo PEG

How MAGPIE's pre-endgame solver compares with Macondo's, and how their
defaults differ. Checked against MAGPIE at 1a73c928 and Macondo at c1647df2.
File:line references are MAGPIE unless marked otherwise. The default changes
this comparison led to are in [better-defaults-plan.md](better-defaults-plan.md).

## How the two solvers work

**Bag sizes**
- MAGPIE: 1 to 4 tiles (`PEG_MIN_BAG`/`PEG_MAX_BAG`, src/def/peg_defs.h:12-13).
  Bags of 5 to 7 are accepted only when every candidate empties the bag
  (peg.c:3031-3065), e.g. `peg empty`.
- Macondo: 1 to 6 tiles (`InBagMaxLimit`, preendgame/peg.go:36, checked at
  peg.go:731).

**Which candidates are solved**
- MAGPIE: every generated move, or the `peg <moves>` / `peg empty` list.
- Macondo: **by default only plays that leave at most 1 tile in the bag**
  (`-max-tiles-left 1`, peg.go:515, peg_generic.go:736, 809). At 4 in the
  bag that means plays of 3 or 4 tiles. Passes and shorter plays are
  skipped. `-max-tiles-left -1` solves everything, and 0 solves only plays
  that empty the bag. `-only-solve` moves bypass the filter.

**Bag scenarios**
- MAGPIE groups draws into multisets. For each split of the unseen tiles into
  (the mover's draw, the rest of the bag, the opponent's rack) it computes one
  weight from binomials, then shares that weight among the orderings of the
  rest of the bag (peg.c:2008-2205). `-pegstride` can sample about 1/stride
  of the scenarios at bag 3 or more, reweighted to keep the expected total.
- Macondo enumerates every ordered draw (`generatePermutations`,
  peg_generic.go:1619) and solves each one separately.
  - When there are fewer plays than threads, it parallelizes over
    (play, ordering) pairs, ordered hardest-for-us first so losses surface
    early.
  - It caches nested solves by information state.

**Evaluating a candidate**
- MAGPIE runs a staged cascade:
  - Stage 0 plays out every candidate in every scenario with a greedy
    playout.
  - Stages 1 to 5 keep the top 32, 16, 8, 4 and 2 candidates and re-solve
    them at endgame depth 2, 3, 4, 5 and 6 plies (peg.c:63, 3417-3419).
  - `-pegtopk all` replaces the cascade with one exhaustive stage at 40
    plies.
- Macondo solves every remaining candidate in full, inside iterative
  deepening over endgame plies 1 to `-endgameplies`, default **3**
  (peg.go:510, 648-651).

**Endgame searches inside PEG**
- Both engines **finish the game greedily at the search horizon** instead
  of scoring the position as it stands:
  - MAGPIE: `negamax_leaf_value` calls `negamax_greedy_leaf_playout`
    whenever heuristics are on (endgame.c:2519-2535), and PEG turns them on.
  - Macondo's negamax uses a translation of the same code (26f43470).
    Measured there: greedy finishing cuts the error at a fixed depth from
    197 to 20 over a 3×3 grid of positions and depths, and one position is
    solved at 5 plies instead of 13.
  - That accuracy is Macondo's stated reason for a PEG endgame depth of
    only 3 (3c54b19f).
  - So a 2-6 ply search in either engine is a good estimate of the final
    spread, not the spread at the horizon.
- After a play that empties the bag:
  - MAGPIE runs a full-window search (`first_win=false`) to the stage's
    depth (peg.c:1877-1935).
  - Macondo runs a first-win search (`SetFirstWinOptim(true)`, peg.go:842)
    that answers only win, draw or loss. Its commit message gives the
    reason: "the pre-endgame wants the sign of thousands of cheap solves"
    (3c54b19f).

**Plays that don't empty the bag (including pass)**
- MAGPIE:
  - At stage 0, a greedy playout.
  - At stages 1 and later, a nested inner PEG for the **opponent's**
    pre-endgame, from the opponent's point of view. Nesting is on by
    default, at depth 1 with an inner cascade of {8, 4, 2} (config.c:4723).
  - With `-pegnested false`, a greedy or pessimistic rollout instead.
- Macondo:
  - The **opponent's** replies are listed flat on the fixed bag ordering,
    and the scenario is a loss as soon as any reply makes it one
    (`iterateOppReplies`, peg_generic.go:1151-1185). This opponent
    effectively knows the bag and is worst-case for us.
  - If the opponent doesn't empty the bag either, **our** next turn is a
    nested sub-PEG under imperfect information (`nestedOurTurnSolve`,
    peg_generic.go:1191-1222).
  - `-max-nested-depth 1` (default): lines that would need a second nested
    sub-PEG are skipped. The help text calls this "a slight optimistic
    approximation".
  - `-skip-deep-pass true` (default): no passes inside nested sub-PEGs,
    except a pass back after a pass (peg_generic.go:1333).
- See [the opponent model in detail](#the-opponent-model-in-detail).

**Opponent model**: summarized here, and explained in
[the opponent model in detail](#the-opponent-model-in-detail).
- The two engines model **opposite sides** realistically:
  - MAGPIE models the **opponent** as a player who can't see our rack.
  - Macondo models the opponent as omniscient and worst-case, and models
    **our own** later turn as the player who can't see.
- After plays that empty the bag, both play a perfect-information endgame.

**Pruning**
- MAGPIE prunes only by top-K halving.
- Macondo has these prunes:
  - **Early cutoff.** A play is abandoned once its losses exceed the best
    play's potential losses (peg_generic.go:289-310). `Init` turns it on
    (peg.go:514), but the shell turns it **off** unless `-early-cutoff true`
    is passed, because `Bool()` is false when the flag is absent
    (shell/api.go:855, 926). The elite bot and the game analyzer turn it on.
    `numCombos` ignores `-opprack` (peg_generic.go:289).
  - **`-avoid-prune`**: listed moves are never cut off by early cutoff.
  - **Skip-loss** (`-skip-loss`, peg_generic.go:691) is optional.
  - **Max-tiles-left**, above, works as a candidate filter.
  - **Loss-first ordering**: scenarios are ordered so losses are found
    early.

**Ranking and ties**
- MAGPIE scores a tie as half a win and ranks by
  `win_pct + 1e-4 * mean_spread` (peg.c:2358-2370), with `win_pct` from 0 to
  1 (peg.h:374). One point of mean spread is therefore worth 0.01 percentage
  points of win%, so 100 points equals 1 pp. Spread is not a pure tiebreaker:
  a large enough spread edge can outrank a small win% deficit. See
  [first-win](#first-win-does-magpie-have-it).
- Macondo also scores a draw as half a win (peg.go:148-158). It computes
  spread only in a final tiebreak pass, with a full window, over the plays
  tied for first:
  - only plays that empty the bag;
  - the top 20 by score (`TieBreakerPlays`, peg.go:37);
  - plus any `-avoid-prune` play that empties the bag
    (peg_generic.go:495-525).
  - Its help text says 10, which doesn't match the code.

**Opponent tiles already known**
- MAGPIE ignores them. It sets the opponent's rack to the unseen tiles minus
  the bag (peg.c:676-700).
- Macondo accepts a full or partial rack with `-opprack`.
  - Its game analyzer fills this in from a challenged-off phony
    (gameanalysis/analyzer.go:968-975).
  - The elite bot fills it in only when `UseOppRacksInAnalysis` is set, an
    analysis-mode cheat (ai/bot/elite.go:210-214).

**Exchanges**: neither engine generates them, since the bag is too small.

## First-win: does MAGPIE have it?

**Short answer.** The endgame solver supports it, but PEG never turns it on.

- `EndgameArgs.first_win` searches the narrow window [-1, +1]. The search
  then returns only win, draw or loss, and alpha-beta cuts off far more
  (endgame.h:205-208).
- A more general version, `use_initial_window` with `initial_alpha` and
  `initial_beta`, answers whether the value is above or below any threshold
  (endgame.h:216-229). Only the play chooser uses it (play_chooser.c:587).
- PEG passes `first_win=false` and `use_initial_window=false` in both of its
  endgame calls:
  - the outer leaf (peg.c:1892-1893);
  - the nested leaf (peg.c:1408-1409).

**It has been measured, in a test harness.** Commit 7431b0c0 (2026-06-11)
set `first_win=true` in the pessimistic full-search harness
(test/peg_pess_test.c). The win/draw/loss results came out the same, each
solve was **about 3x faster**, and the endgame TT hit rate went from about
36% to about 99.7%. The production solver in src/impl/peg.c never adopted
it.

**Macondo relies on it.**
- Every PEG endgame is first-win (peg.go:842). Full spread is computed only
  in the final tiebreak among tied plays.
- Its quick endgame for autoplay also uses the (-1, 1) root window
  (9e66225d).
- Its reason for a shallow PEG depth applies to MAGPIE too: "the
  pre-endgame wants the sign of thousands of cheap solves ... while
  `endgame` is one deliberate solve where the point is a proven answer"
  (3c54b19f).

**What PEG uses exact spread for today.** Three things:
1. **The ranking key**, both at each cut and in the final answer. Because the
   key is `win_pct + 1e-4 * mean_spread`, spread matters wherever two
   candidates' win% differ by less than (spread difference × 0.01 pp). At
   1 in the bag, win% moves in coarse steps (one draw of 8 unseen tiles is
   12.5 pp), so spread mostly breaks exact ties. At 3 or 4 in the bag the
   steps are much finer, and spread can reorder candidates.
2. **The nested opponent's choice.** The inner PEG picks the opponent's move
   by highest *mean spread*, not win% (peg.c:1655-1662, 1784-1800). That
   needs real spreads from its own leaves.
3. **The displayed mean spread** in the results table.

**Stages that cut nothing.**
- Stage *s* keeps `min(live_count, counts[s-1])` candidates
  (peg.c:3420-3424). When there are fewer live candidates than the count,
  nothing is cut, and the stage's ranking doesn't affect what survives.
- Example: `peg 8D.WORD,11J.MEH,1F.VENeY` with the default 32,16,8,4,2.
  Stages 1 to 4 (2 to 5 plies) all keep all three moves. Only stage 5
  (6 plies) cuts to two. The same happens whenever the generated move list
  is short, or `peg empty` leaves only a few moves.
- What such a stage still provides:
  - **An answer if time runs out.** On a deadline, the last completed stage
    is returned (peg.c:3436-3438, 3495-3537). With an unlimited default (the
    plan's Phase 3a), this disappears.
  - **A warm transposition table.** All workers share one endgame TT across
    stages, so a deeper stage reuses the shallower one's work (peg.c:95-101).
    This is the same reason iterative deepening pays off, but it is partial
    reuse, not free.
  - **Live progress** in `shpeg`, the WASM client and the JSON output.
- Even at a stage that does cut, the cut is decided by win% except near the
  cut line. A first-win solve gives exactly the same win/draw/loss for each
  scenario at that depth, so it gives the same win%. It only loses the
  spread term of the key.

**Ideas** (each needs a benchmark, and a check that decisions don't change):
1. **Skip stages that cut nothing** when there is no deadline: jump to the
   next stage that cuts, or to the final stage. This is the cheapest change.
2. **First-win at the intermediate stages, full window at the final stage.**
   Intermediate stages rank by win% alone. The final stage keeps exact
   spread, so the answer and the displayed spread are unchanged.
   - Risk: candidates tied on win% at a cut line are then cut arbitrarily.
     Mitigation: keep all candidates tied at the cut line (the cut grows a
     little), or re-solve only the tied ones with a full window. The second
     is what Macondo's tiebreak pass does.
3. **A threshold window instead of [-1, +1].** With `use_initial_window`,
   use a null window at a spread threshold (e.g. "does this win by 30 or
   more?"). Useful only if spread at the cut line is worth keeping.
4. **Leave the nested opponent alone at first.** It maximizes mean spread,
   which needs a full window. Changing what the opponent maximizes is a
   model change, not a speedup (see the next section).
5. **Settle the spread weight.** 1e-4 was chosen as a tiebreaker. If it is
   meant as a pure tiebreaker, compare win% first and spread second. Then
   spread only matters for exact ties, and ideas 1 and 2 become simpler and
   safer.

## The opponent model in detail

Terms used below:
- The **mover** is the player PEG is choosing a move for.
- A **scenario** is one way the unseen tiles can be split: which tiles the
  mover draws after the candidate, the order of what's left in the bag, and
  therefore the opponent's rack (peg.c:733-786).
- Within one scenario, everything is known. A playout or endgame evaluated
  "on the scenario" sees both racks and the bag order.

The opponent model is what the opponent does in the game PEG imagines after
the mover's candidate. MAGPIE's answer depends on the stage and on whether
the candidate empties the bag.

**Case 1. The candidate empties the bag (stages 1 and later).**
- With the bag empty, both racks are known, so the rest of the game is a
  perfect-information endgame.
- PEG searches it with alpha-beta to the stage's depth (peg.c:1877-1897).
  Both sides play their best moves within that depth.
- Here there is no "model" to choose. Rational, pessimistic and Macondo all
  agree, apart from search depth (MAGPIE 2 to 6 plies, Macondo 1 to 3 by
  iterative deepening). Both finish the game greedily at the horizon.

**Case 2. Stage 0, any candidate: greedy playout** (peg.c:809-885)
- Both sides play their highest static-equity move each turn (highest score
  once the bag is empty), until the game ends.
- This is what "rational" means in the help text. It is **greedy**, not
  optimal: no lookahead, and the opponent never sets up or blocks anything
  on purpose.
- Its only job is a cheap first ranking of every candidate, to choose the
  32 that go on.

**Case 3. `-pegpess true`: pessimistic playout** (peg.c:928-1060)
- The mover still plays greedy.
- On each opponent turn, it tries every reply, or the top `inner_top_k` by
  equity (0, meaning all, from the CLI). It rolls out each reply greedily to
  the end, and plays the one that leaves the mover with the lowest spread.
- This opponent is **omniscient.** It is scored on the scenario, so it
  effectively knows the mover's rack and the bag order. It also looks ahead
  only one move of its own, with a greedy rollout after that.
- The resulting spread is never better than the greedy playout's, so it is
  a lower bound in that sense ("guaranteed-win analysis" in the help).
- **Where it applies:** only where a rollout is used:
  - stage 0;
  - stages 1 and later when `-pegnested false`.
  - With nesting on (the default), stages 1 and later use case 4 for these
    plays, and **`-pegpess` changes only stage 0**, that is, which 32
    candidates survive the first cut (peg.c:1835-1847).

**Case 4. The candidate doesn't empty the bag, at stages 1 and later, with
nesting on (the default): nested inner PEG** (peg.c:1327-1813)
- After the candidate, the opponent faces a pre-endgame of its own. PEG
  solves that sub-problem from the **opponent's point of view**:
  1. **What the opponent can see.** Unseen tiles are counted from the
     opponent's side: everything except its own rack and the board
     (`peg_compute_unseen(game, on_turn)`, peg.c:1725). So the opponent
     knows its own rack but not the mover's rack or the bag order. This is
     the realistic information model.
  2. **Its candidates.** Its top 8 moves by static equity, narrowed to 4
     and then 2 over the inner stages (`PEG_NESTED_DEFAULT_CAND_CAPS`).
     Inner stages run at 2, 3, ... plies, capped at the outer stage's depth
     minus 1 (peg.c:1756-1768). At outer stage 1 (2 plies) the inner PEG
     runs a single 2-ply stage.
  3. **How each candidate is scored.** The weighted mean over the
     opponent's own scenarios (peg.c:1649-1662). In each one:
     - if the bag is now empty, an endgame search;
     - otherwise (depth 1 is used up), a greedy playout.
  4. **Its choice.** The candidate with the highest mean spread for the
     opponent (peg.c:1784-1800). It maximizes expected spread, not win%.
- **What this means for the mover's win%.** The value the nested solve
  returns is the opponent's *expected* spread, averaged over every mover
  rack the opponent considers possible. It is not the result of the outer
  scenario's actual racks.
  - The inner solve re-deals the mover's tiles: the outer mover's rack is
    rebuilt from unseen minus each inner bag (peg.c:678-700, 744-755).
  - So in two outer scenarios with the same opponent rack but different
    mover draws, a non-emptying candidate gets the **same** value.
  - The outer PEG then counts the scenario as a win if that average is
    below 0 from the opponent's side (peg.c:1840-1841).
  - For these plays, win% therefore means "the share of opponent racks
    whose best expected reply leaves the mover ahead". That is not the
    probability that the mover actually wins.
  - My reading of the code is that the opponent's *choice* is modeled
    realistically, but its *outcome* is averaged rather than played out on
    the real racks. That would make the outer scenario enumeration for
    these candidates partly redundant, since mover draws that share an
    opponent rack give the same value. Confirm this with the author or a
    small test before acting on it.
  - If it holds, the fix is to pick the opponent's reply with the nested
    solve, then score that reply on the outer scenario's actual racks.
    **Macondo's nested solve does exactly this** for its own side
    (`nestedOurTurnSolve`, 861b5574, "imperfect-info aggregation"):
    - choose the play under uncertainty, across every ordering consistent
      with what that player can see;
    - return that play's outcome **at the actual bag ordering**;
    - take the worst outcome among plays tied for best;
    - cache the per-ordering verdicts by information state, so scenarios
      that share it reuse one solve.
    - Its commit cites a position where averaging over orderings gives DRAW
      and scoring at the actual ordering gives WIN, matching another engine
      (Ortograf). It is a working blueprint.

**Macondo, for comparison**
- **After a play that empties the bag:** a perfect-information endgame, as
  in case 1, but first-win.
- **The opponent's reply after a play that doesn't empty the bag:** every
  reply is tried on the fixed bag ordering, and the scenario is a loss as
  soon as any reply loses for us (peg_generic.go:1151-1185). This opponent
  is **omniscient and worst-case**, like MAGPIE's case 3 but tried with a
  full search rather than a greedy rollout. The design notes defend it: "we
  should always be pessimistic" (preendgame/README.md).
- **Our next turn, if the bag is not yet empty:** a nested sub-PEG under
  imperfect information (`nestedOurTurnSolve`, peg_generic.go:1191-1222):
  - We see only our rack and the board.
  - Every candidate is scored by wins + ½ draws over every bag ordering
    consistent with that.
  - The best-scoring play's verdict at the *actual* ordering is what
    counts.
- **Depth:** with `-max-nested-depth 1`, a line that would need a second
  nested sub-PEG is skipped. The help text calls this a slight optimistic
  approximation.
- **Net effect:** Macondo is pessimistic about the opponent and realistic
  about us. MAGPIE is realistic about the opponent (with the averaging
  question above) and greedy about our later turns, since depth 1 ends in a
  greedy playout.

**Summary**

| Situation | MAGPIE default | MAGPIE `-pegpess` | Macondo |
|---|---|---|---|
| Stage 0, any candidate | Greedy both sides | Opponent: 1-ply worst-for-mover, omniscient | n/a (no stage 0) |
| Empties the bag, stage 1+ | Endgame search, full window, 2-6 plies + greedy finish | Same | Endgame, first-win, 1-3 plies + greedy finish |
| Opponent's reply after a non-emptier | Picks best expected-spread reply from its own view; value is that expectation | Same (pess ignored with nesting on) | Omniscient worst case over all replies |
| Our later turn, bag not yet empty | Greedy (nested depth 1) | Greedy | Nested sub-PEG, imperfect info, verdict at actual ordering |
| Non-emptiers, `-pegnested false` | Greedy both sides | Opponent: 1-ply worst-for-mover, omniscient | n/a |
| Non-emptiers leaving 2+ in the bag | Solved | Solved | **Skipped** by default (`-max-tiles-left 1`) |

## Default behavior

| Setting | MAGPIE `peg` | Macondo `peg` |
|---|---|---|
| Time limit | `-pegtlim 0` falls back to `-tlim`, which is **60 s** (config.c:4737-4739, 8823) | `-maxtime 0` = **unlimited** (shell/specs.go:109) |
| Endgame depth | Set by the stage schedule: 2 to 6 plies | `-endgameplies` **3**. The `endgame` command uses 4 (shell/api.go:45-56) |
| Greedy finish at the horizon | On | On (26f43470) |
| Iterative deepening | n/a (the cascade plays that role) | On (`-disable-id` turns it off) |
| Early cutoff | n/a | **Off** from the shell: `Init` sets it on, the shell's `Bool()` resets it (shell/api.go:855, 926) |
| Which plays are solved | All (`peg empty` = only emptiers) | **Only plays leaving ≤ 1 tile** (`-max-tiles-left 1`); `-1` = all, `0` = only emptiers |
| Nested depth | 1 (`-pegnested true`) | 1 (`-max-nested-depth 1`); deeper lines skipped |
| Passes inside nested solves | Allowed | Suppressed (`-skip-deep-pass true`) |
| Opponent model | `-pegpess false` (greedy at stage 0, nested PEG after) | Fixed: see [the opponent model in detail](#the-opponent-model-in-detail) |
| Endgame window | Full window everywhere | First-win; full window only in the tiebreak pass |
| Scenario sampling | Off (`-pegstride 0`) | n/a |
| Threads | All cores | All CPUs |
| Results shown | All, with per-draw outcomes (`-pegoutcomes true`) | `-maxsolutions 3` |
| Explain one draw | n/a (the outcomes column covers some of this) | `-eventuality GRL`: a narrative for one draw order |
| Used by the bot | No: autoplay `-pegbag1/2` default to 0 | Elite bot: **1 in the bag only**; plies by \|spread\| (≥100 → 2, ≥75 → 3, ≥50 → 4, else 5); early cutoff on (ai/bot/elite.go:61, 196-215) |
| Used by game analysis | `analyze`: PEG at 1-4 in the bag, `-tlim` 60 s | Analyzer: **PEG at 1 in the bag only**, 2-7 in the bag uses sim; 4 plies, early cutoff on, played move protected (`SetAvoidPrune`), 5-minute timeout (gameanalysis/analyzer.go:57, 730-739, 961-990) |

## What each has that the other lacks

**MAGPIE only**
- The staged top-K cascade with exact spread at every stage.
- A choice of rational or pessimistic opponent.
- An opponent modeled with imperfect information.
- Scenario stride sampling.
- The per-draw outcomes column for every play.
- Live progress, JSON output, checkpoint/resume (`peg resume`).
- A transposition table shared across autoplay games.

**Macondo only**
- **Known opponent tiles (`-opprack`).** This is the most valuable gap.
  After an exchange, a challenged phony or a tracked draw, MAGPIE solves a
  different position from the one the player actually knows. Macondo's
  analyzer uses it automatically after a challenged phony.
- First-win endgames, early cutoff and skip-loss pruning.
- **`-max-tiles-left N`**: solve only plays that leave at most N tiles. This
  is the "leaves N" todo, but "at most N" rather than "exactly N".
- Our own later turn modeled with imperfect information, scored at the
  actual bag ordering.
- An explicit endgame-depth flag, separate from the pruning schedule.
- A switch to skip the tiebreak pass.
- `-eventuality`: an explanation of one draw order.
- Bags of 5 or 6 with any play.

**Both**
- A "solve only these moves" list: MAGPIE `peg <moves>`, Macondo
  `-only-solve` (repeatable).
- A "never prune these" list: MAGPIE `-pnoprune`, Macondo `-avoid-prune`
  (repeatable). Macondo's protects only against early cutoff.
- Nested lookahead, though on opposite sides (see the opponent model).
- Greedy finish at the endgame horizon.

## Takeaways
- **The two engines model opposite sides realistically.** Macondo's
  imperfect-information aggregation is a tested blueprint for fixing
  MAGPIE's averaged nested value (case 4). Apply it to MAGPIE's
  opponent-side nesting: pick by expected value, score at the actual racks.
- **First-win is the biggest speed difference.** MAGPIE pays for full
  spread on every endgame. Macondo pays for it only in a final tiebreak, and
  says so explicitly. Since MAGPIE's cuts mostly depend on win%, first-win at
  the intermediate stages is the obvious speedup to try.
- **Defaults.** Macondo favors completeness over time (no time limit) but
  limits the work: emptiers plus plays leaving 1 tile, depth 3, nested depth
  1, and PEG only at 1 in the bag in analysis and the bot. MAGPIE has a 60 s
  limit but no restriction on which plays it solves, and analysis uses PEG
  at 1-4 in the bag.
- **For the todo list:**
  - The "analysis should use PEG only at 2 in the bag" item points in the
    same direction as Macondo, which goes further (1 in the bag).
  - `-max-tiles-left` is prior art for "leaves N".
  - `-only-solve` and `-avoid-prune` as two repeatable options is one
    answer to the move-list question (the plan's Phase 5).
- **Also worth adopting:** known opponent tiles, and possibly an early
  cutoff between stages.
