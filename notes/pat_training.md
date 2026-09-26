# Training PAT weights for a lexicon

`test/pat_build.sh` (15x15) and `test/pat_build_super.sh` (21x21) run the whole recipe
for one lexicon and validate the result against the lexicon's current weights; `test/pat_build_super_all.sh`
runs the 21x21 build for one lexicon per language. This note explains the steps they run.
Every step is a magpie command, run from the repository root with release builds
(`make magpie magpie_test BUILD=no_pgo_release`, with `BOARD_DIM=21` for the super board).

Training and validation pass `-patcap 1000`, which leaves the PAT adjustment unclipped; that is
how the shipped weights were built and validated. Play defaults to `-patcap 0` (see README).

## 0. Bootstrap

`test/pat_bootstrap.pat` is an all-zero file carrying the option rows the recipe trains under:

    gamma,0.500000
    own_asset_discount,0.000000
    lexicon_floaters,1
    signed_through,1
    fit_scaled,0
    fit_residual,5
    exact_created_hooks,1

`fit_residual,5` fits the hook-score channels as a residual on top of the other weights, and
`exact_created_hooks,1` resolves a move's created hooks on the lexicon instead of approximating
them. The file is lexicon-independent.

## 1. Iterative training

    ./bin/magpie patgen 30000,30000,30000,30000,30000 NAME_v3 -lex LEX -leaves LEAVES \
      -gp true -threads 10 -seed SEED -wmp true -patcap 1000 -pat NAME_bootstrap

Five generations of 30K game pairs, each generation's self-play under the previous
generation's weights; the label is the opponent's reply score; ridge with lambda =
1/observations; the unseen pool for each training row excludes the mover's leave.

Seed variance is real: identical recipes at different seeds differ by 0.2-0.4 equity per
disagreement, so compare seeds with whole-game validation if it matters.

## 2. Run-keyed through table and residual refit

Copy `NAME_v3.pat` to `NAME_v3_runres.pat` with `run_through,1` and `fit_residual,3`, then

    ./bin/magpie patgen 150000 NAME_v4 -lex LEX -leaves LEAVES -gp true -threads 10 \
      -seed 4242 -wmp true -patcap 1000 -pat NAME_v3_runres

One generation of 150K game pairs under the v3 weights (frozen), refitting only the
`float_through_*` weights against the run-keyed table. In the output, set `fit_residual,0`
so a later retrain from the file fits every channel.

## 3. Opening table

    ./bin/magpie_test patopeningsim:LEX:NAME_v4:RACKS:LEAVES[:LD]

Sims the top 12 static candidates of RACKS seeded opening racks four plies (400 iterations
each, round robin) under NAME_v4 -- weights WITHOUT an opening table, or the gap is measured
against a static equity that already carries one -- and prints the `opening_tiles_N` and
`opening_exchange` rows (milli-equity, relative to the best bin, all <= 0). The scripts use
1000 racks with a WMP and 300 without.

## 4. Utility correction

The scripts append `utility_adjust,350` (see `notes/pat_utility_correction.md`); its tables
come from the distribution's `winpct_<ld>` (`winpct_<ld>_super` on 21x21) when the weights
are loaded.

## 5. Validation

Whole-game mirrored pairs are the arbiter:

    ./bin/magpie autoplay games 500000 -lex LEX -leaves LEAVES -gp true -threads 10 \
      -seed SEED -wmp true -patcap 1000 -pat LEX -pat1 NAME -pat2 LEX

Read the `Player 1 spread per mirrored pair` line (mean, SE, 95% CI). Use 1M pairs for
effects near 0.1 points per pair.
