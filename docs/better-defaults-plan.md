# Better defaults: plan

Source: the `magpie_todo.txt` list. Each phase states what to change and the
context behind the change. Checked against MAGPIE at 1a73c928; file:line
references are MAGPIE unless marked otherwise. Per AGENTS.md, each phase (and
each lettered sub-phase) ships as its own focused PR, and behavior changes
are validated with game pairs or before/after on-demand tests.

## The todo items

| # | Todo item | Phase |
|---|---|---|
| 1 | Diff MAGPIE PEG vs Macondo PEG, and their default behaviors | 1 |
| 2 | PEG "leaves N" in the bag, generalizing `empty` (= leaves 0) | 6 |
| 3 | Rethink PEG positional args and `-pnoprune` | 5 |
| 4 | See which features should be defaults | 1, 4, 7, 8 |
| 5 | Clarify the default WMP / RIT / WIT behavior | 2, 7 |
| 7 | Find other settings that should be on for strength or speed | 8 |
| 8 | Analysis should use PEG only at 2 in the bag by default | 3b |
| 9 | PEG and endgame time limits unlimited by default | 3a, 4 |

Two goals shape the default changes:
- **PEG and analyze default to complete, untimed, results-only solves**
  (Phases 1, 3 and 4). The user in mind is analyzing their games after a tournament,
  not an engine playing on a clock.
- **WMP, RIT and WIT are on by default, when the machine has the disk and
  memory for them** (Phase 7).

## Cross-cutting: saved settings pin old defaults

`-savesettings` writes `-tlim`, `-threads` and the per-player `-w1/-w2`,
`-rit1/-rit2`, `-wit1/-wit2` flags explicitly (config.c:15139-15190). Users
with an existing `settings.txt` will not pick up any new default from this
branch. Decide once, before changing defaults: either write only values that
differ from the defaults, or bump a settings version and drop the stale keys
when loading. Either way, list the changes in the release notes.

---

## Phase 1: PEG: the Macondo comparison, and complete, untimed, results-only solves (todos 1, 4)

The comparison is written up, with no code change, in
[peg-vs-macondo.md](peg-vs-macondo.md). The recommendations below come out
of it.

The target user is analyzing their games after a tournament. They have no
clock, they want the right answer rather than a fast one, and only the result
counts: a win by 1 is as good as a win by 100. Today's PEG defaults serve the
opposite case, an engine on a clock: a 60 s limit, a cascade that cuts on
shallow estimates, exact spread everywhere and spread in the ranking key.

Each lettered item is its own PR.

### 1a. The time limit picks the strategy

- Without a time limit (the new default, Phase 3a), PEG runs the complete
  solve described in 1b to 1e. With one, it runs today's cascade, whose
  stages give an anytime answer (peg.c:3436-3438). Anyone on a clock (the
  WASM client during play, a bot) passes `-pegtlim` and keeps today's
  behavior. An explicit `-pegtopk` still picks the cascade.
- **Long solves need feedback.** Show progress against a known total
  (candidates and scenarios done, per stage), so the user can estimate the
  remaining time.

### 1b. Rank by results only

- Compare win% (wins + ½ draws) first. Use spread only to break exact ties,
  or not at all. Today the key is `win_pct + 1e-4 * mean_spread`
  (peg.c:2358-2370), so 100 points of mean spread can outrank 1 pp of win%.
- Compare with a small tolerance, or keep the weights in integer units, so
  floating-point noise doesn't separate plays that are really tied.
- Show W/D/L separately in the results, not only win%. A draw matters to
  this user, and 50% can mean all draws or half wins.

### 1c. First-win endgames inside PEG

- Pass `first_win=true` in both PEG endgame calls: the outer leaf
  (peg.c:1892) and the nested leaf (peg.c:1408). Today neither uses it. The
  test harness measured identical W/D/L, about 3x faster solves, and a TT hit
  rate of 99.7% instead of 36% (7431b0c0). The [-1, +1] window still
  separates draws from wins and losses.
- Spread becomes optional. If it is wanted for display, compute it at the end
  for the top play or plays only, as Macondo's tiebreak pass does.
- The default pessimistic opponent (1e) needs only W/D/L. With `-pegpess
  false`, the realistic opponent needs a W/D/L objective too, since today it
  picks by mean spread (peg.c:1784-1800), which needs a full window.
- This applies only to the endgames PEG runs as leaves. The standalone
  `endgame` command keeps its full window (Phase 4).
- For the timed cascade, the narrower variants (first-win at the
  intermediate stages only, skipping stages that cut nothing) are in Phase 8.

### 1d. Sound pruning, over plays that leave at most 1 tile

The cascade cuts plays on 2 to 5 ply estimates, so it can drop the best play.
A complete solve must cut only plays that provably can't win.
- **Default to plays that leave at most 1 tile in the bag**, as Macondo does
  (`-max-tiles-left 1`). At 3 and 4 in the bag, plays that leave 2 or more
  tiles are the most expensive candidates, since each needs nested solves,
  and they are rarely best. In Phase 5's grammar this is the default
  candidate set, `leaves1` (Phase 6). `peg all` solves every move, and moves
  given in the second positional are always added. A pass qualifies only at
  1 in the bag.
- `analyze`'s PEG turns use the same default set, plus the move actually
  played, which analyze already protects.
- **Solve every candidate's emptier scenarios to the end of the game.** Use
  the exhaustive depth (`PEG_EXHAUSTIVE_PLIES` = 40, peg_defs.h:51) rather
  than the cascade's 6. First-win makes deep searches affordable.
- **Early cutoff, made exact.** With exact W/D/L per scenario, a candidate's
  best possible win% is (wins + ½ draws + weight still unsolved) / total.
  Stop solving it once that falls below the best candidate's proven win%.
  This is Macondo's early cutoff (peg_generic.go:289-310). It is sound here
  because the leaf verdicts are exact, not estimates.
- **Order the work so the bound rises early.** Use stage 0 (greedy) and a
  2-ply pass only to order candidates, best first. Within each candidate,
  solve the scenarios most likely to lose first, as Macondo does. A strong
  first candidate makes every later cutoff come sooner.
- **Keep full scenario enumeration.** `-pegstride` stays 0 (it already is).

### 1e. A pessimistic opponent by default, and a complete model of later turns

Every candidate that doesn't empty the bag is valued through the opponent's
reply and, if the bag still isn't empty, the mover's next turn. A solve is
only as complete as that model. Today:
- `-pegpess` defaults to false (the rational opponent). With nesting on, the
  default, `-pegpess true` changes only stage 0, the first cut
  (peg.c:1835-1847). At stages 1 and later, the opponent after a non-emptier
  is a nested PEG from its own point of view, picking the reply with the
  highest mean spread (peg.c:1784-1800).
