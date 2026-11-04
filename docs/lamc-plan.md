# Locally Adaptive Monte Carlo: plan

Source: Morris Greenberg, "The Past, Present, and Future of Scrabble Engines"
(University of Toronto brown bag, January 13, 2026), slides 29 to 31:
"Future part 1: Locally Adaptive Monte Carlo for Comprehensive Search" and
"Future part 2: Locally Adaptive Monte Carlo on Plies". This document plans
both parts as two phases of work in MAGPIE. It describes the code as of
753d44d5 on main. Nothing here is implemented yet.

Every option name, file name and default below is a proposal. Each phase
ends with tests that decide whether its work ships.

## Contents

- [The idea](#the-idea)
- [Terms](#terms)
- [How the sim works today](#how-the-sim-works-today)
- [Ground rules for both phases](#ground-rules-for-both-phases)
- [The play embedding](#the-play-embedding): the shared, modular feature space
- [Phase 1: adaptive search over the turn's plays](#phase-1-adaptive-search-over-the-turns-plays)
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
