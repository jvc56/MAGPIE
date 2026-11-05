# Locally Adaptive Monte Carlo: plan

Source: Morris Greenberg, "The Past, Present, and Future of Scrabble Engines"
(University of Toronto brown bag, January 13, 2026), slides 29 to 31:
"Future part 1: Locally Adaptive Monte Carlo for Comprehensive Search" and
"Future part 2: Locally Adaptive Monte Carlo on Plies". This document plans
both parts as two phases of work in MAGPIE. It describes the code as of
753d44d5 on main.

Phase 1 is implemented, off by default, on the `lamc-phase1` branch. What
was built, how it differs from the plan, and what the tests found are in
[Phase 1 as built](#phase-1-as-built). Phase 2 is not started. Elsewhere,
option names, file names and defaults are the plan's proposals.

## Contents

- [The idea](#the-idea)
- [Terms](#terms)
- [How the sim works today](#how-the-sim-works-today)
- [Ground rules for both phases](#ground-rules-for-both-phases)
- [The play embedding](#the-play-embedding): the shared, modular feature space
- [Phase 1: adaptive search over the turn's plays](#phase-1-adaptive-search-over-the-turns-plays)
- [Phase 1 as built](#phase-1-as-built): the code, the tests, the results
  and the [open questions](#open-questions)
- [Phase 2: adaptive search at every ply](#phase-2-adaptive-search-at-every-ply)
- [Risks and open questions](#risks-and-open-questions)
- [Future work](#future-work)

## The idea

A standard sim takes the top P plays by static equity as its candidates, and
in every rollout each player answers with its top-equity play. Two
assumptions follow, and the talk targets one in each phase:

1. **The best play is among the top P by static equity.** Phase 1 drops this.
   It embeds every legal play in a continuous feature space, samples a broad
   spread of plays, fits a Gaussian Process (GP) from embedding to win
   percentage, and moves the search toward regions the GP predicts are
   better. The slides' procedure:
   1. Initialize P candidate plays covering a broad region of the space.
   2. Run a small number of rollouts per play to estimate win percentages.
   3. Fit a GP surrogate mapping play embeddings to win probability.
   4. Retain the top K plays by estimated win percentage.
   5. Move the other P - K plays by gradient ascent in the embedding space
      toward higher predicted win probability.

   Steps 2 to 5 repeat until the top K stabilizes.
2. **Each player answers with the top-equity play.** Phase 2 drops this. It
   applies the adaptive search at every ply of a rollout. That multiplies
   the cost, so Phase 2 also makes each tile permutation carry more
   information. The slides propose a rack similarity learned by matrix
   factorization, used the way antithetic variates are used, so that far
   fewer permutations are needed.

The talk's motivating case is Nigel Richards' exchange of 5 (slides 25 to 27):
the engines preferred a scoring play, and expert analysis afterwards made a
strong case for the exchange. An exchange scores nothing and ranks low by
static equity, and the opponent's real replies are not always the
top-equity play. Each phase removes one of those two blind spots.

## Terms

The slides' notation, used throughout:

| Symbol | Meaning |
|---|---|
| N | Legal plays in the position (the whole move list, exchanges and pass included) |
| D | Dimensions of the play embedding |
| P | Plays in the active pool each round |
| K | Plays retained from one round to the next |
| E | Plays evaluated so far (every play that has had rollouts); grows each round |
| p | Plies per rollout (`-plies`) |
| I_perm | Tile permutations (rollouts per play) a standard sim uses |
| I'_perm | Tile permutations an adaptive sim uses; Phase 2 needs I'_perm much smaller than I_perm |
| I_shallow | Rollouts per play per round in the adaptive loop |
| I_adapt | Rounds of the adaptive loop |

Costs from slide 30:

- Standard: T_standard = O(P * p * I_perm).
- Adaptive at every ply: T_adaptive = O(I'_perm * p * I_adapt * [P * I_shallow + GP(P, D)]).

LAMC means Locally Adaptive Monte Carlo below.

## How the sim works today

The places LAMC touches:

- `get_top_simming_move` ([src/impl/simmer.c](../src/impl/simmer.c))
  generates the move list (`MOVE_RECORD_ALL`) and calls `simulate`.
- `simulate` runs inference if asked, then builds a `RandomVariables` of type
  `RANDOM_VARIABLES_SIMMED_PLAYS` over the top `num_plays` plays and hands it
  to `bai()` ([src/impl/bai.h](../src/impl/bai.h)). BAI picks which play gets
  the next sample, prunes plays with the cutoff, and stops on the threshold,
  sample limit or time limit.
- One sample is one rollout, `sim_rollout` in
  [src/impl/random_variable.c](../src/impl/random_variable.c): play the
  candidate, then for each ply play
  `get_top_equity_move_with_carry` (or `census_rollout_get_best_move` when
  `-scensus` is on), then score the horizon with the win percentage table.
  This per-ply choice is where Phase 2 plugs in.
- Samples use common random numbers: every play's n-th sample uses the same
  seed (`rvs_arms_share_seeds`). The seeds come from a PRNG in each
  `SimmedPlay` (`simmed_play_get_seed` in
  [src/ent/sim_results.c](../src/ent/sim_results.c)), which starts over when
  `sim_results_reset` rebuilds the plays. A play simmed again from a rebuilt
  `SimResults` would repeat its earlier draws, so LAMC must keep one
  `SimResults` across its rounds (see [Phase 1](#integration-with-the-sim)).
- `-sbagcycle` already structures the permutations: one sample plays every
  rotation of a shuffled bag. Phase 2's stratified sampling is compared
  against it.
- Static features that already exist and can feed the embedding: score,
  leave value (`klv`), static equity, the placement adjustment
  ([src/ent/static_eval.h](../src/ent/static_eval.h)), the PAT features
  ([src/def/pat_defs.h](../src/def/pat_defs.h),
  [src/ent/pat_features.h](../src/ent/pat_features.h)), and the census
  lattice, which gives every multiset of 0 to 7 tiles a dense rank
  (`census_lattice_rank` in [src/impl/census.h](../src/impl/census.h)).

## Ground rules for both phases

These come from AGENTS.md and apply to every PR below.

- **Free when off.** With LAMC off, the sim must cost nothing measurable: no
  new `SimmedPlay` fields, per-sample stores, or branches the standard path
  pays for. LAMC lives behind its own `RandomVariables` type and its own
  entry point. Every speed benchmark runs both states, and the off state is
  the control that must measure about 0.
- **Equal budgets.** Every strength comparison holds either the rollout
  count or the wall time equal between the two players, and the report says
  which. LAMC spreads its rollouts over more plays; comparing at unequal
  budgets proves nothing.
- **Determinism.** A sim with a fixed seed gives the same answer on 1 thread
  and on N threads, as `simdetsweep` and `simdetigp` check today. LAMC's
  rounds are scheduled from committed statistics only, as BAI's are.
- **Performance work and quality work stay separate.** A PR that changes
  which play is chosen is validated by game pairs. A PR that only makes LAMC
  faster must leave its choices bit-identical.
- **Small PRs.** Each phase lists its PR sequence. No PR mixes a refactor
  with a behavior change.
- **Portability.** All new math (Cholesky, triangular solves, kernels) is
  plain C99 in `src/util` or `src/impl`, with no new dependencies. Binary
  data files are read through the little-endian helpers in `src/compat`,
  which gain a float helper if one is needed.

---

## The play embedding

Both phases search a space of plays. Its dimensions will change more than
anything else in this work: some will turn out useless, new ones will be
proposed, and Phase 2 adds learned ones. So the dimensions sit behind a
registry, and nothing outside a dimension's own code knows what it computes.

### What a dimension is

A dimension (a *feature*) is one value of a plain enum. It may contribute
several columns (the PAT bins, a learned rack vector).

```c
// src/def/play_feature_defs.h
typedef enum {
  PLAY_FEATURE_SCORE,
  PLAY_FEATURE_LEAVE_VALUE,
  PLAY_FEATURE_TILES_PLAYED,
  ...
  NUMBER_OF_PLAY_FEATURES,
} play_feature_t;
```

Everything about a feature is answered by a function that switches on
`play_feature_t`, all in `src/impl/play_feature_registry.c`:

```c
const char *play_feature_get_name(play_feature_t feature);
int play_feature_get_num_columns(play_feature_t feature);
play_feature_kind_t play_feature_get_kind(play_feature_t feature);
play_feature_cost_t play_feature_get_cost(play_feature_t feature);
// Once per position, before any extraction.
void play_feature_prepare(play_feature_t feature, PlayFeatureCtx *ctx);
// Writes num_columns raw values for each of the moves.
void play_feature_extract(play_feature_t feature, const PlayFeatureCtx *ctx,
                          const MoveList *moves, double *out);
```

None of these switches has a `default`. Every feature has a case, and
`NUMBER_OF_PLAY_FEATURES` has one that breaks out of the switch to a
`log_fatal` after it, as `contribute_tables.c` and `players_data.c` do:

```c
const char *play_feature_get_name(const play_feature_t feature) {
  switch (feature) {
  case PLAY_FEATURE_SCORE:
    return "score";
  case PLAY_FEATURE_LEAVE_VALUE:
    return "leave";
  ...
  case NUMBER_OF_PLAY_FEATURES:
    break;
  }
  log_fatal("unknown play feature %d", (int)feature);
  return NULL;
}
```

`-Wall` turns on `-Wswitch`, and the dev and sanitize builds add `-Werror`,
so a feature missing from any switch fails the build. A new feature cannot
be half-registered, and a removed one cannot linger in a switch.

`play_feature_extract` switches once per feature per position, and each case
calls that feature's own function, which loops over the moves. The switch is
not on the per-play path.

`PlayFeatureCtx` holds what extractors read: the game, the player on turn,
the KLV, PAT weights, the unseen tile counts, and a per-feature scratch area
that `prepare` fills. Extractors do not write to the game. A feature with
nothing to prepare has a `prepare` case that only breaks.

The embedding builder (`src/ent/play_embedding.c`) runs `prepare` for each
active feature, calls `extract` for every play, and then standardizes every
column across the position's plays (subtract the mean, divide by the
standard deviation, with a floor for constant columns). Extractors return raw
values and never scale them, so standardization is one piece of code that no
feature can get wrong. Categorical columns (move type, direction) are one-hot
and left unscaled.

### Selecting dimensions

- `-lamcfeatures score,leave,tiles,...` names the active set by feature
  name. Parsing loops from 0 to `NUMBER_OF_PLAY_FEATURES`, comparing
  `play_feature_get_name`, so it needs no table of its own. An unknown name is
  a config error that lists the known names.
- The default set is one string constant in `play_feature_defs.h`, so
  changing the defaults is a one-line PR.
- Phase 2's per-ply search has its own `-lamcplyfeatures`, so the turn search
  and the ply search can use different (for the ply search, cheaper) sets.

### Hyperparameters are keyed by name

The GP's per-dimension length scales live with the feature, by name, in a
small text file (`data/lamc/<lexicon>.hyp`, one `name value` line per
column), never by column index. Removing or reordering features cannot
misalign them. A feature with no entry gets a default and a warning through
the status mechanism.

### Adding, removing and updating a dimension

To **add** one:

1. Add one value to `play_feature_t`, before `NUMBER_OF_PLAY_FEATURES`.
2. Build. The compiler names every switch in `play_feature_registry.c` that
   lacks the new case; give it one in each.
3. Write its extract function (and its prepare function, if it has one) in
   the matching group file: `play_feature_equity.c`, `play_feature_board.c`,
   `play_feature_rack.c`, or a new file for a new group.
4. Add a semantic unit test for it (below). The contract tests cover it
   without any change.
5. Run the `lamcablate` on-demand test (Phase 1 testing) and put its
   numbers in the PR.

To **remove** one, delete its enum value. The compiler then names each case
label that refers to it; delete those cases, its functions and its semantic
test. Configs that name it fail with the unknown-name error, which says what
to fix.

To **update** one, change its extract function and its test. Nothing else
reads its values.

### Tests every feature gets for free

One test, `playfeatures`, loops over every `play_feature_t` value, so a new
feature is covered the moment it is added:

- Deterministic: two extractions of the same play in the same position are
  bit-identical.
- Finite: no NaN or infinity for any play in a fixed set of positions,
  exchanges and pass included.
- Read-only: the game's board, racks and bag are unchanged after `prepare`
  and `extract` (compare against a copy).
- Width: exactly `num_columns` values are written (guard values around the
  output buffer).
- Board size: runs under both `BOARD_DIM=15` and `BOARD_DIM=21`, as CI does.

### First set of dimensions

The slides name three families: equity components, board features and rack
structure. The first set, to be pruned or kept by ablation:

| Family | Feature | Columns | Cost | Notes |
|---|---|---|---|---|
| Equity | `score` | 1 | cheap | |
| Equity | `leave` | 1 | cheap | KLV value of the leave |
| Equity | `pat` | 1 | moderate | PAT term of static equity, 0 when PAT is off |
| Equity | `placement` | 1 | cheap | Opening placement adjustment |
| Move | `type` | 3 | cheap | One-hot: tile placement, exchange, pass |
| Move | `tiles` | 1 | cheap | Tiles played (exchanged, for an exchange) |
| Move | `bagafter` | 1 | cheap | Tiles left in the bag after the draw; matters near the end |
| Board | `opp_access` | 4 | moderate | Opponent's post-move access to TWS, DWS, TLS, DLS: summed PAT bins by premium class |
| Board | `hooks` | 2 | moderate | Hook spots and floaters the play creates |
| Board | `lanes` | 2 | moderate | Open bingo lanes created and blocked |
| Rack | `leave_shape` | 5 | cheap | Leave size, vowels, consonants, blanks, S count |
| Rack | `leave_dups` | 1 | cheap | Duplicate tiles kept |

About 23 columns. The slides call the space high-dimensional; a GP with
automatic relevance determination (ARD) handles this size, and ablation will
likely shrink it.

Exchanges have no placement, so their board columns describe the unchanged
board. That is correct, not a gap: an exchange's "board change" is none.

---

## Phase 1: adaptive search over the turn's plays

### Goal

Pick a better play than the standard sim at the same rollout budget, by
looking beyond the top P by static equity.

### The algorithm

Defaults are starting points for tuning.

0. **Embed.** Generate every legal play (N of them) into a LAMC-owned move
   list and build the N x D embedding. Optionally drop plays more than
   `-lamcfloor` points of static equity below the best, to bound N in
   positions with blanks.
1. **Initialize the pool** of P plays: the top `P_seed = P / 4` by static
   equity, which keeps the standard sim's strongest candidates in view, then
   farthest-point sampling (greedy k-center in the standardized space) for
   the rest. This covers the space broadly, as step 1 of the slides asks.
2. **Sample.** Give each play in the pool I_shallow more rollouts (round
   robin; I_shallow = 8 to start). A play's samples continue its own seed
   sequence, so no draw repeats, and all plays still share their n-th seed.
3. **Fit the GP** on every evaluated play in E, not only the pool:
   - Input: the play's standardized embedding.
   - Target: the sample mean of win percentage. A logit transform is the
     first variant to test, since win percentage bunches near 0 and 1.
   - Noise: each point's own variance, sample variance / n
     (heteroscedastic), so a play with 40 rollouts counts for more than one
     with 8.
   - Kernel: Matern 5/2 with ARD length scales, from the hyperparameter
     file; a constant mean (the variant: a linear mean on static equity).
   - Size: E stays at most `E_max = 256`, so the Cholesky factorization is
     at most 256 x 256.
4. **Retain** the top K plays of E by GP posterior mean (K = P / 4 to
   start). The posterior mean pools a play's own samples with its
   neighbors', which is steadier than the raw mean at I_shallow samples. A
   play that left the pool earlier can come back.
5. **Propose** P - K new plays, through the proposal strategy:
   - `ascent` (the slides' step 5): from each non-retained play's embedding
     x, step to x + r * g / |g|, where g is the gradient of the GP mean
     (analytic for the Matern kernel) and r a trust-region radius in
     standardized units (the "locally" in the name). Categorical columns do
     not move. Snap the target to the nearest unevaluated legal play. If
     several targets snap to one play, the later ones take their next
     nearest.
   - `acquire` (variant): score every unevaluated legal play by an upper
     confidence bound, mean + beta * standard deviation, and take the top
     P - K. The plays form a finite set, so this maximizes the acquisition
     exactly instead of by gradient steps and snapping.

   Both strategies sit behind one interface, so they can be compared
   directly and new ones added.
6. **Stop exploring** when the top K is unchanged for `S = 2` rounds, after
   `I_adapt` rounds, or once the exploration share of the budget
   (`-lamcexplore 0.5`) is spent.
7. **Decide.** Hand the top K to BAI with the remaining budget. BAI's
   cutoff, threshold and time limit then pick the play, with the same
   statistical stopping guarantees as today.

Budget: total rollouts equal `-iterations`, and the explore and decide
stages share it. LAMC never takes more rollouts than the standard sim would.

### Integration with the sim

- **Entry point.** `simulate` calls `lamc_simulate` when LAMC is on, after
  inference and before BAI. The standard path does not change.
- **One `SimResults` across rounds.** `sim_results_reset` reserves room for
  `E_max` plays when LAMC is on, and a new `sim_results_add_play` appends
  each newly evaluated play. Every play keeps its `SimmedPlay` and its seed
  PRNG for the whole sim, which keeps the draws fresh and the common random
  numbers intact.
- **A subset view.** A new random variable type,
  `RANDOM_VARIABLES_SIMMED_PLAY_SUBSET`, maps BAI's arm k to the play at
  `pool[k]`, so both the round sampling and the final BAI run over
  whichever plays are active. A separate type, rather than a field checked
  in `rv_sim_sample_impl`, keeps the standard path free when LAMC is off.
- **The final BAI.** First question for PR 1d: can `bai()` start from the
  samples the rounds already took, as the resume path does for `SimmedPlay`
  statistics? If not, the final stage starts BAI fresh on the finalists with
  the remaining budget, and the rounds' samples still feed the display and
  the GP.
- **Display.** `shmoves` and the status output show the evaluated plays as
  today. LAMC adds a summary line (rounds, plays evaluated, plays outside the
  static top P, and whether the winner is one of them) through the status
  mechanism, never `printf`.
- **Trace.** `-lamctrace <file>` writes each round's pool, GP predictions and
  proposals, for the analysis tests below. It is off by default and costs
  nothing when off.

### Modules

| File | Contents |
|---|---|
| `src/def/play_feature_defs.h` | `play_feature_t`, kinds, cost tiers, default set |
| `src/ent/play_feature.h` | `PlayFeatureCtx` |
| `src/impl/play_feature_equity.c`, `_board.c`, `_rack.c` | Extract functions, by family |
| `src/impl/play_feature_registry.c/.h` | The switches on `play_feature_t`, lookup by name, parsing `-lamcfeatures` |
| `src/ent/play_embedding.c/.h` | N x D matrix, standardization, nearest-neighbor search |
| `src/util/dense_linalg.c/.h` | Cholesky, triangular solves, jitter on failure |
| `src/impl/gp.c/.h` | Kernels (pluggable by enum), fit, predict, gradient of the mean, hyperparameter file |
| `src/impl/lamc_proposal.c/.h` | Initialization and the proposal strategies |
| `src/impl/lamc.c/.h` | The rounds, budget and stopping rules |
| `src/ent/lamc_args.h` | Parameters, filled the way `sim_args_fill` fills `SimArgs` |

Dependencies point one way: features read entities, the embedding reads the
registry, the GP reads only matrices, and `lamc.c` reads all of them.
`find_circ_deps.py` checks this on every PR.

### Options

`-lamc` (with `-lamc1`/`-lamc2` per player, as `-pl1`/`-pl2` work today),
`-lamcpool` (P), `-lamckeep` (K), `-lamcshallow` (I_shallow),
`-lamcrounds` (the cap on I_adapt), `-lamcexplore`, `-lamcfloor`,
`-lamcfeatures`, `-lamcproposal ascent|acquire`, `-lamctrace`. All off or
unset by default. Each gets a help entry and a `config_test` case.

### PR sequence

| PR | Contents | Behavior change |
|---|---|---|
| 1a | Feature registry, first features, embedding, `playfeatures` test | None (unused) |
| 1b | `dense_linalg`, GP, their unit tests | None (unused) |
| 1c | `SimResults` capacity and append, subset random variable type; free-when-off benchmark | None |
| 1d | `lamc.c`, proposal strategies, options, determinism tests | Only with `-lamc true` |
| 1e | Trace output and the on-demand analysis tests below | None |
| 1f | Tuned defaults and hyperparameter file, with the strength results | Only with `-lamc true` |
| 1g | If the acceptance criteria are met: decide whether `-lamc` becomes the default | Default |

### Phase 1 testing and proof

#### Correctness, in CI (`lamc` and `playfeatures` test keys)

- **Linear algebra.** Cholesky of known matrices against hand-computed
  factors; solving with a factor reproduces the right-hand side; a matrix
  that is not positive definite fails cleanly or is fixed by jitter.
- **GP.** With no noise, the posterior mean interpolates the training points
  and the variance there is near 0. The variance grows away from the data.
  The analytic gradient of the mean matches central finite differences to
  1e-6. On a known smooth function of 2 dimensions, the GP recovers the
  maximizer's neighborhood from 30 points.
- **Proposal.** Snapping never returns an evaluated play. Duplicates resolve
  to distinct plays. Categorical columns never move. Initialization always
  includes the top `P_seed` static plays.
- **Budget.** Over a sweep of options, LAMC's rollouts never exceed
  `-iterations`, and the explore share is respected.
- **Determinism.** The same seed gives the same chosen play and the same
  statistics on 1, 2 and 8 threads (as `simdetsweep` does).
- **Off is unchanged.** With `-lamc false`, every existing test passes
  untouched, and a fixed autoplay run gives results bit-identical to main.

#### Surrogate quality and dimension ablation (on demand: `lamcsurrogate`, `lamcablate`)

These test whether the embedding carries signal, which the whole method rests on.

- **Position set.** A `genlamccgps` on-demand test, like `genpegcgps`,
  writes a few hundred positions from autoplay, split by game stage (bag
  size).
- **Reference values.** For each position, sim a broad set of plays with a
  large budget: the top 100 by static equity plus 100 plays chosen by
  farthest-point sampling, at a fixed seed.
- **Surrogate quality.** Train the GP on a random 30% of the plays and
  predict the rest. Report the Spearman rank correlation and the held-out
  predictive log likelihood. Compare against static equity alone as the
  predictor. The GP must beat static equity, or the space carries no extra
  signal.
- **Ablation.** `lamcablate` reruns the surrogate test once without each
  feature in turn, and once with each feature alone added to `score`, and
  prints the change in held-out log likelihood per feature. This is the
  evidence a feature PR attaches, and the evidence for removing one.

#### Search quality against a reference (on demand: `lamcoracle`)

- For each position, the reference best play is the best of the union of
  every play any method evaluated plus the reference set above, simmed to a
  tight confidence. This is the practical ground truth: the true best might
  lie outside every set, and the report says so.
- At equal rollout budgets (for example 2,000, 5,000 and 20,000), compare
  standard sim and LAMC on:
  - **Agreement:** the share of positions where the method picks the
    reference best play.
  - **Regret:** the reference win percentage of the reference best play
    minus that of the chosen play, averaged.
  - **Discovery:** among positions whose reference best play is outside the
    static top P, the share where LAMC finds it. This is the case Phase 1
    exists for.
- Repeat on positions with few enough tiles in the bag for PEG to solve
  (within its `-pegbag1`/`-pegbag2` range). There the exact solver gives a true best play, with no
  reference sim needed.
- Add the slides' exchange-5 position (transcribed from slide 26 into a
  CGP) to a small named set, and report each method's ranking of the
  exchange. This is a qualitative check, not a pass or fail test.

#### Strength (game pairs)

LAMC against the standard sim, same rollout budget, game pairs, one thread
per sim for reproducibility:

```bash
set -lex CSW21 -wmp true -s1 equity -s2 equity -r1 all -r2 all \
    -pl1 2 -pl2 2 -np1 15 -np2 15 -i1 2000 -i2 2000 -mi1 10 -mi2 10 \
    -lamc1 true -lamc2 false -threads 32 -mtmode pgp
autoplay games 5000 -gp true -seed 42
```

Then the same at equal wall time (`-tl1`, `-tl2`), which charges LAMC for
its overhead. Then LAMC against static play, to compare with the standard
sim's 59 to 60% against static from the talk.

- Fix the number of pairs before the run, and do not stop early on a good
  result. Report the pentanomial counts autoplay already keeps for game
  pairs, with a confidence interval.
- Break results down by game stage (bag size at the time of each LAMC
  decision), since the gain may be concentrated in some stages.
- Register the runs as on-demand tests (`lamcquality`, following
  `sim_quality_test.c`) so anyone can repeat them.

#### Speed

- Time each LAMC stage (embedding, GP fits, proposals, rollouts) through the
  status output, and profile `make release` builds per AGENTS.md.
- Overhead target: LAMC's non-rollout work under 5% of sim time at the
  default budget.
- Free when off: the standard sim at equal settings, LAMC off, against main.
  The difference must be within noise.

#### Phase 1 acceptance

Phase 1 ships behind `-lamc` when all hold:

1. All correctness tests pass in CI under both board sizes.
2. Off is bit-identical to main and speed-neutral.
3. The GP beats static equity at predicting held-out plays.
4. Regret at equal rollouts is lower than the standard sim's on the
   reference set.
5. Game pairs at equal rollouts show a gain whose confidence interval
   excludes 0. At equal time, LAMC is no worse.

Making it the default (PR 1g) also requires the equal-time result to be a
gain.

---

## Phase 1 as built

### Summary

Phase 1 works, and it helps only a little and only late in the game.

- **What it does.** `-lamc true` (or `-lamc1`/`-lamc2` in autoplay) has a
  search over every play within 30 points of the best choose the plays a
  sim simulates, as the slides propose, then simulates them as usual with
  the rest of the budget. It is off by default, and with it off the sim is
  unchanged.
- **Where it helps.** On recorded rollouts of 768 fresh positions with 1 to
  20 tiles in the bag, it lowers regret against the standard sim (the top
  15 by static equity) by 0.06 points of win percentage at 3,000 rollouts
  (1.8 standard errors) and 0.09 at 10,000 (2.6). Its surrogate ranks those
  plays better than static equity does (rank correlation 0.59 against
  0.54).
- **Where it does not.** With 30 or more tiles in the bag, static equity's
  top 15 nearly always holds the best play, the surrogate ranks plays
  slightly worse than static equity, and the search's rollouts are lost to
  the final choice. So by default LAMC searches only with 20 or fewer tiles
  in the bag, and always hands the sim static equity's top 10.
- **In games.** 1,500 game pairs at 3,000 rollouts are neutral (50.2%,
  -0.04 points of spread per pair, SE 0.84), as they must be: a gain of a
  few tenths of a point of win rate needs on the order of 100,000 games.
- **What made it work.** A prior mean linear in static equity (without it
  the surrogate is no better than static equity), acquisition by upper
  confidence bound rather than the slides' gradient step, keeping static
  equity's best plays in the pool and among the candidates, and letting the
  sim continue the search's samples.

Against the plan's acceptance criteria: the `lamc` test passes (with the
sanitizers), and every file builds with `BOARD_DIM=21` and for Windows;
LAMC off gives byte-identical autoplay results to main at the same speed;
the GP beats static equity at ranking late plays but not midgame ones;
regret is lower in the late game; and the game pairs do not show a gain. So
it stays behind `-lamc` and should not become a default.

### The code

| File | Contents |
|---|---|
| `src/def/play_feature_defs.h` | `play_feature_t`, feature kinds, the default feature set |
| `src/ent/play_feature.h` | `PlayFeatureCtx`, what extraction reads |
| `src/impl/play_feature_registry.c/.h` | The switches on `play_feature_t`, lookup by name, parsing `-lamcfeatures` |
| `src/impl/play_feature_equity.c`, `_rack.c`, `_board.c` | The extract functions, by family |
| `src/util/dense_linalg.c/.h` | Cholesky factorization and triangular solves |
| `src/impl/gp.c/.h` | The Gaussian process: Matern 5/2 or squared exponential kernel, fit, predict, gradient of the mean, log marginal likelihood |
| `src/def/lamc_defs.h`, `src/ent/lamc_args.h` | Kernel and proposal enums, defaults, `LamcArgs` (a field of `SimArgs`) |
| `src/impl/lamc.c/.h` | The search: universe, embedding, rounds, fit, proposals, candidates |
| `src/impl/random_variable.c/.h` | Two new types: `RANDOM_VARIABLES_SUBSET`, a view of some arms of another random variable that can accumulate their samples, and `RANDOM_VARIABLES_TABLE`, which replays recorded samples |
| `src/ent/sim_results.c/.h` | `sim_results_reset_from_plays`, which seeds a sim with plays' statistics and seed streams from another |
| `src/impl/simmer.c` | The hook: after inference, before the standard sim |
| `test/lamc_test.c` | The `lamc` CI test and the `lamcreplay` and `lamcautoplay` on-demand tests |

The features are a plain enum. Every property of a feature is a function
that switches on it with no `default`, so `-Wswitch` (on with `-Wall`, an
error under `-Werror`) names every switch a new or removed feature leaves
unhandled. The registry is the only file that knows the features; the
embedding, the GP and the search see columns of numbers.

### How a search runs

With `-lamc true` (or `-lamc1`/`-lamc2` in autoplay), and the bag holding at
least one tile and at most `-lamcmaxbag` (20). Defaults are in parentheses;
they are the best settings the replay found.

1. **Universe.** Every play by static equity, at most `-lamcuniverse`
   (400), within `-lamcfloor` (30) points of the best. If there are no more
   than the sim would simulate anyway, the search does not run.
2. **Embedding.** Each active feature's columns (`-lamcfeatures`; the
   default set is every feature but `equity` and `leavedups`), then every
   continuous column standardized across the universe. Static equity,
   standardized, is kept apart for the prior mean.
3. **First pool.** `-lamcseed` (12) plays by static equity, then the rest of
   `-lamcpool` (16) by farthest-point sampling in the embedding.
4. **Rounds.** Each pool play gets `-lamcshallow` (4) samples: BAI with
   round robin, no threshold, over a subset view of a sim over the universe,
   so the round's samples use the sim's threads and its deterministic
   scheduling. The view adds each sample to per-play sums in BAI's fixed
   point, so the statistics do not depend on thread timing. A play's samples
   continue its own seed stream, so no draw repeats across rounds. Search
   samples are single rollouts unless `-lamccycle` makes them bag cycles.
5. **Fit.** The prior mean, linear in static equity (`-lamcequitymean`, on)
   or constant, is fit by least squares weighted by each play's precision;
   the GP fits the residuals. Each play's noise is a pooled per-sample
   variance over its sample count. Each fit chooses its length scale among
   0.5, 1, 2 and 4 times `-lamclength` (1.5), and its signal variance among
   0.1, 0.3 and 1 times the residuals' excess variance, by marginal
   likelihood. A small signal variance leaves the predictions on the prior
   mean when the samples show nothing beyond it.
6. **Keep and propose.** The top `-lamckeep` (4) by the surrogate stay; the
   rest of the pool is new plays, by `acquire` (the highest upper confidence
   bounds, `-lamcbeta` (1) standard deviations; the default) or `ascent` (a
   step of `-lamcradius` up the gradient of the mean from each dropped play,
   snapped to the nearest unevaluated play, as in the slides).
7. **Stop** after `-lamcrounds` (12) rounds, when the kept plays are the
   same for `-lamcstable` (3) rounds, or when the next round would pass the
   `-lamcexplore` (0.3) share of the sim's rollouts or time.
8. **Candidates.** The first `-lamcfinalstatic` (10) are the top static
   plays; the rest are the best by the surrogate's mean, among all plays
   (`-lamcfinalall`, on) or among the evaluated ones. There are as many as
   the sim would simulate, unless `-lamcfinal` says otherwise.
9. **The sim** of the candidates runs as usual, with what is left of the
   rollouts and time. With `-lamcreuse` (on), it resumes from the search's
   samples of the candidates (their statistics and seed streams); otherwise
   it starts over with other seeds, so that plays the search favored for
   lucky draws do not get those draws again.

With LAMC off, a sim pays one test of a flag per sim, and nothing per
sample.

### Differences from the plan

- **No hyperparameter file.** One length scale for every column, with each
  fit choosing among its multiples, instead of per-feature length scales
  stored by name. The ablation below suggests the next step: per-feature
  scales, or dropping features that do not help.
- **The final BAI question is settled by resuming.** BAI keeps its own arm
  statistics from zero, but `sim_results_reset_from_plays` starts the sim's
  plays from the search's statistics and seed streams, and the sim picks its
  play from those, so the search's samples count (`-lamcreuse`).
- **One `SimResults` for the search, over the universe,** instead of one that
  grows: every play in the universe has its seed stream from the start, so
  nothing needs appending.
- **Added after the first results:** the prior mean linear in static equity,
  candidates among all plays, the floor of static candidates, sample reuse
  and the bag limit. Each answered a failure the replay showed.
- **Tests.** One harness, `lamcreplay`, does the work of the planned
  `lamcsurrogate`, `lamcablate` and `lamcoracle`. Not done: the PEG
  comparison, the exchange-5 position from the slides, `-lamctrace`, and
  the status line for `shmoves`.

### The tests

**`lamc` (CI).** Cholesky and solves against known factors; the GP
interpolates without noise, returns to its prior far from the data, has
gradients matching finite differences, and finds the maximum of a bowl, for
both kernels; every feature, through the registry, writes exactly its
columns, finite and deterministic, without changing the game, with
categorical columns one-hot; feature sets parse and reject unknown,
repeated and empty names, through the config too; and LAMC sims, in four
configurations, give identical results on 1 and 4 threads while spending
their share of the budget.

**`lamcreplay` (on demand).** Records every play's rollouts in a set of
positions once, then has the standard sim and LAMC variants choose plays
from the recordings at equal budgets. Each play's first half of samples is
what the methods draw; the second half estimates its true win percentage.
Reports, per variant and budget, against the standard sim (the top
`LAMC_REPLAY_PLAYS` by static equity, sampled evenly):

- **regret:** the true win percentage of the best play recorded, less that
  of the chosen play, in percentage points, averaged over positions, and
  the paired difference with its standard error;
- **set regret:** the same for the best play among the candidates, which
  measures the candidate set apart from the final choice;
- **rank correlation:** Spearman's, between the true win percentages and the
  surrogate's means, and between them and static equity.

Its settings come from the environment (see the comment in the test):
positions, stage (`mid`, 30 or more tiles in the bag, or `late`, 1 to 20),
budgets, recorded samples, universe size, threads, sample reuse, a game
offset for fresh positions, and the variants as `|`-separated option lists.

**`lamcautoplay` (on demand).** Autoplay with the given settings, printing
the results with LAMC's totals: searches, rounds, plays evaluated, search
rollouts and time, and how often the chosen play was outside the static
top plays.

### Results

All replay numbers are regret in percentage points of win percentage:
LAMC's minus the standard sim's, with its standard error, so negative favors
LAMC. Both choose from the same recorded samples at the same budget; the
standard sim takes the top 15 plays by static equity. In the replay both
the standard sim and LAMC's final stage sample their plays evenly, where the
real sim uses BAI, so the replay compares candidate choice and budget, not
BAI's allocation. Rollouts are 2 plies,
CSW21, single rollouts (no bag cycling), no inference; each position's
universe is its top 100 plays by static equity. "Set regret" is the best
play LAMC (or the standard sim) handed the final stage, against the best
play recorded; "rank correlation" is Spearman's, of the surrogate's means
or of static equity with the true win percentages, over all 100 plays.

**Midgame (30 or more tiles in the bag), 384 positions, 250 recorded
samples per play for the methods and 250 for the truth.** Static equity's
top 15 almost always holds the best play: its set regret is 0.007 points.
There is nothing for a wider search to find, and every rollout the search
spends is lost to the final choice. These are the positions the floor and
the explore share were chosen on; no fresh midgame set was run.

| Variant | 1,000 rollouts | 3,000 rollouts | Set regret | Rank correlation |
|---|---|---|---|---|
| Equity mean, all candidates, no static floor, explore 0.5 | +0.18 (0.05) | +0.11 (0.04) | 0.111 | 0.721 (static 0.746) |
| The same, static floor of 10 | +0.07 (0.03) | +0.004 (0.007) | 0.007 | 0.721 |
| The defaults (floor 10, explore 0.3, signal variance fit) | +0.04 (0.02) | +0.005 (0.006) | 0.007 | 0.718 |

The surrogate ranks midgame plays slightly *worse* than static equity, so
the floor is what keeps LAMC from harm there, and the bag limit keeps it out
of the midgame by default.

**Late game (1 to 20 tiles in the bag).** Here static equity's top 15
misses the best play more often (set regret about 0.3 to 0.4 points), and the
surrogate with an equity prior mean ranks plays *better* than static equity.

All three sets below are fresh: none of their positions was used for
tuning.

| Positions (samples per play) | Variant | 1,000 | 3,000 | 10,000 | Set regret, standard / LAMC | Rank correlation, LAMC / static |
|---|---|---|---|---|---|---|
| 768 (250) | Equity mean, all candidates, no floor, explore 0.5 | +0.03 (0.04) | +0.01 (0.03) | | 0.289 / 0.276 | |
| 384 (800) | The same | | -0.10 (0.12) | -0.12 (0.12) | 0.411 / 0.302 | 0.584 / 0.531 |
| 384 (800) | The same, static floor of 10 | | -0.21 (0.11) | -0.22 (0.11) | 0.411 / 0.197 | 0.584 / 0.531 |
| 384 (800) | Constant mean, all candidates, no floor | | -0.01 (0.14) | -0.02 (0.14) | 0.411 / 0.396 | 0.502 / 0.531 |
| 768 (400 / 700) | The defaults | | -0.06 (0.03) | -0.09 (0.03) | 0.417 / 0.334 | 0.586 / 0.544 |

The last row is the best estimate: on 768 fresh late positions, LAMC with
its defaults lowers regret by 0.06 points of win percentage at 3,000
rollouts (1.8 standard errors from zero) and 0.09 at 10,000 (2.6). At
10,000 the set regrets are 0.411 and 0.324 and the rank correlations 0.588
and 0.547. Its candidate sets are better
(0.33 against 0.42 points of set regret at 3,000), and the final stage turns
0.06 of that 0.08 into lower regret at 3,000 and all of it (0.09) at
10,000. The gain is small: 0.06 to 0.09 points of win percentage per late
decision, so with about three such decisions a game, 0.2 to 0.3 points of win
rate per game, far below what the game pairs below can measure.

To repeat the last row at 3,000 rollouts (an empty variant is the defaults;
`make magpie_test BUILD=no_pgo_release` first; about 20 minutes on 12
cores):

```bash
LAMC_REPLAY_FIRST_GAME=300000 LAMC_REPLAY_STAGE=late LAMC_REPLAY_REUSE=1 \
LAMC_REPLAY_POSITIONS=768 LAMC_REPLAY_SAMPLES=400 LAMC_REPLAY_BUDGETS=3000 \
LAMC_REPLAY_VARIANTS="" ./bin/magpie_test lamcreplay
```

For 10,000 rollouts, record 700 samples per play: the standard sim takes
about 670 of each of its 15 plays.

**What the tuning found** (late game, 384 positions used for tuning, 250
samples; the differences are within noise one by one, but consistent):

- **Prior mean.** A constant prior mean makes the surrogate rank worse than
  static equity (0.502 against 0.531 on the fresh late set) and LAMC no
  better than the standard sim. The prior mean linear in static equity is
  the one change that made the surrogate useful.
- **Proposals.** With the equity prior mean, `acquire` beat `ascent` (the
  slides' gradient step) at 3,000 rollouts: -0.10 (0.13) against -0.05
  (0.13) on 384 positions, both with candidates from all plays, and -0.29
  (0.29) against -0.17 (0.27) on 96, where `acquire` took its candidates
  from the evaluated plays and `ascent` from all. A gradient of a surrogate
  fit to a few noisy samples per play is mostly noise.
- **Features.** At 3,000 rollouts: the default set -0.19 (0.11); rack
  features only (score, leave, tiles, type, bagafter, leaveshape,
  leavedups) -0.12 (0.11); board features only (score, leave, type,
  location, direction, premium, perimeter) 0.00 (0.10); score, leave and
  equity only -0.02 (0.12); the default set with leavedups -0.17 (0.10). The
  rack features carry most of the gain and the board features add to them;
  neither alone, nor the equity terms alone, does much.
- **Sample reuse.** Counting the search's samples in the final choice cut
  regret by 0.06 to 0.08 points at 1,000 rollouts in each of five variants.
- **Explore share.** At 1,000 rollouts: 0.2 +0.20 (0.13), 0.3 about -0.04,
  0.5 -0.10 (0.12) late, but 0.3 beat 0.5 in the midgame. At 3,000 the
  search stops on stable kept plays before its share runs out.
- **Static plays in the first pool.** At 3,000 rollouts, taking 12 of the
  16 by static equity instead of 4 cut set regret from 0.30 to 0.21 and
  regret from -0.10 to -0.21 against the standard sim; at 1,000 it made no
  clear difference.
- **Pool shape.** A pool of 24 keeping 6, with 16 static plays in the first
  pool, did worse than 16 keeping 4 with 12: +0.17 (0.13) against -0.04
  (0.13) at 1,000 rollouts and -0.01 (0.13) against -0.19 (0.11) at 3,000.
  Larger pools spend the search's share in fewer rounds.
- Length scale (1.5 or 3) and `-lamcbeta` (1 or 2) made no clear
  difference.

**Game pairs.** LAMC with its defaults (`-lamc1 true`) against the standard
sim, both simming 15 plays at 2 plies with 3,000 rollouts, no threshold,
no inference, CSW21, one thread per game (`-mtmode pgp`), seed 12: 1,500
pairs (3,000 games). LAMC scored 50.22% (1,506.5 to 1,493.5), "better with
confidence 58.7%", and -0.04 points of spread per pair (SE 0.84; 95%
interval -1.7 to +1.6). The 1,688 games of pairs that diverged scored 50.39%.
LAMC searched 6,246 times (about two searches a game per LAMC player),
5.4 rounds and 67 plays each, 339 rollouts per search, and the sim chose a
play outside the static top 15 in 377 of them (6%). So the games are
neutral, as they must be at this size: a gain of 0.2 to 0.3 points of win
rate needs on the order of 100,000 games to see. Use `lamcautoplay` to
repeat this:

```bash
LAMC_AUTOPLAY_SETTINGS="-pl1 2 -pl2 2 -np1 15 -np2 15 -i1 3000 -i2 3000 \
  -mi1 10 -mi2 10 -sc1 none -sc2 none -tl1 0 -tl2 0 -sinfer false \
  -lamc1 true -threads 12 -mtmode pgp" \
LAMC_AUTOPLAY_COMMAND="autoplay games 1500 -gp true -seed 12" \
./bin/magpie_test lamcautoplay
```

**Speed.** 160 game pairs, both players simming at 3,000 rollouts on 12
cores (`-mtmode pgp`), timed one after another on an idle machine:

| Binary | Seconds |
|---|---|
| main (753d44d5) | 322, 304 |
| This branch, LAMC off | 310, 305 |
| This branch, `-lamc true` for both players | 308 |

LAMC off gives byte-identical results to main, at the same speed. With it on,
games take the same time: the search spends part of the sim's rollouts, and
its own work (movegen of every play, features, a dozen small GP fits a
round) is small beside them. In the 3,000-game run a search took 168 ms on
average including its 339 rollouts, with 12 games running at once.

### Every run

This is the record of every replay and game run that finished, in the
order they ran, so later work can check a claim above or pick up a thread
without rerunning. The summaries above quote from it.

**Reading the replay tables.** Every number is in percentage points of win
percentage. "Standard" and "LAMC" are mean regret over the positions, with
its standard error; "Difference" is LAMC's minus the standard sim's, paired
by position, with its standard error, so negative favors LAMC. "Set regret"
is the standard sim's and LAMC's, for the plays handed to the final stage.
"Outside" counts the positions where LAMC chose a play outside static
equity's top 15. "Rounds" and "Evaluated" are per search, on average.
Every run used CSW21, 2 plies, single rollouts, no inference, and a universe
of each position's top 100 plays by static equity with no equity floor (so
the live defaults of 400 plays within 30 points were never replayed).

Variants are named by the options they add to the defaults of the time:

| Name | Options |
|---|---|
| mean | `-lamcequitymean true` |
| all | `-lamcfinalall true` |
| acquire | `-lamcproposal acquire` |
| seed *n* | `-lamcseed n` |
| explore *x* | `-lamcexplore x` |
| floor *n* | `-lamcfinalstatic n` |
| features *list* | `-lamcfeatures list`; "rack" is score, leave, tiles, type, bagafter, leaveshape, leavedups; "board" is score, leave, type, location, direction, premium, perimeter |

Until `be3d3a03` the defaults were: constant prior mean, `ascent`,
candidates from the evaluated plays, 4 static plays in the first pool,
explore 0.3, no static floor, and no fit of the signal variance. Sample
reuse is the replay's `LAMC_REPLAY_REUSE`; it was off until run 5.

Position sets come from games of top static equity plays. Each set starts at
a game number, so sets with different starting games share no positions:

| Set | Starting game | Used for |
|---|---|---|
| Midgame (30 or more tiles in the bag) | 1 | Tuning |
| Late (1 to 20 tiles in the bag) | 1 | Tuning |
| Late, fresh A | 100,000 | Checking |
| Late, fresh B | 200,000 | Checking |
| Late, fresh C | 300,000 | The final numbers |

Two early runs are left out: a 24-position smoke test, and a run whose
variants inherited each other's options through the shared config (the bug
fixed by giving each variant a fresh config).

**Run 1.** 96 midgame positions, 150 samples per play, defaults of the time,
no reuse. The 3,000-rollout budget ran out of recorded samples.

| Variant | Budget | Standard | LAMC | Difference | Set regret | Outside | Rounds | Evaluated |
|---|---|---|---|---|---|---|---|---|
| Defaults | 1,000 | 0.346 (0.086) | 0.529 (0.111) | +0.183 (0.096) | 0.081 / 0.232 | 7 | 4.8 | 61.9 |
| acquire | 1,000 | 0.346 (0.086) | 0.726 (0.148) | +0.380 (0.131) | 0.081 / 0.396 | 8 | 4.9 | 62.9 |
| features score, leave, equity | 1,000 | 0.346 (0.086) | 0.648 (0.139) | +0.302 (0.116) | 0.081 / 0.407 | 5 | 4.7 | 60.8 |

The first version lost to the standard sim, and acquisition alone made it
worse. This is what led to the equity prior mean.

**Run 2.** The same 96 midgame positions, 250 samples per play, no reuse.

| Variant | Budget | Standard | LAMC | Difference | Set regret | Outside | Rounds | Evaluated |
|---|---|---|---|---|---|---|---|---|
| Defaults | 1,000 | 0.294 (0.073) | 0.455 (0.084) | +0.160 (0.077) | 0.080 / 0.186 | 5 | 4.8 | 61.0 |
| mean | 1,000 | 0.294 (0.073) | 0.363 (0.107) | +0.069 (0.107) | 0.080 / 0.140 | 6 | 4.1 | 53.5 |
| mean, all | 1,000 | 0.294 (0.073) | 0.511 (0.142) | +0.217 (0.124) | 0.080 / 0.209 | 4 | 4.1 | 53.5 |
| mean, acquire | 1,000 | 0.294 (0.073) | 0.428 (0.107) | +0.134 (0.098) | 0.080 / 0.118 | 2 | 4.2 | 54.0 |
| mean, explore 0.15 | 1,000 | 0.294 (0.073) | 0.395 (0.122) | +0.101 (0.117) | 0.080 / 0.234 | 5 | 2.0 | 28.0 |
| mean, seed 12 | 1,000 | 0.294 (0.073) | 0.449 (0.107) | +0.155 (0.095) | 0.080 / 0.133 | 2 | 4.2 | 54.8 |
| Defaults | 3,000 | 0.194 (0.059) | 0.358 (0.103) | +0.164 (0.095) | 0.080 / 0.259 | 6 | 6.6 | 82.9 |
| mean | 3,000 | 0.194 (0.059) | 0.334 (0.122) | +0.140 (0.101) | 0.080 / 0.214 | 5 | 4.5 | 57.5 |
| mean, all | 3,000 | 0.194 (0.059) | 0.313 (0.122) | +0.119 (0.100) | 0.080 / 0.208 | 4 | 4.5 | 57.5 |
| mean, acquire | 3,000 | 0.194 (0.059) | 0.315 (0.123) | +0.122 (0.101) | 0.080 / 0.208 | 4 | 4.6 | 58.9 |
| mean, explore 0.15 | 3,000 | 0.194 (0.059) | 0.207 (0.066) | +0.014 (0.043) | 0.080 / 0.091 | 4 | 4.4 | 56.4 |
| mean, seed 12 | 3,000 | 0.194 (0.059) | 0.200 (0.061) | +0.006 (0.022) | 0.080 / 0.080 | 0 | 4.7 | 60.0 |

Nothing beat the standard sim in the midgame. The variants that came closest
were the ones that stayed closest to static equity (fewer search rollouts,
more static plays in the pool). This is what led to trying late positions.

**Run 3.** 96 late positions, 250 samples per play, no reuse.

| Variant | Budget | Standard | LAMC | Difference | Set regret | Outside | Rounds | Evaluated |
|---|---|---|---|---|---|---|---|---|
| Defaults | 1,000 | 0.745 (0.321) | 0.933 (0.202) | +0.188 (0.361) | 0.473 / 0.615 | 15 | 4.8 | 61.8 |
| mean | 1,000 | 0.745 (0.321) | 0.802 (0.159) | +0.057 (0.340) | 0.473 / 0.421 | 16 | 4.4 | 56.5 |
| mean, all | 1,000 | 0.745 (0.321) | 0.659 (0.140) | -0.086 (0.272) | 0.473 / 0.273 | 12 | 4.4 | 56.5 |
| mean, acquire | 1,000 | 0.745 (0.321) | 0.743 (0.166) | -0.002 (0.264) | 0.473 / 0.294 | 9 | 4.5 | 57.9 |
| mean, explore 0.15 | 1,000 | 0.745 (0.321) | 0.909 (0.196) | +0.164 (0.339) | 0.473 / 0.609 | 16 | 2.0 | 28.0 |
| mean, seed 12 | 1,000 | 0.745 (0.321) | 0.811 (0.166) | +0.065 (0.279) | 0.473 / 0.298 | 11 | 4.4 | 57.4 |
| Defaults | 3,000 | 0.684 (0.319) | 0.802 (0.203) | +0.118 (0.345) | 0.473 / 0.628 | 18 | 6.9 | 86.5 |
| mean | 3,000 | 0.684 (0.319) | 0.697 (0.163) | +0.012 (0.287) | 0.473 / 0.415 | 12 | 5.3 | 67.6 |
| mean, all | 3,000 | 0.684 (0.319) | 0.512 (0.140) | -0.173 (0.272) | 0.473 / 0.288 | 9 | 5.3 | 67.6 |
| mean, acquire | 3,000 | 0.684 (0.319) | 0.395 (0.120) | -0.289 (0.293) | 0.473 / 0.228 | 9 | 5.5 | 70.2 |
| mean, explore 0.15 | 3,000 | 0.684 (0.319) | 0.983 (0.343) | +0.298 (0.130) | 0.473 / 0.664 | 10 | 5.0 | 64.4 |
| mean, seed 12 | 3,000 | 0.684 (0.319) | 0.402 (0.108) | -0.283 (0.279) | 0.473 / 0.262 | 8 | 5.4 | 68.8 |

The first sign of a late-game gain: static equity's top 15 misses the best
play by 0.47 points here, against 0.08 in the midgame.

**Run 4.** 384 late tuning positions, 250 samples per play, no reuse.

| Variant | Budget | Standard | LAMC | Difference | Set regret | Outside | Rounds | Evaluated |
|---|---|---|---|---|---|---|---|---|
| mean, all, acquire | 1,000 | 0.689 (0.133) | 0.722 (0.088) | +0.033 (0.118) | 0.392 / 0.276 | 38 | 4.5 | 57.5 |
| mean, acquire | 1,000 | 0.689 (0.133) | 0.711 (0.088) | +0.022 (0.118) | 0.392 / 0.276 | 38 | 4.5 | 57.5 |
| mean, all, acquire, seed 12 | 1,000 | 0.689 (0.133) | 0.714 (0.098) | +0.025 (0.129) | 0.392 / 0.305 | 36 | 4.5 | 57.5 |
| mean, all, acquire, explore 0.5 | 1,000 | 0.689 (0.133) | 0.767 (0.101) | +0.078 (0.133) | 0.392 / 0.298 | 31 | 5.5 | 69.5 |
| mean, all, acquire, `-lamcshallow 8` | 1,000 | 0.689 (0.133) | 0.662 (0.081) | -0.027 (0.124) | 0.392 / 0.236 | 39 | 2.0 | 28.0 |
| mean, all | 1,000 | 0.689 (0.133) | 0.815 (0.113) | +0.126 (0.136) | 0.392 / 0.393 | 40 | 4.4 | 56.3 |
| mean, all, acquire, `-lamcbeta 2` | 1,000 | 0.689 (0.133) | 0.720 (0.088) | +0.031 (0.118) | 0.392 / 0.277 | 37 | 4.5 | 57.5 |
| mean, all, acquire | 3,000 | 0.546 (0.127) | 0.445 (0.088) | -0.102 (0.129) | 0.392 / 0.298 | 35 | 5.5 | 69.5 |
| mean, acquire | 3,000 | 0.546 (0.127) | 0.445 (0.088) | -0.102 (0.129) | 0.392 / 0.298 | 35 | 5.5 | 69.5 |
| mean, all, acquire, seed 12 | 3,000 | 0.546 (0.127) | 0.339 (0.057) | -0.208 (0.113) | 0.392 / 0.212 | 33 | 5.5 | 69.4 |
| mean, all, acquire, explore 0.5 | 3,000 | 0.546 (0.127) | 0.445 (0.088) | -0.102 (0.129) | 0.392 / 0.298 | 35 | 5.5 | 69.5 |
| mean, all, acquire, `-lamcshallow 8` | 3,000 | 0.546 (0.127) | 0.431 (0.072) | -0.115 (0.130) | 0.392 / 0.221 | 39 | 5.3 | 67.6 |
| mean, all | 3,000 | 0.546 (0.127) | 0.492 (0.092) | -0.054 (0.126) | 0.392 / 0.318 | 32 | 5.2 | 67.0 |
| mean, all, acquire, `-lamcbeta 2` | 3,000 | 0.546 (0.127) | 0.444 (0.087) | -0.103 (0.129) | 0.392 / 0.298 | 36 | 5.4 | 69.2 |

At 3,000 rollouts the search ends on stable kept plays before it reaches its
explore share, which is why several rows are identical.

**Run 5.** The same 384 late positions at 1,000 rollouts, every variant with
mean, all, acquire and seed 12, each with and without sample reuse.

| Features | Difference, no reuse | Difference, reuse | Set regret |
|---|---|---|---|
| Default set | +0.025 (0.129) | -0.036 (0.131) | 0.392 / 0.305 |
| score, leave, equity | +0.140 (0.136) | +0.061 (0.134) | 0.392 / 0.374 |
| Default set and equity | +0.065 (0.131) | +0.003 (0.134) | 0.392 / 0.319 |
| rack | +0.089 (0.132) | +0.016 (0.129) | 0.392 / 0.346 |
| board | +0.111 (0.122) | +0.030 (0.118) | 0.392 / 0.361 |

Reuse lowered regret by 0.06 to 0.08 in every row, which is why it became a
sim option and later a default.

**Run 6.** The same 384 late positions, with reuse; every variant adds to
mean, all, acquire and seed 12 (the first row).

| Variant | Budget | Standard | LAMC | Difference | Set regret | Outside | Rounds | Evaluated |
|---|---|---|---|---|---|---|---|---|
| (base) | 1,000 | 0.689 (0.133) | 0.653 (0.099) | -0.036 (0.131) | 0.392 / 0.305 | 38 | 4.5 | 57.5 |
| features score, leave, equity | 1,000 | 0.689 (0.133) | 0.750 (0.107) | +0.061 (0.134) | 0.392 / 0.374 | 39 | 4.4 | 56.9 |
| features rack | 1,000 | 0.689 (0.133) | 0.705 (0.105) | +0.016 (0.129) | 0.392 / 0.346 | 38 | 4.5 | 57.5 |
| features board | 1,000 | 0.689 (0.133) | 0.719 (0.099) | +0.030 (0.118) | 0.392 / 0.361 | 37 | 4.5 | 57.6 |
| features default and leavedups | 1,000 | 0.689 (0.133) | 0.695 (0.121) | +0.006 (0.116) | 0.392 / 0.362 | 36 | 4.5 | 57.5 |
| `-lamcpool 24 -lamckeep 6`, seed 16 | 1,000 | 0.689 (0.133) | 0.858 (0.143) | +0.169 (0.125) | 0.392 / 0.493 | 45 | 3.0 | 60.0 |
| `-lamclength 3` | 1,000 | 0.689 (0.133) | 0.699 (0.101) | +0.010 (0.128) | 0.392 / 0.331 | 40 | 4.5 | 57.5 |
| explore 0.2 | 1,000 | 0.689 (0.133) | 0.892 (0.140) | +0.203 (0.126) | 0.392 / 0.550 | 41 | 3.0 | 40.0 |
| explore 0.5 | 1,000 | 0.689 (0.133) | 0.592 (0.082) | -0.097 (0.116) | 0.392 / 0.212 | 33 | 5.5 | 69.4 |
| (base) | 3,000 | 0.546 (0.127) | 0.359 (0.060) | -0.188 (0.113) | 0.392 / 0.212 | 33 | 5.5 | 69.4 |
| features score, leave, equity | 3,000 | 0.546 (0.127) | 0.523 (0.093) | -0.023 (0.120) | 0.392 / 0.352 | 35 | 5.3 | 67.7 |
| features rack | 3,000 | 0.546 (0.127) | 0.423 (0.072) | -0.123 (0.107) | 0.392 / 0.248 | 38 | 5.4 | 69.3 |
| features board | 3,000 | 0.546 (0.127) | 0.547 (0.113) | +0.001 (0.098) | 0.392 / 0.376 | 35 | 5.5 | 69.8 |
| features default and leavedups | 3,000 | 0.546 (0.127) | 0.374 (0.061) | -0.173 (0.103) | 0.392 / 0.214 | 35 | 5.4 | 69.1 |
| `-lamcpool 24 -lamckeep 6`, seed 16 | 3,000 | 0.546 (0.127) | 0.535 (0.097) | -0.011 (0.129) | 0.392 / 0.354 | 39 | 4.9 | 88.6 |
| `-lamclength 3` | 3,000 | 0.546 (0.127) | 0.348 (0.052) | -0.199 (0.113) | 0.392 / 0.201 | 35 | 5.5 | 69.4 |
| explore 0.2 | 3,000 | 0.546 (0.127) | 0.359 (0.060) | -0.188 (0.113) | 0.392 / 0.212 | 33 | 5.5 | 69.4 |
| explore 0.5 | 3,000 | 0.546 (0.127) | 0.359 (0.060) | -0.188 (0.113) | 0.392 / 0.212 | 33 | 5.5 | 69.4 |

**Run 7.** Late, fresh A: 768 positions, 250 samples per play, with reuse;
the base is mean, all, acquire, seed 12 and explore 0.5, with no static
floor.

| Variant | Budget | Standard | LAMC | Difference | Set regret | Outside | Rounds | Evaluated |
|---|---|---|---|---|---|---|---|---|
| (base) | 1,000 | 0.611 (0.060) | 0.642 (0.062) | +0.031 (0.038) | 0.289 / 0.276 | 49 | 5.4 | 68.6 |
| `-lamclength 3` | 1,000 | 0.611 (0.060) | 0.627 (0.062) | +0.016 (0.037) | 0.289 / 0.262 | 50 | 5.4 | 68.6 |
| (base) | 3,000 | 0.433 (0.048) | 0.447 (0.053) | +0.014 (0.033) | 0.289 / 0.276 | 39 | 5.4 | 68.6 |
| `-lamclength 3` | 3,000 | 0.433 (0.048) | 0.431 (0.052) | -0.002 (0.032) | 0.289 / 0.262 | 41 | 5.4 | 68.6 |

The gain of -0.19 on the tuning set did not hold on fresh positions. This is
what led to the static floor.

**Run 8.** The 384 midgame tuning positions, 250 samples per play, with
reuse; the base is mean, all, acquire, seed 12 and explore 0.5. The rows
marked "signal variance fit" ran after that fit was added, which moved the
surrogate's rank correlation from 0.721 to 0.725 (0.718 with explore 0.3;
static equity: 0.746).

| Variant | Budget | Standard | LAMC | Difference | Set regret | Outside | Rounds | Evaluated |
|---|---|---|---|---|---|---|---|---|
| (base) | 1,000 | 0.254 (0.029) | 0.433 (0.056) | +0.179 (0.047) | 0.007 / 0.111 | 10 | 4.7 | 60.7 |
| `-lamclength 3` | 1,000 | 0.254 (0.029) | 0.430 (0.056) | +0.176 (0.047) | 0.007 / 0.111 | 10 | 4.7 | 60.7 |
| floor 8 | 1,000 | 0.254 (0.029) | 0.323 (0.034) | +0.069 (0.026) | 0.007 / 0.007 | 2 | 4.7 | 60.7 |
| floor 10 | 1,000 | 0.254 (0.029) | 0.323 (0.034) | +0.069 (0.026) | 0.007 / 0.007 | 2 | 4.7 | 60.7 |
| floor 12 | 1,000 | 0.254 (0.029) | 0.333 (0.036) | +0.079 (0.024) | 0.007 / 0.007 | 2 | 4.7 | 60.7 |
| (base), signal variance fit | 1,000 | 0.254 (0.029) | 0.406 (0.050) | +0.152 (0.041) | 0.007 / 0.088 | 7 | 4.7 | 60.1 |
| floor 10, signal variance fit | 1,000 | 0.254 (0.029) | 0.320 (0.034) | +0.066 (0.026) | 0.007 / 0.007 | 0 | 4.7 | 60.1 |
| floor 10, explore 0.3, signal variance fit | 1,000 | 0.254 (0.029) | 0.290 (0.031) | +0.037 (0.019) | 0.007 / 0.007 | 0 | 4.2 | 54.8 |
| (base) | 3,000 | 0.129 (0.017) | 0.236 (0.045) | +0.108 (0.041) | 0.007 / 0.111 | 8 | 4.7 | 60.7 |
| `-lamclength 3` | 3,000 | 0.129 (0.017) | 0.238 (0.046) | +0.109 (0.041) | 0.007 / 0.111 | 8 | 4.7 | 60.7 |
| floor 8, 10 or 12 | 3,000 | 0.129 (0.017) | 0.133 (0.017) | +0.004 (0.007) | 0.007 / 0.007 | 0 | 4.7 | 60.7 |
| (base), signal variance fit | 3,000 | 0.129 (0.017) | 0.218 (0.039) | +0.089 (0.035) | 0.007 / 0.088 | 7 | 4.7 | 60.1 |
| floor 10, with or without explore 0.3, signal variance fit | 3,000 | 0.129 (0.017) | 0.133 (0.017) | +0.005 (0.006) | 0.007 / 0.007 | 0 | 4.7 | 60.1 |

**Run 9.** Late, fresh B: 384 positions, 800 samples per play, with reuse;
the base is mean, all, acquire, seed 12 and explore 0.5. The rank
correlation was 0.584 with the equity prior mean and 0.502 with a constant
one; static equity's was 0.531.

| Variant | Budget | Standard | LAMC | Difference | Set regret | Outside | Rounds | Evaluated |
|---|---|---|---|---|---|---|---|---|
| (base) | 3,000 | 0.508 (0.123) | 0.404 (0.077) | -0.104 (0.122) | 0.411 / 0.302 | 38 | 5.4 | 68.8 |
| floor 10 | 3,000 | 0.508 (0.123) | 0.298 (0.057) | -0.209 (0.111) | 0.411 / 0.197 | 24 | 5.4 | 68.8 |
| floor 12 | 3,000 | 0.508 (0.123) | 0.307 (0.057) | -0.201 (0.110) | 0.411 / 0.204 | 23 | 5.4 | 68.8 |
| Constant mean | 3,000 | 0.508 (0.123) | 0.500 (0.075) | -0.007 (0.139) | 0.411 / 0.396 | 68 | 6.8 | 85.3 |
| (base) | 10,000 | 0.461 (0.122) | 0.342 (0.075) | -0.119 (0.122) | 0.411 / 0.302 | 38 | 5.4 | 68.8 |
| floor 10 | 10,000 | 0.461 (0.122) | 0.238 (0.053) | -0.224 (0.110) | 0.411 / 0.197 | 25 | 5.4 | 68.8 |
| floor 12 | 10,000 | 0.461 (0.122) | 0.245 (0.054) | -0.217 (0.110) | 0.411 / 0.204 | 24 | 5.4 | 68.8 |
| Constant mean | 10,000 | 0.461 (0.122) | 0.443 (0.070) | -0.019 (0.138) | 0.411 / 0.396 | 62 | 6.8 | 85.3 |

**Run 10.** Late, fresh C: 768 positions, the defaults as committed (explore
0.3 and 0.5 gave identical results). 400 samples per play at 3,000 rollouts;
the 10,000-rollout budget ran out of samples, so it was rerun on the same
positions with 700.

| Budget | Standard | LAMC | Difference | Set regret | Outside | Rounds | Evaluated | Rank correlation, LAMC / static |
|---|---|---|---|---|---|---|---|---|
| 3,000 | 0.524 (0.074) | 0.463 (0.061) | -0.060 (0.033) | 0.417 / 0.334 | 43 | 5.3 | 67.4 | 0.586 / 0.544 |
| 10,000 | 0.470 (0.072) | 0.383 (0.057) | -0.087 (0.033) | 0.411 / 0.324 | 47 | 5.3 | 67.4 | 0.588 / 0.547 |

**Games.** Both with `-lamc1 true` against the standard sim, at the settings
in [Results](#results).

| Run | Pairs | LAMC's score | Spread per pair (SE) | Searches | Rounds | Plays evaluated | Search rollouts | Search time | Candidates outside the static top 15, per search | Choices outside it |
|---|---|---|---|---|---|---|---|---|---|---|
| Pilot, seed 11 | 160 | 50.31% | -2.20 (2.69) | 662 | 5.29 | 65.6 | 331 | 147 ms | 1.04 | 39 (5.9%) |
| Seed 12 | 1,500 | 50.22% | -0.04 (0.84) | 6,246 | 5.42 | 67.1 | 339 | 168 ms | 1.06 | 377 (6.0%) |

The speed runs (160 pairs, seed 11) of LAMC off and of main gave identical
results, as they should.

### What to do next

1. **Measure it in games where it acts.** LAMC changes only the last few
   turns of a game, so whole-game pairs need tens of thousands of games to
   see it. Game pairs started from late positions (bag of 20 or fewer),
   at a real time limit, would measure the same thing with far fewer.
2. **Find where the gain stops.** The bag limit of 20 is where the evidence
   is; positions with 21 to 29 tiles were not tested.
3. **Compare with PEG** in the positions PEG solves, as the plan says: the
   exact answer is the best test of the late-game gain.
4. **A better prior mean.** The prior mean linear in static equity is what
   made the surrogate useful; a stronger static evaluator (PAT, or the CNN
   evaluator from the talk) as the prior mean is the most direct next gain.
5. **Per-feature length scales.** The ablation shows the rack features
   matter most and the board features help only with them. Per-feature
   length scales, fit by marginal likelihood or stored by name as the plan
   proposed, should let the surrogate weight them accordingly.
6. **Late-game features.** Static equity misranks late plays for reasons
   the features only partly see: whether a play empties the bag, what the
   opponent can go out with, and what a play leaves for the last draw.
7. **Let BAI count the search's samples.** The final sim resumes from them
   for its statistics, but BAI's own stopping rule starts from zero, so it
   may sample the search's best plays more than it needs to.
8. **Phase 2 starts from here.** The replay harness, the subset view and
   the registry carry over unchanged. The late game, where the rollout
   policy is most likely wrong, is where to test adaptive responses first.

### Open questions

What Phase 1 did not settle. Each question says what the runs showed and
what would answer it.

1. **Is the late-game gain real in games?** On fresh positions the replay
   shows 0.06 to 0.09 points of win percentage per late decision, about two
   standard errors from zero, and whole-game pairs cannot see a gain that
   small. Game pairs started from late positions, or a comparison with PEG,
   would answer it (items 1 and 3 of [What to do next](#what-to-do-next)).
2. **Is it the surrogate, or only a wider look?** LAMC hands the sim plays
   from beyond static equity's top 15. No run compared it with a standard
   sim that simply takes more static plays (the top 30, say) at the same
   budget, or with candidates chosen by the prior mean alone, without
   search rollouts. If either matches LAMC, the GP is not what helps. Each
   is one replay run.
3. **Why does the surrogate rank midgame plays worse than static equity?**
   With a prior mean linear in static equity and a fitted signal variance,
   the surrogate should fall back to static equity's order when the samples
   show nothing more. Its rank correlation is still 0.72 against 0.75.
   Possible causes:
   - the prior mean's slope is fit to noisy means of a few samples each;
   - the smallest signal variance on offer (0.1 times the excess) is not
     small enough;
   - one length scale for every column lets weak features add noise.

   Measuring the rank correlation with each changed in turn, starting with
   the signal variance forced to zero, would find which.
4. **Does the replay predict the live sim?** The replay samples the final
   stage evenly, where the sim uses BAI. It never used a stopping
   threshold, a time limit, inference, bag cycling, more than 2 plies or a
   lexicon other than CSW21. It never used the live universe either: the
   live default is up to 400 plays within 30 points, and the replay took
   the top 100. The `lamcautoplay` runs used the live sim, but only at 3,000
   rollouts with no threshold and no inference. Any of these could shrink
   the gain or grow it.
5. **Does BAI's stopping rule work on resumed statistics?** With reuse, the
   sim's plays start from the search's statistics, but BAI's own counts
   start from zero, so its threshold test does not see the search's
   samples. No run used a threshold, so whether this wastes rollouts or
   stops too early is unknown.
6. **Where does the gain end?** It was measured with 1 to 20 tiles in the
   bag and is absent with 30 or more. 21 to 29 tiles were never tested.
   Nor was the split within 1 to 20: the gain could all come from the
   positions where a play can empty the bag. Replay sets by bag size would
   answer both.
7. **How much of the tuning is noise?** The defaults were chosen on 384
   late and 384 midgame positions. On fresh positions, the late gain
   shrank from 0.19 to 0.06. The final late numbers come from a set never
   used for tuning; the midgame numbers come from the tuning set. Each
   default was picked
   from comparisons within each other's standard errors. A sweep of the
   main settings on a fresh set, with paired differences between variants
   rather than against the standard sim, would show which defaults matter.
8. **Are the GP's uncertainties calibrated?** Acquisition ranks plays by
   the mean plus a multiple of the standard deviation. The noise model
   treats samples as independent, though common random numbers correlate
   them. Nothing checked whether the predicted standard deviations match
   the errors. `-lamcbeta` 1 and 2 did alike, which says little either way.
9. **Should the search spend more at large budgets?** At 3,000 and 10,000
   rollouts it stops on stable kept plays after about 340 rollouts, 11% and
   3% of the budget, so explore shares above that change nothing. More
   rounds, a larger pool, or more samples per play at large budgets were
   not tested.
10. **Which features matter, one by one?** The ablation compared groups,
    each within a standard error of the others. Whether `leavedups`,
    `equity`, `perimeter` or `premium` helps alone is unknown. Per-feature
    length scales fit by marginal likelihood (item 5 of What to do next)
    would read the answer from the searches' own samples.
11. **Is Phase 2 worth starting on this evidence?** Phase 1's gain is small
    and confined to the late game. Phase 2 adapts the rollout policy,
    which is a different lever: the late game is also where that policy
    (the top static play) is most likely wrong. The answer to question 1
    bears on whether a per-ply search will repay its cost.

---

## Phase 2: adaptive search at every ply

### Goal

Stop assuming each player answers with the top-equity play inside rollouts,
and keep the cost manageable by making each permutation more informative
(I'_perm much smaller than I_perm).

### 2a. A rollout policy seam

`sim_rollout` already chooses each ply's play in one expression (top equity,
or the census). Turn it into a named choice:

```c
typedef enum {
  ROLLOUT_POLICY_TOP_EQUITY,
  ROLLOUT_POLICY_CENSUS,
  ROLLOUT_POLICY_ADAPTIVE,
} rollout_policy_t;
```

It is a `switch` in the `Simmer`, set once per sim, in place of today's
`use_census_rollout` ternary. This PR adds only the enum and the switch, and
must be bit-identical and speed-neutral (benchmarked against main on
`censusrolloutbench` and a plain sim). A future policy, such as a learned one
from Phase 3, is one more case.

### 2b. Adaptive responses

With `ROLLOUT_POLICY_ADAPTIVE`, at each ply listed in `-lamcplies` (for
example `0` for only the reply to the candidate, or `0,1,2`), the player on
turn chooses by a small Phase 1 search instead of top equity:

- Embed the ply's legal plays with the `-lamcplyfeatures` set.
- Evaluate a small pool (P' plays, for example 8) with I_shallow *nested*
  rollouts each. A nested rollout plays the rest of the horizon with the
  top-equity policy, so the nesting is one level deep.
- Fit the GP, retain, propose, for at most `I_adapt` rounds (for example 2),
  and play the best by posterior mean.

Ply 0, the opponent's reply, is the first target: it is where the slides'
argument (human opponents do not play the top-equity reply) bites hardest.
The other plies are added only if the tests below show a gain.

The cost of this step alone is the slides' T_adaptive. It is implemented
plainly first, so that 2c and 2e can be measured against it.

### 2c. Amortization within a sim

The rollouts of one candidate share its board after the candidate is played.
Only the opponent's rack and the bag differ. So the reply search at ply 0
learns almost the same function in every permutation. Instead of fitting a
fresh GP per permutation:

- Keep one surrogate per candidate and ply for the whole sim. Its inputs are
  the play embedding plus a rack embedding of the responder's rack (2d).
- Each permutation adds its nested results to that surrogate, then spends
  nested rollouts only where the surrogate is uncertain (posterior standard
  deviation over a threshold). Later permutations need few or none.
- The surrogate's size is capped (`E_max` points per candidate and ply, the
  oldest dropped first), and it is only read and written by the thread that
  owns that candidate's rollouts, or under the `SimmedPlay` lock. Which of
  the two is decided by measurement.

This is how each adaptive iteration extracts more signal per permutation,
as slide 31 asks.

### 2d. Rack similarity

The slides' procedure:

1. Generate N game positions.
2. Build a sparse outcome matrix Y, racks by positions. English has about
   3.2 million distinct 7-tile racks.
3. For each position, sample racks and record the win percentage of the top
   simming play with that rack.
4. Factorize Y = F G^T + e, with F in R^(N x d) and G in R^(racks x d). The
   rack similarity kernel is K = G G^T.

The plan:

- **Data generation in C.** A new command, `rackembedgen`, built like
  `leavegen` and `contribute`: multithreaded, checkpointed, writing
  (position, rack index, win percentage, rollouts) rows. Rack index is
  `census_lattice_rank` of the rack, which already gives every multiset of
  up to 7 tiles a dense rank with no stored table.
- **Factorization offline, first.** A Python script in `scripts/` does
  alternating least squares with regularization on the sparse Y, weighting
  each entry by its rollouts, and picks d by held-out error. Racks never
  observed get the embedding of their nearest observed neighbors by leave
  (sharing 6 of 7 tiles). If the pipeline is kept, the factorization moves
  to C so that generation needs no Python.
- **The file.** `data/lamc/<lexicon>.rackemb`: a header (lexicon,
  distribution, d, quantization, row count) and G, indexed by lattice rank.
  It is documented in [data-formats.md](data-formats.md) and read through
  the `src/compat` endian helpers. Memory is in [Future work](#memory).
- **A variational autoencoder** (slide 31) is an alternative to the linear
  factorization. It is out of scope for Phase 2 unless the linear one fails
  the tests below.
- **A rack feature.** The rack embedding also becomes a registered feature
  (`leave_latent`, the embedding of the play's leave). It enters the Phase 1
  space through the same registry and is judged by the same ablation.

### 2e. Using rack similarity in sampling

The talk's aim is "not cheaper simulations, but far more informative
permutations, both within and across plies". Three uses, each behind its own
flag so each can be measured alone:

- **Stratified opponent racks** (`-sstrat`). Cluster the racks by G (k-means
  on the embedding, done once into the file). Draw each permutation's
  opponent rack stratified across clusters in proportion to each cluster's
  probability given the unseen tiles, and weight samples so that the
  estimate stays unbiased. With inference on, the inferred rack
  distribution supplies the cluster probabilities. This generalizes what
  `-sbagcycle` does with rotations, and is compared against it.
- **Antithetic pairing** (`-santithetic`). Pair each permutation with one
  whose opponent rack is far from it in the embedding (similar probability,
  opposite position on the leading component). That is the analogue of
  antithetic variates the slides name: paired outcomes are negatively
  correlated, so their mean has lower variance.
- **Sharing across permutations.** The 2c surrogate's kernel over racks is K
  restricted to the racks seen, so a result for one rack informs similar
  racks in later permutations.

### Modules

| File | Contents |
|---|---|
| `src/def/sim_defs.h` | `rollout_policy_t` |
| `src/impl/rollout_adaptive.c/.h` | The ply search and its amortized surrogates |
| `src/ent/rack_embedding.c/.h` | Loading, lookup by lattice rank, clusters |
| `src/impl/rack_sampler.c/.h` | Stratified and antithetic opponent racks |
| `src/impl/rackembedgen.c/.h` | The data generation command |
| `scripts/rack_factorize.py` | The offline factorization |

The ply search reuses `play_embedding`, `gp` and `lamc_proposal` from Phase
1 unchanged. If Phase 2 needs a change to them, that change is its own PR,
with the Phase 1 tests rerun.

### Options

`-rpolicy top|census|adaptive` (with `-scensus` kept as an alias for
`census`), `-lamcplies`, `-lamcplyfeatures`, `-lamcplypool` (P'),
`-lamcplyrounds`, `-lamcamortize`, `-rackemb <file>`, `-sstrat`,
`-santithetic`.

### PR sequence

| PR | Contents | Behavior change |
|---|---|---|
| 2a | Rollout policy enum and switch | None; bit-identical, speed-neutral |
| 2b | Adaptive policy at chosen plies, plain | Only with `-rpolicy adaptive` |
| 2c | Amortized surrogates | Only with `-lamcamortize` |
| 2d-1 | `rackembedgen` and its data rows | None |
| 2d-2 | Factorization script, file format, loader, `leave_latent` feature | None until selected |
| 2e | Stratified and antithetic sampling, rack kernel sharing | Only with their flags |
| 2f | Tuned defaults with the results below | Only with the flags |

### Phase 2 testing and proof

#### Correctness, in CI

- **Policy seam.** With `-rpolicy top` and `census`, every sim and autoplay
  test gives bit-identical results to main.
- **Adaptive policy.** Deterministic across thread counts. Nested rollouts
  never recurse past one level. Rollout and nested-rollout counts match the
  budget formula exactly on small cases.
- **Rack embedding.** File round trip (write, load, compare). Every full
  rack drawable from the distribution has a row. Loading a file for another
  lexicon or distribution fails with a clear error.
- **Unbiased sampling.** On a small bag where every opponent rack can be
  enumerated, the weighted mean of a known function of the rack under
  stratified and antithetic sampling converges to the exact expectation
  (checked to a tolerance over many seeds), and so does the variance
  estimate.

#### Variance reduction (on demand: `lamcvariance`)

On the Phase 1 position set, for fixed plays, measure the standard error of
the win percentage estimate after I permutations with plain iid racks,
`-sbagcycle`, stratified, antithetic, and both. Report the effective sample
size ratio: how many iid permutations each method's I is worth. This is the
direct test of "I'_perm much smaller than I_perm"; a ratio near 1 means 2e
does not work and the cost of 2b is not paid for.

#### The cost model (on demand: `lamccost`)

Measure T_standard and T_adaptive on the position set across P, p and
I_perm, with and without 2c, and fit the constants of the slides' formulas.
The results set the defaults for `-lamcplies` and the nested budgets, and
show how much of the cost amortization removes.

#### Response quality

- **Against an exact solver.** In positions PEG can solve, PEG's best play
  is exact and accounts for the opponent's best replies, not just
  top-equity ones. Compare the agreement and regret of standard sim, Phase 1
  LAMC and Phase 2 LAMC there, broken down by tiles in the bag. The talk
  says the top-equity assumption is weakest in the pre-endgame, so this is
  where Phase 2 should gain most.
- **Against human replies (optional).** Over GCGs of strong players' games,
  compute how likely each policy finds the reply actually played (its rank
  among the policy's choices). An adaptive policy that predicts real replies
  better models opponents better.

#### Strength

Game pairs, equal wall time (the only fair budget once rollouts differ in
cost), three matchups: Phase 2 against Phase 1, Phase 2 against the standard
sim, and each 2e flag on against off. Same protocol as Phase 1: fixed number
of pairs, pentanomial reporting, breakdown by bag size. Equal rollouts are
also reported, as a measure of quality per rollout.

#### Phase 2 acceptance

1. 2a is bit-identical and speed-neutral.
2. At least one 2e method gives an effective sample size ratio well above 1
   (for example 2 or more).
3. Phase 2 has lower regret than Phase 1 against PEG in pre-endgame
   positions.
4. Game pairs at equal time show a gain over Phase 1 whose confidence
   interval excludes 0, overall or in a stage named before the run (for
   example 1 to 14 tiles in the bag). A stage-only gain ships as a
   stage-gated default.

---

## Risks and open questions

These are the risks as planned. What Phase 1 found about them, and the
questions it raised, are in [Open questions](#open-questions).

- **The space may carry little signal beyond equity.** Then the GP is no
  better than sorting by equity and Phase 1 is wasted rollouts. The
  surrogate test settles this early, in PR 1e, before any tuning.
- **Noise at small I_shallow.** Win percentage over 8 rollouts is very
  noisy. Common random numbers help comparisons between plays, but the GP's
  per-point noise model treats points as independent, which they are not.
  If the fits are poor, model the shared-seed correlation, or fit
  differences from a reference play instead of raw means.
- **Snapping.** A gradient step can land where there are no plays, and the
  nearest play may be far away. The `acquire` strategy avoids this, and the
  tests compare the two.
- **Can BAI resume?** Covered in [Integration](#integration-with-the-sim);
  answered in PR 1d.
- **Nested cost.** If amortization does not cut the nested rollouts, Phase
  2 may only pay at ply 0, or only in the pre-endgame.
- **Rack data cost.** Filling Y well enough for 3.2 million racks needs a
  very large number of sims. Most racks will be seen rarely or never, so the
  embedding of rare racks rests on the neighbor rule.

---

## Future work

### Strength of play

- **Distillation (the talk's part 3).** Use LAMC sims as the training signal
  for a deep model of move choice: the continuous embeddings are richer
  inputs, and higher-fidelity sims are better targets. Train it to imitate
  LAMC (supervised distillation), then improve it by reinforcement learning
  against itself. The result could serve as a rollout policy
  (`ROLLOUT_POLICY_LEARNED`), a GP mean function, or a static player.
- **Learned embeddings.** Replace hand-made features with embeddings learned
  from sim results (an encoder trained so that distance predicts win
  percentage difference). They plug into the registry as features.
- **Better acquisition.** Expected improvement, knowledge gradient, or
  batch acquisition (choose plays jointly, to fill threads without
  redundant picks). Trust-region Bayesian optimization (TuRBO-style) for
  sharper local search.
- **Multi-fidelity.** Treat 1-ply sims, 2-ply sims and static equity as
  cheaper, correlated fidelities of the same function, and spend rollouts at
  the fidelity that buys the most information.
- **Per-position hyperparameters.** Refit length scales by marginal
  likelihood in each position when the budget is large (analysis rather
  than play).
- **Spread-aware targets.** Use the existing utility blend (`-uspread`) as
  the GP target, for positions where spread matters.
- **Inference.** Feed inferred opponent racks into 2e's stratification, and
  let the inference itself use LAMC replies (what would an adaptive opponent
  have kept).
- **Time management.** Let the explore share and the stopping rule respond
  to the clock and to how settled the top K is.
- **The endgame boundary.** Hand off to PEG and the endgame solver from
  inside rollouts once the bag is small, instead of the win percentage
  table.

### Speed

- **Incremental GP updates.** Extend the Cholesky factor by one row per new
  point (O(n^2)) instead of refactoring (O(n^3)).
- **Approximate GPs.** Inducing points or random Fourier features if
  `E_max` must grow well past 256, or for the per-ply surrogates.
- **Cheaper embeddings.** Compute only the features of plays that could be
  proposed (within the trust region of the pool); cache per-position
  `prepare` work across a turn's rounds; compute PAT-based features from the
  PAT evaluation already done for static equity.
- **Faster nested rollouts.** Steer nested generation with the census, and
  use WMP; reuse nested results across candidates whose boards agree in the
  relevant lanes (as the census does for anchors).
- **Early pruning inside the explore stage.** Let BAI's cutoff drop clearly
  losing pool plays mid-round, saving their remaining shallow rollouts.
- **Parallel rounds.** Overlap one round's GP fit with the next round's
  rollouts of retained plays, which do not depend on the fit.
- **PGO.** Add a LAMC autoplay run to the PGO training set once LAMC is a
  default.

### Memory

- **Rack embedding size.** Indexing by lattice rank, the full-rack block has
  about 4.27 million rows (all 7-multisets over 27 kinds), about 25% of
  them racks the English distribution cannot draw. At d = 16: 273 MB as float32, 137 MB
  as float16, 68 MB as int8 with a per-column scale. A compact index over
  only the 3.2 million drawable racks saves a quarter. Quantization and the
  compact index are measured against the effective sample size results
  before they are adopted.
- **Memory-mapping.** Map the rack embedding file (as `-ritmmap` does for
  the RIT) so that processes share it and pages load on demand.
- **Lower d.** Choose d by the variance-reduction test, not only by
  factorization error: a smaller d that keeps the gain is better.
- **WASM and small machines.** Keep the rack embedding optional and
  loadable at a lower precision, and check it against the `wasm-tests` job's
  memory limits.
- **Per-thread arenas.** Allocate embeddings, GP matrices and the per-ply
  surrogates from per-worker arenas sized once per sim (the pattern of
  `small_move_arena`), so autoplay sims stay allocation-free like today's.
- **Bounded surrogates.** Cap the 2c surrogates' total memory per sim, and
  drop the least-used candidates' surrogates first when BAI prunes them.