- The `peg` command hard-codes nested depth 1, inner caps {8, 4, 2} and the
  per-bag inner strides 1, 1, 5, 7 (config.c:4744-4751, peg.c:1567-1586). So
  at 4 in the bag, a 1-tile play's inner PEG samples 1/5 of its scenarios.
  `-pegndepth`, `-pegncaps` and `-pegnstrides` exist but apply only to
  autoplay (config.c:1532-1552).
- Even `-pegtopk all` isn't complete. Its nested inner stages still run at 2,
  3 and 4 plies (peg.c:1756-1768), and depth 1 ends in a greedy playout.
- The nested value is the opponent's expected spread, averaged over every
  mover rack it considers possible, not the result on the scenario's actual
  racks. So win% for a non-emptier isn't the probability of winning
  (peg-vs-macondo.md, "The opponent model in detail", case 4).

Change:
- **`-pegpess true` by default, and it defines the opponent at every
  stage**, not just stage 0. After a non-emptier, every opponent reply is
  tried on the scenario, so the opponent effectively knows the racks and the
  bag order. The scenario takes the worst result for the mover, and stops
  at the first reply that makes it a loss. This is Macondo's model
  (`iterateOppReplies`, peg_generic.go:1151-1185), and it makes a win% a
  guaranteed lower bound. Each reply is evaluated exactly:
  - a reply that empties the bag: a first-win endgame to the end of the
    game;
  - a reply that doesn't: the mover's next turn, below.
- **The mover's next turn, when the bag still isn't empty: a nested PEG
  from the mover's point of view.** The mover sees only its own rack and
  the board. Its play is chosen by wins + ½ draws over every bag consistent
  with that, then scored on the scenario's actual racks (Macondo's
  `nestedOurTurnSolve`, 861b5574).
- **Complete nesting.** Inner strides of 1, inner endgames at full depth
  with first-win, and nested depth up to the bag size, so no line ends in a
  greedy playout or is skipped. Macondo skips lines that need a second
  nested sub-PEG (`-max-nested-depth 1`); MAGPIE shouldn't. Measure the cost
  before setting the nested depth and caps; these are the expensive knobs.
- Expose `-pegndepth`, `-pegncaps` and `-pegnstrides` to `peg` and
  `analyze`.
- **`-pegpess false` keeps a realistic opponent, fixed.** The opponent
  chooses its reply from its own view of the tiles, by its wins + ½ draws
  rather than mean spread, and that reply is scored on the scenario's actual
  racks rather than averaged. Confirm the averaging with a small exact test
  before changing it.
- Update the `-pegpess` help, which says false is the default. Autoplay's
  `-pegpess1/2` keep their own default (false).

### 1f. Known opponent tiles

- Add known opponent tiles to `peg` (Macondo's `-opprack`). Today PEG
  ignores them and sets the opponent's rack to the unseen tiles minus the
  bag (peg.c:676-700). After an exchange, a challenged phony or a tracked
  draw, MAGPIE solves a different position from the one the player actually
  knows.
- In `analyze`, fill them in from the GCG: exchanges, challenged-off phonies
  and tracked tiles, as Macondo's analyzer does for phonies
  (gameanalysis/analyzer.go:968-975).
- Use only what the player could see at that turn. Hindsight about the
  opponent's actual rack would grade the player against information they
  didn't have.

### Validation

1. W/D/L agreement for emptiers: the complete solve against `-pegtopk all`
   with today's settings, at 1 and 2 in the bag. For plays that empty the
   bag, any disagreement should trace to 1c or 1d's depth, not to a bug.
   Non-emptiers will differ by design (1e).
2. Pruning soundness: early cutoff on and off must give identical best plays
   and identical win% for the best play.
3. Pessimism check: with `-pegpess true`, no non-emptier's win% exceeds its
   win% with `-pegpess false` on the same positions.
4. Wall time per position at 1 to 4 in the bag, on a fixed set of tournament
   positions, with the median and the worst case. State the estimate before
   running (the runs at 4 in the bag may be long). This decides Phase 3b's
   default.

### Recommended changes

**Changes to defaults**

| Change | Reason | MAGPIE default | Macondo default |
|---|---|---|---|
| With no time limit, run a complete solve instead of the cascade (1a) | The cascade cuts plays on shallow estimates and can drop the best play | Cascade: greedy stage 0, then the top 32/16/8/4/2 at 2-6 plies, within 60 s | Every remaining candidate, by iterative deepening over plies 1-3; no time limit |
| Rank by win% (wins + ½ draws); spread only breaks exact ties (1b) | Only the result counts. Today 100 points of mean spread outrank 1 pp of win% | `win_pct + 1e-4 * mean_spread` | Wins + ½ draws; spread only in a tiebreak pass over the top 20 tied emptiers |
| First-win endgames inside PEG (1c) | Same W/D/L, about 3x faster, in the test harness | Full window | First-win; full window only in the tiebreak pass |
| Solve only plays that leave at most 1 tile in the bag (1d) | Plays that leave 2 or more are the most expensive candidates and rarely best | Every generated move | Plays that leave at most 1 tile (`-max-tiles-left 1`) |
| Solve emptier scenarios to the end of the game (1d) | A horizon with a greedy finish is an estimate, not a result | 2-6 plies by stage, then a greedy finish (40 plies with `-pegtopk all`) | 3 plies (`-endgameplies`), then a greedy finish |
| `-pegpess true`, defining the opponent at every stage: every reply tried on the scenario, worst result for the mover (1e) | A win% that is a guaranteed lower bound | False (rational). True changes only stage 0 while nesting is on | Omniscient and worst case for the mover over all replies (fixed) |
| The mover's next turn, bag not yet empty: a nested PEG from its own view, scored on the actual racks (1e) | Models the mover as a player who can't see the opponent's rack or the bag | Greedy playout (nested depth 1 ends there) | A nested sub-PEG, scored at the actual ordering |
| Complete nesting: inner strides 1, inner endgames at full depth with first-win, depth up to the bag size (1e) | No line ends in a greedy playout or is skipped | Depth 1, caps 8/4/2, inner strides 1/1/5/7 by bag, inner endgames at 2-4 plies | `-max-nested-depth 1` (deeper lines skipped), `-skip-deep-pass true` |
| With `-pegpess false`, the opponent maximizes its wins + ½ draws (1e) | Consistent with a results-only objective, and lets nested leaves use first-win | Maximizes mean spread, from its own view of the tiles | |

**New features**

| Change | Reason | MAGPIE default | Macondo default |
|---|---|---|---|
| Progress against a known total (1a) | Untimed solves can run long; the user needs an estimate | Live per-stage progress in `shpeg`, WASM and JSON, without a total |  |
| Exact early cutoff (1d) | Stops solving a play once it provably can't beat the best; sound with exact verdicts | None | Early cutoff: on in the bot and analyzer, off from the shell |
| Candidates ordered best first, scenarios likely losses first (1d) | Raises the cutoff bound early |  | Loss-first scenario ordering |
| `-pegndepth`, `-pegncaps` and `-pegnstrides` for `peg` and `analyze` (1e) | Lets users set the nested model | Autoplay only | `-max-nested-depth` |
| W/D/L columns in the results (1b) | 50% can mean all draws or half wins | Win% and mean spread |  |
| Known opponent tiles: a `peg` option, filled in from the GCG by `analyze` (1f) | Solves the position the player actually knew | Ignored: the opponent's rack is the unseen tiles minus the bag | `-opprack`; the analyzer fills it in after a challenged phony |

**Fixes**

| Change | Reason | MAGPIE default | Macondo default |
|---|---|---|---|
| With `-pegpess false`, score the opponent's chosen reply on the scenario's actual racks (1e) | Today win% for a non-emptier isn't the probability of winning | Averaged over every mover rack the opponent considers possible | |
| `-pegpess` help gives the new default (1e) | It says false is the default | | |

### Default MAGPIE PEG vs default Macondo PEG, after these changes

This compares the `peg` command's defaults, assuming every recommendation in
this plan is applied: Phase 1, the unlimited `-pegtlim` (3a), the move list
(Phase 5) and `leaves<N>` (Phase 6). Macondo is as of c1647df2. Details and
references for Macondo are in [peg-vs-macondo.md](peg-vs-macondo.md).

**What becomes the same**
- No time limit by default.
- Candidates: plays that leave at most 1 tile in the bag, widened with
  `leaves<N>` / `-max-tiles-left N` (at most N left).
- The opponent after a non-emptier: omniscient, and the worst case for the
  mover over all its replies.
- The mover's later turn: a nested PEG from the mover's own view, scored at
  the actual racks.
- Ranking by wins + ½ draws.
- First-win endgames for the W/D/L verdicts, with spread only as an
  optional tiebreak at the end.
- Known opponent tiles as an input.
- A list that restricts the solve to given moves, and one that protects
  moves from pruning.

**What still differs**

| | MAGPIE (after this plan) | Macondo (today) |
|---|---|---|
| Goal | Complete: the proven W/D/L of every candidate, under the pessimistic model | Bounded: a fast, close estimate |
| Bag sizes | 1 to 4; 5 to 7 when every candidate leaves at most 4, which the default `leaves1` satisfies (if the inner levels handle it) | 1 to 6, with any play |
| Scenarios | Multisets of draws, weighted, fully enumerated | Every ordered draw, solved separately |
| Search | Every candidate solved at full depth; greedy and 2-ply passes only order the work | Iterative deepening over 1 to 3 plies for every candidate |
| Endgame depth after an emptier | To the end of the game: a proven result | 3 plies, then a greedy finish: an estimate |
| Nesting depth | Up to the bag size; no line skipped (depth and caps set by measurement) | One nested sub-PEG; deeper lines skipped (`-max-nested-depth 1`), which its help calls a slight optimistic approximation; no passes inside (`-skip-deep-pass true`) |
| Pruning | Exact early cutoff, on by default: a play is dropped only once it provably can't beat the best | Early cutoff off from the shell (on in the bot and analyzer); optional skip-loss |
| Spread tiebreak | Exact ties only, optional | A full-window pass over the top 20 tied emptiers |
| Opponent model choice | `-pegpess false` gives a realistic opponent that chooses from its own view of the tiles | Fixed |
| Move lists | Two positionals: a set keyword (`leaves1` by default, `all`, `empty`, `leaves<N>`, `only`), then moves that are added and protected; with `only`, the moves are the set | `-only-solve` restricts and `-avoid-prune` protects, both repeatable |
| Results shown | Every candidate, with W/D/L columns and per-draw outcomes | The top 3 (`-maxsolutions 3`) |
| With a time limit | Switches to the staged cascade, which always has an answer from the last completed stage | The same algorithm, stopped at the deadline |
| In game analysis | PEG at 1 to 4 in the bag (or 1 to 2, by measurement), unlimited, opponent tiles filled in from the GCG | PEG at 1 in the bag only, 4 plies, early cutoff on, 5-minute timeout, opponent tiles filled in after a challenged phony |

**Net effect.** The two engines end up with the same model: the same
candidates, a pessimistic opponent, and a mover who can't see the bag. They
differ in how far they take it. MAGPIE's default solves that model exactly:
every scenario, every endgame to the end, every nested line, cut only when a
play is provably beaten. Its cost is time, which is why the cascade stays
for timed use. Macondo approximates it: shallow endgames with a greedy
finish and nested lines skipped past one level, which keeps its solves fast
at the price of being sometimes wrong in the lines it cuts short.

---

## Phase 2: Fix help text and comments that describe the wrong default (todo 5)

These are documentation-only fixes. They are cheap and unblock the later
phases, because the help text has to describe the defaults correctly before
we change them.

### Recommended changes

**Fixes**

| Change | Reason | MAGPIE default | Macondo default |
|---|---|---|---|
| Fix the `-wmp` help (config.c:2135-2142) | It says "on by default" | On for the CLI. Off for `magpie_create()` (cmd_api.c:59-60), WASM and browser_game |  |
| Fix the `-pegoutcomes` help (:2368-2382) | It says "Default false" | True (:14832) |  |
| Fix the `pegstride` comment (peg.h:177-179), or implement the per-bag default (Phase 8) | It says "0 = bag-size default" | 0 is full enumeration (peg.c:3201-3204) |  |
| Document `peg resume` in the `peg` help | The help omits it | `resume` needs `-pegjson` and `-pegoutcomes` |  |
| Use the current name in the PEG error text (:4855-4858) | It says "pegonly 'empty'" |  |  |
| Fix the `analyze` help (:1877-1882) | It says "simulation or endgame solving" | Also static (`-plies 0`) and PEG (1-4 in the bag) |  |
| Fix the `snoprune` help examples (:1736-1748) | Both examples fail to parse |  |  |
| Delete `endgame_compute_turn_limits` (endgame_time.c), or wire it in during 3a | It has no callers |  |  |
| Add a "Defaults" section to README.md: what WMP, RIT, WIT and PAT do, per entry point (CLI, libmagpie, WASM), and why | The defaults differ by entry point and are documented nowhere |  |  |


---

## Phase 3: Time limits and the analyze PEG range (todos 8, 9)

### 3a. One independent time limit per solver, with no fallbacks

Today:
- **Sim:** `-tlim`, 60 s. 0 means no time limit. The clock starts after
  inference.
- **Inference:** no time limit. It runs until it finishes or the user stops
  it.
- **Endgame:** `-etlim`, default 0, which is unlimited for the `endgame`
  command. But `analyze` falls back to `-tlim` when `-etlim` is 0
  (config.c:13676-13682), so its endgame turns get 60 s.
- **PEG:** `-pegtlim`, default 0, which falls back to `-tlim`
  (`config_fill_peg_args`, config.c:4737-4739), so `peg` and analyze's PEG
  turns get 60 s.
- Autoplay's endgame and PEG solves have no time limit; their depth bounds
  them.

Change:
- Each timed solver has its own time limit, set separately, which no other
  limit reads or overrides. 0 always means unlimited.

  | Solver | Flag | Default |
  |---|---|---|
  | Sim | `-tlim` (per player in autoplay: `-tlim1`, `-tlim2`) | 60 s |
  | Endgame | `-etlim` | Unlimited |
  | PEG | `-pegtlim` | Unlimited |

- Inference gets no time limit. It is exhaustive and all or nothing, so a
  cutoff would leave no usable result, and it is fast. When a sim runs
  inference first (`-sinfer`), `-tlim` still starts after it, as it does
  today.

- Remove both fallbacks: `peg`'s from `-pegtlim` to `-tlim`, and
  `analyze`'s from `-etlim` to `-tlim`. Each `analyze` turn uses the limit
  of the solver it runs: `-tlim` for sim turns, `-etlim` for endgame turns,
  `-pegtlim` for PEG turns.
- `-tlim` still sets `-tlim1` and `-tlim2` together in autoplay. Those are
  per-player forms of the same sim limit, not a fallback between solvers.
- A user who wants a bound sets `-pegtlim` or `-etlim` explicitly. For PEG, a limit also selects the cascade (1a).
- Fix the help: `-etlim` and `-pegtlim` both say "0 (the default) falls back
  to -tlim"; both become "0 (the default) is unlimited".
- The WASM client sends `-tlim 0` for unlimited, which today also unbounds
  PEG and analyze's endgames. Once the fallbacks are gone, it must send
  `-pegtlim` and `-etlim` itself (analysis-model.mjs:321, 373).

Risks:
- An unlimited, complete PEG at 3-4 in the bag can run for a long time.
  Phase 1's wall-time measurement comes first. If it is excessive, narrow
  the analyze bag range (3b) rather than reinstating a time limit.
- Interactive use must stay stoppable. Confirm the poll/stop path works on a
  long solve in both the CLI and WASM.

### 3b. The analyze PEG bag range

Today analyze.c:1585-1590 routes 1..4 in the bag to PEG, with the bounds
hard-coded to `PEG_MIN_BAG`/`PEG_MAX_BAG`. There is no flag for it.

Change: add an analyze option for the largest bag size that analyze solves
with PEG. Name it to fit the existing `-peg*` names, e.g. `-apegmaxbag`.
Clamp it to `[0, PEG_MAX_BAG]`. 0 disables PEG in analyze. Bags above the
limit fall through to sim, as they would today for bags of 5+.

The default:
- The todo says 2, for speed. Macondo's analyzer and elite bot go further
  and use PEG at 1 in the bag only.
- For untimed analysis, a complete PEG is worth more than a sim wherever it
  is affordable. Keep 1 to 4 if the complete solve's wall time at 3 and 4 in
  the bag allows it (Phase 1's validation); otherwise default to 2.
- Open question: the todo says "only for 2 in the bag". This plan reads that
  as "up to 2", because a 1-in-bag PEG is exact and cheap, and routing it to
  sim would be a regression. If exactly 2 was meant, use a min/max pair
  instead.

Validation: run analyze on a fixed set of GCGs. Confirm that turns above the
limit use sim and turns within it use PEG, and record the wall time per game
before and after. Update the analyze help text.

### Recommended changes

**Changes to defaults**

| Change | Reason | MAGPIE default | Macondo default |
|---|---|---|---|
| `-pegtlim` defaults to unlimited, with no `-tlim` fallback (3a) | Each solver has its own limit; complete solves for untimed analysis | 0, which falls back to `-tlim`: 60 s | `peg -maxtime 0`: unlimited |
| `-etlim` defaults to unlimited everywhere, with no `-tlim` fallback in `analyze` (3a) | Same | 0: unlimited for `endgame`, but `-tlim` (60 s) in `analyze` | `endgame -maxtime 0`: unlimited. Analyzer: 3 minutes per endgame |
| `analyze` uses each solver's own limit for its turns: `-tlim` for sims, `-etlim` for endgames, `-pegtlim` for PEGs (3a) | No limit reads or overrides another | `-tlim` (60 s) for all three | Analyzer: fixed 5 minutes per PEG, 3 minutes per endgame |
| `-tlim` stays 60 s and bounds only sims (3a) | The one solver whose default stays timed | 60 s; also read by PEG, and by analyze's endgames, when their limits are 0 | |
| Analyze PEG bag range: 1-4 if the complete solve is affordable there, otherwise 1-2 (3b) | A complete PEG beats a sim wherever it fits. The todo asks for 2, for speed | 1-4, hard-coded | 1 only; 2-7 in the bag use sim |

**New features**

| Change | Reason | MAGPIE default | Macondo default |
|---|---|---|---|
| `-apegmaxbag`, 0 to 4; 0 disables PEG in analyze (3b) | Makes the range adjustable | None: hard-coded to `PEG_MIN_BAG`/`PEG_MAX_BAG` | None: fixed at 1 in the analyzer |

**Fixes**

| Change | Reason | MAGPIE default | Macondo default |
|---|---|---|---|
| `-etlim` and `-pegtlim` help: "0 (the default) is unlimited" (3a) | Both say "0 (the default) falls back to -tlim" | | |

---

## Phase 4: Endgame (todos 4, 9)

Today:
- The standalone `endgame` command searches to `-eplies` 6 (config.c:14820,
  at most 25) with a full window, by iterative deepening, and with no time
  limit by default. Heuristics are on, so lines still running at the horizon
  are finished greedily (endgame.c:2519-2535): a 6-ply answer is a good
  estimate of the final spread, not a proven one. There is no first-win
  option on the command line.
- `analyze`'s endgame turns use the same depth and full window, bounded by
  `-tlim` until 3a removes that. The played move's exact value comes from
  the same search as the best move's (`actual_move`). Turns are graded on
  spread lost, with a win turned into a draw or a loss always graded large
  (analyze.c:163-202); a draw turned into a loss is graded by its points.
  The comment at analyze.c:72-76 says "exact win/loss is already known
  there", which holds only at full depth.

The standalone `endgame` command keeps its full window and never becomes
first-win by default: a user who runs it wants the best move and its exact
spread. Macondo draws the same line: "the pre-endgame wants the sign of
thousands of cheap solves ... while `endgame` is one deliberate solve where
the point is a proven answer" (Macondo 3c54b19f). First-win is used only
where the result alone is needed: inside PEG (1c) and in analyze's endgame
turns (4c).

### 4a. The standalone `endgame` command searches to the end of the game

With no time limit, default `-eplies` to 25 (`MAX_SEARCH_DEPTH`,
game_defs.h:9), so the answer is proven rather than estimated. Iterative
deepening means a user who stops it early still gets the deepest completed
depth. Measure the full-window cost at 25 plies on a sample of endgames
before changing the default.

### 4b. Analyze searches endgame turns to the end of the game

Default analyze's endgame depth to 25, so "exact win/loss is known" becomes
true for both the best move and the played one.

### 4c. Analyze grades endgame turns on the result, solved first-win

- Grade on the result only: a win turned into a draw or a loss, or a draw
  turned into a loss. Spread lost is no longer graded.
- Solve these turns first-win, which makes 4b's full depth affordable.
  First-win gives no spread, so the endgame spread columns in analyze's
  report (analyze.c:460-501) are empty by default.
- Add an option that restores today's full-window solve, for users who want
  the spread lost shown.

### Recommended changes

**Changes to defaults**

| Change | Reason | MAGPIE default | Macondo default |
|---|---|---|---|
| Standalone `endgame` searches to the end of the game (25 plies) when there is no time limit, if the measured cost allows (4a) | 6 plies with a greedy finish is an estimate, not a proven answer | `-eplies` 6 | `-plies` 4 |
| Analyze searches endgame turns to the end of the game (4b) | "Exact win/loss is known" holds only at full depth | 6 plies | 7 plies, 3-minute timeout |
| Analyze grades endgame turns on the result only (4c) | Only the result counts | Spread lost graded; win to draw or loss always large, draw to loss graded by points | Spread lost graded; win to loss or tie, and tie to loss, always large |
| Analyze solves endgame turns first-win (4c) | Makes full depth affordable once spread isn't graded | Full window | Full window |

**New features**

| Change | Reason | MAGPIE default | Macondo default |
|---|---|---|---|
| An analyze option for a full-window solve of endgame turns (4c) | For users who want the spread lost shown | Always full window; no option | Always full window |

---

## Phase 5: PEG positional args and `-pnoprune` (todo 3)

Today:
- `peg <moves>` (positional) solves **only** the listed moves, for one
  command. `peg empty` and `peg resume` are its other forms.
- `-pnoprune <moves>` (option) **protects** the listed moves through every
  stage. It is sticky: cleared only by `cgp`, `newgame` or a GCG load
  (config.c:925-933).

Problems:
1. **Two lists in one format, with different meanings and lifetimes.**
   Nothing about `peg A` vs `peg -pnoprune A` says that one restricts and the
   other protects.
2. **A stale protected list breaks the next solve.** After `commit`,
   `addmoves`, `goto`, `next`, `previous` or `challenge`, the old list is
   re-checked against the new position, usually no longer fits, and the
   solve stops with an error.
3. **The lists don't combine.** A protected move only matters if it is
   already a candidate (peg.c:3305-3306, 3344-3347). Under
   `peg empty -pnoprune 8D.WORD`, 8D.WORD is silently dropped when it doesn't
   empty the bag.
4. **The PEG positional disagrees with `endgame` and `analyze`.** There, a
   listed move is evaluated in addition to the normal search and always
   shown. For `peg`, it replaces the search.

### Change: a candidate set, then an optional move list

`peg` takes up to two positionals:
1. **The candidate set**, a keyword: `all`, `empty`, `leaves<N>` (Phase 6),
   `only` or `resume`. Without it, the set is `leaves1`, plays that leave at
   most 1 tile in the bag (1d).
2. **A move list**, optional, comma-separated: moves that are **added to the
   set and protected** through every stage, as in `endgame` and `analyze`.
   With `only`, the list *is* the set.

```
peg                          plays that leave at most 1 tile (= leaves1)
peg all                      every move
peg empty                    every play that empties the bag (= leaves0)
peg empty 8D.WORD            those, plus 8D.WORD, protected
peg leaves2 8D.WORD,11J.MEH  plays that leave at most 2 tiles, plus these two
peg only 8D.WORD,11J.MEH     exactly these two moves
peg resume                   resume a checkpointed solve
```

Errors:
- `peg only` with no move list: `only` is defined by its list.
- `peg resume` with a move list: a resumed solve keeps its own moves.
- A move list in the first position, e.g. `peg 8D.WORD`: the first positional
  must be a set keyword. The message suggests `peg leaves1 8D.WORD` or
  `peg only 8D.WORD`.

This removes the duplicate list and the sticky-state bug, and gives
`leaves<N>` a natural home. Today's `peg 8D.WORD,11J.MEH` becomes an error
rather than silently changing meaning, so scripts and tests that use the
only-list fail loudly and are fixed by adding `only`. The WASM client must
switch from `-pnoprune` (analysis-model.mjs:337-373) to the second
positional.

Implementation:
- The `peg` command takes 0 to 2 positionals instead of 0 to 1
  (`cmd(ARG_TOKEN_PEG, "peg", 0, 1, ...)`, config.c:14610).
- The move list is space-free UCGI moves, comma-separated, as the current
  positional and `-pnoprune` use.
- Listed moves are always added to the candidate set and protected through
  every stage, whatever the set.
- Remove `-pnoprune`. Keep it as a parse error that points to the second
  positional, for one release.
- Reject exchanges when the list is parsed, not later (they slip through
  today, config.c:4787-4794).
- `snoprune` consistency is a separate, smaller follow-up: accept the period
  format alongside its `[opp_rack],8G QI` space format, and fix its
  examples. Don't change its semantics in this PR.

Tests: add `test/` cases for each set keyword alone and with a move list,
for listed moves being added and protected, for `only` with and without a
list, for `resume` with a list, for a move list in the first position, for
the removed `-pnoprune`, and for exchange rejection.

Fallback, if the migration is judged too costly: keep both lists, always add
protected moves to the candidate set, and clear `-pnoprune` with one hook on
every board, rack or turn change rather than three call sites. Record the
choice and the reason in the help text.

### Recommended changes

**Changes to defaults**

| Change | Reason | MAGPIE default | Macondo default |
|---|---|---|---|
| The first positional is the candidate set; a move list there is an error | Separates "which moves" from "which extra moves", and makes old only-lists fail loudly instead of changing meaning | `peg <moves>` solves only the listed moves | `-only-solve` restricts and `-avoid-prune` protects, both repeatable and per command |
| With no set keyword, the set is `leaves1` (1d) | The default candidate set of the complete solve | Every generated move | `-max-tiles-left 1` |
| Remove `-pnoprune`; for one release, a parse error that points to the second positional | Its sticky list breaks the next solve after the position changes | Sticky option, cleared only by `cgp`, `newgame` or a GCG load | `-avoid-prune`, per command, guards only against early cutoff |

**New features**

| Change | Reason | MAGPIE default | Macondo default |
|---|---|---|---|
| Second positional: moves added to the set and protected, e.g. `peg empty 8D.WORD` | Matches `endgame` and `analyze`; a protected move is solved whatever the set | `-pnoprune`: a protected move is dropped unless it is already a candidate | `-avoid-prune` |
| `only` set keyword, which requires a move list: `peg only 8D.WORD,11J.MEH` | Keeps "exactly these moves" available | The current positional's meaning | `-only-solve` |
| `all` set keyword: every move | Widens the default `leaves1` set to everything | `peg` with no arguments | `-max-tiles-left -1` |
| `snoprune` accepts the period format (follow-up PR) | One move-list format across commands | Space format with a rack first |  |

**Fixes**

| Change | Reason | MAGPIE default | Macondo default |
|---|---|---|---|
| Reject exchanges when the list is parsed | They slip through today (config.c:4787-4794) |  |  |

---

## Phase 6: PEG "leaves N" in the bag (todo 2)

Today `empty` uses `config_generate_peg_bag_emptying_moves` (config.c:4815-4867)
and `peg_move_empties_bag` (peg.c:3008-3011, `tiles_played >= bag_size`).

Change:
- Add the keyword `leaves<N>` to Phase 5's list (e.g. `peg leaves2`,
  `peg leaves2 8D.WORD`). It selects moves that leave **at most** N tiles in
  the bag: `max(bag_size - tiles_played, 0) <= N`, the same rule as
  Macondo's `-max-tiles-left N`.
- `leaves1` is the default base set (1d). `empty` becomes an alias for
  `leaves0`, and `all` is any N of at least the bag size.
- Pass leaves the whole bag, so it is included when N is at least the bag
  size: at 1 in the bag, the default `leaves1` includes it.
- Generalize `peg_move_empties_bag` to a `peg_move_bag_after(move, bag_size)`
  helper and build the filter on it. The `ERROR_STATUS_PEG_EMPTY_NO_MOVES`
  error becomes "no moves leave N".
- **Bag-size gate.** peg_solve accepts bags of 5..7 only when every candidate
  empties the bag (peg.c:3031-3065). Relax it to "every candidate leaves at
  most `PEG_MAX_BAG`", so that the nested inner solve stays within the
  supported range. Example: 6 in bag, `leaves2` works through a 2-in-bag
  inner PEG. With the default `leaves1`, this makes bags of 5-7 available by
  default, each candidate leading to at most a 1-in-bag inner PEG. Only do
  this if the inner levels and the outer scenario enumeration handle it;
  otherwise keep the gate at bag ≤4 when N > 0.
- Listed moves are added and protected as Phase 5 specifies.

Tests:
- `leaves0` gives the same results as `empty`.
- `leaves1` at 2 and 3 in the bag selects plays of at least 1 and at least 2
  tiles; at 1 in the bag, it also selects pass.
- `leaves<N>` with N at least the bag size gives the same results as `all`.
- Choosing an N that no move produces raises the error.
- The bag-gate cases at 5-7 in the bag.

### Recommended changes

**Changes to defaults**

| Change | Reason | MAGPIE default | Macondo default |
|---|---|---|---|
| Accept bags of 5-7 when every candidate leaves at most 4, if the inner levels handle it | Lets `leaves<N>`, including the default `leaves1`, work from larger bags | 5-7 only when every candidate empties the bag | 1-6 with any play |

**New features**

| Change | Reason | MAGPIE default | Macondo default |
|---|---|---|---|
| `leaves<N>` keyword: moves that leave at most N tiles in the bag; `empty` becomes an alias for `leaves0` | Generalizes `empty`, so a solve can be narrowed or widened without listing moves | `empty` only (= leaves 0) | `-max-tiles-left N` (at most N), default 1 |

---

## Phase 7: WMP / RIT / WIT on by default when the machine has room (todos 4, 5)

All three are pure speed structures: none changes which move is chosen, so
leaving one out costs speed, never correctness.

| Table | Size (CSW24) | Build peak | Gain (`tablebench`, CSW24, 4 threads) |
|---|---|---|---|
| WMP | 179 MB | ~710 MB | 3-4x movegens/s on its own |
| WIT | 122 MB | ~310 MB | +9% simmer with a WMP; nothing without one |
| RIT | 1.9 GB | ~2.4 GB | +17% simmer, +6% static with a WMP; all three together +49% simmer over WMP alone |

Today:
- **CLI:** WMP on, building a missing `.wmp` on first use
  (`config_wordmap_for_first_use`, config.c:7698-7790). RIT and WIT off:
  `players_data_create` hard-codes them false (players_data.c:315-337).
- **libmagpie** `magpie_create()`: everything off (cmd_api.c:59-60).
- **WASM:** the JS builds and installs a WMP. No RIT, and no WIT since #829.
- **A flag that is on with its file missing is an error,** except the WMP
  first-use build, which falls back to "playing X without a wordmap".
- **`contribute` already does what this phase wants.** A task's tables mean
  "use it if this machine can": `contribute_choose_tables`
  (contribute_tables.c:391) keeps tables in value-per-byte order (WMP, WIT,
  RIT) while they fit the disk beyond `diskreserve` (2 GB) and the memory
  beyond `memreserve` (1 GB). It never keeps a RIT without its WMP, builds on
  demand, evicts only tables its manifest says it built, and names RITs per
  (lexicon, leaves) pair. The CLI should reuse this rather than invent a
  second policy.

Problems to fix on the way:
- **Sticky WMP-off.** `players_data_set_data` sets
  `use_when_available = !!data` (players_data.c:189-197). After a lexicon
  with no wordmap, WMP stays off when the user switches back to one that has
  one.
- **The RIT isn't checked against the leaves.** It caches KLV leave values,
  but `-k1 custom -rit true` silently uses the stock leaves. The WIT is
  checked against its KWG (players_data.c:473-495).
- **Saved settings pin these flags** (see the cross-cutting item).

### R1. A third value, `auto`, as the default for all three

- `-wmp`, `-rit` and `-wit` (and the per-player forms) take
  `auto | true | false`:
  - `auto` (the new default) uses the table if the machine has room for it,
    building it first if needed, and otherwise runs without it and says so in
    one line;
  - `true` requires it. A table that can't be had is an error, as today;
  - `false` never uses it.
- Decide when the lexicon loads, in `config_load_lexicon_dependent_data`, and
  decide again on every lexicon or leaves change.
- `-savesettings` saves the setting chosen (`auto`, `true` or `false`),
  never the state the load resolved to. Today it writes the loaded state as
  a boolean (`players_data_get_use_when_available`, config.c:15160-15177).
  Under `auto` that would pin the machine's decision at save time: a RIT
  skipped for lack of memory would be saved as `-rit1 false` and stay off
  after a memory upgrade. Store the tri-state setting separately from what
  is loaded. Whether default values are written at all is the cross-cutting
  settings decision. Existing files that say `-rit1 false` or `-wit1 false`
  only because those were the old defaults are handled there too.
- Fix sticky WMP-off first. Under `auto`, "this lexicon has no wordmap" must
  not become "never use a wordmap".

### R2. Decide with `contribute_choose_tables`

- Feed it a CLI budget: free disk on the data directory's filesystem
  (`cfile_free_bytes`) and available memory (`get_available_memory_bytes`,
  memory_info.h:153).
- It already handles priority, dependencies, build-peak memory as well as
  hold memory (`CONTRIBUTE_*_BUILD_PERMILLE`, contribute_tables.h:74-76), and
  gives a reason for each table it skips, which the CLI can print.
- Move the chooser out of the contribute-specific files if the CLI is to
  depend on it, so the dependency doesn't run CLI → contribute.

### R3. A memory budget that can't push the machine into swap

- **Subtract the search TT.** Endgame and PEG allocate `-ttfraction` (0.25)
  of total memory (`get_search_memory_budget_base`, memory_info.h:79). A RIT
  that fits in available memory with nothing else running can still push a
  later solve into swap. Budget = available − memreserve − the TT the
  configured fraction will take.
- **Add a floor based on total RAM**, e.g. hold each table only while the
  three together stay under a quarter of physical memory. MemAvailable
  changes from run to run; a floor tied to the hardware makes the decision
  repeatable on one machine. In practice that is a RIT on 8 GB machines and
  up, and WMP and WIT on nearly everything.
- **macOS has no available-memory figure.** `get_available_memory_bytes`
  returns false there (memory_info.h:149-152), and contribute treats that as
  unconstrained. For a default, that would load a 1.9 GB RIT on any Mac.
  Implement it in src/compat (free + inactive + purgeable pages from
  `host_statistics64`). Until then, use the total-RAM floor alone:
  `hw.memsize` is already read at memory_info.h:97-104.
- **Map the RIT by default** (`-ritmmap true`) where the platform maps files
  (cmap, 9a9a1c48) and the host is little-endian. That gives about 400 ms
  faster startup, one copy shared by processes using the same data
  directory, and pages the OS can reclaim under pressure instead of 1.9 GB
  of anonymous memory. A mapped table still has to fit in memory to be fast,
  since lookups are random, so the hold check stays.

### R4. A disk budget that respects a personal machine

- The CLI's disk isn't dedicated the way a contributor's is. Use a larger
  reserve than contribute's 2 GB, e.g. the larger of 10 GB and 10% of the
  filesystem, before an automatic build.
- **Manage only what MAGPIE built.** Record automatic builds in a manifest
  (contribute's `contribute_tables.txt` machinery). Tables the user built or
  copied in are never deleted.
- **Cap the total.** Each English RIT is 1.9 GB, so a user who tries five
  lexicons accumulates about 10 GB. Evict automatic builds least recently
  used first once their total passes a cap, as contribute does.
- **Estimate before building.** contribute assumes CSW24's sizes when the
  server states none (contribute_tables.h:77-79). Other alphabets have more
  distinct racks and larger tables, so the CLI should estimate from the
  distribution rather than assume English. The WMP first-use check already
  estimates ("about thirty times its lexicon's .kwg; ask for forty",
  config.c:7746).

### R5. Build on first use, and say so

- **WMP**: already built on first use. Keep it, and put it under the same
  budget.
- **WIT**: build it on first use the same way, with `kwg2witifneeded`.
- **RIT**: it needs more care, at 1.9 GB on disk and a 2.4 GB build peak:
  - build it automatically only when R3 and R4 both pass;
  - print one line before starting, e.g. "building the rack info table for
    CSW24 (1.9 GB, once), which makes simulation about 17% faster";
  - at an interactive prompt, ask once, the way the first run offers to
    download data (README.md:69);
  - in a non-interactive run, use a RIT only if one is already present;
  - measure its build time and state it in the message.
- Report what each lexicon load chose, in one line, e.g. "CSW24: wordmap,
  word info table; rack info table skipped: 1.2 GB available, 1.9 GB
  needed".

### R6. Make the RIT safe to turn on by itself

This is the prerequisite for an automatic RIT.
- Record the KLV's hash (and the KWG's) in the RIT header, under a version
  bump, and refuse a mismatch. A mismatched RIT counts as absent under
  `auto`, and as an error under `true`.
- Use one naming scheme for both the CLI and contribute. The CLI uses
  `CSW24.rit`; contribute uses `CSW24.CSW24.rit`. A machine used for both
  ends up with two 1.9 GB copies of what is probably the same table. Name it
  by (lexicon, leaves) as contribute does, and have the CLI find a table
  contribute built, and the reverse.
- Leavegen still forces RIT off (config.c:7927-7932), since its KLV changes
  every generation.

### R7. Per entry point

| Entry point | Recommended default |
|---|---|
| CLI | `auto` for all three |
| libmagpie `magpie_create()` | `auto`, under the same budget. Note it in cmd_api.h, since the host process's memory is involved. `magpie_create_with_options()` can still say `false` |
| WASM | WMP stays installed by the JS. RIT and WIT stay off: 32-bit address space, no available-memory figure in the browser, and the browser deliberately stopped building WITs (#829) |
| browser_game.c | Same as WASM |
| `contribute` | Unchanged. The task decides, and `settings.txt` is ignored |
| Tests | `false` unless a test asks. CI runners must not build a 1.9 GB RIT, and test timing shouldn't depend on the runner's free memory |

### R8. Keep `false` free

`auto` decides once per load, outside move generation. Per AGENTS.md,
benchmark each table on and off. The off state must measure ~0 against
today, so `auto` that skips a table must cost exactly what `false` costs now.

### Validation

1. Unit tests for the decision: the chooser with budgets that keep all
   three, drop the RIT, drop the WIT with its WMP, and a macOS-style unknown
   available memory.
2. `tablebench` with what `auto` chose against the same tables forced by
   hand: the numbers should match.
3. On a machine under 8 GB, or with memory pressure simulated by a lower
   reserve, check that the RIT is skipped with its one-line reason, and that a
   solve afterwards doesn't swap.
4. Start with a fresh data directory: first run builds the WMP and WIT, asks
   about the RIT, and the second run builds nothing.
5. Unit tests at `BOARD_DIM` 15 and 21.

### Recommended changes

**Changes to defaults**

| Change | Reason | MAGPIE default | Macondo default |
|---|---|---|---|
| `-wmp auto` | 3-4x movegen wherever there is room | CLI: on, built on first use. libmagpie: off. WASM: installed by the JS |  |
| `-wit auto` | +9% simmer with a WMP | Off |  |
| `-rit auto` | +17% simmer, +6% static with a WMP | Off |  |
| `-ritmmap true` where the platform maps files and is little-endian | About 400 ms faster startup, one shared copy, pages the OS can reclaim | Off (on for `contribute`) |  |
| libmagpie `magpie_create()` uses `auto` | Library users get the speed without knowing about the tables | Off |  |
| `-savesettings` saves the setting chosen (`auto`, `true` or `false`), not the loaded state | A saved resolved state pins one machine's decision: a RIT skipped for lack of memory stays off after an upgrade | The loaded state, as a boolean |  |

**New features**

| Change | Reason | MAGPIE default | Macondo default |
|---|---|---|---|
| An `auto` value for `-wmp`, `-rit` and `-wit`, decided by `contribute_choose_tables` with a CLI budget (R1, R2) | Use each table only when the machine has room | `true`/`false` only; on with the file missing is an error (except the WMP first-use build) |  |
| Memory budget: subtract the search TT, and cap the tables at a quarter of RAM (R3) | No swapping, and the same decision on every run |  |  |
| Disk budget: a larger reserve, a manifest of automatic builds, a size cap with LRU eviction, and a size estimate (R4) | A personal machine's disk isn't dedicated to MAGPIE |  |  |
| Build the WIT on first use (R5) |  | Built only by `convert kwg2wit` |  |
| Build the RIT on first use when the budget allows, asking first at an interactive prompt (R5) | It is 1.9 GB on disk and 2.4 GB at its build peak | Built only by `make prepare_data` or `convert klvwmp2rit` |  |
| A one-line report of the tables each lexicon load chose (R5) |  |  |  |
| An available-memory figure for macOS, in src/compat (R3) | Without it, `auto` would load a 1.9 GB RIT on any Mac | None: treated as unconstrained |  |

**Fixes**

| Change | Reason | MAGPIE default | Macondo default |
|---|---|---|---|
| Sticky WMP-off (R1) | Under `auto`, a lexicon without a wordmap must not turn WMP off for later lexicons | Stays off after a lexicon with no wordmap |  |
| Record the KLV and KWG hashes in the RIT header, and refuse a mismatch (R6) | `-k1 custom` with an automatic RIT would silently play the stock leaves | Not checked |  |
| One RIT naming scheme for the CLI and `contribute` (R6) | Otherwise a machine used for both keeps two 1.9 GB copies of one table | CLI: `CSW24.rit`. `contribute`: `CSW24.CSW24.rit` |  |

---

## Phase 8: Other strength/performance defaults (todos 4, 7)

Each item below is a separate measurement and, if it wins, a separate
PR. Quality changes use game pairs:

```
set -lex CSW21 -wmp true <variant flags> ...
autoplay games N -gp true -threads 12 -seed 42
```

Estimate each run from a short timed run and state the estimate before
launching. Keep runs under ~1 h.

### Other candidates (todo 7)

Already on, and should stay on unless data says otherwise: `-sinfer`,
`-sbagcycle`, `-pegnested`, `-ebestreply`, and `-threads` = all cores.

The candidates are listed under Recommended changes below.

### Recommended changes

Each waits for its measurement.

**Changes to defaults**

| Change | Reason | MAGPIE default | Macondo default |
|---|---|---|---|
| First-win at the timed cascade's intermediate stages, full window at the final stage, and every candidate tied at a cut line kept | About 3x per endgame solve. Check: decisions and time vs today | Full window | First-win throughout its PEG |
| Skip timed-cascade stages that cut nothing | Saves whole stages on short candidate lists. Check: time at equal decisions | Every stage runs (peg.c:3420-3424) |  |
| `-pegstride` by bag size in the timed cascade | Much faster at 3-4 in the bag; the nested inner solve already uses strides 5 and 7 there (peg.c:1570-1586). Check: decision agreement and time vs full enumeration | 0 (full enumeration) |  |
| Bots solve endgames and PEGs | Stronger autoplay bots, and better data for leavegen and PAT training. Check: game pairs, cost per game | `-eplies1/2` and `-pegbag1/2` 0 (off) | Elite bot: PEG at 1 in the bag only, 2-5 plies by spread, early cutoff on |
| `-ttfraction` 0.5 | Faster endgame and PEG solves, above all the long untimed solves of Phases 1 and 4. Check: solve time at 0.25 vs 0.5 | 0.25 (0.125 in WASM) | 0.20 |
| `-smargin` on | Sim strength in close games. Check: sim game pairs | Off |  |
| PEG `inner_top_k` exposed in the CLI | Only if nested PEG becomes a bottleneck | Fixed at 0 in the CLI (API only) |  |

**New features**

| Change | Reason | MAGPIE default | Macondo default |
|---|---|---|---|
| Early cutoff in the timed cascade (from Macondo) | Fewer wasted solves of losing moves. Check: time saved at identical decisions | None | Off from the shell; on in the bot and analyzer |

---

## Order and dependencies

1. Phase 2 (help/docs). Phase 1's write-up is done.
2. The settings-file decision (cross-cutting) lands before the first
   default-changing PR.
3. Phase 1's wall-time measurement before Phase 3 ships: it decides 3b's
   default and whether 3a's unlimited PEG is safe at 3-4 in the bag.
4. Phase 1:
   - 1c before 1d: exact early cutoff needs exact leaf verdicts.
   - 1e's pessimistic opponent and nested mover turn before 1a makes the
     complete solve the default: they give non-emptiers exact verdicts.
   - 1d's default `leaves1` set lands with Phase 6, which adds the filter.
   - With `-pegpess false`, the averaged-value fix (1e) before 1b is relied
     on for non-emptiers.
   - 1a needs 3a; 1f is independent.
5. Phase 3: 3a, then 3b. These are the user-visible default changes the
   todo asks for directly.
6. Phase 4: 4b and 4c together; 4a's depth change waits for its measurement.
7. Phase 5, then Phase 6. Phase 6 depends on Phase 5's grammar.
8. Phase 7:
   - the sticky WMP-off fix and the RIT hash check and naming (R6) before
     any `auto` RIT;
   - a macOS available-memory figure before `auto` RIT on macOS.
9. Phase 8 measurements. They can run in parallel with the rest, but each
   default flip waits for its data.
